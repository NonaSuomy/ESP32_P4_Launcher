/*
 * Bluetooth gamepads on a USB dongle (the C6 is BLE only and stays the Wi-Fi chip):
 *   Classic: Bluedroid's HID host (DualShock 4, DualSense, Switch Pro, 8BitDo in those modes). The
 *   pad calls us when it is switched on: the board stays connectable while a pad is saved.
 *   BLE: HID over GATT (Xbox Wireless, 8BitDo in BLE mode, ...), a small client here. A BLE pad only
 *   advertises when it is switched on, so we keep one low-duty LE connection request pending
 *   (12.5 % of the radio, rotating over the saved BLE pads, each request ~30 s) until it answers.
 *
 * A pad is picked from the Bluetooth screen's scan (lx_bt_speaker_scan: inquiry + BLE scan), paired
 * and saved in /sd/config/bt_pads.txt: one "addr name" line per Classic pad, "addr le<type> name" per
 * BLE pad. Bluedroid keeps the bonds (and the Classic pads' HID descriptors) in its NVS store.
 *
 * Reports go to the input manager (lx_input_hidpad_*), which reads them by model or by the HID
 * descriptor, so a Bluetooth pad is a player like a USB one.
 *
 * Tasks: Bluedroid's BTC task runs the HID and GATT callbacks (connection table, reports);
 * attaching and detaching players (reads the mapping files on the card) and connection requests
 * run on the lx_bt task.
 */
#include "sdkconfig.h"

#if CONFIG_BT_BLUEDROID_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID && CONFIG_BT_HID_HOST_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lx_bt_priv.h"
#include "lx_common.h"
#include "lx_input.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_hidh_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

static const char *TAG = "lx_bt_pad";

#define CFG_FILE    "/sd/config/bt_pads.txt"
#define TICK_MS     2000
#define PAIR_MS     40000   /* from the connect request: a DualShock 4 answered after 25 s */
#define SETUP_MS    20000       /* BLE: encryption + reading the pad's HID service */
#define GATTC_APP   0x0B1D
#define ENC_FAILS   3           /* a saved BLE pad's bond goes after this many failed encryptions in a row */
#define ENC_BOND_MS 5000        /* BLE: time a saved pad has to start encryption itself (enc_job) */
#define MAX_REPORTS 8

typedef struct {
    uint8_t addr[6];
    char    name[48];
    bool    ble;
    uint8_t atype;              /* BLE address type */
    uint8_t enc_fails;          /* BLE: encryption failures in a row (the bond goes at ENC_FAILS) */
} saved_t;

enum { ST_NONE, ST_ENCRYPT, ST_DISCOVER, ST_READ_MAP, ST_READ_PNP, ST_READ_REF, ST_SUBSCRIBE, ST_READY };

typedef struct {
    uint16_t val, ref, ccc;
    uint8_t  id;
} report_t;

typedef struct {
    bool         used, ble;
    uint8_t      handle;        /* Classic: HID host handle */
    uint16_t     conn_id;       /* BLE */
    uint8_t      addr[6];
    uint32_t     gen;           /* +1 at every open: a late attach job sees the slot was reused */
    lx_hidpad_t *pad;           /* the player, once the descriptor arrived */
    /* BLE set-up */
    int          st, step;
    int64_t      t0;
    bool         enc_asked;     /* we asked for encryption, or it is done */
    uint16_t     hid_s, hid_e, dis_s, dis_e, map_h, pnp_h;
    report_t     rep[MAX_REPORTS];
    int          nrep;
    bool         ids;           /* the report map uses report ids */
    uint8_t     *map;
    uint16_t     map_len, vid, pid;
} conn_t;

static saved_t s_saved[LX_BT_MAX_PADS];
static volatile int s_nsaved;
static conn_t  s_conn[LX_BT_MAX_PADS];
static SemaphoreHandle_t s_lock;            /* s_conn[].pad between the BTC task and the lx_bt task */
static volatile bool s_held, s_hh_up, s_app;
static volatile bool s_pairing;
static lx_bt_found_t s_pair;
static TickType_t s_pair_t;
static char    s_err[128];
static TimerHandle_t s_timer;

/* BLE */
static volatile esp_gatt_if_t s_gif = ESP_GATT_IF_NONE;
static volatile bool s_opening;             /* an LE connection request is pending */
static uint8_t s_open_addr[6];
static int64_t s_open_t;
static int     s_next_ble;                  /* round robin over the saved BLE pads */
static esp_timer_handle_t s_enc_timer;

static const char *addr_str(const uint8_t *a)
{
    static char b[4][18];
    static int k;
    char *s = b[k++ & 3];
    snprintf(s, 18, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
    return s;
}

/* ── Saved pads ─────────────────────────────────────────────────────────── */

static void cfg_save(void)
{
    if (!lx_sd_write_try_lock()) { ESP_LOGW(TAG, "cannot save Bluetooth pad settings: SD card is busy"); return; }
    if (!s_nsaved) {
        remove(CFG_FILE);
        lx_sd_write_unlock();
        return;
    }
    FILE *f = fopen(CFG_FILE ".tmp", "w");
    if (!f) { lx_sd_write_unlock(); ESP_LOGW(TAG, "cannot write " CFG_FILE); return; }
    for (int i = 0; i < s_nsaved; i++) {
        if (s_saved[i].ble) fprintf(f, "%s le%u %s\n", addr_str(s_saved[i].addr), s_saved[i].atype, s_saved[i].name);
        else fprintf(f, "%s %s\n", addr_str(s_saved[i].addr), s_saved[i].name);
    }
    bool ok = ferror(f) == 0;
    if (fclose(f) != 0) ok = false;
    if (ok) ok = lx_sd_replace_file(CFG_FILE ".tmp", CFG_FILE) == 0;
    if (!ok) {
        remove(CFG_FILE ".tmp");
        ESP_LOGW(TAG, "could not finish writing " CFG_FILE);
    }
    lx_sd_write_unlock();
}

static void cfg_load(void)
{
    FILE *f = fopen(CFG_FILE, "r");
    if (!f) return;
    char line[96];
    unsigned a[6];
    int n = 0;
    while (n < LX_BT_MAX_PADS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        int used = 0;
        if (sscanf(line, "%x:%x:%x:%x:%x:%x %n", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &used) < 6) continue;
        saved_t *s = &s_saved[n];
        memset(s, 0, sizeof(*s));
        for (int i = 0; i < 6; i++) s->addr[i] = (uint8_t)a[i];
        const char *rest = used ? line + used : "";
        if (rest[0] == 'l' && rest[1] == 'e' && rest[2] >= '0' && rest[2] <= '3' && (rest[3] == ' ' || !rest[3])) {
            s->ble = true;
            s->atype = (uint8_t)(rest[2] - '0');
            rest += rest[3] ? 4 : 3;
        }
        snprintf(s->name, sizeof(s->name), "%.47s", rest);
        n++;
    }
    fclose(f);
    s_nsaved = n;
}

static int saved_find(const uint8_t *addr)
{
    for (int i = 0; i < s_nsaved; i++) if (!memcmp(s_saved[i].addr, addr, 6)) return i;
    return -1;
}

static void set_err(const char *e)
{
    snprintf(s_err, sizeof(s_err), "%s", e);
    if (e[0]) ESP_LOGW(TAG, "%s", e);
}

static int nsaved_ble(void)
{
    int n = 0;
    for (int i = 0; i < s_nsaved; i++) n += s_saved[i].ble;
    return n;
}

/* ── Holding the stack (lx_bt task) ─────────────────────────────────────── */

static bool want_stack(void) { return s_nsaved > 0 || s_pairing; }

static void hold_job(void *a)
{
    (void)a;
    if (s_held || !want_stack() || !lx_bt_usb_present()) return;
    if (!lx_bt_acquire(LX_BT_USER_PADS, true, NULL)) {
        set_err(lx_bt_running() ? "Bluetooth is busy on the Wi-Fi chip (an app uses BLE)"
                                : lx_bt_usb_why()[0] ? lx_bt_usb_why() : "Bluetooth did not start on the dongle");
        return;
    }
    s_held = true;
}

static void let_go(void)
{
    if (!s_held || want_stack()) return;
    s_held = false;
    lx_bt_release(LX_BT_USER_PADS);
}

/* ── Players (lx_bt task) ───────────────────────────────────────────────── */

static const char *model_name(uint16_t vid, uint16_t pid)
{
    if (vid == 0x054C && (pid == 0x05C4 || pid == 0x09CC)) return "DualShock 4";
    if (vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2)) return "DualSense";
    if (vid == 0x057E && pid == 0x2009) return "Switch Pro Controller";
    if (vid == 0x045E) return "Xbox Wireless Controller";
    return NULL;
}

static void save_pad(const uint8_t *addr, const char *name, bool ble, uint8_t atype)
{
    int k = saved_find(addr);
    if (k >= 0) {
        if ((name[0] && strcmp(s_saved[k].name, name)) || s_saved[k].ble != ble) {
            if (name[0]) snprintf(s_saved[k].name, sizeof(s_saved[k].name), "%s", name);
            s_saved[k].ble = ble;
            s_saved[k].atype = atype;
            cfg_save();
        }
        return;
    }
    if (s_nsaved == LX_BT_MAX_PADS) {               /* full: the oldest goes */
        memmove(&s_saved[0], &s_saved[1], sizeof(saved_t) * (LX_BT_MAX_PADS - 1));
        s_nsaved--;
    }
    saved_t *s = &s_saved[s_nsaved];
    memset(s, 0, sizeof(*s));
    memcpy(s->addr, addr, 6);
    snprintf(s->name, sizeof(s->name), "%s", name);
    s->ble = ble;
    s->atype = atype;
    s_nsaved++;
    cfg_save();
}

typedef struct {
    int      slot;
    uint32_t gen;
    uint16_t vid, pid;
    size_t   len;
    uint8_t  desc[];
} attach_t;

static void attach_job(void *arg)
{
    attach_t *a = arg;
    conn_t *c = &s_conn[a->slot];
    if (c->used && c->gen == a->gen && !c->pad) {
        int k = saved_find(c->addr);
        char name[48];
        const char *model = model_name(a->vid, a->pid);
        if (k >= 0 && s_saved[k].name[0]) snprintf(name, sizeof(name), "%s", s_saved[k].name);
        else if (model) snprintf(name, sizeof(name), "%s", model);
        else snprintf(name, sizeof(name), "Bluetooth pad %04X:%04X", a->vid, a->pid);
        if (k >= 0 && !s_saved[k].name[0]) save_pad(c->addr, name, s_saved[k].ble, s_saved[k].atype);
        lx_hidpad_t *p = lx_input_hidpad_open(name, a->vid, a->pid, a->desc, a->len);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (c->used && c->gen == a->gen) c->pad = p;
        else lx_input_hidpad_close(p);          /* it went away meanwhile */
        xSemaphoreGive(s_lock);
    }
    free(a);
}

static void detach_job(void *arg)
{
    lx_input_hidpad_close((lx_hidpad_t *)arg);
}

static void post_attach(conn_t *c, uint16_t vid, uint16_t pid, const uint8_t *desc, size_t len)
{
    attach_t *a = malloc(sizeof(attach_t) + len);
    if (!a) return;
    a->slot = (int)(c - s_conn);
    a->gen = c->gen;
    a->vid = vid;
    a->pid = pid;
    a->len = desc ? len : 0;
    if (a->len) memcpy(a->desc, desc, a->len);
    lx_bt_post(attach_job, a);
}

/* ── Connection table (BTC task) ────────────────────────────────────────── */

static conn_t *conn_by_handle(uint8_t h)
{
    for (int i = 0; i < LX_BT_MAX_PADS; i++)
        if (s_conn[i].used && !s_conn[i].ble && s_conn[i].handle == h) return &s_conn[i];
    return NULL;
}

static conn_t *conn_by_ble(uint16_t conn_id)
{
    for (int i = 0; i < LX_BT_MAX_PADS; i++)
        if (s_conn[i].used && s_conn[i].ble && s_conn[i].conn_id == conn_id) return &s_conn[i];
    return NULL;
}

static conn_t *conn_by_addr(const uint8_t *a)
{
    for (int i = 0; i < LX_BT_MAX_PADS; i++)
        if (s_conn[i].used && !memcmp(s_conn[i].addr, a, 6)) return &s_conn[i];
    return NULL;
}

static conn_t *conn_new(const uint8_t *a)
{
    conn_t *c = conn_by_addr(a);
    for (int i = 0; !c && i < LX_BT_MAX_PADS; i++) if (!s_conn[i].used) c = &s_conn[i];
    if (!c) return NULL;
    uint32_t gen = c->gen + 1;
    free(c->map);
    memset(c, 0, sizeof(*c));
    c->gen = gen;
    c->used = true;
    memcpy(c->addr, a, 6);
    c->t0 = esp_timer_get_time();
    return c;
}

static void conn_drop(conn_t *c)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    lx_hidpad_t *p = c->pad;
    c->pad = NULL;
    c->used = false;
    xSemaphoreGive(s_lock);
    free(c->map);
    c->map = NULL;
    if (p) lx_bt_post(detach_job, p);
}

bool lx_bt_pads_owns(const uint8_t *bda)
{
    if (!bda) return false;
    if (s_opening && !memcmp(s_open_addr, bda, 6)) return true;
    if (s_pairing && s_pair.ble && !memcmp(s_pair.addr, bda, 6)) return true;
    for (int i = 0; i < LX_BT_MAX_PADS; i++)
        if (s_conn[i].used && s_conn[i].ble && !memcmp(s_conn[i].addr, bda, 6)) return true;
    return false;
}

int lx_bt_pads_linked(void)
{
    int n = 0;
    for (int i = 0; i < LX_BT_MAX_PADS; i++) n += s_conn[i].used;
    return n;
}


uint32_t lx_bt_pads_still_ms(void)
{
    uint32_t m = UINT32_MAX;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < LX_BT_MAX_PADS; i++)
        if (s_conn[i].used && s_conn[i].pad) {
            uint32_t v = lx_input_hidpad_still_ms(s_conn[i].pad);
            if (v < m) m = v;
        }
    xSemaphoreGive(s_lock);
    return m;
}

bool lx_bt_pads_stats(const uint8_t *addr, lx_hidpad_stats_t *out)
{
    bool ok = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    conn_t *c = conn_by_addr(addr);
    if (c && c->pad) {
        lx_input_hidpad_stats(c->pad, out);
        ok = true;
    }
    xSemaphoreGive(s_lock);
    return ok;
}

static void pairing_end(void)
{
    if (!s_pairing) return;
    s_pairing = false;
    lx_bt_speaker_pause(false);
}

static void saved_add_job(void *a)
{
    (void)a;
    save_pad(s_pair.addr, s_pair.name, s_pair.ble, s_pair.addr_type);
    pairing_end();
    ESP_LOGI(TAG, "paired with %s", s_pair.name);
}

static void pair_failed_job(void *a)
{
    const char *why = a;
    pairing_end();
    set_err(why ? why : "Pairing failed: is the controller in pairing mode?");
    let_go();
}

/* ── Classic: HID host callback (BTC task) ──────────────────────────────── */

/* A pad Bluedroid holds a bond for (paired with this dongle before, here or on a PC) that
 * connects by itself: kept, named from its model when its descriptor arrives */
static void adopt_job(void *arg)
{
    uint8_t *a = arg;
    if (saved_find(a) < 0) {
        save_pad(a, "", false, 0);
        ESP_LOGI(TAG, "%s: paired before, kept", addr_str(a));
    }
    if (s_pairing) pairing_end();       /* whatever was being paired, this pad is the one that came */
    hold_job(NULL);                     /* a saved pad keeps the stack up */
    free(a);
}

static bool bonded(const uint8_t *addr)
{
    int n = esp_bt_gap_get_bond_device_num();
    if (n <= 0) return false;
    esp_bd_addr_t *list = malloc(sizeof(esp_bd_addr_t) * n);
    if (!list) return false;
    bool found = false;
    if (esp_bt_gap_get_bond_device_list(&n, list) == ESP_OK)
        for (int i = 0; i < n && !found; i++) found = !memcmp(list[i], addr, 6);
    free(list);
    return found;
}

static void hh_cb(esp_hidh_cb_event_t ev, esp_hidh_cb_param_t *p)
{
    switch (ev) {
    case ESP_HIDH_INIT_EVT:
        s_hh_up = p->init.status == ESP_HIDH_OK;
        if (!s_hh_up) ESP_LOGE(TAG, "HID host init: %d", p->init.status);
        break;
    case ESP_HIDH_OPEN_EVT: {
        const uint8_t *a = p->open.bd_addr;
        bool pairing = s_pairing && !s_pair.ble && !memcmp(a, s_pair.addr, 6);
        if (p->open.status == ESP_HIDH_OK && p->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTING)
            break;      /* a known pad: Bluedroid acknowledges the request first, the result follows */
        if (p->open.status != ESP_HIDH_OK || p->open.conn_status != ESP_HIDH_CONN_STATE_CONNECTED) {
            if (pairing) lx_bt_post(pair_failed_job, NULL);
            else ESP_LOGW(TAG, "connection failed: %d", p->open.status);
            break;
        }
        if (!pairing && saved_find(a) < 0 && bonded(a)) {
            uint8_t *copy = malloc(6);
            if (copy) {
                memcpy(copy, a, 6);
                lx_bt_post(adopt_job, copy);
            }
        } else if (!pairing && saved_find(a) < 0) {
            ESP_LOGW(TAG, "%s is not a saved pad: disconnected", addr_str(a));
            esp_bt_hid_host_disconnect((uint8_t *)a);
            break;
        }
        conn_t *c = conn_new(a);
        if (!c) {
            esp_bt_hid_host_disconnect((uint8_t *)a);
            break;
        }
        c->handle = p->open.handle;
        set_err("");
        if (pairing) lx_bt_post(saved_add_job, NULL);
        ESP_LOGI(TAG, "connected: %s (%s)", addr_str(a), p->open.is_orig ? "we called" : "it called");
        break;
    }
    case ESP_HIDH_GET_DSCP_EVT: {
        conn_t *c = conn_by_handle(p->dscp.handle);
        if (!c || c->pad) break;
        ESP_LOGI(TAG, "pad %04X:%04X, descriptor %u bytes", p->dscp.vendor_id, p->dscp.product_id,
                 (unsigned)p->dscp.dl_len);
        post_attach(c, p->dscp.vendor_id, p->dscp.product_id, p->dscp.dsc_list, p->dscp.dl_len);
        break;
    }
    case ESP_HIDH_DATA_IND_EVT: {
        conn_t *c = conn_by_handle(p->data_ind.handle);
        if (!c) break;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (c->pad) lx_input_hidpad_report(c->pad, p->data_ind.data, p->data_ind.len);
        xSemaphoreGive(s_lock);
        break;
    }
    case ESP_HIDH_CLOSE_EVT: {
        conn_t *c = conn_by_handle(p->close.handle);
        if (c) {
            ESP_LOGI(TAG, "disconnected (reason %d)", p->close.reason);
            conn_drop(c);
        }
        break;
    }
    case ESP_HIDH_VC_UNPLUG_EVT: {
        conn_t *c = conn_by_handle(p->unplug.handle);
        if (c) conn_drop(c);
        break;
    }
    default:
        break;
    }
}

/* ── BLE: HID over GATT client (BTC task) ───────────────────────────────── */

static void ble_fail(conn_t *c, const char *why)
{
    ESP_LOGW(TAG, "%s: %s", addr_str(c->addr), why);
    bool pairing = s_pairing && s_pair.ble && !memcmp(s_pair.addr, c->addr, 6);
    esp_ble_gap_disconnect(c->addr);
    if (pairing) lx_bt_post(pair_failed_job, (void *)why);
}

static uint16_t uuid16(const esp_bt_uuid_t *u) { return u->len == ESP_UUID_LEN_16 ? u->uuid.uuid16 : 0; }

/* the HID service's characteristics, from Bluedroid's cache after the search */
static bool ble_collect(conn_t *c)
{
    esp_gattc_char_elem_t ch[24];
    uint16_t n = 24;
    if (esp_ble_gattc_get_all_char(s_gif, c->conn_id, c->hid_s, c->hid_e, ch, &n, 0) != ESP_GATT_OK) return false;
    c->nrep = 0;
    for (int i = 0; i < n; i++) {
        uint16_t u = uuid16(&ch[i].uuid);
        if (u == 0x2A4B) c->map_h = ch[i].char_handle;
        else if (u == 0x2A4D && (ch[i].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) && c->nrep < MAX_REPORTS) {
            report_t *r = &c->rep[c->nrep++];
            memset(r, 0, sizeof(*r));
            r->val = ch[i].char_handle;
            esp_gattc_descr_elem_t d[4];
            uint16_t dn = 4;
            if (esp_ble_gattc_get_all_descr(s_gif, c->conn_id, r->val, d, &dn, 0) == ESP_GATT_OK)
                for (int k = 0; k < dn; k++) {
                    if (uuid16(&d[k].uuid) == 0x2908) r->ref = d[k].handle;
                    else if (uuid16(&d[k].uuid) == 0x2902) r->ccc = d[k].handle;
                }
        }
    }
    if (c->dis_s) {
        n = 24;
        if (esp_ble_gattc_get_all_char(s_gif, c->conn_id, c->dis_s, c->dis_e, ch, &n, 0) == ESP_GATT_OK)
            for (int i = 0; i < n; i++) if (uuid16(&ch[i].uuid) == 0x2A50) c->pnp_h = ch[i].char_handle;
    }
    return c->map_h && c->nrep;
}

/* next set-up step: one GATT request in flight at a time */
static void ble_step(conn_t *c)
{
    for (;;) {
        switch (c->st) {
        case ST_READ_MAP:
            if (esp_ble_gattc_read_char(s_gif, c->conn_id, c->map_h, ESP_GATT_AUTH_REQ_NONE) == ESP_OK) return;
            ble_fail(c, "cannot read the report map");
            return;
        case ST_READ_PNP:
            if (c->pnp_h && esp_ble_gattc_read_char(s_gif, c->conn_id, c->pnp_h, ESP_GATT_AUTH_REQ_NONE) == ESP_OK) return;
            c->st = ST_READ_REF;
            c->step = 0;
            continue;
        case ST_READ_REF:
            while (c->step < c->nrep && !c->rep[c->step].ref) c->step++;
            if (c->step < c->nrep &&
                esp_ble_gattc_read_char_descr(s_gif, c->conn_id, c->rep[c->step].ref, ESP_GATT_AUTH_REQ_NONE) == ESP_OK)
                return;
            c->st = ST_SUBSCRIBE;
            c->step = 0;
            continue;
        case ST_SUBSCRIBE:
            while (c->step < c->nrep) {
                report_t *r = &c->rep[c->step];
                esp_ble_gattc_register_for_notify(s_gif, c->addr, r->val);
                if (r->ccc) {
                    uint8_t on[2] = { 0x01, 0x00 };
                    if (esp_ble_gattc_write_char_descr(s_gif, c->conn_id, r->ccc, 2, on, ESP_GATT_WRITE_TYPE_RSP,
                                                       ESP_GATT_AUTH_REQ_NONE) == ESP_OK)
                        return;                     /* WRITE_DESCR comes back */
                }
                c->step++;
            }
            c->st = ST_READY;
            continue;
        case ST_READY: {
            for (int i = 0; i < c->nrep; i++) if (c->rep[i].id) c->ids = true;
            ESP_LOGI(TAG, "%s: HID over GATT ready, %04X:%04X, map %u bytes, %d input report%s%s", addr_str(c->addr),
                     c->vid, c->pid, c->map_len, c->nrep, c->nrep == 1 ? "" : "s", c->ids ? " with ids" : "");
            bool pairing = s_pairing && s_pair.ble && !memcmp(s_pair.addr, c->addr, 6);
            if (pairing) lx_bt_post(saved_add_job, NULL);
            set_err("");
            post_attach(c, c->vid, c->pid, c->map, c->map_len);
            return;
        }
        default:
            return;
        }
    }
}

static void gattc_cb(esp_gattc_cb_event_t ev, esp_gatt_if_t gif, esp_ble_gattc_cb_param_t *p)
{
    if (ev == ESP_GATTC_REG_EVT) {
        if (p->reg.app_id == GATTC_APP && p->reg.status == ESP_GATT_OK) s_gif = gif;
        return;
    }
    if (gif != s_gif || s_gif == ESP_GATT_IF_NONE) return;
    switch (ev) {
    case ESP_GATTC_OPEN_EVT: {
        const uint8_t *a = p->open.remote_bda;
        bool mine = s_opening && !memcmp(a, s_open_addr, 6);
        if (mine) s_opening = false;
        bool pairing = s_pairing && s_pair.ble && !memcmp(a, s_pair.addr, 6);
        if (p->open.status != ESP_GATT_OK) {
            if (pairing) lx_bt_post(pair_failed_job, "No answer from the controller: is it in pairing mode?");
            else
                ESP_LOGD(TAG, "%s: no answer (0x%x)", addr_str(a), p->open.status);
            break;
        }
        conn_t *c = conn_new(a);
        if (!c) { esp_ble_gap_disconnect((uint8_t *)a); break; }
        c->ble = true;
        c->conn_id = p->open.conn_id;
        c->st = ST_ENCRYPT;
        ESP_LOGI(TAG, "%s: BLE link up (%s), encrypting", addr_str(a), pairing ? "pairing" : "saved pad");
        esp_ble_gattc_send_mtu_req(s_gif, c->conn_id);
        if (pairing) {
            c->enc_asked = true;
            if (esp_ble_set_encryption((uint8_t *)a, ESP_BLE_SEC_ENCRYPT_NO_MITM) != ESP_OK) ble_fail(c, "encryption request refused");
        } else if (s_enc_timer && !esp_timer_is_active(s_enc_timer)) {
            esp_timer_start_once(s_enc_timer, ENC_BOND_MS * 1000);
        }
        break;
    }
    case ESP_GATTC_CANCEL_OPEN_EVT:
        s_opening = false;
        break;
    case ESP_GATTC_SEARCH_RES_EVT: {
        conn_t *c = conn_by_ble(p->search_res.conn_id);
        if (!c) break;
        uint16_t u = uuid16(&p->search_res.srvc_id.uuid);
        if (u == 0x1812 && !c->hid_s) { c->hid_s = p->search_res.start_handle; c->hid_e = p->search_res.end_handle; }
        else if (u == 0x180A && !c->dis_s) { c->dis_s = p->search_res.start_handle; c->dis_e = p->search_res.end_handle; }
        break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
        conn_t *c = conn_by_ble(p->search_cmpl.conn_id);
        if (!c || c->st != ST_DISCOVER) break;
        if (p->search_cmpl.status != ESP_GATT_OK || !c->hid_s) { ble_fail(c, "no HID service: not a controller"); break; }
        if (!ble_collect(c)) { ble_fail(c, "HID service without input reports"); break; }
        c->st = ST_READ_MAP;
        ble_step(c);
        break;
    }
    case ESP_GATTC_READ_CHAR_EVT:
    case ESP_GATTC_READ_DESCR_EVT: {
        conn_t *c = conn_by_ble(p->read.conn_id);
        if (!c) break;
        bool ok = p->read.status == ESP_GATT_OK;
        if (c->st == ST_READ_MAP) {
            if (!ok || !p->read.value_len) { ble_fail(c, "report map not readable"); break; }
            c->map = malloc(p->read.value_len);
            if (!c->map) { ble_fail(c, "no memory"); break; }
            memcpy(c->map, p->read.value, p->read.value_len);
            c->map_len = p->read.value_len;
            c->st = ST_READ_PNP;
        } else if (c->st == ST_READ_PNP) {
            if (ok && p->read.value_len >= 5) {
                c->vid = (uint16_t)(p->read.value[1] | p->read.value[2] << 8);
                c->pid = (uint16_t)(p->read.value[3] | p->read.value[4] << 8);
            }
            c->st = ST_READ_REF;
            c->step = 0;
        } else if (c->st == ST_READ_REF) {
            if (ok && p->read.value_len >= 1 && c->step < c->nrep) c->rep[c->step].id = p->read.value[0];
            c->step++;
        } else {
            break;
        }
        ble_step(c);
        break;
    }
    case ESP_GATTC_WRITE_DESCR_EVT: {
        conn_t *c = conn_by_ble(p->write.conn_id);
        if (!c || c->st != ST_SUBSCRIBE) break;
        c->step++;
        ble_step(c);
        break;
    }
    case ESP_GATTC_NOTIFY_EVT: {
        conn_t *c = conn_by_ble(p->notify.conn_id);
        if (!c || c->st != ST_READY) break;
        uint8_t id = 0;
        for (int i = 0; i < c->nrep; i++) if (c->rep[i].val == p->notify.handle) id = c->rep[i].id;
        uint8_t buf[80];
        size_t n = p->notify.value_len;
        if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
        size_t o = 0;
        if (c->ids) buf[o++] = id;
        memcpy(buf + o, p->notify.value, n);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (c->pad) lx_input_hidpad_report(c->pad, buf, o + n);
        xSemaphoreGive(s_lock);
        break;
    }
    case ESP_GATTC_DISCONNECT_EVT: {
        conn_t *c = conn_by_ble(p->disconnect.conn_id);
        if (!c || memcmp(c->addr, p->disconnect.remote_bda, 6)) break;
        ESP_LOGI(TAG, "%s: disconnected (reason 0x%x)", addr_str(c->addr), p->disconnect.reason);
        bool pairing = s_pairing && s_pair.ble && !memcmp(s_pair.addr, c->addr, 6) && c->st != ST_READY;
        conn_drop(c);
        if (pairing) lx_bt_post(pair_failed_job, "The controller hung up while pairing");
        break;
    }
    default:
        break;
    }
}

static void gap_cb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p)
{
    switch (ev) {
    case ESP_GAP_BLE_SEC_REQ_EVT: {
        /* the pad asked: Bluedroid is already on it, we only grant (asking too would queue a second request) */
        conn_t *c = conn_by_addr(p->ble_security.ble_req.bd_addr);
        if (c && c->ble) c->enc_asked = true;
        if (lx_bt_pads_owns(p->ble_security.ble_req.bd_addr)) esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, true);
        break;
    }
    case ESP_GAP_BLE_NC_REQ_EVT:
        if (lx_bt_pads_owns(p->ble_security.key_notif.bd_addr)) esp_ble_confirm_reply(p->ble_security.key_notif.bd_addr, true);
        break;
    case ESP_GAP_BLE_PASSKEY_REQ_EVT:
        if (lx_bt_pads_owns(p->ble_security.ble_req.bd_addr)) esp_ble_passkey_reply(p->ble_security.ble_req.bd_addr, false, 0);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
        const esp_ble_auth_cmpl_t *a = &p->ble_security.auth_cmpl;
        conn_t *c = conn_by_addr(a->bd_addr);
        if (c && c->ble && c->st != ST_ENCRYPT && !a->success)
            ESP_LOGW(TAG, "%s: a second encryption failed (0x%x)", addr_str(c->addr), a->fail_reason);
        if (!c || !c->ble || c->st != ST_ENCRYPT) break;
        c->enc_asked = true;
        if (!a->success) {
            /* a saved pad that lost our keys (paired elsewhere since): after ENC_FAILS failures in a row
             * the bond goes (Bluedroid keeps it: one timeout, say at start-up, must not unpair a pad) */
            int si = saved_find(c->addr);
            if (si >= 0 && !(s_pairing && !memcmp(s_pair.addr, c->addr, 6)) && ++s_saved[si].enc_fails >= ENC_FAILS) {
                s_saved[si].enc_fails = 0;
                esp_ble_remove_bond_device(c->addr);
                ESP_LOGW(TAG, "%s: encryption failed %d times: keys dropped, it pairs again next time",
                         addr_str(c->addr), ENC_FAILS);
            }
            char why[64];
            snprintf(why, sizeof(why), "encryption failed (0x%x): pair the controller again", a->fail_reason);
            ble_fail(c, s_pairing ? "Pairing refused by the controller" : "encryption failed: pair it again");
            ESP_LOGW(TAG, "%s", why);
            break;
        }
        int si = saved_find(c->addr);
        if (si >= 0) s_saved[si].enc_fails = 0;
        ESP_LOGI(TAG, "%s: encrypted%s after %d ms, reading its HID service", addr_str(c->addr),
                 a->key_present ? " (new keys)" : "", (int)((esp_timer_get_time() - c->t0) / 1000));
        c->st = ST_DISCOVER;
        if (esp_ble_gattc_search_service(s_gif, c->conn_id, NULL) != ESP_OK) ble_fail(c, "service search refused");
        break;
    }
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        if (conn_by_addr(p->update_conn_params.bda))
            ESP_LOGI(TAG, "%s: interval %.2f ms, latency %u", addr_str(p->update_conn_params.bda),
                     p->update_conn_params.conn_int * 1.25, p->update_conn_params.latency);
        break;
    default:
        break;
    }
}

/*
 * Encryption of a saved pad's new link (lx_bt task, ENC_BOND_MS after it came up). Most pads ask for
 * it themselves (security request) as soon as they are connected, and Bluedroid then encrypts a
 * bonded link on its own (no event for us until AUTH_CMPL). Asking as well while that runs gets our
 * request queued ("BTM_SetEncryption busy"), and seconds later it runs on the encrypted link, fails
 * ("No key available") and Bluedroid deletes the bond. So for a saved pad we ask only when neither
 * it nor Bluedroid has started. A pad being paired: we ask at once (ESP_GATTC_OPEN_EVT), its own
 * request is then only granted.
 */
static void enc_job(void *a)
{
    (void)a;
    int64_t now = esp_timer_get_time();
    bool young = false;
    for (int i = 0; i < LX_BT_MAX_PADS; i++) {
        conn_t *c = &s_conn[i];
        if (!c->used || !c->ble || c->st != ST_ENCRYPT || c->enc_asked) continue;
        if (now - c->t0 < (ENC_BOND_MS - 50) * 1000LL) { young = true; continue; }
        c->enc_asked = true;
        ESP_LOGI(TAG, "%s: asking for encryption", addr_str(c->addr));
        if (esp_ble_set_encryption(c->addr, ESP_BLE_SEC_ENCRYPT_NO_MITM) != ESP_OK) ble_fail(c, "encryption request refused");
    }
    if (young && s_enc_timer) esp_timer_start_once(s_enc_timer, 300 * 1000);
}

static void enc_tick(void *a) { (void)a; lx_bt_post(enc_job, NULL); }

/* LE connection request (lx_bt task). fast: pairing (the user waits); else 20 ms of every 160 */
static bool ble_open(const uint8_t *addr, uint8_t atype, bool fast)
{
    if (s_gif == ESP_GATT_IF_NONE || s_opening) return false;
    static const esp_ble_conn_params_t k_slow = {
        .scan_interval = 0x100, .scan_window = 0x20,          /* 160 / 20 ms */
        .interval_min = 9, .interval_max = 12, .latency = 0,  /* 11.25..15 ms */
        .supervision_timeout = 300,
    };
    static const esp_ble_conn_params_t k_fast = {
        .scan_interval = 0x30, .scan_window = 0x30,
        .interval_min = 9, .interval_max = 12, .latency = 0, .supervision_timeout = 300,
    };
    esp_ble_gatt_creat_conn_params_t cp = {
        .remote_addr_type = (esp_ble_addr_type_t)atype, .is_direct = true, .is_aux = false,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC, .phy_mask = ESP_BLE_PHY_1M_PREF_MASK,
        .phy_1m_conn_params = fast ? &k_fast : &k_slow,
    };
    memcpy(cp.remote_bda, addr, 6);
    memcpy(s_open_addr, addr, 6);
    s_opening = true;
    s_open_t = esp_timer_get_time();
    if (esp_ble_gattc_enh_open(s_gif, &cp) != ESP_OK) {
        s_opening = false;
        return false;
    }
    return true;
}

static void ble_cancel(void)
{
    if (!s_opening || s_gif == ESP_GATT_IF_NONE) return;
    esp_ble_gattc_cancel_open_params_t c = { .gattc_if = s_gif };
    memcpy(c.remote_bda, s_open_addr, 6);
    esp_ble_gattc_cancel_open(&c);
    for (int i = 0; i < 20 && s_opening; i++) vTaskDelay(pdMS_TO_TICKS(50));
    s_opening = false;
}

/* a saved BLE pad that is not connected: ask for it (lx_bt task) */
static void ble_reconnect(void)
{
    if (!s_held || !lx_bt_classic() || s_gif == ESP_GATT_IF_NONE || s_opening || s_pairing || s_app ||
        lx_bt_speaker_scanning() || lx_bt_speaker_paging())
        return;
    int n = s_nsaved;
    for (int k = 0; k < n; k++) {
        int i = (s_next_ble + k) % n;
        if (!s_saved[i].ble || conn_by_addr(s_saved[i].addr)) continue;
        s_next_ble = i + 1;
        ble_open(s_saved[i].addr, s_saved[i].atype, false);
        return;
    }
}

static void ble_sec_params(void)
{
    esp_ble_auth_req_t auth = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t io = ESP_IO_CAP_NONE;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK, ksize = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &ksize, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, 1);
}

/* ── Stack hooks (lx_bt task) ───────────────────────────────────────────── */

void lx_bt_pads_stack_up(void)
{
    s_hh_up = false;
    esp_bt_hid_host_register_callback(hh_cb);
    esp_err_t e = esp_bt_hid_host_init();
    if (e != ESP_OK) ESP_LOGE(TAG, "HID host init: %s", esp_err_to_name(e));
    s_gif = ESP_GATT_IF_NONE;
    s_opening = false;
    ble_sec_params();
    e = esp_ble_gattc_app_register(GATTC_APP);
    if (e != ESP_OK) ESP_LOGE(TAG, "GATT client register: %s", esp_err_to_name(e));
}

void lx_bt_pads_stack_down(void)
{
    for (int i = 0; i < LX_BT_MAX_PADS; i++) {
        if (!s_conn[i].used) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        lx_hidpad_t *p = s_conn[i].pad;
        s_conn[i].pad = NULL;
        s_conn[i].used = false;
        xSemaphoreGive(s_lock);
        free(s_conn[i].map);
        s_conn[i].map = NULL;
        lx_input_hidpad_close(p);
    }
    if (s_hh_up) esp_bt_hid_host_deinit();
    s_hh_up = false;
    s_gif = ESP_GATT_IF_NONE;
    s_opening = false;
    s_held = false;
    pairing_end();
}

void lx_bt_pads_dongle(bool present)
{
    if (present && want_stack()) lx_bt_post(hold_job, NULL);
}

void lx_bt_pads_cancel_open(void) { ble_cancel(); }

void lx_bt_pads_app(bool active)
{
    s_app = active;
    if (active) ble_cancel();               /* the app's own connects go first */
    else if (lx_bt_classic()) ble_sec_params();
}

static void timer_job(void *a)
{
    (void)a;
    if (s_pairing && xTaskGetTickCount() - s_pair_t > pdMS_TO_TICKS(PAIR_MS)) {
        pairing_end();
        if (s_pair.ble) ble_cancel();
        set_err("No answer from the controller: is it in pairing mode?");
        let_go();
    }
    hold_job(NULL);
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < LX_BT_MAX_PADS; i++) {
        conn_t *c = &s_conn[i];
        if (c->used && c->ble && c->st != ST_READY && now - c->t0 > SETUP_MS * 1000LL) {
            ESP_LOGW(TAG, "%s: set-up stuck (step %d): disconnected", addr_str(c->addr), c->st);
            esp_ble_gap_disconnect(c->addr);
        }
    }
    if (s_opening && now - s_open_t > 40 * 1000000LL) ble_cancel();     /* Bluedroid gives up at 30 s */
    ble_reconnect();
}

static void tick(TimerHandle_t t)
{
    (void)t;
    if (want_stack() && lx_bt_usb_present()) lx_bt_post(timer_job, NULL);
}

void lx_bt_pads_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    cfg_load();
    if (s_nsaved) ESP_LOGI(TAG, "%d saved pad%s (%d BLE)", s_nsaved, s_nsaved == 1 ? "" : "s", nsaved_ble());
    lx_bt_ble_handlers(LX_BT_BLE_PADS, gap_cb, gattc_cb);
    const esp_timer_create_args_t ta = { .callback = enc_tick, .name = "bt_pad_enc" };
    esp_timer_create(&ta, &s_enc_timer);
    s_timer = xTimerCreate("bt_pads", pdMS_TO_TICKS(TICK_MS), pdTRUE, NULL, tick);
    if (s_timer) xTimerStart(s_timer, 0);
}

/* ── API (UI task) ──────────────────────────────────────────────────────── */

static void pair_job(void *a)
{
    (void)a;
    lx_bt_speaker_pause(true);                  /* undone by pairing_end */
    hold_job(NULL);
    if (!s_held && !lx_bt_classic()) {
        pairing_end();
        return;                                 /* hold_job said why */
    }
    for (int i = 0; i < 50 && (s_pair.ble ? s_gif == ESP_GATT_IF_NONE : !s_hh_up); i++)
        vTaskDelay(pdMS_TO_TICKS(20));          /* just starting */
    esp_bt_gap_cancel_discovery();
    lx_bt_speaker_wait_page();
    bool ok;
    if (s_pair.ble) {
        ble_cancel();                           /* a saved pad's request: this one first */
        if (conn_by_addr(s_pair.addr)) esp_ble_gap_disconnect(s_pair.addr);
        esp_ble_remove_bond_device(s_pair.addr);    /* fresh keys */
        ok = ble_open(s_pair.addr, s_pair.addr_type, true);
    } else {
        ok = esp_bt_hid_host_connect(s_pair.addr) == ESP_OK;
    }
    s_pair_t = xTaskGetTickCount();             /* the time limit counts from the request, not the waits above */
    if (!ok) {
        pairing_end();
        set_err("connect request failed");
        let_go();
    }
}

void lx_bt_pads_pair(const lx_bt_found_t *f)
{
    if (s_pairing || !f) return;
    s_pair = *f;
    if (!s_pair.name[0]) snprintf(s_pair.name, sizeof(s_pair.name), "%s", addr_str(f->addr));
    set_err("");
    s_pair_t = xTaskGetTickCount();
    s_pairing = true;
    lx_bt_post(pair_job, NULL);
}

bool lx_bt_pads_pair_saved(const uint8_t addr[6])
{
    int i = saved_find(addr);
    if (i < 0) return false;
    lx_bt_found_t f = { .hid = true, .ble = s_saved[i].ble, .addr_type = s_saved[i].atype };
    memcpy(f.addr, addr, 6);
    snprintf(f.name, sizeof(f.name), "%s", s_saved[i].name);
    lx_bt_pads_pair(&f);
    return true;
}

bool lx_bt_pads_pairing(void) { return s_pairing; }

static void forget_job(void *a)
{
    (void)a;
    bool up = lx_bt_running() && lx_bt_classic();
    if (up) ble_cancel();
    for (int i = 0; i < s_nsaved; i++) {
        if (!up) continue;
        if (s_saved[i].ble) {
            esp_ble_gap_disconnect(s_saved[i].addr);
            esp_ble_remove_bond_device(s_saved[i].addr);
        } else {
            esp_bt_hid_host_virtual_cable_unplug(s_saved[i].addr);     /* disconnects, drops the HID record */
            esp_bt_gap_remove_bond_device(s_saved[i].addr);
        }
    }
    s_nsaved = 0;
    cfg_save();
    let_go();
}

void lx_bt_pads_forget_all(void) { lx_bt_post(forget_job, NULL); }

int lx_bt_pads_list(lx_bt_pad_t *out, int max)
{
    int n = s_nsaved < max ? s_nsaved : max;
    for (int i = 0; i < n; i++) {
        memcpy(out[i].addr, s_saved[i].addr, 6);
        snprintf(out[i].name, sizeof(out[i].name), "%s", s_saved[i].name);
        out[i].ble = s_saved[i].ble;
        conn_t *c = conn_by_addr(s_saved[i].addr);
        out[i].connected = c && (!c->ble || c->st == ST_READY);
    }
    return n;
}

const char *lx_bt_pads_error(void) { return s_err; }

#elif CONFIG_BT_BLUEDROID_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID   /* Bluedroid without HID host */

#include "lx_bt_priv.h"
void lx_bt_pads_init(void) {}
void lx_bt_pads_stack_up(void) {}
void lx_bt_pads_stack_down(void) {}
void lx_bt_pads_dongle(bool present) { (void)present; }
void lx_bt_pads_app(bool active) { (void)active; }
void lx_bt_pads_cancel_open(void) {}
bool lx_bt_pads_owns(const uint8_t *bda) { (void)bda; return false; }
bool lx_bt_pads_stats(const uint8_t *addr, lx_hidpad_stats_t *out) { (void)addr; (void)out; return false; }
uint32_t lx_bt_pads_still_ms(void) { return UINT32_MAX; }
int         lx_bt_pads_list(lx_bt_pad_t *out, int max) { (void)out; (void)max; return 0; }
int         lx_bt_pads_linked(void) { return 0; }
void        lx_bt_pads_pair(const lx_bt_found_t *f) { (void)f; }
bool        lx_bt_pads_pair_saved(const uint8_t addr[6]) { (void)addr; return false; }
bool        lx_bt_pads_pairing(void) { return false; }
void        lx_bt_pads_forget_all(void) {}
const char *lx_bt_pads_error(void) { return ""; }

#endif
