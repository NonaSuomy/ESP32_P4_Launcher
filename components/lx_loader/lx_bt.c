/*
 * Bluetooth core: see lx_bt.h. Owns Bluedroid's start and stop and the choice of controller
 * (USB dongle in lx_bt_usb.c, or the C6 here). Every start and stop runs on the "lx_bt" task.
 */
#include "sdkconfig.h"
#include "lx_bt.h"

#if CONFIG_BT_BLUEDROID_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lx_bt_priv.h"
#include "lx_input.h"
#include "esp_attr.h"
#include "esp_gap_bt_api.h"
#include "esp_bt_device.h"
#include "lx_net.h"
#include "lx_c6.h"
#include "esp_log.h"
#include "esp_hosted_misc.h"
#include "esp_hosted_bluedroid.h"
#include "esp_bt_main.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "lx_bt";

static QueueHandle_t s_jobq;
static TaskHandle_t  s_task;
static volatile int  s_users;
static volatile bool s_running, s_classic;
static volatile uint32_t s_gen;
static bool s_c6_failed, s_ctrl_init;
static int  s_afh_chan = -1;                    /* Wi-Fi channel the hop sets avoid now (0: none) */

/* ── BLE event fan-out ──────────────────────────────────────────────────── */

static esp_gap_ble_cb_t s_gap[LX_BT_BLE_N];
static esp_gattc_cb_t   s_gattc[LX_BT_BLE_N];

void lx_bt_ble_handlers(int slot, esp_gap_ble_cb_t gap, esp_gattc_cb_t gattc)
{
    if (slot < 0 || slot >= LX_BT_BLE_N) return;
    s_gap[slot] = gap;
    s_gattc[slot] = gattc;
}

static void gap_ble_all(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p)
{
    for (int i = 0; i < LX_BT_BLE_N; i++) if (s_gap[i]) s_gap[i](ev, p);
}

static void gattc_all(esp_gattc_cb_event_t ev, esp_gatt_if_t gif, esp_ble_gattc_cb_param_t *p)
{
    for (int i = 0; i < LX_BT_BLE_N; i++) if (s_gattc[i]) s_gattc[i](ev, gif, p);
}

/* ── Wi-Fi channel kept out of the hop sets ─────────────────────────────── *
 * The C6's Wi-Fi and the dongle share 2.4 GHz a few cm apart: Classic (AFH) and BLE (data channel
 * map) are told to leave the 22 MHz of the Wi-Fi channel alone. */
static void afh_apply(void)
{
    if (!s_running || !s_classic) return;
    int ch = lx_net_channel();      /* not lx_net_info: that asks the C6, seconds while Wi-Fi joins */
    if (ch < 1 || ch > 13) ch = 0;
    if (ch == s_afh_chan) return;
    s_afh_chan = ch;
    int lo = 0, hi = -1;                        /* MHz above 2400 to avoid */
    if (ch) { int f = 12 + 5 * (ch - 1); lo = f - 11; hi = f + 11; }
    esp_bt_gap_afh_channels bt;                 /* channel k = 2402 + k MHz, bit k */
    memset(bt, 0, sizeof(bt));
    for (int k = 0; k < 79; k++) if (2 + k < lo || 2 + k > hi) bt[k / 8] |= 1 << (k % 8);
    esp_gap_ble_channels le;                    /* data channel i: 2404 + 2i (i < 11), 2428 + 2(i - 11) */
    memset(le, 0, sizeof(le));
    for (int i = 0; i < 37; i++) {
        int f = i < 11 ? 4 + 2 * i : 28 + 2 * (i - 11);
        if (f < lo || f > hi) le[i / 8] |= 1 << (i % 8);
    }
    esp_err_t e1 = esp_bt_gap_set_afh_channels(bt), e2 = esp_gap_ble_set_channels(le);
    if (ch) ESP_LOGI(TAG, "Wi-Fi on channel %d: Bluetooth hops stay off %d..%d MHz (%s, %s)", ch,
                     2400 + lo, 2400 + hi, esp_err_to_name(e1), esp_err_to_name(e2));
    else ESP_LOGI(TAG, "Bluetooth hops on all channels (%s, %s)", esp_err_to_name(e1), esp_err_to_name(e2));
}

static void afh_job(void *a) { (void)a; afh_apply(); }
static void afh_tick(TimerHandle_t t) { (void)t; if (s_running && s_classic) lx_bt_post(afh_job, NULL); }

/* ── Jobs on the lx_bt task ─────────────────────────────────────────────── */

typedef struct {
    int  (*fn)(void *);
    void (*post)(void *);
    void *arg;
    int  *ret;
    SemaphoreHandle_t done;
} job_t;

static void bt_task(void *a)
{
    job_t j;
    for (;;) {
        if (!xQueueReceive(s_jobq, &j, portMAX_DELAY)) continue;
        if (j.fn) {
            *j.ret = j.fn(j.arg);
            xSemaphoreGive(j.done);
        } else if (j.post) {
            j.post(j.arg);
        }
    }
}

int lx_bt_run(int (*fn)(void *), void *arg)
{
    if (!s_jobq) return -1;
    if (xTaskGetCurrentTaskHandle() == s_task) return fn(arg);
    int ret = -1;
    job_t j = { .fn = fn, .arg = arg, .ret = &ret, .done = xSemaphoreCreateBinary() };
    if (!j.done) return -1;
    xQueueSend(s_jobq, &j, portMAX_DELAY);
    xSemaphoreTake(j.done, portMAX_DELAY);
    vSemaphoreDelete(j.done);
    return ret;
}

void lx_bt_post(void (*fn)(void *), void *arg)
{
    job_t j = { .post = fn, .arg = arg };
    if (s_jobq) xQueueSend(s_jobq, &j, pdMS_TO_TICKS(100));
}

/* ── C6 controller ──────────────────────────────────────────────────────── */

/* A C6 still on 1.x firmware (a Tab5 as it ships) starts its BLE controller at boot, keeps it on and
 * does not know the 2.x call that switches it (#48): left alone */
static bool c6_switches_bt(void)
{
    const char *v = lx_c6_version();
    return !((v[0] == '0' || v[0] == '1') && v[1] == '.');
}

/* Bluedroid built with Classic asks every controller for two Classic answers at start-up and
 * asserts on a short reply. The C6 is LE only and answers both "unknown command" (status 1, no
 * parameters). Those two replies are replaced on the way up: no Classic buffers, no Classic
 * features (page 0 all clear), so Bluedroid runs LE only on the C6. Everything else passes. */
#define HCI_EVT_PKT        0x04
#define HCI_CMD_COMPLETE   0x0E
#define OP_READ_EXT_FEAT   0x1004
#define OP_READ_BUF_SIZE   0x1005

static const esp_bluedroid_hci_driver_callbacks_t *s_host_cb;
static volatile uint8_t s_feat_page;                /* the page of the last Read Local Extended Features */

static void c6_send(uint8_t *d, uint16_t len)
{
    if (len >= 5 && d[0] == 0x01 && d[1] == (OP_READ_EXT_FEAT & 0xFF) && d[2] == (OP_READ_EXT_FEAT >> 8))
        s_feat_page = d[4];
    hosted_hci_bluedroid_send(d, len);
}

static void c6_send_available(void) { if (s_host_cb && s_host_cb->notify_host_send_available) s_host_cb->notify_host_send_available(); }

static int c6_recv(uint8_t *d, uint16_t len)
{
    if (!s_host_cb) return 0;
    if (len >= 7 && d[0] == HCI_EVT_PKT && d[1] == HCI_CMD_COMPLETE) {
        uint16_t op = (uint16_t)(d[4] | d[5] << 8);
        if (op == OP_READ_BUF_SIZE && d[6] != 0) {
            /* status 0, ACL 251 bytes, SCO 0, 1 ACL buffer, 0 SCO */
            static uint8_t r[] = { HCI_EVT_PKT, HCI_CMD_COMPLETE, 11, 1, 0x05, 0x10, 0, 251, 0, 0, 1, 0, 0, 0 };
            r[3] = d[3];
            return s_host_cb->notify_host_recv(r, sizeof(r));
        }
        if (op == OP_READ_EXT_FEAT) {
            /* status 0, page n of 1: page 0 = LE supported, BR/EDR not (byte 4: 0x40 | 0x20);
             * page 1 = LE host supported (byte 0: 0x02), which Bluedroid needs to turn LE on */
            static uint8_t r[] = { HCI_EVT_PKT, HCI_CMD_COMPLETE, 14, 1, 0x04, 0x10, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
            r[3] = d[3];
            r[7] = s_feat_page;
            memset(&r[9], 0, 8);
            if (s_feat_page == 0) r[9 + 4] = 0x60;
            else if (s_feat_page == 1) r[9] = 0x02;
            return s_host_cb->notify_host_recv(r, sizeof(r));
        }
    }
    return s_host_cb->notify_host_recv(d, len);
}

static const esp_bluedroid_hci_driver_callbacks_t k_c6_cb = {
    .notify_host_send_available = c6_send_available,
    .notify_host_recv = c6_recv,
};

static esp_err_t c6_register_cb(const esp_bluedroid_hci_driver_callbacks_t *cb)
{
    s_host_cb = cb;
    return hosted_hci_bluedroid_register_host_callback(&k_c6_cb);
}

static const esp_bluedroid_hci_driver_operations_t k_c6_ops = {
    .send = c6_send,
    .check_send_available = hosted_hci_bluedroid_check_send_available,
    .register_host_callback = c6_register_cb,
};

/* The C6's controller on, its HCI open; false: no BLE on this C6 */
static bool c6_up(volatile bool *abort)
{
    if (s_c6_failed) return false;
    if (lx_net_radio_up(abort) != ESP_OK) return false;
    if (!s_ctrl_init && c6_switches_bt()) {
        esp_err_t e = esp_hosted_bt_controller_init();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "C6 BT controller init: %s (C6 firmware without BT?)", esp_err_to_name(e));
            s_c6_failed = true;
            return false;
        }
    }
    s_ctrl_init = true;
    if (c6_switches_bt()) {
        esp_err_t e = esp_hosted_bt_controller_enable();
        if (e != ESP_OK) ESP_LOGW(TAG, "C6 BT controller enable: %s", esp_err_to_name(e));   /* may be on already */
    }
    hosted_hci_bluedroid_open();
    return true;
}

static void c6_down(void)
{
    hosted_hci_bluedroid_close();
    if (c6_switches_bt()) esp_hosted_bt_controller_disable();
}

/* ── Start / stop ───────────────────────────────────────────────────────── */

typedef struct {
    int user;
    bool need_classic;
    volatile bool *abort;
} acq_t;

static int acquire_job(void *a)
{
    acq_t *q = a;
    if (s_running) {
        if (q->need_classic && !s_classic) return 0;
        s_users |= q->user;
        if (q->user == LX_BT_USER_APP) lx_bt_pads_app(true);
        return 1;
    }
    bool dongle = lx_bt_usb_present();
    if (q->need_classic && !dongle) return 0;
    if (dongle && !lx_bt_usb_open()) {
        lx_bt_usb_close();
        if (q->need_classic) return 0;
        dongle = false;                 /* an unusable dongle: BLE runs on the C6 */
    }
    if (!dongle && !c6_up(q->abort)) return 0;
    esp_err_t e = esp_bluedroid_attach_hci_driver(dongle ? lx_bt_usb_ops() : &k_c6_ops);
    if (e == ESP_OK) e = esp_bluedroid_init();
    if (e == ESP_OK) {
        e = esp_bluedroid_enable();
        if (e != ESP_OK) esp_bluedroid_deinit();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid start on %s: %s", dongle ? lx_bt_usb_name() : "the C6", esp_err_to_name(e));
        if (dongle) lx_bt_usb_close();
        else {
            c6_down();
            s_c6_failed = true;         /* not again until the next app */
        }
        return 0;
    }
    s_classic = dongle;
    s_gen++;
    s_running = true;
    s_users |= q->user;
    esp_ble_gap_register_callback(gap_ble_all);
    esp_ble_gattc_register_callback(gattc_all);
    /* A bonded BLE pad sends reports as soon as the link is encrypted (its CCC is kept), before we
     * have registered for them again: Bluedroid warns for each one ("drop notif"), 60+ lines a
     * second per pad, which blocks its task on the console and stalls the pads' set-up. Same for
     * reports that arrive while the ATT channel is still opening ("Ignored L2CAP data"). */
    esp_log_level_set("BT_APPL", ESP_LOG_ERROR);
    esp_log_level_set("BT_GATT", ESP_LOG_ERROR);
    const uint8_t *ba = esp_bt_dev_get_address();
    ESP_LOGI(TAG, "Bluedroid up on %s, address %02X:%02X:%02X:%02X:%02X:%02X",
             dongle ? lx_bt_usb_name() : "the C6 (BLE only)",
             ba ? ba[0] : 0, ba ? ba[1] : 0, ba ? ba[2] : 0, ba ? ba[3] : 0, ba ? ba[4] : 0, ba ? ba[5] : 0);
    if (dongle) {
        s_afh_chan = -1;
        afh_apply();
        lx_bt_speaker_stack_up();
        lx_bt_pads_stack_up();
    }
    if (q->user == LX_BT_USER_APP) lx_bt_pads_app(true);
    return 1;
}

static void stop_stack(void)
{
    if (!s_running) return;
    lx_bt_ble_stack_down();
    if (s_classic) {
        lx_bt_pads_stack_down();
        lx_bt_speaker_stack_down();
    }
    esp_err_t e = esp_bluedroid_disable();
    if (e == ESP_OK) e = esp_bluedroid_deinit();
    if (e != ESP_OK) ESP_LOGW(TAG, "Bluedroid shutdown: %s", esp_err_to_name(e));
    if (s_classic) lx_bt_usb_close();
    else c6_down();
    s_host_cb = NULL;
    s_running = false;
    s_classic = false;
    s_users = 0;
    ESP_LOGI(TAG, "Bluedroid stopped");
}

static int release_job(void *a)
{
    int user = (int)(intptr_t)a;
    s_users &= ~user;
    if (user == LX_BT_USER_APP) {
        s_c6_failed = false;                            /* the next app may try again */
        if (s_users) lx_bt_pads_app(false);
    }
    if (!s_users) stop_stack();
    return 0;
}

static void lost_job(void *a)
{
    (void)a;
    if (!s_running || !s_classic) return;
    ESP_LOGW(TAG, "the Bluetooth dongle went away: stack stopped");
    stop_stack();
}

bool lx_bt_acquire(int user, bool need_classic, volatile bool *abort)
{
    acq_t q = { user, need_classic, abort };
    return lx_bt_run(acquire_job, &q) == 1;
}

void lx_bt_release(int user)
{
    if (s_users & user) lx_bt_run(release_job, (void *)(intptr_t)user);
    else if (user == LX_BT_USER_APP) s_c6_failed = false;
}

void lx_bt_transport_lost(void) { lx_bt_post(lost_job, NULL); }

bool     lx_bt_app_holds(void)     { return s_running && (s_users & LX_BT_USER_APP); }
bool     lx_bt_supported(void)     { return true; }
bool     lx_bt_running(void)       { return s_running; }
bool     lx_bt_classic(void)       { return s_running && s_classic; }
uint32_t lx_bt_generation(void)    { return s_gen; }
bool     lx_bt_dongle_present(void) { return lx_bt_usb_present(); }
const char *lx_bt_dongle_name(void) { return lx_bt_usb_present() ? lx_bt_usb_name() : ""; }

void lx_bt_init(void)
{
    if (s_jobq) return;
    s_jobq = xQueueCreate(8, sizeof(job_t));
    if (!s_jobq || xTaskCreatePinnedToCore(bt_task, "lx_bt", 6144, NULL, 5, &s_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "no lx_bt task");
        return;
    }
    lx_bt_speaker_init();
    lx_bt_pads_init();
    lx_bt_usb_init();
    TimerHandle_t t = xTimerCreate("bt_afh", pdMS_TO_TICKS(10000), pdTRUE, NULL, afh_tick);
    if (t) xTimerStart(t, 0);
    lx_bt_testpad_init();
}

/* ── DevLink "bt" (test aid) ────────────────────────────────────────────── */

static EXT_RAM_BSS_ATTR char s_dl[3072];
static EXT_RAM_BSS_ATTR lx_bt_found_t s_dl_found[LX_BT_MAX_FOUND];
static int s_dl_nfound;

static bool parse_addr(const char *t, uint8_t *a)
{
    unsigned v[6];
    if (sscanf(t, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
    for (int i = 0; i < 6; i++) a[i] = (uint8_t)v[i];
    return true;
}

static void json_str(char **o, char *end, const char *v)
{
    if (*o < end) *(*o)++ = '"';
    for (; *v && *o < end - 2; v++) {
        if (*v == '"' || *v == '\\') *(*o)++ = '\\';
        *(*o)++ = (unsigned char)*v < 32 ? ' ' : *v;
    }
    if (*o < end) *(*o)++ = '"';
    **o = 0;
}

#define OUT(...) do { if (o < end) o += snprintf(o, end - o, __VA_ARGS__); if (o > end) o = end; } while (0)

static const char *dl_status(void)
{
    static const char *const k_spk[] = { "off", "no dongle", "connecting", "connected", "playing", "retry" };
    char *o = s_dl, *end = s_dl + sizeof(s_dl) - 1;
    const uint8_t *ba = s_running ? esp_bt_dev_get_address() : NULL;
    OUT("{\"dongle\": ");
    json_str(&o, end, lx_bt_dongle_name());
    OUT(", \"running\": %d, \"classic\": %d, \"users\": %d, \"addr\": \"", s_running, s_classic, s_users);
    if (ba) OUT("%02X:%02X:%02X:%02X:%02X:%02X", ba[0], ba[1], ba[2], ba[3], ba[4], ba[5]);
    char un[96];
    int unacked = lx_bt_usb_present() ? lx_bt_usb_unacked(un, sizeof(un)) : 0;
    OUT("\", \"wifi_channel_avoided\": %d, \"acl_unacked\": %d, \"acl_links\": \"%s\"",
        s_afh_chan > 0 ? s_afh_chan : 0, unacked, unacked ? un : "");
    uint32_t rx, skipped;
    if (lx_bt_usb_serial_stats(&rx, &skipped))          /* ESP32 dongle: bytes in, bytes skipped resyncing */
        OUT(", \"serial_rx\": %lu, \"serial_skipped\": %lu", (unsigned long)rx, (unsigned long)skipped);
    OUT(",\n \"speaker\": {\"name\": ");
    json_str(&o, end, lx_bt_speaker_name());
    lx_bt_spk_state_t st = lx_bt_speaker_state();
    OUT(", \"state\": \"%s\", \"on\": %d, \"frames_taken\": %lu, \"acl_sent\": %lu, \"error\": ",
        st <= LX_BT_SPK_RETRY ? k_spk[st] : "?", lx_bt_speaker_enabled(),
        (unsigned long)lx_bt_speaker_taken(), (unsigned long)(lx_bt_usb_present() ? lx_bt_usb_sent() : 0));
    json_str(&o, end, lx_bt_speaker_error());
    OUT("},\n \"pairing\": %d, \"pads_error\": ", lx_bt_pads_pairing());
    json_str(&o, end, lx_bt_pads_error());
    OUT(", \"pads\": [");
    lx_bt_pad_t pads[LX_BT_MAX_PADS];
    int n = lx_bt_pads_list(pads, LX_BT_MAX_PADS);
    for (int i = 0; i < n; i++) {
        const uint8_t *a = pads[i].addr;
        OUT("%s\n  {\"addr\": \"%02X:%02X:%02X:%02X:%02X:%02X\", \"name\": ", i ? "," : "", a[0], a[1], a[2], a[3], a[4], a[5]);
        json_str(&o, end, pads[i].name);
        OUT(", \"ble\": %d, \"connected\": %d", pads[i].ble, pads[i].connected);
        lx_hidpad_stats_t ps;
        if (pads[i].connected && lx_bt_pads_stats(a, &ps))
            OUT(", \"player\": %d, \"reports\": %u, \"ms\": %u, \"gap_max_ms\": %u, \"stalls\": %u, \"stalls_in_use\": %u, \"idle_ms\": %u, \"still_ms\": %u",
                ps.player + 1, (unsigned)ps.reports, (unsigned)ps.ms, (unsigned)ps.gap_max_ms,
                (unsigned)ps.stalls, (unsigned)ps.stalls_in_use, (unsigned)ps.idle_ms, (unsigned)ps.still_ms);
        OUT("}");
    }
    OUT("]}");
    return s_dl;
}

static const char *dl_results(void)
{
    char *o = s_dl, *end = s_dl + sizeof(s_dl) - 1;
    int n = lx_bt_speaker_results(s_dl_found, LX_BT_MAX_FOUND);
    if (n < 0) return "scanning";
    s_dl_nfound = n;
    OUT("[");
    for (int i = 0; i < n; i++) {
        const lx_bt_found_t *f = &s_dl_found[i];
        OUT("%s\n {\"addr\": \"%02X:%02X:%02X:%02X:%02X:%02X\", \"name\": ", i ? "," : "",
            f->addr[0], f->addr[1], f->addr[2], f->addr[3], f->addr[4], f->addr[5]);
        json_str(&o, end, f->name);
        OUT(", \"rssi\": %d, \"audio\": %d, \"hid\": %d, \"ble\": %d}", f->rssi, f->audio, f->hid, f->ble);
    }
    OUT("]");
    return s_dl;
}

const char *lx_bt_devlink(const char *cmd)
{
    uint8_t a[6];
    if (!cmd || !cmd[0] || !strcmp(cmd, "status")) return dl_status();
    if (!strcmp(cmd, "scan")) return lx_bt_speaker_scan() ? "scanning" : "no dongle";
    if (!strcmp(cmd, "results")) return dl_results();
    if (!strncmp(cmd, "pair ", 5) && parse_addr(cmd + 5, a)) {
        for (int i = 0; i < s_dl_nfound; i++) {
            const lx_bt_found_t *f = &s_dl_found[i];
            if (memcmp(f->addr, a, 6)) continue;
            if (f->hid) { lx_bt_pads_pair(f); return "pairing a controller"; }
            lx_bt_speaker_pick(f->addr, f->name);
            return "speaker picked";
        }
        if (lx_bt_pads_pair_saved(a)) return "pairing a saved controller again";
        return "not in the last results (scan, then results)";
    }
    if (!strcmp(cmd, "speaker on")) { lx_bt_speaker_enable(true); return "ok"; }
    if (!strcmp(cmd, "speaker off")) { lx_bt_speaker_enable(false); return "ok"; }
    if (!strncmp(cmd, "speaker ", 8) && parse_addr(cmd + 8, a)) {
        const char *name = strlen(cmd) > 25 ? cmd + 26 : "";
        lx_bt_speaker_pick(a, name);
        return "speaker picked";
    }
    if (!strcmp(cmd, "forget-speaker")) { lx_bt_speaker_forget(); return "ok"; }
    if (!strcmp(cmd, "forget-pads")) { lx_bt_pads_forget_all(); return "ok"; }
    return "commands: status | scan | results | pair <addr> | speaker <addr> [name] | speaker on|off | "
           "forget-speaker | forget-pads";
}

#else   /* no Bluedroid: no dongle, no speaker */

void        lx_bt_init(void) {}
bool        lx_bt_supported(void) { return false; }
bool        lx_bt_acquire(int user, bool need_classic, volatile bool *abort) { (void)user; (void)need_classic; (void)abort; return false; }
void        lx_bt_release(int user) { (void)user; }
bool        lx_bt_running(void) { return false; }
bool        lx_bt_classic(void) { return false; }
uint32_t    lx_bt_generation(void) { return 0; }
bool        lx_bt_dongle_present(void) { return false; }
const char *lx_bt_dongle_name(void) { return ""; }
lx_bt_spk_state_t lx_bt_speaker_state(void) { return LX_BT_SPK_OFF; }
const char *lx_bt_speaker_name(void) { return ""; }
bool        lx_bt_speaker_scan(void) { return false; }
int         lx_bt_speaker_results(lx_bt_found_t *out, int max) { (void)out; (void)max; return 0; }
void        lx_bt_speaker_pick(const uint8_t addr[6], const char *name) { (void)addr; (void)name; }
void        lx_bt_speaker_enable(bool on) { (void)on; }
bool        lx_bt_speaker_enabled(void) { return false; }
void        lx_bt_speaker_forget(void) {}
const char *lx_bt_speaker_error(void) { return ""; }
int         lx_bt_pads_list(lx_bt_pad_t *out, int max) { (void)out; (void)max; return 0; }
int         lx_bt_pads_linked(void) { return 0; }
void        lx_bt_pads_pair(const lx_bt_found_t *f) { (void)f; }
const char *lx_bt_devlink(const char *cmd) { (void)cmd; return "no Bluetooth in this build"; }
bool        lx_bt_pads_pairing(void) { return false; }
void        lx_bt_pads_forget_all(void) {}
const char *lx_bt_pads_error(void) { return ""; }

#endif
