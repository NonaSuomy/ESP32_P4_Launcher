/*
 * BLE interface on Bluedroid: the same papp_if_ble_t as lx_svc_ble.c (NimBLE), for the boards that
 * build Bluedroid (dual mode: the launcher's Bluetooth Classic, A2DP to speakers, needs it). The
 * controller is a USB Bluetooth dongle when one is plugged in, else the C6 over esp_hosted (lx_bt.c).
 *
 * The stack is taken from lx_bt on the first call and let go at teardown: it stops then unless the
 * launcher's Bluetooth speaker still uses it, in which case only the app's part goes (advertising,
 * scan, its links and GATT services; the GATT registrations stay for the next app). Bluedroid calls back on its
 * BTC task: events are copied into a queue, GAP requests and client operations complete a
 * semaphore. Every wait is in 100 ms slices that give up when the app is asked to exit.
 *
 * Connections: the API's conn_id is Bluedroid's link index (BTC adds the gatt_if on the way down),
 * the same for the GATT server and client, so the app's papp_ble_conn_t is that link index. GATTS_CONNECT and
 * GATTS_DISCONNECT come for every link (the server is always registered), centrals' included.
 * Addresses: esp_bd_addr_t is most significant byte first, papp_ble_addr_t least first.
 * Bonds: Bluedroid keeps them in NVS (its own store; written on its BTC task, internal stack).
 */
#include "sdkconfig.h"
#include "lx_services.h"

#if CONFIG_BT_BLUEDROID_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lx_bt_priv.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_bt_main.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_common_api.h"

static const char *TAG = "lx_svc_ble";

#define SLICE_MS      100
#define GAP_MS        3000
#define REG_MS        3000
#define EVQ_LEN       32
#define DEF_NAME      "ESP32-P4"
#define MAX_FIND_SVC  16
#define MAX_FIND_CHR  64
#define OP_BUF        512
#define MAX_DSC_PER_CHR 4
#define LOCAL_MTU     256
#define APP_ID_S      0x55
#define APP_ID_C      0x56

/* ── State ──────────────────────────────────────────────────────────────── */

typedef struct {
    bool            used;
    uint8_t         idx;                        /* Bluedroid's link index = the app's conn */
    esp_bd_addr_t   bda;
    papp_ble_addr_t addr;
    uint16_t        mtu;
    uint8_t         sub[PAPP_BLE_MAX_CHRS];     /* server: 0 off, 1 notify, 2 indicate */
    bool            central;                    /* the app connected out (papp_ble_connect) */
} conn_t;

typedef struct {
    uint16_t flags, max_len;
    uint16_t val_handle, cccd_handle;
} chr_t;

static SemaphoreHandle_t s_life;        /* start / stop / register */
static SemaphoreHandle_t s_lock;        /* connections */
static SemaphoreHandle_t s_reg_sem, s_tab_sem;
static QueueHandle_t     s_evq;
static volatile bool     s_running, s_failed;
static uint16_t          s_svc_h[PAPP_BLE_MAX_SVCS];   /* the app's services, deleted at teardown */
static int               s_nsvc;
static volatile esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE, s_gattc_if = ESP_GATT_IF_NONE;
static esp_ble_addr_type_t s_own_type = BLE_ADDR_TYPE_PUBLIC;
static char              s_name[32] = DEF_NAME;
static conn_t            s_conns[PAPP_BLE_MAX_CONN];
static volatile bool     s_adv_active, s_scanning;

/* GATT server table */
static chr_t   s_chrs[PAPP_BLE_MAX_CHRS];
static int     s_chr_count;
static volatile esp_gatt_status_t s_tab_status;
static volatile uint16_t s_tab_n;
static uint16_t s_tab_handles[1 + PAPP_BLE_MAX_CHRS * (3 + MAX_DSC_PER_CHR)];
static volatile uint16_t s_prep_handle = 0, s_prep_conn = 0xFFFF;   /* a prepared (long) write under way */

/* v2: pairing, appearance, own address */
static papp_ble_sec_cfg_t s_sec;
static uint16_t           s_appearance;
static bool               s_rand_set;
static uint8_t            s_rand[6];            /* least significant byte first, as the app gave it */

/* GAP requests: one at a time, each waits for its completion event */
static SemaphoreHandle_t s_gap_lock, s_gap_done;
static volatile int      s_gap_expect = -1;
static volatile int      s_gap_status;

/* Client operations: one at a time */
enum { OP_NONE, OP_MTU, OP_SEARCH, OP_READ, OP_WRITE, OP_CONNECT };
static SemaphoreHandle_t s_op_lock, s_op_done;
static struct {
    volatile int kind;
    int      status;
    uint16_t conn_id, handle;
    uint8_t  buf[OP_BUF];
    size_t   len;
    uint16_t mtu;
    esp_bd_addr_t bda;                      /* OP_CONNECT */
    uint16_t open_conn;
    int      nsvc, nchr;
    papp_ble_found_svc_t svc[MAX_FIND_SVC];
    papp_ble_found_chr_t chr[MAX_FIND_CHR];
} *s_op;

/* CCCD -> value handle, from the last discovery (notifications need the value handle registered) */
static struct { uint16_t cccd, val; } s_cccd_map[MAX_FIND_CHR];
static int s_cccd_n;

/* ── Helpers ────────────────────────────────────────────────────────────── */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static int map_err(esp_err_t e)
{
    switch (e) {
    case ESP_OK:                 return PAPP_E_OK;
    case ESP_ERR_NO_MEM:         return PAPP_E_NOMEM;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:   return PAPP_E_ARG;
    case ESP_ERR_NOT_SUPPORTED:  return PAPP_E_UNSUPPORTED;
    case ESP_ERR_INVALID_STATE:  return PAPP_E_LIMIT;
    case ESP_ERR_TIMEOUT:        return PAPP_E_TIMEOUT;
    default:                     return PAPP_E_IO;
    }
}

static int map_gatt(int st)
{
    switch (st) {
    case ESP_GATT_OK:               return PAPP_E_OK;
    case ESP_GATT_INVALID_HANDLE:
    case ESP_GATT_INVALID_ATTR_LEN:
    case ESP_GATT_INVALID_PDU:
    case ESP_GATT_ILLEGAL_PARAMETER: return PAPP_E_ARG;
    case ESP_GATT_NO_RESOURCES:     return PAPP_E_NOMEM;
    case ESP_GATT_BUSY:
    case ESP_GATT_PENDING:          return PAPP_E_LIMIT;
    case ESP_GATT_REQ_NOT_SUPPORTED: return PAPP_E_UNSUPPORTED;
    default:                        return PAPP_E_IO;
    }
}

/* Waits for a semaphore in slices; PAPP_E_OK, PAPP_E_TIMEOUT or PAPP_E_EXIT */
static int wait_sem(SemaphoreHandle_t s, uint32_t timeout_ms)
{
    int64_t end = now_ms() + timeout_ms;
    for (;;) {
        if (xSemaphoreTake(s, pdMS_TO_TICKS(SLICE_MS)) == pdTRUE) return PAPP_E_OK;
        if (lx_svc_exit_flag) return PAPP_E_EXIT;
        if (timeout_ms != PAPP_WAIT_FOREVER && now_ms() >= end) return PAPP_E_TIMEOUT;
    }
}

/* Waits without giving up on exit (start-up and teardown must finish what they began) */
static bool wait_plain(SemaphoreHandle_t s, uint32_t timeout_ms)
{
    return xSemaphoreTake(s, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

static void bda_to_papp(const uint8_t *b, uint8_t type, papp_ble_addr_t *o)
{
    o->type = type > 1 ? type - 2 : type;           /* RPA_PUBLIC/RANDOM: their identity's type */
    for (int i = 0; i < 6; i++) o->a[i] = b[5 - i];
}

static void papp_to_bda(const uint8_t a[6], esp_bd_addr_t b)
{
    for (int i = 0; i < 6; i++) b[i] = a[5 - i];
}

static bool uuid_ok(const papp_ble_uuid_t *u) { return u->len == 2 || u->len == 4 || u->len == 16; }

/* 2 bytes stay 16-bit; 4 bytes become 128-bit on the Bluetooth base (Bluedroid's tables take 16 or 128) */
static uint8_t uuid_bytes(const papp_ble_uuid_t *u, uint8_t out[16])
{
    if (u->len == 2) { out[0] = u->u[0]; out[1] = u->u[1]; return 2; }
    if (u->len == 16) { memcpy(out, u->u, 16); return 16; }
    static const uint8_t base[12] = { 0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, 0x00, 0x10, 0x00, 0x00 };
    memcpy(out, base, 12);
    memcpy(out + 12, u->u, 4);
    return 16;
}

static void uuid_from_esp(const esp_bt_uuid_t *u, papp_ble_uuid_t *o)
{
    memset(o, 0, sizeof(*o));
    if (u->len == ESP_UUID_LEN_16) { o->len = 2; o->u[0] = u->uuid.uuid16 & 0xFF; o->u[1] = u->uuid.uuid16 >> 8; }
    else if (u->len == ESP_UUID_LEN_32) { o->len = 4; for (int i = 0; i < 4; i++) o->u[i] = (uint8_t)(u->uuid.uuid32 >> (8 * i)); }
    else { o->len = 16; memcpy(o->u, u->uuid.uuid128, 16); }
}

static void post(const papp_ble_event_t *e)
{
    if (!s_evq) return;
    if (xQueueSend(s_evq, e, 0) == pdTRUE) return;
    if (e->type == PAPP_BLE_EV_ADV) return;            /* a full queue drops advertisements */
    papp_ble_event_t old;
    xQueueReceive(s_evq, &old, 0);                     /* other events push out the oldest */
    xQueueSend(s_evq, e, 0);
}

static void post_simple(uint8_t type, uint16_t conn, uint16_t handle, uint16_t code)
{
    papp_ble_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = type; e.conn = conn; e.handle = handle; e.code = code;
    post(&e);
}

static conn_t *conn_by_idx(uint16_t idx)
{
    for (int i = 0; i < PAPP_BLE_MAX_CONN; i++)
        if (s_conns[i].used && s_conns[i].idx == idx) return &s_conns[i];
    return NULL;
}

static conn_t *conn_by_bda(const uint8_t *b)
{
    for (int i = 0; i < PAPP_BLE_MAX_CONN; i++)
        if (s_conns[i].used && !memcmp(s_conns[i].bda, b, 6)) return &s_conns[i];
    return NULL;
}

/* The API's conn_id is the link index (BTC adds the interface on the way down) */
static uint16_t sconn(uint16_t idx) { return idx; }
static uint16_t cconn(uint16_t idx) { return idx; }

static int chr_by_handle(uint16_t h)
{
    for (int i = 0; i < s_chr_count; i++)
        if (s_chrs[i].val_handle == h) return i;
    return -1;
}

static int chr_by_cccd(uint16_t h)
{
    for (int i = 0; i < s_chr_count; i++)
        if (s_chrs[i].cccd_handle && s_chrs[i].cccd_handle == h) return i;
    return -1;
}

/* A GAP request and its completion event: PAPP_E_* */
static int gap_call(int evt, esp_err_t (*fn)(void *), void *arg)
{
    xSemaphoreTake(s_gap_lock, portMAX_DELAY);
    xSemaphoreTake(s_gap_done, 0);
    s_gap_expect = evt;
    esp_err_t e = fn(arg);
    int r;
    if (e != ESP_OK) r = map_err(e);
    else if (!wait_plain(s_gap_done, GAP_MS)) r = PAPP_E_TIMEOUT;
    else r = s_gap_status == ESP_BT_STATUS_SUCCESS ? PAPP_E_OK : PAPP_E_IO;
    s_gap_expect = -1;
    xSemaphoreGive(s_gap_lock);
    return r;
}

static void gap_complete(int evt, int status)
{
    if (evt != s_gap_expect) return;
    s_gap_status = status;
    xSemaphoreGive(s_gap_done);
}

/* ── GAP ────────────────────────────────────────────────────────────────── */

static void gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p)
{
    papp_ble_event_t e;
    switch (event) {
    case ESP_GAP_BLE_SEC_REQ_EVT:
    case ESP_GAP_BLE_NC_REQ_EVT:
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (lx_bt_pads_owns(event == ESP_GAP_BLE_AUTH_CMPL_EVT ? p->ble_security.auth_cmpl.bd_addr
                                                               : p->ble_security.ble_req.bd_addr))
            return;                                 /* a Bluetooth gamepad's (lx_bt_pads.c) */
        break;
    default:
        break;
    }
    switch (event) {
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (!s_scanning) break;                     /* another module's scan */
        if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            memset(&e, 0, sizeof(e));
            e.type = PAPP_BLE_EV_ADV;
            e.rssi = (int8_t)p->scan_rst.rssi;
            e.code = p->scan_rst.ble_evt_type;
            e.conn = PAPP_BLE_CONN_NONE;
            bda_to_papp(p->scan_rst.bda, p->scan_rst.ble_addr_type, &e.addr);
            int n = p->scan_rst.adv_data_len + p->scan_rst.scan_rsp_len;
            e.len = n > (int)sizeof(e.data) ? sizeof(e.data) : n;
            memcpy(e.data, p->scan_rst.ble_adv, e.len);
            post(&e);
        } else if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
            s_scanning = false;
            post_simple(PAPP_BLE_EV_SCAN_DONE, PAPP_BLE_CONN_NONE, 0, 0);
        }
        break;
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:  gap_complete(event, p->adv_data_raw_cmpl.status); break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT: gap_complete(event, p->scan_rsp_data_raw_cmpl.status); break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:         gap_complete(event, p->adv_start_cmpl.status); break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:          gap_complete(event, p->adv_stop_cmpl.status); break;
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:    gap_complete(event, p->scan_param_cmpl.status); break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:        gap_complete(event, p->scan_start_cmpl.status); break;
    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:         gap_complete(event, p->scan_stop_cmpl.status); break;
    case ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT:       gap_complete(event, p->set_rand_addr_cmpl.status); break;
    case ESP_GAP_BLE_SEC_REQ_EVT:                    /* a central asks for security: go along */
        esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_NC_REQ_EVT:                     /* numeric comparison: no screen to show it on */
        esp_ble_confirm_reply(p->ble_security.key_notif.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:                /* no keyboard for a passkey: refused */
        esp_ble_passkey_reply(p->ble_security.ble_req.bd_addr, false, 0);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
        const esp_ble_auth_cmpl_t *a = &p->ble_security.auth_cmpl;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        conn_t *c = conn_by_bda(a->bd_addr);
        uint16_t idx = c ? c->idx : PAPP_BLE_CONN_NONE;
        bool central = c && c->central;
        xSemaphoreGive(s_lock);
        if (!a->success) ESP_LOGW(TAG, "pairing failed: reason 0x%x", a->fail_reason);
        /* Bluedroid keeps a bond after a failed pairing (CONFIG_BT_BLE_SMP_REMOVE_BOND_ON_PAIR_FAIL_AS_CENTRAL
         * off, for the gamepads): an app's outgoing link keeps the old behaviour, the bond goes */
        if (!a->success && central) esp_ble_remove_bond_device((uint8_t *)a->bd_addr);
        post_simple(PAPP_BLE_EV_ENCRYPTED, idx, 0, a->success ? 0 : (uint16_t)(a->fail_reason ? a->fail_reason : 1));
        break;
    }
    default:
        break;
    }
}

/* ── GATT server events ─────────────────────────────────────────────────── */

static void post_write(uint16_t conn, uint16_t handle, const uint8_t *v, uint16_t len)
{
    papp_ble_event_t e;
    memset(&e, 0, sizeof(e));
    e.type = PAPP_BLE_EV_WRITE;
    e.conn = conn;
    e.handle = handle;
    e.len = len > sizeof(e.data) ? sizeof(e.data) : len;
    if (v) memcpy(e.data, v, e.len);
    post(&e);
}

static void gatts_cb(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *p)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (p->reg.status == ESP_GATT_OK) s_gatts_if = gatts_if;
        xSemaphoreGive(s_reg_sem);
        break;
    case ESP_GATTS_CREAT_ATTR_TAB_EVT: {
        s_tab_status = p->add_attr_tab.status;
        uint16_t n = p->add_attr_tab.num_handle;
        if (n > sizeof(s_tab_handles) / sizeof(s_tab_handles[0])) n = sizeof(s_tab_handles) / sizeof(s_tab_handles[0]);
        if (p->add_attr_tab.handles) memcpy(s_tab_handles, p->add_attr_tab.handles, n * sizeof(uint16_t));
        s_tab_n = n;
        xSemaphoreGive(s_tab_sem);
        break;
    }
    case ESP_GATTS_CONNECT_EVT: {
        if (lx_bt_pads_owns(p->connect.remote_bda)) break;     /* a gamepad link (lx_bt_pads.c) */
        uint16_t idx = p->connect.conn_id;
        papp_ble_event_t e;
        memset(&e, 0, sizeof(e));
        e.type = PAPP_BLE_EV_CONNECTED;
        e.conn = idx;
        bda_to_papp(p->connect.remote_bda, p->connect.ble_addr_type, &e.addr);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        conn_t *c = conn_by_idx(idx);
        for (int i = 0; i < PAPP_BLE_MAX_CONN && !c; i++)
            if (!s_conns[i].used) c = &s_conns[i];
        if (c) {
            memset(c, 0, sizeof(*c));
            c->used = true;
            c->idx = (uint8_t)idx;
            memcpy(c->bda, p->connect.remote_bda, 6);
            c->addr = e.addr;
            c->mtu = 23;
        }
        xSemaphoreGive(s_lock);
        if (!c) {
            esp_ble_gap_disconnect(p->connect.remote_bda);
            break;
        }
        post(&e);
        if (p->connect.link_role == 1 && s_adv_active) {   /* we were advertising: a central connected */
            s_adv_active = false;
            post_simple(PAPP_BLE_EV_ADV_DONE, PAPP_BLE_CONN_NONE, 0, 0);
        }
        if (s_sec.initiate) {                               /* v2: start pairing (or encryption, if bonded) */
            esp_err_t r = esp_ble_set_encryption(p->connect.remote_bda,
                                                 s_sec.mitm ? ESP_BLE_SEC_ENCRYPT_MITM : ESP_BLE_SEC_ENCRYPT_NO_MITM);
            if (r) ESP_LOGW(TAG, "set_encryption: %s", esp_err_to_name(r));
        }
        break;
    }
    case ESP_GATTS_DISCONNECT_EVT: {
        if (lx_bt_pads_owns(p->disconnect.remote_bda)) break;
        uint16_t idx = p->disconnect.conn_id;
        papp_ble_event_t e;
        memset(&e, 0, sizeof(e));
        e.type = PAPP_BLE_EV_DISCONNECTED;
        e.conn = idx;
        e.code = (uint16_t)p->disconnect.reason;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        conn_t *c = conn_by_idx(idx);
        if (c) { e.addr = c->addr; c->used = false; }
        else bda_to_papp(p->disconnect.remote_bda, 0, &e.addr);
        xSemaphoreGive(s_lock);
        if (s_prep_conn == idx) { s_prep_handle = 0; s_prep_conn = 0xFFFF; }
        post(&e);
        break;
    }
    case ESP_GATTS_WRITE_EVT: {
        uint16_t idx = p->write.conn_id;
        int i = chr_by_cccd(p->write.handle);
        if (i >= 0) {                                       /* a peer (un)subscribes */
            uint16_t v = p->write.len >= 1 ? p->write.value[0] : 0;
            uint16_t mode = (v & 2) ? 2 : (v & 1) ? 1 : 0;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            conn_t *c = conn_by_idx(idx);
            if (c) c->sub[i] = (uint8_t)mode;
            xSemaphoreGive(s_lock);
            post_simple(PAPP_BLE_EV_SUBSCRIBE, idx, s_chrs[i].val_handle, mode);
            break;
        }
        if (chr_by_handle(p->write.handle) < 0) break;     /* a descriptor or the stack's own */
        if (p->write.is_prep) {                             /* the value arrives in pieces: posted at execute */
            s_prep_handle = p->write.handle;
            s_prep_conn = idx;
            break;
        }
        post_write(idx, p->write.handle, p->write.value, p->write.len);
        break;
    }
    case ESP_GATTS_EXEC_WRITE_EVT:
        if (p->exec_write.exec_write_flag == ESP_GATT_PREP_WRITE_EXEC && s_prep_handle &&
            s_prep_conn == (p->exec_write.conn_id)) {
            uint16_t len = 0;
            const uint8_t *v = NULL;
            if (esp_ble_gatts_get_attr_value(s_prep_handle, &len, &v) == ESP_GATT_OK)
                post_write(s_prep_conn, s_prep_handle, v, len);
        }
        s_prep_handle = 0;
        s_prep_conn = 0xFFFF;
        break;
    case ESP_GATTS_MTU_EVT: {
        uint16_t idx = p->mtu.conn_id;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        conn_t *c = conn_by_idx(idx);
        if (c) c->mtu = p->mtu.mtu;
        xSemaphoreGive(s_lock);
        post_simple(PAPP_BLE_EV_MTU, idx, 0, p->mtu.mtu);
        break;
    }
    default:
        break;
    }
}

/* ── GATT client events ─────────────────────────────────────────────────── */

static bool op_is(int kind, uint16_t conn_id) { return s_op && s_op->kind == kind && s_op->conn_id == conn_id; }

static void op_finish(int status)
{
    s_op->status = status;
    s_op->kind = OP_NONE;
    xSemaphoreGive(s_op_done);
}

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *p)
{
    /* lx_bt.c hands every client event to every module: ours are those of our registration */
    if (event == ESP_GATTC_REG_EVT ? p->reg.app_id != APP_ID_C : gattc_if != s_gattc_if) return;
    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (p->reg.status == ESP_GATT_OK) s_gattc_if = gattc_if;
        xSemaphoreGive(s_reg_sem);
        break;
    case ESP_GATTC_OPEN_EVT:
        if (p->open.status == ESP_GATT_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            conn_t *oc = conn_by_bda(p->open.remote_bda);
            if (oc) oc->central = true;
            xSemaphoreGive(s_lock);
        }
        if (s_op && s_op->kind == OP_CONNECT && !memcmp(s_op->bda, p->open.remote_bda, 6)) {
            s_op->open_conn = p->open.conn_id;
            op_finish(p->open.status == ESP_GATT_OK ? PAPP_E_OK : PAPP_E_REFUSED);
        }
        if (p->open.status != ESP_GATT_OK) {
            papp_ble_event_t e;
            memset(&e, 0, sizeof(e));
            e.type = PAPP_BLE_EV_CONNECTED;
            e.conn = PAPP_BLE_CONN_NONE;
            e.code = (uint16_t)p->open.status;
            bda_to_papp(p->open.remote_bda, 0, &e.addr);
            post(&e);
        }
        break;
    case ESP_GATTC_CFG_MTU_EVT:
        if (op_is(OP_MTU, p->cfg_mtu.conn_id)) {
            s_op->mtu = p->cfg_mtu.mtu;
            op_finish(map_gatt(p->cfg_mtu.status));
        }
        if (p->cfg_mtu.status == ESP_GATT_OK) {
            uint16_t idx = p->cfg_mtu.conn_id;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            conn_t *c = conn_by_idx(idx);
            if (c) c->mtu = p->cfg_mtu.mtu;
            xSemaphoreGive(s_lock);
            post_simple(PAPP_BLE_EV_MTU, idx, 0, p->cfg_mtu.mtu);
        }
        break;
    case ESP_GATTC_SEARCH_RES_EVT:
        if (op_is(OP_SEARCH, p->search_res.conn_id) && s_op->nsvc < MAX_FIND_SVC) {
            papp_ble_found_svc_t *f = &s_op->svc[s_op->nsvc++];
            uuid_from_esp(&p->search_res.srvc_id.uuid, &f->uuid);
            f->start = p->search_res.start_handle;
            f->end = p->search_res.end_handle;
        }
        break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
        if (op_is(OP_SEARCH, p->search_cmpl.conn_id)) op_finish(map_gatt(p->search_cmpl.status));
        break;
    case ESP_GATTC_READ_CHAR_EVT:
    case ESP_GATTC_READ_DESCR_EVT:
        if (op_is(OP_READ, p->read.conn_id) && s_op->handle == p->read.handle) {
            size_t n = p->read.value_len > OP_BUF ? OP_BUF : p->read.value_len;
            if (p->read.status == ESP_GATT_OK && n) memcpy(s_op->buf, p->read.value, n);
            s_op->len = p->read.status == ESP_GATT_OK ? n : 0;
            op_finish(map_gatt(p->read.status));
        }
        break;
    case ESP_GATTC_WRITE_CHAR_EVT:
    case ESP_GATTC_WRITE_DESCR_EVT:
        if (op_is(OP_WRITE, p->write.conn_id) && s_op->handle == p->write.handle) op_finish(map_gatt(p->write.status));
        break;
    case ESP_GATTC_NOTIFY_EVT: {
        papp_ble_event_t e;
        memset(&e, 0, sizeof(e));
        e.type = PAPP_BLE_EV_NOTIFY;
        e.conn = p->notify.conn_id;
        e.handle = p->notify.handle;
        e.code = p->notify.is_notify ? 0 : 1;
        e.len = p->notify.value_len > sizeof(e.data) ? sizeof(e.data) : p->notify.value_len;
        memcpy(e.data, p->notify.value, e.len);
        post(&e);
        break;
    }
    default:
        break;
    }
}

/* ── Stack lifecycle ────────────────────────────────────────────────────── */

static esp_err_t set_sec_params(void)
{
    esp_ble_auth_req_t auth = (s_sec.bond ? ESP_LE_AUTH_BOND : 0) | (s_sec.mitm ? ESP_LE_AUTH_REQ_MITM : 0) |
                              (s_sec.sc ? ESP_LE_AUTH_REQ_SC_ONLY : 0);
    esp_ble_io_cap_t io = s_sec.io_cap;             /* PAPP_BLE_IO_* = ESP_IO_CAP_* values */
    uint8_t keys = s_sec.bond ? (ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK) : 0;
    uint8_t ksize = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &ksize, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, 1);
    return esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, 1);
}

static esp_err_t do_set_rand(void *arg) { esp_bd_addr_t b; papp_to_bda(s_rand, b); (void)arg; return esp_ble_gap_set_rand_addr(b); }
static esp_err_t do_adv_stop(void *arg) { (void)arg; return esp_ble_gap_stop_advertising(); }
static esp_err_t do_scan_stop(void *arg) { (void)arg; return esp_ble_gap_stop_scanning(); }

static void stop(void)
{
    if (!s_running) return;
    if (s_adv_active) { gap_call(ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT, do_adv_stop, NULL); s_adv_active = false; }
    if (s_scanning) { gap_call(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, do_scan_stop, NULL); s_scanning = false; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PAPP_BLE_MAX_CONN; i++)
        if (s_conns[i].used) esp_ble_gap_disconnect(s_conns[i].bda);
    xSemaphoreGive(s_lock);
    for (int t = 0; t < 20; t++) {                     /* 1 s for the peers to go */
        bool any = false;
        for (int i = 0; i < PAPP_BLE_MAX_CONN; i++) any |= s_conns[i].used;
        if (!any) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    for (int i = 0; i < s_nsvc; i++) esp_ble_gatts_delete_service(s_svc_h[i]);   /* if the stack stays up */
    s_nsvc = 0;
    s_running = false;
    lx_bt_release(LX_BT_USER_APP);                      /* stops Bluedroid unless the speaker holds it */
    memset(s_conns, 0, sizeof(s_conns));
}

/* lx_bt.c, before Bluedroid stops (last user gone, or the dongle pulled out): nothing of ours survives */
void lx_bt_ble_stack_down(void)
{
    s_running = false;
    s_gatts_if = s_gattc_if = ESP_GATT_IF_NONE;
    s_adv_active = s_scanning = false;
    s_nsvc = 0;
    memset(s_conns, 0, sizeof(s_conns));
}

/* Takes the stack (a dongle, else the C6) and registers our GATT server and client on it once per
 * stack start; false: no BLE */
static bool start(void)
{
    if (s_running) return true;
    if (s_failed) return false;
    if (!lx_bt_acquire(LX_BT_USER_APP, false, &lx_svc_exit_flag)) {
        if (!lx_svc_exit_flag) s_failed = true;
        return false;
    }
    s_running = true;
    lx_bt_ble_handlers(LX_BT_BLE_APP, gap_cb, gattc_cb);   /* GAP and GATT client: shared, lx_bt.c */
    if (s_gatts_if == ESP_GATT_IF_NONE || s_gattc_if == ESP_GATT_IF_NONE) {
        esp_ble_gatts_register_callback(gatts_cb);
        esp_ble_gatt_set_local_mtu(LOCAL_MTU);
        xSemaphoreTake(s_reg_sem, 0);
        xSemaphoreTake(s_reg_sem, 0);
        esp_ble_gatts_app_register(APP_ID_S);
        esp_ble_gattc_app_register(APP_ID_C);
        bool reg = wait_plain(s_reg_sem, REG_MS) && wait_plain(s_reg_sem, REG_MS) &&
                   s_gatts_if != ESP_GATT_IF_NONE && s_gattc_if != ESP_GATT_IF_NONE;
        if (!reg) {
            ESP_LOGE(TAG, "GATT registration failed");
            stop();
            s_failed = true;
            return false;
        }
    }
    esp_ble_gap_set_device_name(s_name);
    if (s_appearance) esp_ble_gap_config_local_icon(s_appearance);
    set_sec_params();
    s_own_type = BLE_ADDR_TYPE_PUBLIC;
    if (s_rand_set && gap_call(ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT, do_set_rand, NULL) == PAPP_E_OK)
        s_own_type = BLE_ADDR_TYPE_RANDOM;
    const uint8_t *a = esp_bt_dev_get_address();
    if (a) ESP_LOGI(TAG, "up (Bluedroid on %s), address %02x:%02x:%02x:%02x:%02x:%02x",
                    lx_bt_classic() ? lx_bt_dongle_name() : "the C6", a[0], a[1], a[2], a[3], a[4], a[5]);
    return true;
}

/* Bluedroid must not start on a task whose stack is in PSRAM (every app thread's), and its bond store
 * (NVS) is kept off those stacks too: those run on a short-lived task with its stack in internal RAM. */
typedef struct {
    int (*fn)(void *);
    void *arg;
    int ret;
    SemaphoreHandle_t done;
} job_t;

static void job_task(void *a)
{
    job_t *j = a;
    j->ret = j->fn(j->arg);
    xSemaphoreGive(j->done);
    vTaskDelete(NULL);
}

static int on_internal_stack(int (*fn)(void *), void *arg)
{
    if (esp_ptr_in_dram((const void *)esp_cpu_get_sp())) return fn(arg);
    job_t j = { .fn = fn, .arg = arg, .ret = 0, .done = xSemaphoreCreateBinary() };
    if (!j.done) return fn(arg);
    if (xTaskCreatePinnedToCore(job_task, "lx_ble_job", 8192, &j, 5, NULL, tskNO_AFFINITY) != pdPASS) {
        vSemaphoreDelete(j.done);
        return -1;
    }
    xSemaphoreTake(j.done, portMAX_DELAY);
    vSemaphoreDelete(j.done);
    return j.ret;
}

static int start_job(void *arg) { (void)arg; return start() ? 1 : 0; }

static bool ensure(void)
{
    if (s_running) return true;
    xSemaphoreTake(s_life, portMAX_DELAY);
    bool ok = on_internal_stack(start_job, NULL) == 1;
    xSemaphoreGive(s_life);
    return ok;
}

/* ── GATT server ────────────────────────────────────────────────────────── */

static const uint16_t k_primary = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t k_chr_decl = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t k_cccd = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static const uint8_t  k_zero[2] = { 0, 0 };

static uint8_t chr_props(uint16_t f)
{
    return (f & (PAPP_BLE_READ | PAPP_BLE_READ_ENC) ? ESP_GATT_CHAR_PROP_BIT_READ : 0) |
           (f & (PAPP_BLE_WRITE | PAPP_BLE_WRITE_ENC) ? ESP_GATT_CHAR_PROP_BIT_WRITE : 0) |
           (f & PAPP_BLE_WRITE_NR ? ESP_GATT_CHAR_PROP_BIT_WRITE_NR : 0) |
           (f & PAPP_BLE_NOTIFY ? ESP_GATT_CHAR_PROP_BIT_NOTIFY : 0) |
           (f & PAPP_BLE_INDICATE ? ESP_GATT_CHAR_PROP_BIT_INDICATE : 0);
}

static uint16_t chr_perm(uint16_t f)
{
    uint16_t p = 0;
    if (f & PAPP_BLE_READ_ENC) p |= ESP_GATT_PERM_READ_ENCRYPTED;
    else if (f & PAPP_BLE_READ) p |= ESP_GATT_PERM_READ;
    if (f & PAPP_BLE_WRITE_ENC) p |= ESP_GATT_PERM_WRITE_ENCRYPTED;
    else if (f & (PAPP_BLE_WRITE | PAPP_BLE_WRITE_NR)) p |= ESP_GATT_PERM_WRITE;
    return p;
}

/* One service into Bluedroid: the attribute table, then started. Fills s_chrs[first..] handles. */
static int add_service(const papp_ble_svc2_t *s, int svc_index, int first)
{
    int n = 1;
    for (int k = 0; k < s->chr_count; k++) {
        const papp_ble_chr2_t *c = &s->chrs[k];
        n += 2 + ((c->flags & (PAPP_BLE_NOTIFY | PAPP_BLE_INDICATE)) ? 1 : 0) + c->dsc_count;
    }
    esp_gatts_attr_db_t *db = heap_caps_calloc(n, sizeof(*db), MALLOC_CAP_SPIRAM);
    uint8_t (*uu)[16] = heap_caps_calloc(n, 16, MALLOC_CAP_SPIRAM);           /* each attribute's UUID */
    uint8_t *props = heap_caps_calloc(s->chr_count ? s->chr_count : 1, 1, MALLOC_CAP_SPIRAM);
    uint8_t *zeros = heap_caps_calloc(1, 512, MALLOC_CAP_SPIRAM);            /* initial (empty) values */
    int vidx[PAPP_BLE_MAX_CHRS], cidx[PAPP_BLE_MAX_CHRS];
    int r = PAPP_E_OK;
    if (!db || !uu || !props || !zeros) { r = PAPP_E_NOMEM; goto out; }

    int a = 0;
    uint8_t len = uuid_bytes(&s->uuid, uu[a]);
    db[a].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
    db[a].att_desc = (esp_attr_desc_t){ ESP_UUID_LEN_16, (uint8_t *)&k_primary, ESP_GATT_PERM_READ, len, len, uu[a] };
    a++;
    for (int k = 0; k < s->chr_count; k++) {
        const papp_ble_chr2_t *c = &s->chrs[k];
        props[k] = chr_props(c->flags);
        db[a].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        db[a].att_desc = (esp_attr_desc_t){ ESP_UUID_LEN_16, (uint8_t *)&k_chr_decl, ESP_GATT_PERM_READ, 1, 1, &props[k] };
        a++;
        len = uuid_bytes(&c->uuid, uu[a]);
        db[a].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
        db[a].att_desc = (esp_attr_desc_t){ len, uu[a], chr_perm(c->flags), c->max_len, 0, zeros };
        vidx[k] = a++;
        cidx[k] = -1;
        if (c->flags & (PAPP_BLE_NOTIFY | PAPP_BLE_INDICATE)) {
            db[a].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
            db[a].att_desc = (esp_attr_desc_t){ ESP_UUID_LEN_16, (uint8_t *)&k_cccd,
                                                ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, 2, 2, (uint8_t *)k_zero };
            cidx[k] = a++;
        }
        for (int j = 0; j < c->dsc_count; j++) {
            const papp_ble_dsc_t *d = &c->dscs[j];
            len = uuid_bytes(&d->uuid, uu[a]);
            db[a].attr_control.auto_rsp = ESP_GATT_AUTO_RSP;
            db[a].att_desc = (esp_attr_desc_t){ len, uu[a],
                (uint16_t)(d->flags & PAPP_BLE_READ_ENC ? ESP_GATT_PERM_READ_ENCRYPTED : ESP_GATT_PERM_READ),
                sizeof(d->value), d->len, (uint8_t *)d->value };
            a++;
        }
    }
    xSemaphoreTake(s_tab_sem, 0);
    esp_err_t e = esp_ble_gatts_create_attr_tab(db, s_gatts_if, (uint8_t)n, (uint8_t)svc_index);
    if (e != ESP_OK) { r = map_err(e); goto out; }
    if (!wait_plain(s_tab_sem, REG_MS)) { r = PAPP_E_TIMEOUT; goto out; }
    if (s_tab_status != ESP_GATT_OK || s_tab_n != n) {
        ESP_LOGE(TAG, "GATT table: status 0x%x, %u of %d handles", s_tab_status, s_tab_n, n);
        r = PAPP_E_LIMIT;
        goto out;
    }
    for (int k = 0; k < s->chr_count; k++) {
        chr_t *m = &s_chrs[first + k];
        m->flags = s->chrs[k].flags;
        m->max_len = s->chrs[k].max_len;
        m->val_handle = s_tab_handles[vidx[k]];
        m->cccd_handle = cidx[k] >= 0 ? s_tab_handles[cidx[k]] : 0;
    }
    e = esp_ble_gatts_start_service(s_tab_handles[0]);
    if (e != ESP_OK) r = map_err(e);
    if (s_nsvc < PAPP_BLE_MAX_SVCS) s_svc_h[s_nsvc++] = s_tab_handles[0];
out:
    heap_caps_free(db);
    heap_caps_free(uu);
    heap_caps_free(props);
    heap_caps_free(zeros);
    return r;
}

typedef struct { const papp_ble_svc2_t *svcs; int count; } reg_arg_t;

static int register_job(void *arg)
{
    const reg_arg_t *ra = arg;
    int first = 0;
    for (int s = 0; s < ra->count; s++) {
        int r = add_service(&ra->svcs[s], s, first);
        if (r) return r;
        first += ra->svcs[s].chr_count;
        s_chr_count = first;
    }
    return PAPP_E_OK;
}

static int register_table(const papp_ble_svc2_t *svcs, int count)
{
    if (!svcs || count <= 0 || count > PAPP_BLE_MAX_SVCS) return PAPP_E_ARG;
    int nchr = 0;
    for (int s = 0; s < count; s++) {
        if (!uuid_ok(&svcs[s].uuid) || !svcs[s].chrs || !svcs[s].chr_count) return PAPP_E_ARG;
        for (int k = 0; k < svcs[s].chr_count; k++) {
            const papp_ble_chr2_t *c = &svcs[s].chrs[k];
            if (!uuid_ok(&c->uuid) || !c->flags || !c->max_len || c->max_len > 512 ||
                c->dsc_count > MAX_DSC_PER_CHR || (c->dsc_count && !c->dscs)) return PAPP_E_ARG;
            for (int j = 0; j < c->dsc_count; j++)
                if (!uuid_ok(&c->dscs[j].uuid) || c->dscs[j].len > sizeof(c->dscs[j].value)) return PAPP_E_ARG;
        }
        nchr += svcs[s].chr_count;
    }
    if (nchr > PAPP_BLE_MAX_CHRS) return PAPP_E_LIMIT;

    SVC_ENTER();
    int r = PAPP_E_UNSUPPORTED;
    if (!ensure()) goto out_nolock;
    xSemaphoreTake(s_life, portMAX_DELAY);
    if (s_chr_count) { r = PAPP_E_LIMIT; goto out; }               /* once per run */
    reg_arg_t ra = { svcs, count };
    r = on_internal_stack(register_job, &ra);
    if (r == PAPP_E_OK) {
        int ci = 0;
        for (int s = 0; s < count; s++)
            for (int k = 0; k < svcs[s].chr_count; k++, ci++)
                if (svcs[s].chrs[k].handle) *svcs[s].chrs[k].handle = s_chrs[ci].val_handle;
    }
out:
    xSemaphoreGive(s_life);
out_nolock:
    SVC_LEAVE();
    return r;
}

/* v1: the same table without descriptors */
static int ble_gatt_register(const papp_ble_svc_t *svcs, int count)
{
    if (!svcs || count <= 0 || count > PAPP_BLE_MAX_SVCS) return PAPP_E_ARG;
    papp_ble_svc2_t s2[PAPP_BLE_MAX_SVCS];
    static papp_ble_chr2_t c2[PAPP_BLE_MAX_CHRS];
    int n = 0;
    for (int s = 0; s < count; s++) {
        if (!svcs[s].chrs || n + svcs[s].chr_count > PAPP_BLE_MAX_CHRS) return svcs[s].chrs ? PAPP_E_LIMIT : PAPP_E_ARG;
        s2[s].uuid = svcs[s].uuid;
        s2[s].chrs = &c2[n];
        s2[s].chr_count = svcs[s].chr_count;
        for (int k = 0; k < svcs[s].chr_count; k++, n++) {
            const papp_ble_chr_t *c = &svcs[s].chrs[k];
            c2[n] = (papp_ble_chr2_t){ .uuid = c->uuid, .flags = (uint16_t)(c->flags & 31), .max_len = c->max_len,
                                       .handle = c->handle };
        }
    }
    return register_table(s2, count);
}

static int ble_gatt_register2(const papp_ble_svc2_t *svcs, int count) { return register_table(svcs, count); }

static int ble_set_value(uint16_t handle, const void *buf, size_t len)
{
    SVC_ENTER();
    int r = PAPP_E_OK;
    int i = chr_by_handle(handle);
    if (i < 0 || len > s_chrs[i].max_len || (len && !buf)) r = PAPP_E_ARG;
    else if (s_running) r = map_err(esp_ble_gatts_set_attr_value(handle, (uint16_t)len, len ? buf : k_zero));
    SVC_LEAVE();
    return r;
}

static int ble_notify(papp_ble_conn_t c, uint16_t handle, const void *buf, size_t len)
{
    int r = ble_set_value(handle, buf, len);
    if (r) return r;
    SVC_ENTER();
    int i = chr_by_handle(handle);
    uint16_t idx[PAPP_BLE_MAX_CONN], mtu[PAPP_BLE_MAX_CONN];
    uint8_t modes[PAPP_BLE_MAX_CONN];
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < PAPP_BLE_MAX_CONN; k++) {
        conn_t *cn = &s_conns[k];
        if (!cn->used || !cn->sub[i]) continue;
        if (c != PAPP_BLE_CONN_ALL && cn->idx != c) continue;
        idx[n] = cn->idx;
        mtu[n] = cn->mtu;
        modes[n++] = cn->sub[i];
    }
    xSemaphoreGive(s_lock);
    if (!n) r = c == PAPP_BLE_CONN_ALL ? PAPP_E_OK : PAPP_E_NOT_CONNECTED;  /* nobody listening */
    for (int k = 0; k < n; k++) {
        size_t m = len > (size_t)(mtu[k] - 3) ? (size_t)(mtu[k] - 3) : len;  /* as NimBLE: cut to the MTU */
        esp_err_t e = esp_ble_gatts_send_indicate(s_gatts_if, sconn(idx[k]), handle, (uint16_t)m,
                                                  (uint8_t *)(len ? buf : k_zero), modes[k] == 2);
        if (e && !r) r = map_err(e);
    }
    SVC_LEAVE();
    return r;
}

/* ── Advertising ────────────────────────────────────────────────────────── */

typedef struct { const uint8_t *d; uint8_t len; } raw_t;
static esp_err_t do_adv_data(void *arg) { raw_t *x = arg; return esp_ble_gap_config_adv_data_raw((uint8_t *)x->d, x->len); }
static esp_err_t do_rsp_data(void *arg) { raw_t *x = arg; return esp_ble_gap_config_scan_rsp_data_raw((uint8_t *)x->d, x->len); }
static esp_err_t do_adv_start(void *arg) { return esp_ble_gap_start_advertising((esp_ble_adv_params_t *)arg); }

static int ble_adv_start(const papp_ble_adv_cfg_t *cfg)
{
    if (!cfg || cfg->adv_len > 31 || cfg->rsp_len > 31 || (cfg->adv_len && !cfg->adv) ||
        (cfg->rsp_len && !cfg->rsp)) return PAPP_E_ARG;
    SVC_ENTER();
    int r = PAPP_E_UNSUPPORTED;
    if (ensure()) {
        if (s_adv_active) { gap_call(ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT, do_adv_stop, NULL); s_adv_active = false; }
        raw_t adv = { cfg->adv_len ? cfg->adv : k_zero, cfg->adv_len };
        raw_t rsp = { cfg->rsp_len ? cfg->rsp : k_zero, cfg->rsp_len };
        r = PAPP_E_OK;                                                 /* Bluedroid refuses empty data */
        if (adv.len) r = gap_call(ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT, do_adv_data, &adv);
        if (!r && rsp.len) r = gap_call(ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT, do_rsp_data, &rsp);
        uint32_t ms = cfg->interval_ms ? cfg->interval_ms : 100;
        uint32_t itvl = ms * 8 / 5;                                   /* 0.625 ms units */
        if (itvl < 0x20) itvl = 0x20;
        if (itvl > 0x4000) itvl = 0x4000;
        esp_ble_adv_params_t p = {
            .adv_int_min = (uint16_t)itvl, .adv_int_max = (uint16_t)itvl,
            .adv_type = cfg->connectable ? ADV_TYPE_IND : (cfg->rsp_len ? ADV_TYPE_SCAN_IND : ADV_TYPE_NONCONN_IND),
            .own_addr_type = s_own_type,
            .channel_map = ADV_CHNL_ALL,
            .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
        };
        if (!r) r = gap_call(ESP_GAP_BLE_ADV_START_COMPLETE_EVT, do_adv_start, &p);
        if (!r) s_adv_active = true;
        else ESP_LOGW(TAG, "adv_start %d", r);
    }
    SVC_LEAVE();
    return r;
}

static void ble_adv_stop(void)
{
    SVC_ENTER();
    if (s_running && s_adv_active) {
        gap_call(ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT, do_adv_stop, NULL);
        s_adv_active = false;
    }
    SVC_LEAVE();
}

static int ble_set_name(const char *name)
{
    if (!name || !name[0]) return PAPP_E_ARG;
    SVC_ENTER();
    snprintf(s_name, sizeof(s_name), "%s", name);
    if (s_running) esp_ble_gap_set_device_name(s_name);
    SVC_LEAVE();
    return PAPP_E_OK;
}

/* ── Basics ─────────────────────────────────────────────────────────────── */

static bool ble_available(void)
{
    SVC_ENTER();
    bool ok = ensure();
    SVC_LEAVE();
    return ok;
}

static int ble_own_addr(papp_ble_addr_t *out)
{
    if (!out) return PAPP_E_ARG;
    SVC_ENTER();
    int r = PAPP_E_UNSUPPORTED;
    if (ensure()) {
        if (s_own_type == BLE_ADDR_TYPE_RANDOM) {
            out->type = 1;
            memcpy(out->a, s_rand, 6);
            r = PAPP_E_OK;
        } else {
            const uint8_t *a = esp_bt_dev_get_address();
            if (a) { bda_to_papp(a, 0, out); r = PAPP_E_OK; }
            else r = PAPP_E_IO;
        }
    }
    SVC_LEAVE();
    return r;
}

static bool ble_poll_event(papp_ble_event_t *out)
{
    if (!out || !s_evq) return false;
    return xQueueReceive(s_evq, out, 0) == pdTRUE;
}

static bool ble_wait_event(papp_ble_event_t *out, uint32_t timeout_ms)
{
    if (!out || !s_evq) return false;
    SVC_ENTER();
    bool got = false;
    int64_t end = now_ms() + timeout_ms;
    for (;;) {
        if (xQueueReceive(s_evq, out, pdMS_TO_TICKS(timeout_ms < SLICE_MS ? timeout_ms : SLICE_MS)) == pdTRUE) {
            got = true;
            break;
        }
        if (lx_svc_exit_flag) break;
        if (timeout_ms != PAPP_WAIT_FOREVER && now_ms() >= end) break;
    }
    SVC_LEAVE();
    return got;
}

/* ── Central ────────────────────────────────────────────────────────────── */

static esp_err_t do_scan_params(void *arg) { return esp_ble_gap_set_scan_params((esp_ble_scan_params_t *)arg); }
static esp_err_t do_scan_start(void *arg) { return esp_ble_gap_start_scanning(*(uint32_t *)arg); }

static int ble_scan_start(const papp_ble_scan_cfg_t *cfg)
{
    SVC_ENTER();
    int r = PAPP_E_UNSUPPORTED;
    if (ensure()) {
        if (s_scanning) { gap_call(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, do_scan_stop, NULL); s_scanning = false; }
        uint32_t itvl = cfg && cfg->interval_ms ? cfg->interval_ms * 8 / 5 : 0x50;
        uint32_t win = cfg && cfg->window_ms ? cfg->window_ms * 8 / 5 : 0x30;
        if (itvl < 4) itvl = 4;
        if (itvl > 0x4000) itvl = 0x4000;
        if (win < 4) win = 4;
        if (win > itvl) win = itvl;
        esp_ble_scan_params_t p = {
            .scan_type = cfg && cfg->active ? BLE_SCAN_TYPE_ACTIVE : BLE_SCAN_TYPE_PASSIVE,
            .own_addr_type = s_own_type,
            .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
            .scan_interval = (uint16_t)itvl,
            .scan_window = (uint16_t)win,
            .scan_duplicate = cfg && cfg->filter_duplicates ? BLE_SCAN_DUPLICATE_ENABLE : BLE_SCAN_DUPLICATE_DISABLE,
        };
        /* Bluedroid counts whole seconds (0 = until stopped) */
        uint32_t secs = cfg && cfg->duration_ms ? (cfg->duration_ms + 999) / 1000 : 0;
        r = gap_call(ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT, do_scan_params, &p);
        if (!r) r = gap_call(ESP_GAP_BLE_SCAN_START_COMPLETE_EVT, do_scan_start, &secs);
        if (!r) s_scanning = true;
        else ESP_LOGW(TAG, "scan_start %d", r);
    }
    SVC_LEAVE();
    return r;
}

static void ble_scan_stop(void)
{
    SVC_ENTER();
    if (s_running && s_scanning) {
        gap_call(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, do_scan_stop, NULL);
        s_scanning = false;
    }
    SVC_LEAVE();
}

/* Client operations: begin under s_op_lock, the callback checks the kind, connection and handle,
 * end waits and clears the kind (a late completion is then ignored). */
static void op_begin(int kind, uint16_t conn_id, uint16_t handle)
{
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    xSemaphoreTake(s_op_done, 0);
    s_op->status = 0;
    s_op->len = 0;
    s_op->conn_id = conn_id;
    s_op->handle = handle;
    s_op->kind = kind;
}

static int op_end(esp_err_t e, uint32_t timeout_ms)
{
    int r;
    if (e != ESP_OK) r = map_err(e);
    else {
        r = wait_sem(s_op_done, timeout_ms);
        if (r == PAPP_E_OK) r = s_op->status;
    }
    s_op->kind = OP_NONE;
    xSemaphoreGive(s_op_lock);
    return r;
}

static int ble_connect(const papp_ble_addr_t *peer, uint32_t timeout_ms, papp_ble_conn_t *out)
{
    if (!peer) return PAPP_E_ARG;
    SVC_ENTER();
    int r = PAPP_E_UNSUPPORTED;
    if (out) *out = PAPP_BLE_CONN_NONE;
    if (ensure()) {
        if (s_scanning) { gap_call(ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT, do_scan_stop, NULL); s_scanning = false; }
        op_begin(OP_CONNECT, 0xFFFF, 0);
        papp_to_bda(peer->a, s_op->bda);
        esp_ble_gatt_creat_conn_params_t cp = {
            .is_direct = true, .is_aux = false, .own_addr_type = s_own_type,
            .phy_mask = 0, .remote_addr_type = (esp_ble_addr_type_t)peer->type,
        };
        memcpy(cp.remote_bda, s_op->bda, 6);
        esp_err_t e = esp_ble_gattc_enh_open(s_gattc_if, &cp);
        if (e != ESP_OK) {
            r = map_err(e);
            s_op->kind = OP_NONE;
            xSemaphoreGive(s_op_lock);
        } else {
            r = wait_sem(s_op_done, timeout_ms == PAPP_WAIT_FOREVER ? timeout_ms : timeout_ms + 500);
            if (r == PAPP_E_OK) {
                r = s_op->status;
                if (r == PAPP_E_OK && out) *out = s_op->open_conn;
            } else {
                esp_ble_gattc_cancel_open_params_t cancel = { .gattc_if = s_gattc_if };
                memcpy(cancel.remote_bda, s_op->bda, 6);
                esp_ble_gattc_cancel_open(&cancel);
            }
            s_op->kind = OP_NONE;
            xSemaphoreGive(s_op_lock);
        }
    }
    SVC_LEAVE();
    return r;
}

static void ble_disconnect(papp_ble_conn_t c)
{
    SVC_ENTER();
    if (s_running) {
        esp_bd_addr_t b;
        bool found = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        conn_t *cn = conn_by_idx(c);
        if (cn) { memcpy(b, cn->bda, 6); found = true; }
        xSemaphoreGive(s_lock);
        if (found) esp_ble_gap_disconnect(b);
    }
    SVC_LEAVE();
}

static bool conn_bda(uint16_t c, esp_bd_addr_t b)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    conn_t *cn = conn_by_idx(c);
    if (cn) memcpy(b, cn->bda, 6);
    xSemaphoreGive(s_lock);
    return cn != NULL;
}

static int ble_mtu_exchange(papp_ble_conn_t c, uint32_t timeout_ms)
{
    SVC_ENTER();
    int r = PAPP_E_NOT_CONNECTED;
    esp_bd_addr_t b;
    if (s_running && conn_bda(c, b)) {
        op_begin(OP_MTU, cconn(c), 0);
        r = op_end(esp_ble_gattc_send_mtu_req(s_gattc_if, cconn(c)), timeout_ms);
        if (r == PAPP_E_OK) r = s_op->mtu;
        else {                                                      /* exchanged already: the current one */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            conn_t *cn = conn_by_idx(c);
            if (cn && cn->mtu > 23) r = cn->mtu;
            xSemaphoreGive(s_lock);
        }
    }
    SVC_LEAVE();
    return r;
}

static int ble_discover(papp_ble_conn_t c, papp_ble_found_svc_t *svcs, int max_svcs, int *n_svcs,
                        papp_ble_found_chr_t *chrs, int max_chrs, int *n_chrs, uint32_t timeout_ms)
{
    SVC_ENTER();
    int r = PAPP_E_NOT_CONNECTED;
    esp_bd_addr_t b;
    if (n_svcs) *n_svcs = 0;
    if (n_chrs) *n_chrs = 0;
    if (!s_running || !conn_bda(c, b)) goto done;
    uint16_t cid = cconn(c);
    op_begin(OP_SEARCH, cid, 0);
    s_op->nsvc = s_op->nchr = 0;
    r = op_end(esp_ble_gattc_search_service(s_gattc_if, cid, NULL), timeout_ms);
    if (r != PAPP_E_OK) goto done;
    /* Characteristics and their CCCDs from Bluedroid's cache of the peer's table */
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    static esp_gattc_char_elem_t ce[MAX_FIND_CHR];
    s_cccd_n = 0;
    for (int s = 0; s < s_op->nsvc && s_op->nchr < MAX_FIND_CHR; s++) {
        uint16_t cnt = (uint16_t)(MAX_FIND_CHR - s_op->nchr);
        if (esp_ble_gattc_get_all_char(s_gattc_if, cid, s_op->svc[s].start, s_op->svc[s].end, ce, &cnt, 0) != ESP_GATT_OK)
            continue;
        for (int k = 0; k < cnt; k++) {
            papp_ble_found_chr_t *f = &s_op->chr[s_op->nchr++];
            memset(f, 0, sizeof(*f));
            uuid_from_esp(&ce[k].uuid, &f->uuid);
            f->handle = ce[k].char_handle;
            f->svc_index = (uint16_t)s;
            uint8_t p = ce[k].properties;
            f->flags = (p & ESP_GATT_CHAR_PROP_BIT_READ ? PAPP_BLE_READ : 0) |
                       (p & ESP_GATT_CHAR_PROP_BIT_WRITE ? PAPP_BLE_WRITE : 0) |
                       (p & ESP_GATT_CHAR_PROP_BIT_WRITE_NR ? PAPP_BLE_WRITE_NR : 0) |
                       (p & ESP_GATT_CHAR_PROP_BIT_NOTIFY ? PAPP_BLE_NOTIFY : 0) |
                       (p & ESP_GATT_CHAR_PROP_BIT_INDICATE ? PAPP_BLE_INDICATE : 0);
            if (f->flags & (PAPP_BLE_NOTIFY | PAPP_BLE_INDICATE)) {
                esp_gattc_descr_elem_t de[8];
                uint16_t dn = 8;
                if (esp_ble_gattc_get_all_descr(s_gattc_if, cid, f->handle, de, &dn, 0) == ESP_GATT_OK)
                    for (int j = 0; j < dn; j++)
                        if (de[j].uuid.len == ESP_UUID_LEN_16 && de[j].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
                            f->cccd = de[j].handle;
                            if (s_cccd_n < MAX_FIND_CHR) {
                                s_cccd_map[s_cccd_n].cccd = de[j].handle;
                                s_cccd_map[s_cccd_n++].val = f->handle;
                            }
                            break;
                        }
            }
        }
    }
    int nsvc = s_op->nsvc, nchr = s_op->nchr;
    int ns = nsvc < max_svcs ? nsvc : max_svcs;
    int nc = nchr < max_chrs ? nchr : max_chrs;
    if (svcs && ns > 0) memcpy(svcs, s_op->svc, ns * sizeof(*svcs));
    if (chrs && nc > 0) memcpy(chrs, s_op->chr, nc * sizeof(*chrs));
    if (n_svcs) *n_svcs = svcs ? ns : nsvc;
    if (n_chrs) *n_chrs = chrs ? nc : nchr;
    xSemaphoreGive(s_op_lock);
done:
    SVC_LEAVE();
    return r;
}

static int ble_read(papp_ble_conn_t c, uint16_t handle, void *buf, size_t len, uint32_t timeout_ms)
{
    if (!buf && len) return PAPP_E_ARG;
    SVC_ENTER();
    int r = PAPP_E_NOT_CONNECTED;
    esp_bd_addr_t b;
    if (s_running && conn_bda(c, b)) {
        op_begin(OP_READ, cconn(c), handle);
        r = op_end(esp_ble_gattc_read_char(s_gattc_if, cconn(c), handle, ESP_GATT_AUTH_REQ_NONE), timeout_ms);
        if (r == PAPP_E_OK) {
            size_t n = s_op->len < len ? s_op->len : len;
            memcpy(buf, s_op->buf, n);
            r = (int)n;
        }
    }
    SVC_LEAVE();
    return r;
}

static int write_op(papp_ble_conn_t c, uint16_t handle, const void *buf, size_t len, bool response,
                    uint32_t timeout_ms, bool descr)
{
    esp_bd_addr_t b;
    if (!s_running || !conn_bda(c, b)) return PAPP_E_NOT_CONNECTED;
    uint8_t *v = (uint8_t *)(len ? buf : k_zero);
    if (!response)
        return map_err(esp_ble_gattc_write_char(s_gattc_if, cconn(c), handle, (uint16_t)len, v,
                                                ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE));
    op_begin(OP_WRITE, cconn(c), handle);
    esp_err_t e = descr ? esp_ble_gattc_write_char_descr(s_gattc_if, cconn(c), handle, (uint16_t)len, v,
                                                         ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE)
                        : esp_ble_gattc_write_char(s_gattc_if, cconn(c), handle, (uint16_t)len, v,
                                                   ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
    return op_end(e, timeout_ms);
}

static int ble_write(papp_ble_conn_t c, uint16_t handle, const void *buf, size_t len, bool response,
                     uint32_t timeout_ms)
{
    if ((!buf && len) || len > 512) return PAPP_E_ARG;
    SVC_ENTER();
    int r = write_op(c, handle, buf, len, response, timeout_ms, false);
    SVC_LEAVE();
    return r;
}

static int ble_subscribe(papp_ble_conn_t c, uint16_t cccd, int mode, uint32_t timeout_ms)
{
    if (!cccd || mode < 0 || mode > 2) return PAPP_E_ARG;
    SVC_ENTER();
    esp_bd_addr_t b;
    int r = PAPP_E_NOT_CONNECTED;
    if (s_running && conn_bda(c, b)) {
        /* Bluedroid hands notifications on only for value handles registered with it */
        uint16_t val = (uint16_t)(cccd - 1);
        for (int i = 0; i < s_cccd_n; i++)
            if (s_cccd_map[i].cccd == cccd) { val = s_cccd_map[i].val; break; }
        if (mode) esp_ble_gattc_register_for_notify(s_gattc_if, b, val);
        else esp_ble_gattc_unregister_for_notify(s_gattc_if, b, val);
        uint8_t v[2] = { (uint8_t)(mode == 1 ? 1 : mode == 2 ? 2 : 0), 0 };
        r = write_op(c, cccd, v, 2, true, timeout_ms, true);
    }
    SVC_LEAVE();
    return r;
}

/* ── v2: pairing, appearance, own address ─────────────────────────────── */

static int ble_security(const papp_ble_sec_cfg_t *cfg)
{
    if (!cfg || cfg->io_cap > PAPP_BLE_IO_KEYBOARD_DISPLAY) return PAPP_E_ARG;
    SVC_ENTER();
    s_sec = *cfg;
    if (s_running) set_sec_params();
    SVC_LEAVE();
    return PAPP_E_OK;
}

static int ble_set_appearance(uint16_t appearance)
{
    SVC_ENTER();
    s_appearance = appearance;
    if (s_running) esp_ble_gap_config_local_icon(appearance);
    SVC_LEAVE();
    return PAPP_E_OK;
}

static int ble_set_random_addr(const uint8_t a[6])
{
    if (a && (a[5] & 0xC0) != 0xC0) return PAPP_E_ARG;   /* a static random address has its top two bits set */
    SVC_ENTER();
    int r = PAPP_E_OK;
    s_rand_set = a != NULL;
    if (a) memcpy(s_rand, a, 6);
    if (s_running) {
        if (s_adv_active) r = PAPP_E_LIMIT;
        else if (a) {
            r = gap_call(ESP_GAP_BLE_SET_STATIC_RAND_ADDR_EVT, do_set_rand, NULL);
            if (r == PAPP_E_OK) s_own_type = BLE_ADDR_TYPE_RANDOM;
        } else s_own_type = BLE_ADDR_TYPE_PUBLIC;
    }
    SVC_LEAVE();
    return r;
}

static int forget_job(void *arg)
{
    (void)arg;
    int n = esp_ble_get_bond_device_num();
    if (n <= 0) return PAPP_E_OK;
    esp_ble_bond_dev_t *list = heap_caps_calloc(n, sizeof(*list), MALLOC_CAP_SPIRAM);
    if (!list) return PAPP_E_NOMEM;
    int r = PAPP_E_OK;
    if (esp_ble_get_bond_device_list(&n, list) != ESP_OK) r = PAPP_E_IO;
    for (int i = 0; r == PAPP_E_OK && i < n; i++)
        if (esp_ble_remove_bond_device(list[i].bd_addr) != ESP_OK) r = PAPP_E_IO;
    heap_caps_free(list);
    return r;
}

static int count_job(void *arg) { (void)arg; int n = esp_ble_get_bond_device_num(); return n < 0 ? PAPP_E_IO : n; }

static int ble_forget_bonds(void)
{
    SVC_ENTER();
    int r = ensure() ? on_internal_stack(forget_job, NULL) : PAPP_E_UNSUPPORTED;
    SVC_LEAVE();
    return r;
}

static int ble_bond_count(void)
{
    SVC_ENTER();
    int r = ensure() ? on_internal_stack(count_job, NULL) : PAPP_E_UNSUPPORTED;
    SVC_LEAVE();
    return r;
}

/* ── Interface ──────────────────────────────────────────────────────────── */

const papp_if_ble_t lx_if_ble = {
    .hdr = { PAPP_IF_BLE, PAPP_IF_BLE_VER, sizeof(papp_if_ble_t) },
    .available = ble_available,
    .own_addr = ble_own_addr,
    .poll_event = ble_poll_event,
    .wait_event = ble_wait_event,
    .scan_start = ble_scan_start,
    .scan_stop = ble_scan_stop,
    .connect = ble_connect,
    .disconnect = ble_disconnect,
    .mtu_exchange = ble_mtu_exchange,
    .discover = ble_discover,
    .read = ble_read,
    .write = ble_write,
    .subscribe = ble_subscribe,
    .gatt_register = ble_gatt_register,
    .set_value = ble_set_value,
    .notify = ble_notify,
    .adv_start = ble_adv_start,
    .adv_stop = ble_adv_stop,
    .set_name = ble_set_name,
    .gatt_register2 = ble_gatt_register2,
    .security = ble_security,
    .set_appearance = ble_set_appearance,
    .set_random_addr = ble_set_random_addr,
    .forget_bonds = ble_forget_bonds,
    .bond_count = ble_bond_count,
};

void lx_svc_ble_init(void)
{
    s_life = xSemaphoreCreateMutex();
    s_lock = xSemaphoreCreateMutex();
    s_op_lock = xSemaphoreCreateMutex();
    s_gap_lock = xSemaphoreCreateMutex();
    s_gap_done = xSemaphoreCreateBinary();
    s_reg_sem = xSemaphoreCreateCounting(2, 0);
    s_tab_sem = xSemaphoreCreateBinary();
    s_op_done = xSemaphoreCreateBinary();
    s_evq = xQueueCreateWithCaps(EVQ_LEN, sizeof(papp_ble_event_t), MALLOC_CAP_SPIRAM);
    s_op = heap_caps_calloc(1, sizeof(*s_op), MALLOC_CAP_SPIRAM);
}

static int stop_job(void *arg) { (void)arg; stop(); return 0; }

int lx_svc_ble_end_run(void)
{
    if (!s_life) return 0;
    xSemaphoreTake(s_life, portMAX_DELAY);
    bool was = s_running;
    on_internal_stack(stop_job, NULL);
    memset(s_chrs, 0, sizeof(s_chrs));
    s_chr_count = 0;
    s_cccd_n = 0;
    s_failed = false;                       /* the next app may try again */
    snprintf(s_name, sizeof(s_name), "%s", DEF_NAME);
    memset(&s_sec, 0, sizeof(s_sec));       /* the next app starts from v1's defaults */
    s_appearance = 0;
    s_rand_set = false;
    xSemaphoreGive(s_life);
    if (s_evq) xQueueReset(s_evq);
    return was ? 1 : 0;
}

bool lx_svc_ble_present(void) { return true; }

#endif
