/*
 * BLE interface: Bluetooth LE for apps through NimBLE on the P4; the controller runs on the
 * C6 and HCI goes over esp_hosted (VHCI on the SDIO link).
 *
 * The host starts on the first call (or at gatt_register, which needs the app's services in
 * the table before the host runs) and is shut down at teardown, so it costs nothing while
 * other apps run. NimBLE calls back on its own host task: events are copied into a queue,
 * client operations complete a semaphore. Every wait is in 100 ms slices that give up when
 * the app is asked to exit; an operation that is abandoned is ignored when it completes
 * (each one carries a sequence number), and its data lands in launcher buffers only.
 */
#include "sdkconfig.h"
#include "lx_services.h"

#if CONFIG_BT_NIMBLE_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lx_net.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#if LX_HOSTED_MAJOR >= 2
#include "esp_hosted_misc.h"
#else
/* esp_hosted 1.4 (Tab5): the C6 starts its BLE controller at boot and keeps it on */
static inline esp_err_t esp_hosted_bt_controller_init(void) { return ESP_OK; }
static inline esp_err_t esp_hosted_bt_controller_enable(void) { return ESP_OK; }
static inline esp_err_t esp_hosted_bt_controller_disable(void) { return ESP_OK; }
/* IDF 5.5's nimble_port_deinit calls it; 1.4's VHCI driver has only the init (nothing to undo) */
void ble_transport_ll_deinit(void);
void ble_transport_ll_deinit(void) {}
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "lx_svc_ble";

#define SLICE_MS      100
#define SYNC_MS       6000
#define EVQ_LEN       32
#define DEF_NAME      "ESP32-P4"
#define MAX_FIND_SVC  16
#define MAX_FIND_CHR  64
#define OP_BUF        512

/* ── State ──────────────────────────────────────────────────────────────── */

typedef struct {
    bool            used;
    uint16_t        handle;
    papp_ble_addr_t addr;
    uint8_t         sub[PAPP_BLE_MAX_CHRS];     /* server: 0 off, 1 notify, 2 indicate */
} conn_t;

typedef struct {
    uint16_t  flags, max_len;
    uint16_t  val_handle;
    uint16_t  len;
    uint8_t  *val;
} chr_t;

static SemaphoreHandle_t s_life;        /* start / stop / register */
static SemaphoreHandle_t s_lock;        /* connections, values */
static SemaphoreHandle_t s_sync_sem;
static QueueHandle_t     s_evq;
static volatile bool     s_running, s_synced, s_failed, s_ctrl_init;
static uint8_t           s_own_type;
static char              s_name[32] = DEF_NAME;
static conn_t            s_conns[PAPP_BLE_MAX_CONN];

/* GATT server table (launcher copies of the app's description) */
static struct ble_gatt_svc_def *s_svc_defs;
static struct ble_gatt_chr_def *s_chr_defs;
static ble_uuid_any_t          *s_uuids;
static chr_t                    s_chrs[PAPP_BLE_MAX_CHRS];
static int                      s_chr_count;

/* Client operations: one at a time */
static SemaphoreHandle_t s_op_lock, s_op_done;
static struct {
    volatile uint32_t seq;
    int      status;
    uint8_t  buf[OP_BUF];
    size_t   len;
    uint16_t mtu, conn;
    int      nsvc, nchr, cur;               /* discovery */
    papp_ble_found_svc_t svc[MAX_FIND_SVC];
    papp_ble_found_chr_t chr[MAX_FIND_CHR];
    uint16_t def[MAX_FIND_CHR];             /* declaration handles: where each one ends */
} *s_op;

static void host_task(void *arg);
static int  gap_cb(struct ble_gap_event *ev, void *arg);

/* ── Helpers ────────────────────────────────────────────────────────────── */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static int map_err(int rc)
{
    if (rc == 0) return PAPP_E_OK;
    switch (rc) {
    case BLE_HS_ETIMEOUT: case BLE_HS_ETIMEOUT_HCI: return PAPP_E_TIMEOUT;
    case BLE_HS_ENOMEM: case BLE_HS_ENOMEM_EVT:     return PAPP_E_NOMEM;
    case BLE_HS_ENOTCONN:                          return PAPP_E_NOT_CONNECTED;
    case BLE_HS_EINVAL: case BLE_HS_EMSGSIZE:      return PAPP_E_ARG;
    case BLE_HS_ENOTSUP:                           return PAPP_E_UNSUPPORTED;
    case BLE_HS_EALREADY: case BLE_HS_EBUSY:       return PAPP_E_LIMIT;
    default:                                       return PAPP_E_IO;
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

static void uuid_to_nimble(const papp_ble_uuid_t *u, ble_uuid_any_t *o)
{
    memset(o, 0, sizeof(*o));
    if (u->len == 2) {
        o->u16.u.type = BLE_UUID_TYPE_16;
        o->u16.value = (uint16_t)(u->u[0] | u->u[1] << 8);
    } else if (u->len == 4) {
        o->u32.u.type = BLE_UUID_TYPE_32;
        o->u32.value = (uint32_t)u->u[0] | (uint32_t)u->u[1] << 8 | (uint32_t)u->u[2] << 16 |
                       (uint32_t)u->u[3] << 24;
    } else {
        o->u128.u.type = BLE_UUID_TYPE_128;
        memcpy(o->u128.value, u->u, 16);
    }
}

static void uuid_from_nimble(const ble_uuid_t *u, papp_ble_uuid_t *o)
{
    memset(o, 0, sizeof(*o));
    switch (u->type) {
    case BLE_UUID_TYPE_16: {
        uint16_t v = BLE_UUID16(u)->value;
        o->len = 2; o->u[0] = v & 0xFF; o->u[1] = v >> 8;
        break;
    }
    case BLE_UUID_TYPE_32: {
        uint32_t v = BLE_UUID32(u)->value;
        o->len = 4;
        for (int i = 0; i < 4; i++) o->u[i] = (uint8_t)(v >> (8 * i));
        break;
    }
    default:
        o->len = 16;
        memcpy(o->u, BLE_UUID128(u)->value, 16);
        break;
    }
}

static bool uuid_ok(const papp_ble_uuid_t *u) { return u->len == 2 || u->len == 4 || u->len == 16; }

static void addr_from(const ble_addr_t *a, papp_ble_addr_t *o)
{
    o->type = a->type;
    memcpy(o->a, a->val, 6);
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

static conn_t *conn_find(uint16_t h)
{
    for (int i = 0; i < PAPP_BLE_MAX_CONN; i++)
        if (s_conns[i].used && s_conns[i].handle == h) return &s_conns[i];
    return NULL;
}

static int chr_by_handle(uint16_t h)
{
    for (int i = 0; i < s_chr_count; i++)
        if (s_chrs[i].val_handle == h) return i;
    return -1;
}

/* ── Host lifecycle ─────────────────────────────────────────────────────── */

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_type);
    s_synced = true;
    xSemaphoreGive(s_sync_sem);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
    s_synced = false;
}

static void host_task(void *arg)
{
    nimble_port_run();                  /* returns after nimble_port_stop */
    nimble_port_freertos_deinit();
}

static void table_free(void)
{
    for (int i = 0; i < s_chr_count; i++) heap_caps_free(s_chrs[i].val);
    memset(s_chrs, 0, sizeof(s_chrs));
    s_chr_count = 0;
    heap_caps_free(s_svc_defs); s_svc_defs = NULL;
    heap_caps_free(s_chr_defs); s_chr_defs = NULL;
    heap_caps_free(s_uuids);    s_uuids = NULL;
}

static void stop(void)
{
    if (!s_running) return;
    ble_gap_adv_stop();
    ble_gap_disc_cancel();
    ble_gap_conn_cancel();
    for (int i = 0; i < PAPP_BLE_MAX_CONN; i++)
        if (s_conns[i].used) ble_gap_terminate(s_conns[i].handle, BLE_ERR_REM_USER_CONN_TERM);
    for (int t = 0; t < 20; t++) {                     /* 1 s for the peers to go */
        bool any = false;
        for (int i = 0; i < PAPP_BLE_MAX_CONN; i++) any |= s_conns[i].used;
        if (!any) break;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    int rc = nimble_port_stop();
    if (rc == 0) nimble_port_deinit();
    else ESP_LOGW(TAG, "nimble_port_stop %d", rc);
    esp_hosted_bt_controller_disable();
    s_running = false;
    s_synced = false;
    memset(s_conns, 0, sizeof(s_conns));
}

/* Starts the host with the current table; false: no BLE (C6 without BT, HCI not answering) */
static bool start(void)
{
    if (s_running) return s_synced;
    if (s_failed) return false;
    if (lx_net_radio_up(&lx_svc_exit_flag) != ESP_OK) return false;
    if (!s_ctrl_init) {
        esp_err_t e = esp_hosted_bt_controller_init();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "BT controller init: %s (C6 firmware without BT?)", esp_err_to_name(e));
            s_failed = true;
            return false;
        }
        s_ctrl_init = true;
    }
    esp_err_t e = esp_hosted_bt_controller_enable();
    if (e != ESP_OK) ESP_LOGW(TAG, "BT controller enable: %s", esp_err_to_name(e));   /* may be on already */

    e = nimble_port_init();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(e));
        esp_hosted_bt_controller_disable();
        s_failed = true;
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (s_svc_defs) {
        int rc = ble_gatts_count_cfg(s_svc_defs);
        if (!rc) rc = ble_gatts_add_svcs(s_svc_defs);
        if (rc) ESP_LOGE(TAG, "GATT table: %d", rc);
    }
    ble_svc_gap_device_name_set(s_name);
    xSemaphoreTake(s_sync_sem, 0);
    s_synced = false;
    s_running = true;
    nimble_port_freertos_init(host_task);

    int r = wait_sem(s_sync_sem, SYNC_MS);
    if (r != PAPP_E_OK) {
        if (r == PAPP_E_TIMEOUT) {
            ESP_LOGE(TAG, "no sync with the controller in %d ms", SYNC_MS);
            s_failed = true;
        }
        stop();
        return false;
    }
    uint8_t a[6] = { 0 };
    ble_hs_id_copy_addr(s_own_type, a, NULL);
    ESP_LOGI(TAG, "up, address %02x:%02x:%02x:%02x:%02x:%02x (type %d)",
             a[5], a[4], a[3], a[2], a[1], a[0], s_own_type);
    return true;
}

static bool ensure(void)
{
    if (s_running && s_synced) return true;
    xSemaphoreTake(s_life, portMAX_DELAY);
    bool ok = start();
    xSemaphoreGive(s_life);
    return ok;
}

/* ── GAP ────────────────────────────────────────────────────────────────── */

static volatile uint32_t s_conn_seq;            /* the pending central connect */
static volatile int      s_conn_status;
static volatile uint16_t s_conn_handle;
static SemaphoreHandle_t s_conn_done;

static int gap_cb(struct ble_gap_event *ev, void *arg)
{
    papp_ble_event_t e;
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC: {
        memset(&e, 0, sizeof(e));
        e.type = PAPP_BLE_EV_ADV;
        e.rssi = ev->disc.rssi;
        e.code = ev->disc.event_type;
        e.conn = PAPP_BLE_CONN_NONE;
        addr_from(&ev->disc.addr, &e.addr);
        e.len = ev->disc.length_data > sizeof(e.data) ? sizeof(e.data) : ev->disc.length_data;
        memcpy(e.data, ev->disc.data, e.len);
        post(&e);
        return 0;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        post_simple(PAPP_BLE_EV_SCAN_DONE, PAPP_BLE_CONN_NONE, 0, (uint16_t)ev->disc_complete.reason);
        return 0;
    case BLE_GAP_EVENT_CONNECT: {
        uint16_t h = ev->connect.conn_handle;
        memset(&e, 0, sizeof(e));
        e.type = PAPP_BLE_EV_CONNECTED;
        e.conn = h;
        if (ev->connect.status == 0) {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(h, &d) == 0) addr_from(&d.peer_id_addr, &e.addr);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            conn_t *c = NULL;
            for (int i = 0; i < PAPP_BLE_MAX_CONN && !c; i++)
                if (!s_conns[i].used) c = &s_conns[i];
            if (c) {
                memset(c, 0, sizeof(*c));
                c->used = true;
                c->handle = h;
                c->addr = e.addr;
            }
            xSemaphoreGive(s_lock);
            if (!c) {
                ble_gap_terminate(h, BLE_ERR_CONN_LIMIT);
                return 0;
            }
        } else {
            e.code = (uint16_t)ev->connect.status;
            e.conn = PAPP_BLE_CONN_NONE;
        }
        if (arg && (uint32_t)(uintptr_t)arg == s_conn_seq) {    /* our connect() */
            s_conn_status = ev->connect.status;
            s_conn_handle = h;
            xSemaphoreGive(s_conn_done);
        }
        post(&e);
        return 0;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        uint16_t h = ev->disconnect.conn.conn_handle;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        conn_t *c = conn_find(h);
        if (c) c->used = false;
        xSemaphoreGive(s_lock);
        memset(&e, 0, sizeof(e));
        e.type = PAPP_BLE_EV_DISCONNECTED;
        e.conn = h;
        e.code = (uint16_t)ev->disconnect.reason;
        addr_from(&ev->disconnect.conn.peer_id_addr, &e.addr);
        post(&e);
        return 0;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        post_simple(PAPP_BLE_EV_ADV_DONE, PAPP_BLE_CONN_NONE, 0, (uint16_t)ev->adv_complete.reason);
        return 0;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        memset(&e, 0, sizeof(e));
        e.type = PAPP_BLE_EV_NOTIFY;
        e.conn = ev->notify_rx.conn_handle;
        e.handle = ev->notify_rx.attr_handle;
        e.code = ev->notify_rx.indication ? 1 : 0;
        uint16_t n = 0;
        ble_hs_mbuf_to_flat(ev->notify_rx.om, e.data, sizeof(e.data), &n);
        e.len = n;
        post(&e);
        return 0;
    }
    case BLE_GAP_EVENT_SUBSCRIBE: {
        uint16_t mode = ev->subscribe.cur_indicate ? 2 : ev->subscribe.cur_notify ? 1 : 0;
        int i = chr_by_handle(ev->subscribe.attr_handle);
        if (i < 0) return 0;                            /* the service-changed characteristic */
        xSemaphoreTake(s_lock, portMAX_DELAY);
        conn_t *c = conn_find(ev->subscribe.conn_handle);
        if (c) c->sub[i] = (uint8_t)mode;
        xSemaphoreGive(s_lock);
        post_simple(PAPP_BLE_EV_SUBSCRIBE, ev->subscribe.conn_handle, ev->subscribe.attr_handle, mode);
        return 0;
    }
    case BLE_GAP_EVENT_MTU:
        post_simple(PAPP_BLE_EV_MTU, ev->mtu.conn_handle, 0, ev->mtu.value);
        return 0;
    default:
        return 0;
    }
}

/* ── GATT server ────────────────────────────────────────────────────────── */

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int i = (int)(intptr_t)arg;
    if (i < 0 || i >= s_chr_count) return BLE_ATT_ERR_UNLIKELY;
    chr_t *c = &s_chrs[i];
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int rc = os_mbuf_append(ctxt->om, c->val, c->len);
        xSemaphoreGive(s_lock);
        return rc ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > c->max_len) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        papp_ble_event_t e;
        memset(&e, 0, sizeof(e));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint16_t n = 0;
        ble_hs_mbuf_to_flat(ctxt->om, c->val, c->max_len, &n);
        c->len = n;
        e.len = n > sizeof(e.data) ? sizeof(e.data) : n;
        memcpy(e.data, c->val, e.len);
        xSemaphoreGive(s_lock);
        e.type = PAPP_BLE_EV_WRITE;
        e.conn = conn;
        e.handle = c->val_handle;
        post(&e);
        return 0;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static int ble_gatt_register(const papp_ble_svc_t *svcs, int count)
{
    if (!svcs || count <= 0 || count > PAPP_BLE_MAX_SVCS) return PAPP_E_ARG;
    int nchr = 0;
    for (int s = 0; s < count; s++) {
        if (!uuid_ok(&svcs[s].uuid) || !svcs[s].chrs || !svcs[s].chr_count) return PAPP_E_ARG;
        for (int k = 0; k < svcs[s].chr_count; k++) {
            const papp_ble_chr_t *c = &svcs[s].chrs[k];
            if (!uuid_ok(&c->uuid) || !c->flags || !c->max_len || c->max_len > 512) return PAPP_E_ARG;
        }
        nchr += svcs[s].chr_count;
    }
    if (nchr > PAPP_BLE_MAX_CHRS) return PAPP_E_LIMIT;

    SVC_ENTER();
    xSemaphoreTake(s_life, portMAX_DELAY);
    int r = PAPP_E_OK;
    if (s_chr_count) { r = PAPP_E_LIMIT; goto out; }               /* once per run */
    if (s_running) {
        bool busy = ble_gap_adv_active();
        for (int i = 0; i < PAPP_BLE_MAX_CONN; i++) busy |= s_conns[i].used;
        if (busy) { r = PAPP_E_LIMIT; goto out; }
    }
    s_svc_defs = heap_caps_calloc(count + 1, sizeof(*s_svc_defs), MALLOC_CAP_SPIRAM);
    s_chr_defs = heap_caps_calloc(nchr + count, sizeof(*s_chr_defs), MALLOC_CAP_SPIRAM);
    s_uuids = heap_caps_calloc(nchr + count, sizeof(*s_uuids), MALLOC_CAP_SPIRAM);
    if (!s_svc_defs || !s_chr_defs || !s_uuids) { table_free(); r = PAPP_E_NOMEM; goto out; }
    int ci = 0, di = 0, ui = 0;
    for (int s = 0; s < count; s++) {
        uuid_to_nimble(&svcs[s].uuid, &s_uuids[ui]);
        s_svc_defs[s].type = BLE_GATT_SVC_TYPE_PRIMARY;
        s_svc_defs[s].uuid = &s_uuids[ui++].u;
        s_svc_defs[s].characteristics = &s_chr_defs[di];
        for (int k = 0; k < svcs[s].chr_count; k++, ci++, di++) {
            const papp_ble_chr_t *c = &svcs[s].chrs[k];
            chr_t *m = &s_chrs[ci];
            m->flags = c->flags;
            m->max_len = c->max_len;
            m->val = heap_caps_calloc(1, c->max_len, MALLOC_CAP_SPIRAM);
            if (!m->val) { s_chr_count = ci + 1; table_free(); r = PAPP_E_NOMEM; goto out; }
            uuid_to_nimble(&c->uuid, &s_uuids[ui]);
            struct ble_gatt_chr_def *d = &s_chr_defs[di];
            d->uuid = &s_uuids[ui++].u;
            d->access_cb = chr_access;
            d->arg = (void *)(intptr_t)ci;
            d->val_handle = &m->val_handle;
            d->flags = (c->flags & PAPP_BLE_READ ? BLE_GATT_CHR_F_READ : 0) |
                       (c->flags & PAPP_BLE_WRITE ? BLE_GATT_CHR_F_WRITE : 0) |
                       (c->flags & PAPP_BLE_WRITE_NR ? BLE_GATT_CHR_F_WRITE_NO_RSP : 0) |
                       (c->flags & PAPP_BLE_NOTIFY ? BLE_GATT_CHR_F_NOTIFY : 0) |
                       (c->flags & PAPP_BLE_INDICATE ? BLE_GATT_CHR_F_INDICATE : 0);
        }
        di++;                                                       /* zeroed terminator */
    }
    s_chr_count = nchr;
    stop();                                                         /* the table goes in at start */
    if (!start()) {
        table_free();
        r = lx_svc_exit_flag ? PAPP_E_EXIT : PAPP_E_UNSUPPORTED;
        goto out;
    }
    ci = 0;
    for (int s = 0; s < count; s++)
        for (int k = 0; k < svcs[s].chr_count; k++, ci++)
            if (svcs[s].chrs[k].handle) *svcs[s].chrs[k].handle = s_chrs[ci].val_handle;
out:
    xSemaphoreGive(s_life);
    SVC_LEAVE();
    return r;
}

static int ble_set_value(uint16_t handle, const void *buf, size_t len)
{
    SVC_ENTER();
    int r = PAPP_E_OK;
    int i = chr_by_handle(handle);
    if (i < 0 || len > s_chrs[i].max_len || (len && !buf)) r = PAPP_E_ARG;
    else {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s_chrs[i].val, buf, len);
        s_chrs[i].len = (uint16_t)len;
        xSemaphoreGive(s_lock);
    }
    SVC_LEAVE();
    return r;
}

static int send_one(uint16_t conn, uint16_t handle, uint8_t mode, const void *buf, size_t len)
{
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)len);
    if (!om) return PAPP_E_NOMEM;
    int rc = mode == 2 ? ble_gatts_indicate_custom(conn, handle, om) : ble_gatts_notify_custom(conn, handle, om);
    return map_err(rc);
}

static int ble_notify(papp_ble_conn_t c, uint16_t handle, const void *buf, size_t len)
{
    int r = ble_set_value(handle, buf, len);
    if (r) return r;
    SVC_ENTER();
    int i = chr_by_handle(handle);
    uint16_t hs[PAPP_BLE_MAX_CONN];
    uint8_t modes[PAPP_BLE_MAX_CONN];
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 0; k < PAPP_BLE_MAX_CONN; k++) {
        conn_t *cn = &s_conns[k];
        if (!cn->used || !cn->sub[i]) continue;
        if (c != PAPP_BLE_CONN_ALL && cn->handle != c) continue;
        hs[n] = cn->handle;
        modes[n++] = cn->sub[i];
    }
    xSemaphoreGive(s_lock);
    if (!n) r = c == PAPP_BLE_CONN_ALL ? PAPP_E_OK : PAPP_E_NOT_CONNECTED;  /* nobody listening */
    for (int k = 0; k < n; k++) {
        int e = send_one(hs[k], handle, modes[k], s_chrs[i].val, s_chrs[i].len);
        if (e && !r) r = e;
    }
    SVC_LEAVE();
    return r;
}

static int ble_adv_start(const papp_ble_adv_cfg_t *cfg)
{
    if (!cfg || cfg->adv_len > 31 || cfg->rsp_len > 31 || (cfg->adv_len && !cfg->adv) ||
        (cfg->rsp_len && !cfg->rsp)) return PAPP_E_ARG;
    SVC_ENTER();
    int r = PAPP_E_UNSUPPORTED;
    if (ensure()) {
        ble_gap_adv_stop();
        int rc = ble_gap_adv_set_data(cfg->adv, cfg->adv_len);
        if (!rc && cfg->rsp_len) rc = ble_gap_adv_rsp_set_data(cfg->rsp, cfg->rsp_len);
        struct ble_gap_adv_params p = { 0 };
        p.conn_mode = cfg->connectable ? BLE_GAP_CONN_MODE_UND : BLE_GAP_CONN_MODE_NON;
        p.disc_mode = BLE_GAP_DISC_MODE_GEN;
        uint32_t ms = cfg->interval_ms ? cfg->interval_ms : 100;
        p.itvl_min = p.itvl_max = (uint16_t)(ms * 8 / 5);          /* 0.625 ms units */
        if (!rc) rc = ble_gap_adv_start(s_own_type, NULL, BLE_HS_FOREVER, &p, gap_cb, NULL);
        r = map_err(rc);
        if (rc) ESP_LOGW(TAG, "adv_start %d", rc);
    }
    SVC_LEAVE();
    return r;
}

static void ble_adv_stop(void)
{
    SVC_ENTER();
    if (s_running && s_synced) ble_gap_adv_stop();
    SVC_LEAVE();
}

static int ble_set_name(const char *name)
{
    if (!name || !name[0]) return PAPP_E_ARG;
    SVC_ENTER();
    snprintf(s_name, sizeof(s_name), "%s", name);
    if (s_running) ble_svc_gap_device_name_set(s_name);
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
        out->type = s_own_type;
        r = map_err(ble_hs_id_copy_addr(s_own_type, out->a, NULL));
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

static int ble_scan_start(const papp_ble_scan_cfg_t *cfg)
{
    SVC_ENTER();
    int r = PAPP_E_UNSUPPORTED;
    if (ensure()) {
        struct ble_gap_disc_params p = { 0 };
        if (cfg) {
            p.passive = !cfg->active;
            p.filter_duplicates = cfg->filter_duplicates;
            p.itvl = (uint16_t)(cfg->interval_ms * 8 / 5);
            p.window = (uint16_t)(cfg->window_ms * 8 / 5);
        }
        int32_t dur = cfg && cfg->duration_ms ? (int32_t)cfg->duration_ms : BLE_HS_FOREVER;
        ble_gap_disc_cancel();
        int rc = ble_gap_disc(s_own_type, dur, &p, gap_cb, NULL);
        r = map_err(rc);
        if (rc) ESP_LOGW(TAG, "scan_start %d", rc);
    }
    SVC_LEAVE();
    return r;
}

static void ble_scan_stop(void)
{
    SVC_ENTER();
    if (s_running && s_synced) ble_gap_disc_cancel();
    SVC_LEAVE();
}

static int ble_connect(const papp_ble_addr_t *peer, uint32_t timeout_ms, papp_ble_conn_t *out)
{
    if (!peer) return PAPP_E_ARG;
    SVC_ENTER();
    int r = PAPP_E_UNSUPPORTED;
    if (out) *out = PAPP_BLE_CONN_NONE;
    if (ensure()) {
        xSemaphoreTake(s_op_lock, portMAX_DELAY);
        ble_gap_disc_cancel();                      /* the controller can't scan and connect */
        ble_addr_t a = { .type = peer->type };
        memcpy(a.val, peer->a, 6);
        uint32_t seq = ++s_conn_seq;
        xSemaphoreTake(s_conn_done, 0);
        int32_t t = timeout_ms == PAPP_WAIT_FOREVER ? BLE_HS_FOREVER : (int32_t)timeout_ms;
        int rc = ble_gap_connect(s_own_type, &a, t, NULL, gap_cb, (void *)(uintptr_t)seq);
        if (rc) {
            r = map_err(rc);
        } else {
            r = wait_sem(s_conn_done, timeout_ms == PAPP_WAIT_FOREVER ? timeout_ms : timeout_ms + 500);
            if (r == PAPP_E_OK) {
                if (s_conn_status == 0) {
                    if (out) *out = s_conn_handle;
                } else {
                    r = s_conn_status == BLE_HS_ETIMEOUT ? PAPP_E_TIMEOUT : PAPP_E_REFUSED;
                }
            } else {
                s_conn_seq++;                       /* a late result is ignored */
                ble_gap_conn_cancel();
            }
        }
        xSemaphoreGive(s_op_lock);
    }
    SVC_LEAVE();
    return r;
}

static void ble_disconnect(papp_ble_conn_t c)
{
    SVC_ENTER();
    if (s_running) ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
    SVC_LEAVE();
}

/* Client operations: begin under s_op_lock with a fresh sequence number, the NimBLE callback
 * checks it before touching s_op, end waits and invalidates the number. */
static uint32_t op_begin(uint16_t conn)
{
    xSemaphoreTake(s_op_lock, portMAX_DELAY);
    xSemaphoreTake(s_op_done, 0);
    s_op->status = 0;
    s_op->len = 0;
    s_op->conn = conn;
    return ++s_op->seq;
}

static bool op_mine(void *arg) { return s_op && (uint32_t)(uintptr_t)arg == s_op->seq; }

static void op_finish(int status)
{
    s_op->status = status;
    xSemaphoreGive(s_op_done);
}

static int op_end(int rc, uint32_t timeout_ms)
{
    int r;
    if (rc) r = map_err(rc);
    else {
        r = wait_sem(s_op_done, timeout_ms);
        if (r == PAPP_E_OK) r = s_op->status;
    }
    s_op->seq++;
    xSemaphoreGive(s_op_lock);
    return r;
}

static int status_of(const struct ble_gatt_error *e)
{
    if (!e || e->status == 0 || e->status == BLE_HS_EDONE) return PAPP_E_OK;
    return map_err(e->status);
}

static int mtu_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    if (!op_mine(arg)) return 0;
    s_op->mtu = mtu;
    op_finish(status_of(err));
    return 0;
}

static int ble_mtu_exchange(papp_ble_conn_t c, uint32_t timeout_ms)
{
    SVC_ENTER();
    int r = PAPP_E_NOT_CONNECTED;
    if (s_running && s_synced) {
        uint32_t seq = op_begin(c);
        r = op_end(ble_gattc_exchange_mtu(c, mtu_cb, (void *)(uintptr_t)seq), timeout_ms);
        if (r == PAPP_E_OK) r = s_op->mtu;
        if (r == PAPP_E_LIMIT) r = ble_att_mtu(c);                 /* exchanged already */
    }
    SVC_LEAVE();
    return r;
}

static int dsc_cb(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val, const struct ble_gatt_dsc *dsc,
                  void *arg)
{
    if (!op_mine(arg)) return 0;
    if (err->status == 0 && dsc && ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16)
        s_op->chr[s_op->cur].cccd = dsc->handle;
    if (err->status) op_finish(status_of(err));
    return 0;
}

static int chr_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    if (!op_mine(arg)) return 0;
    if (err->status == 0 && chr) {
        if (s_op->nchr < MAX_FIND_CHR) {
            papp_ble_found_chr_t *f = &s_op->chr[s_op->nchr];
            memset(f, 0, sizeof(*f));
            uuid_from_nimble(&chr->uuid.u, &f->uuid);
            f->handle = chr->val_handle;
            f->svc_index = (uint16_t)s_op->cur;
            uint8_t p = chr->properties;
            f->flags = (p & BLE_GATT_CHR_PROP_READ ? PAPP_BLE_READ : 0) |
                       (p & BLE_GATT_CHR_PROP_WRITE ? PAPP_BLE_WRITE : 0) |
                       (p & BLE_GATT_CHR_PROP_WRITE_NO_RSP ? PAPP_BLE_WRITE_NR : 0) |
                       (p & BLE_GATT_CHR_PROP_NOTIFY ? PAPP_BLE_NOTIFY : 0) |
                       (p & BLE_GATT_CHR_PROP_INDICATE ? PAPP_BLE_INDICATE : 0);
            s_op->def[s_op->nchr] = chr->def_handle;
            s_op->nchr++;
        }
        return 0;
    }
    op_finish(status_of(err));
    return 0;
}

static int svc_cb(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (!op_mine(arg)) return 0;
    if (err->status == 0 && svc) {
        if (s_op->nsvc < MAX_FIND_SVC) {
            papp_ble_found_svc_t *f = &s_op->svc[s_op->nsvc++];
            uuid_from_nimble(&svc->uuid.u, &f->uuid);
            f->start = svc->start_handle;
            f->end = svc->end_handle;
        }
        return 0;
    }
    op_finish(status_of(err));
    return 0;
}

static int ble_discover(papp_ble_conn_t c, papp_ble_found_svc_t *svcs, int max_svcs, int *n_svcs,
                        papp_ble_found_chr_t *chrs, int max_chrs, int *n_chrs, uint32_t timeout_ms)
{
    SVC_ENTER();
    int r = PAPP_E_NOT_CONNECTED;
    if (n_svcs) *n_svcs = 0;
    if (n_chrs) *n_chrs = 0;
    if (!s_running || !s_synced) goto done;
    int64_t end = now_ms() + timeout_ms;
#define LEFT() (timeout_ms == PAPP_WAIT_FOREVER ? PAPP_WAIT_FOREVER : \
                (uint32_t)(end > now_ms() ? end - now_ms() : 1))
    uint32_t seq = op_begin(c);
    s_op->nsvc = s_op->nchr = 0;
    r = op_end(ble_gattc_disc_all_svcs(c, svc_cb, (void *)(uintptr_t)seq), LEFT());
    int nsvc = s_op->nsvc;
    for (int s = 0; s < nsvc && r == PAPP_E_OK; s++) {
        seq = op_begin(c);
        s_op->cur = s;
        r = op_end(ble_gattc_disc_all_chrs(c, s_op->svc[s].start, s_op->svc[s].end, chr_cb,
                                           (void *)(uintptr_t)seq), LEFT());
    }
    int nchr = s_op->nchr;
    for (int k = 0; k < nchr && r == PAPP_E_OK; k++) {         /* CCCDs of the ones that notify */
        papp_ble_found_chr_t *f = &s_op->chr[k];
        if (!(f->flags & (PAPP_BLE_NOTIFY | PAPP_BLE_INDICATE))) continue;
        uint16_t last = s_op->svc[f->svc_index].end;
        if (k + 1 < nchr && s_op->chr[k + 1].svc_index == f->svc_index) last = s_op->def[k + 1] - 1;
        if (last <= f->handle) continue;
        seq = op_begin(c);
        s_op->cur = k;
        r = op_end(ble_gattc_disc_all_dscs(c, f->handle, last, dsc_cb, (void *)(uintptr_t)seq), LEFT());
    }
#undef LEFT
    if (r == PAPP_E_OK) {
        int ns = nsvc < max_svcs ? nsvc : max_svcs;
        int nc = nchr < max_chrs ? nchr : max_chrs;
        if (svcs && ns > 0) memcpy(svcs, s_op->svc, ns * sizeof(*svcs));
        if (chrs && nc > 0) memcpy(chrs, s_op->chr, nc * sizeof(*chrs));
        if (n_svcs) *n_svcs = svcs ? ns : nsvc;
        if (n_chrs) *n_chrs = chrs ? nc : nchr;
    }
done:
    SVC_LEAVE();
    return r;
}

static int read_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (!op_mine(arg)) return 0;
    if (err->status == 0 && attr) {
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        if (s_op->len + n > OP_BUF) n = (uint16_t)(OP_BUF - s_op->len);
        os_mbuf_copydata(attr->om, 0, n, s_op->buf + s_op->len);
        s_op->len += n;
        return 0;
    }
    op_finish(status_of(err));
    return 0;
}

static int ble_read(papp_ble_conn_t c, uint16_t handle, void *buf, size_t len, uint32_t timeout_ms)
{
    if (!buf && len) return PAPP_E_ARG;
    SVC_ENTER();
    int r = PAPP_E_NOT_CONNECTED;
    if (s_running && s_synced) {
        uint32_t seq = op_begin(c);
        r = op_end(ble_gattc_read_long(c, handle, 0, read_cb, (void *)(uintptr_t)seq), timeout_ms);
        if (r == PAPP_E_OK) {
            size_t n = s_op->len < len ? s_op->len : len;
            memcpy(buf, s_op->buf, n);
            r = (int)n;
        }
    }
    SVC_LEAVE();
    return r;
}

static int write_cb(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (op_mine(arg)) op_finish(status_of(err));
    return 0;
}

static int ble_write(papp_ble_conn_t c, uint16_t handle, const void *buf, size_t len, bool response,
                     uint32_t timeout_ms)
{
    if ((!buf && len) || len > 512) return PAPP_E_ARG;
    SVC_ENTER();
    int r = PAPP_E_NOT_CONNECTED;
    if (s_running && s_synced) {
        if (!response) {
            r = map_err(ble_gattc_write_no_rsp_flat(c, handle, buf, (uint16_t)len));
        } else {
            uint32_t seq = op_begin(c);
            int rc;
            if (len + 3 > (size_t)ble_att_mtu(c)) {              /* a prepared (long) write */
                struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)len);
                rc = om ? ble_gattc_write_long(c, handle, 0, om, write_cb, (void *)(uintptr_t)seq) : BLE_HS_ENOMEM;
            } else {
                rc = ble_gattc_write_flat(c, handle, buf, (uint16_t)len, write_cb, (void *)(uintptr_t)seq);
            }
            r = op_end(rc, timeout_ms);
        }
    }
    SVC_LEAVE();
    return r;
}

static int ble_subscribe(papp_ble_conn_t c, uint16_t cccd, int mode, uint32_t timeout_ms)
{
    if (!cccd || mode < 0 || mode > 2) return PAPP_E_ARG;
    uint8_t v[2] = { (uint8_t)(mode == 1 ? 1 : mode == 2 ? 2 : 0), 0 };
    return ble_write(c, cccd, v, 2, true, timeout_ms);
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
};

void lx_svc_ble_init(void)
{
    s_life = xSemaphoreCreateMutex();
    s_lock = xSemaphoreCreateMutex();
    s_op_lock = xSemaphoreCreateMutex();
    s_sync_sem = xSemaphoreCreateBinary();
    s_op_done = xSemaphoreCreateBinary();
    s_conn_done = xSemaphoreCreateBinary();
    s_evq = xQueueCreateWithCaps(EVQ_LEN, sizeof(papp_ble_event_t), MALLOC_CAP_SPIRAM);
    s_op = heap_caps_calloc(1, sizeof(*s_op), MALLOC_CAP_SPIRAM);
}

int lx_svc_ble_end_run(void)
{
    if (!s_life) return 0;
    xSemaphoreTake(s_life, portMAX_DELAY);
    bool was = s_running;
    stop();
    table_free();
    s_failed = false;                       /* the next app may try again */
    snprintf(s_name, sizeof(s_name), "%s", DEF_NAME);
    xSemaphoreGive(s_life);
    if (s_evq) xQueueReset(s_evq);
    return was ? 1 : 0;
}

bool lx_svc_ble_present(void) { return true; }

#else   /* no BLE on this board */

void lx_svc_ble_init(void) {}
int  lx_svc_ble_end_run(void) { return 0; }
bool lx_svc_ble_present(void) { return false; }

#endif
