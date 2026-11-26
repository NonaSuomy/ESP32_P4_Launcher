/*
 * Bluetooth speaker: Bluedroid's A2DP source on a USB dongle (the C6 is BLE only), fed by the
 * launcher's audio mixer (lx_audio_set_tap) while the speaker plays, so apps need no change.
 *
 * The speaker picked in Settings is saved in /sd/config/bt_speaker.txt (addr=, name=, on=). While
 * it is on and a dongle is plugged in, the launcher keeps the stack up, connectable (a speaker that
 * reconnects by itself when switched on finds us), and calls it: again after 10, 20, 40, then every
 * 60 s while it is off or out of range. A call pages the speaker, and while the dongle pages it
 * carries nothing else (Classic or LE: a connected controller's reports stop). So with a controller
 * connected a call pages 2.5 s instead of 5 (a speaker that is on answers in well under that), comes
 * every 30, 60, then 120 s, and only once no input has changed for STILL_MS: nobody is playing. A
 * missing speaker then never freezes a game, and never costs a controller its link. Connect calls at
 * once.
 * Pairing: Secure Simple Pairing without input (speakers have none), or legacy PIN 0000. Bonds stay
 * in Bluedroid's NVS store.
 *
 * Audio: the mixer runs at the board's rate (48 kHz); A2DP (SBC) runs at the rate the speaker
 * picked, usually 44.1 kHz. The tap resamples into a ring that Bluedroid's A2DP task drains through
 * the data callback. The two clocks drift a little: a full ring drops input, an empty one plays
 * silence. The master volume is applied in software (the board's codec gets silence).
 */
#include "sdkconfig.h"
#include "esp_attr.h"     /* EXT_RAM_BSS_ATTR */

#if CONFIG_BT_BLUEDROID_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID

#include <stdio.h>
#include <string.h>
#include "lx_bt_priv.h"
#include "lx_common.h"
#include "esp_timer.h"
#include "lx_audio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_a2dp_legacy_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "lx_bt_spk";

#define CFG_FILE   "/sd/config/bt_speaker.txt"
#define RETRY_MS   10000        /* timer tick; calls are whole ticks apart */
#define PAGE_LONG  0x2000       /* page timeout, 0.625 ms slots: 5.12 s, nothing else connected */
#define PAGE_SHORT 0x1000       /* 2.56 s: a controller is connected */
#define STILL_MS   20000        /* with a controller connected, calls wait until no input changed for this long */
#define CONNECT_MS 20000        /* a connect attempt Bluedroid never answers is given up after this */
#define SCAN_S     10                       /* inquiry length: units of 1.28 s, 10 = ~13 s */
#define RING_MS    250
/* Most the ring holds while playing: the mixer runs on the codec's clock, a bit faster than Bluedroid's
 * 30 ms pulls, so a bigger fill would just sit there as delay (it did: 250 ms). Beyond it frames are
 * dropped, one now and then. */
#define FILL_MS    60
#define DEV_NAME   "ESP32-P4 Launcher"

static uint8_t  s_addr[6];
static char     s_name[48];
static bool     s_saved, s_on;
static volatile lx_bt_spk_state_t s_state = LX_BT_SPK_OFF;
static volatile bool s_linked, s_held, s_scanning, s_scan_hold, s_ble_scan;
static int s_fails, s_wait;         /* failed tries in a row; retry ticks still to skip */
static volatile bool s_inquiry;    /* the dongle is inquiring (set from GAP events: cancel takes a moment) */
static volatile int  s_paused;        /* scans and pad pairing: no reconnect attempts meanwhile */
static volatile int64_t s_connect_t;    /* when the current connect attempt started */
static char     s_err[128];
static TimerHandle_t s_retry;

static EXT_RAM_BSS_ATTR lx_bt_found_t s_found[LX_BT_MAX_FOUND];
static volatile int  s_nfound;

/* Ring of S16 stereo frames at the A2DP rate: the mixer's tap writes, the A2DP task reads */
static int16_t *s_ring;
static uint32_t s_cap, s_lim;
static volatile uint32_t s_head, s_tail;
static volatile int s_rate = 44100;
static uint32_t s_step, s_frac = 0x10000;    /* 16.16 mixer frames per A2DP frame */
static int16_t  s_prev[2], s_cur[2];
static uint32_t s_dropped, s_short;
static volatile uint32_t s_taken;      /* frames Bluedroid took (status) */
static volatile bool s_playing;
static void start_job(void *a);

/* ── Saved speaker ──────────────────────────────────────────────────────── */

static void cfg_save(void)
{
    if (!lx_sd_write_try_lock()) { ESP_LOGW(TAG, "cannot save speaker settings: SD card is busy"); return; }
    if (!s_saved) {
        remove(CFG_FILE);
        lx_sd_write_unlock();
        return;
    }
    FILE *f = fopen(CFG_FILE ".tmp", "w");
    if (!f) { lx_sd_write_unlock(); ESP_LOGW(TAG, "cannot write " CFG_FILE); return; }
    fprintf(f, "addr=%02X:%02X:%02X:%02X:%02X:%02X\nname=%s\non=%d\n",
            s_addr[0], s_addr[1], s_addr[2], s_addr[3], s_addr[4], s_addr[5], s_name, s_on ? 1 : 0);
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
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (sscanf(line, "addr=%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]) == 6) {
            for (int i = 0; i < 6; i++) s_addr[i] = (uint8_t)a[i];
            s_saved = true;
        } else if (!strncmp(line, "name=", 5)) {
            snprintf(s_name, sizeof(s_name), "%.47s", line + 5);
        } else if (!strncmp(line, "on=", 3)) {
            s_on = line[3] == '1';
        }
    }
    fclose(f);
}

/* ── Audio path ─────────────────────────────────────────────────────────── */

static void tap(const int16_t *in, int n)
{
    if (!s_ring) return;
    int vol = lx_audio_master_volume_get();
    int32_t gain = vol * vol * 256 / 10000;          /* 0..256, roughly even steps to the ear */
    uint32_t head = s_head, tail = __atomic_load_n(&s_tail, __ATOMIC_ACQUIRE);
    for (int i = 0; i < n; i++) {
        s_prev[0] = s_cur[0]; s_prev[1] = s_cur[1];
        s_cur[0] = (int16_t)((in[i * 2] * gain) >> 8);
        s_cur[1] = (int16_t)((in[i * 2 + 1] * gain) >> 8);
        while (s_frac <= 0x10000) {                   /* output frames between prev and cur */
            if (head - tail < s_lim) {
                int32_t t = s_frac >> 1;
                int16_t *d = s_ring + (head % s_cap) * 2;
                d[0] = (int16_t)(s_prev[0] + (((s_cur[0] - s_prev[0]) * t) >> 15));
                d[1] = (int16_t)(s_prev[1] + (((s_cur[1] - s_prev[1]) * t) >> 15));
                head++;
            } else {
                s_dropped++;                          /* ring full: the speaker's clock is slower */
            }
            s_frac += s_step;
        }
        s_frac -= 0x10000;
    }
    __atomic_store_n(&s_head, head, __ATOMIC_RELEASE);
}

/* Bluedroid's A2DP task: len bytes of S16 stereo at the A2DP rate (-1: flush) */
static int32_t a2d_data(uint8_t *buf, int32_t len)
{
    if (len < 0) { s_tail = s_head; return 0; }
    if (!buf) return 0;
    int16_t *out = (int16_t *)buf;
    int frames = len / 4;
    uint32_t tail = s_tail, head = __atomic_load_n(&s_head, __ATOMIC_ACQUIRE);
    int i = 0;
    for (; i < frames && tail != head; i++, tail++) {
        const int16_t *f = s_ring + (tail % s_cap) * 2;
        out[i * 2] = f[0];
        out[i * 2 + 1] = f[1];
    }
    if (i < frames) {
        if (i) s_short++;
        memset(out + i * 2, 0, (frames - i) * 4);
    }
    __atomic_store_n(&s_tail, tail, __ATOMIC_RELEASE);
    s_taken += frames;
    return frames * 4;
}

static void audio_on(void)
{
    if (!s_ring) {
        s_cap = (uint32_t)(48000 * RING_MS / 1000);
        s_ring = heap_caps_calloc(s_cap, 4, MALLOC_CAP_SPIRAM);
        if (!s_ring) { ESP_LOGE(TAG, "no memory for the audio ring"); return; }
    }
    s_head = s_tail = 0;
    s_frac = 0x10000;
    s_step = (uint32_t)(((uint64_t)lx_audio_output_rate() << 16) / s_rate);
    s_lim = (uint32_t)(s_rate * FILL_MS / 1000);
    if (s_lim > s_cap) s_lim = s_cap;
    s_dropped = s_short = 0;
    lx_audio_set_tap(tap);
    s_playing = true;
    ESP_LOGI(TAG, "playing on %s at %d Hz", s_name, s_rate);
}

static void audio_off(void)
{
    if (!s_playing) return;
    s_playing = false;
    lx_audio_set_tap(NULL);
    ESP_LOGI(TAG, "audio back on the board (%lu frames dropped, %lu short reads)",
             (unsigned long)s_dropped, (unsigned long)s_short);
}

/* ── Connection (lx_bt task) ────────────────────────────────────────────── */

static void set_err(const char *e)
{
    snprintf(s_err, sizeof(s_err), "%s", e);
    if (e[0]) ESP_LOGW(TAG, "%s", e);
}

static void connect_job(void *a)
{
    (void)a;
    if (!s_saved || !s_on || s_linked || s_state == LX_BT_SPK_CONNECTING || s_scanning || s_paused) return;
    if (!lx_bt_usb_present()) { s_state = LX_BT_SPK_NO_DONGLE; return; }
    if (!s_held) {
        if (!lx_bt_acquire(LX_BT_USER_SPEAKER, true, NULL)) {
            s_state = LX_BT_SPK_RETRY;
            set_err(lx_bt_running() ? "Bluetooth is busy on the Wi-Fi chip (an app uses BLE)"
                                    : lx_bt_usb_why()[0] ? lx_bt_usb_why() : "Bluetooth did not start on the dongle");
            return;
        }
        s_held = true;
    }
    int pads = lx_bt_pads_linked();
    esp_bt_gap_set_page_timeout(pads ? PAGE_SHORT : PAGE_LONG);
    if (pads) ESP_LOGI(TAG, "calling %s (short page: %d controller%s connected)", s_name, pads, pads == 1 ? "" : "s");
    /* a BLE pad's pending connection request makes way: an LE link the CSR8510 sets up while it pages
     * comes up broken (encrypted, but nothing it is sent gets through) */
    lx_bt_pads_cancel_open();
    s_state = LX_BT_SPK_CONNECTING;
    s_connect_t = esp_timer_get_time();
    esp_err_t e = esp_a2d_source_connect(s_addr);
    if (e != ESP_OK) {
        s_state = LX_BT_SPK_RETRY;
        set_err("connect request failed");
    }
}

static void release_job(void *a)
{
    (void)a;
    if (s_linked) esp_a2d_source_disconnect(s_addr);      /* the DISCONNECTED event follows */
    else if (s_held && !s_scan_hold) {
        s_held = false;
        lx_bt_release(LX_BT_USER_SPEAKER);
    }
}

/* on the lx_bt task: the timer task's 2 KB stack is too small for this (stack guard panic) */
static void retry_job(void *a)
{
    (void)a;
    if (s_state == LX_BT_SPK_CONNECTING && !s_linked &&
        esp_timer_get_time() - s_connect_t > CONNECT_MS * 1000LL) {
        ESP_LOGW(TAG, "no answer to the connect request: trying again");
        s_state = LX_BT_SPK_RETRY;
    }
    /* not while a scan or a controller's pairing has the radio */
    if (s_on && s_saved && !s_linked && s_state != LX_BT_SPK_CONNECTING && !s_scanning && !s_paused &&
        lx_bt_usb_present()) {
        if (s_wait > 0) s_wait--;
        else if (lx_bt_pads_still_ms() < STILL_MS) { /* someone is playing: the call waits (it stalls the pads) */ }
        else connect_job(NULL);
    }
}

uint32_t lx_bt_speaker_taken(void) { return s_taken; }

static void retry_tick(TimerHandle_t t)
{
    (void)t;
    lx_bt_post(retry_job, NULL);
}

static void start_job(void *a)
{
    (void)a;
    esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
}

/* ── Bluedroid callbacks (BTC task: no waiting on the lx_bt task here) ──── */

static void a2d_cb(esp_a2d_cb_event_t ev, esp_a2d_cb_param_t *p)
{
    switch (ev) {
    case ESP_A2D_CONNECTION_STATE_EVT:
        if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            if (!s_saved || memcmp(p->conn_stat.remote_bda, s_addr, 6)) {
                ESP_LOGW(TAG, "%02X:%02X:%02X:%02X:%02X:%02X is not the chosen speaker: disconnected",
                         p->conn_stat.remote_bda[0], p->conn_stat.remote_bda[1], p->conn_stat.remote_bda[2],
                         p->conn_stat.remote_bda[3], p->conn_stat.remote_bda[4], p->conn_stat.remote_bda[5]);
                esp_a2d_source_disconnect(p->conn_stat.remote_bda);
                break;
            }
            s_linked = true;
            s_state = LX_BT_SPK_CONNECTED;
            s_fails = s_wait = 0;
            set_err("");
            ESP_LOGI(TAG, "connected to %s", s_name);
            lx_bt_post(start_job, NULL);
        } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            if (s_saved && memcmp(p->conn_stat.remote_bda, s_addr, 6)) break;     /* one we turned away */
            bool was = s_linked;
            s_linked = false;
            audio_off();
            s_state = s_on && s_saved ? LX_BT_SPK_RETRY : LX_BT_SPK_OFF;
            if (was) ESP_LOGI(TAG, "disconnected (%s)", p->conn_stat.disc_rsn == ESP_A2D_DISC_RSN_NORMAL ? "normal" : "link lost");
            else {
                set_err("speaker not found: is it on?");
                /* next call in ticks: 1, 2, 4, 6, 6.. alone; 3, 6, 12, 12.. with a controller connected */
                int f = s_fails < 3 ? s_fails : 3;
                int ticks = lx_bt_pads_linked() ? (f >= 2 ? 12 : 3 << f) : (f >= 3 ? 6 : 1 << f);
                s_wait = ticks - 1;
                s_fails++;
            }
            if (!s_on) lx_bt_post(release_job, NULL);
        }
        break;
    case ESP_A2D_AUDIO_CFG_EVT:
        if (p->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            uint8_t f = p->audio_cfg.mcc.cie.sbc_info.samp_freq;
            s_rate = f & ESP_A2D_SBC_CIE_SF_48K ? 48000 : f & ESP_A2D_SBC_CIE_SF_44K ? 44100 :
                     f & ESP_A2D_SBC_CIE_SF_32K ? 32000 : 16000;
        }
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (p->media_ctrl_stat.status != ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            ESP_LOGW(TAG, "media control %d refused (%d)", p->media_ctrl_stat.cmd, p->media_ctrl_stat.status);
            break;
        }
        if (p->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY) esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        break;
    case ESP_A2D_AUDIO_STATE_EVT:
        if (p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            s_state = LX_BT_SPK_PLAYING;
            audio_on();
        } else {
            audio_off();
            if (s_linked) {
                s_state = LX_BT_SPK_CONNECTED;
                lx_bt_post(start_job, NULL);        /* the speaker paused us: ask again */
            }
        }
        break;
    default:
        break;
    }
}

static void found(esp_bt_gap_cb_param_t *p)
{
    uint32_t cod = 0;
    int8_t rssi = -127;
    char name[48] = "";
    for (int i = 0; i < p->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *q = &p->disc_res.prop[i];
        if (q->type == ESP_BT_GAP_DEV_PROP_COD) cod = *(uint32_t *)q->val;
        else if (q->type == ESP_BT_GAP_DEV_PROP_RSSI) rssi = *(int8_t *)q->val;
        else if (q->type == ESP_BT_GAP_DEV_PROP_BDNAME && !name[0])
            snprintf(name, sizeof(name), "%.*s", q->len, (const char *)q->val);
        else if (q->type == ESP_BT_GAP_DEV_PROP_EIR && !name[0]) {
            uint8_t len = 0;
            uint8_t *n = esp_bt_gap_resolve_eir_data(q->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
            if (!n) n = esp_bt_gap_resolve_eir_data(q->val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &len);
            if (n) snprintf(name, sizeof(name), "%.*s", len, (const char *)n);
        }
    }
    bool audio = esp_bt_gap_is_valid_cod(cod) &&
                 ((esp_bt_gap_get_cod_srvc(cod) & ESP_BT_COD_SRVC_RENDERING) ||
                  esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_AV);
    int n = s_nfound, k;
    for (k = 0; k < n; k++) if (!memcmp(s_found[k].addr, p->disc_res.bda, 6)) break;
    if (k == n) {
        if (n >= LX_BT_MAX_FOUND) return;
        memset(&s_found[k], 0, sizeof(s_found[k]));
        memcpy(s_found[k].addr, p->disc_res.bda, 6);
        s_nfound = n + 1;
    }
    if (name[0]) snprintf(s_found[k].name, sizeof(s_found[k].name), "%s", name);
    if (rssi != -127) s_found[k].rssi = rssi;
    if (cod) {
        s_found[k].audio = audio;
        s_found[k].hid = esp_bt_gap_get_cod_major_dev(cod) == ESP_BT_COD_MAJOR_DEV_PERIPHERAL;
    }
}

/* ── BLE half of the scan: HID adverts (gamepads, keyboards) ───────────── */

static bool adv_field(const uint8_t *d, int len, uint8_t type, const uint8_t **v, int *vlen)
{
    for (int i = 0; i + 1 < len;) {
        int l = d[i];
        if (!l || i + 1 + l > len) break;
        if (d[i + 1] == type) { *v = d + i + 2; *vlen = l - 1; return true; }
        i += 1 + l;
    }
    return false;
}

static void ble_found(const esp_ble_gap_cb_param_t *p)
{
    const uint8_t *d = p->scan_rst.ble_adv, *v;
    int len = p->scan_rst.adv_data_len + p->scan_rst.scan_rsp_len, vl;
    bool hid = false;
    if (adv_field(d, len, 0x19, &v, &vl) && vl >= 2) {           /* appearance: HID category 0x03C0.. */
        int app = v[0] | v[1] << 8;
        hid = app >= 0x03C0 && app <= 0x03FF;
    }
    for (uint8_t t = 0x02; t <= 0x03 && !hid; t++)              /* 16-bit services: 0x1812 HID */
        if (adv_field(d, len, t, &v, &vl))
            for (int i = 0; i + 1 < vl; i += 2) if ((v[i] | v[i + 1] << 8) == 0x1812) hid = true;
    char name[48] = "";
    if (adv_field(d, len, 0x09, &v, &vl) || adv_field(d, len, 0x08, &v, &vl))
        snprintf(name, sizeof(name), "%.*s", vl, (const char *)v);
    int n = s_nfound, k;
    for (k = 0; k < n; k++) if (!memcmp(s_found[k].addr, p->scan_rst.bda, 6)) break;
    if (k == n) {
        if (!hid || n >= LX_BT_MAX_FOUND) return;               /* only controllers and the like */
        memset(&s_found[k], 0, sizeof(s_found[k]));
        memcpy(s_found[k].addr, p->scan_rst.bda, 6);
        s_found[k].ble = true;
        s_found[k].hid = true;
        s_found[k].addr_type = (uint8_t)p->scan_rst.ble_addr_type;
        s_nfound = n + 1;
    } else if (!s_found[k].ble) {
        return;                                                 /* found by the inquiry already */
    }
    if (name[0]) snprintf(s_found[k].name, sizeof(s_found[k].name), "%s", name);
    s_found[k].rssi = (int8_t)p->scan_rst.rssi;
}

static void ble_gap_cb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p)
{
    if (!s_ble_scan) return;
    switch (ev) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (p->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) esp_ble_gap_start_scanning(SCAN_S * 128 / 100);
        else s_ble_scan = false;
        break;
    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (p->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) s_ble_scan = false;
        break;
    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) ble_found(p);
        else if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) s_ble_scan = false;
        break;
    default:
        break;
    }
}

static void ble_scan_start(void)
{
    if (lx_bt_app_holds()) return;          /* an app's BLE scans are its own */
    static esp_ble_scan_params_t sp = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE, .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL, .scan_interval = 0x50, .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
    };
    s_ble_scan = true;
    if (esp_ble_gap_set_scan_params(&sp) != ESP_OK) s_ble_scan = false;
}

static void scan_done_job(void *a)
{
    (void)a;
    if (s_ble_scan) {                       /* the inquiry ended first: the BLE half too */
        esp_ble_gap_stop_scanning();
        s_ble_scan = false;
    }
    s_scanning = false;
    if (s_scan_hold) {
        s_scan_hold = false;
        if (!s_on || !s_saved) release_job(NULL);
    }
}

static void gap_cb(esp_bt_gap_cb_event_t ev, esp_bt_gap_cb_param_t *p)
{
    switch (ev) {
    case ESP_BT_GAP_DISC_RES_EVT:
        found(p);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        s_inquiry = p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED;
        if (p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && s_scanning) lx_bt_post(scan_done_job, NULL);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) ESP_LOGI("lx_bt", "paired with %s", p->auth_cmpl.device_name);
        else ESP_LOGW("lx_bt", "pairing failed: %d", p->auth_cmpl.stat);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = { '0', '0', '0', '0' };
        if (p->pin_req.min_16_digit) {
            memset(pin, '0', sizeof(pin));
            esp_bt_gap_pin_reply(p->pin_req.bda, true, 16, pin);
        } else {
            esp_bt_gap_pin_reply(p->pin_req.bda, true, 4, pin);
        }
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, true);
        break;
    default:
        break;
    }
}

/* ── Stack hooks (lx_bt task) ───────────────────────────────────────────── */

void lx_bt_speaker_stack_up(void)
{
    esp_bt_gap_set_device_name(DEV_NAME);
    esp_bt_gap_register_callback(gap_cb);
    esp_bt_io_cap_t io = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &io, sizeof(io));
    esp_bt_pin_code_t pin = { 0 };
    esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, pin);
    esp_a2d_register_callback(a2d_cb);
    esp_a2d_source_register_data_callback(a2d_data);
    esp_err_t e = esp_a2d_source_init();
    if (e != ESP_OK) ESP_LOGE(TAG, "A2DP source init: %s", esp_err_to_name(e));
    /* Connectable, so a speaker that reconnects by itself when switched on finds us; only the
     * chosen speaker is kept (a2d_cb). Never discoverable. */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    if (s_on && s_saved) lx_bt_post(connect_job, NULL);
}

void lx_bt_speaker_stack_down(void)
{
    audio_off();
    if (s_linked) esp_a2d_source_disconnect(s_addr);
    esp_a2d_source_deinit();
    s_linked = false;
    s_held = false;
    s_scanning = s_scan_hold = s_ble_scan = false;
    s_state = s_on && s_saved ? (lx_bt_usb_present() ? LX_BT_SPK_RETRY : LX_BT_SPK_NO_DONGLE) : LX_BT_SPK_OFF;
}

void lx_bt_speaker_dongle(bool present)
{
    if (present && s_on && s_saved) lx_bt_post(connect_job, NULL);
    if (!present && s_on && s_saved) s_state = LX_BT_SPK_NO_DONGLE;
}

bool lx_bt_speaker_scanning(void) { return s_scanning; }

void lx_bt_speaker_init(void)
{
    lx_bt_ble_handlers(LX_BT_BLE_SCAN, ble_gap_cb, NULL);
    cfg_load();
    s_state = s_on && s_saved ? LX_BT_SPK_NO_DONGLE : LX_BT_SPK_OFF;
    s_retry = xTimerCreate("bt_spk", pdMS_TO_TICKS(RETRY_MS), pdTRUE, NULL, retry_tick);
    if (s_retry) xTimerStart(s_retry, 0);
    if (s_saved) ESP_LOGI(TAG, "saved speaker %s (%s)", s_name, s_on ? "on" : "off");
}

/* A connect attempt pages the speaker for up to ~5 s; the dongle refuses an inquiry or another
 * connection meanwhile ("Cmd Disallowed"). Waits for it to end (lx_bt task). */
bool lx_bt_speaker_paging(void) { return s_state == LX_BT_SPK_CONNECTING && !s_linked; }

void lx_bt_speaker_wait_page(void)
{
    for (int i = 0; i < 80 && s_state == LX_BT_SPK_CONNECTING && !s_linked; i++) vTaskDelay(pdMS_TO_TICKS(100));
    for (int i = 0; i < 30 && s_inquiry; i++) vTaskDelay(pdMS_TO_TICKS(100));  /* a cancelled scan winding down */
}

void lx_bt_speaker_pause(bool on)
{
    if (on) s_paused++;
    else if (s_paused > 0) s_paused--;
}

/* ── API (UI task) ──────────────────────────────────────────────────────── */

static void scan_job(void *a)
{
    (void)a;
    if (!s_held) {
        if (!lx_bt_acquire(LX_BT_USER_SPEAKER, true, NULL)) {
            set_err(lx_bt_running() ? "Bluetooth is busy on the Wi-Fi chip"
                    : lx_bt_usb_why()[0] ? lx_bt_usb_why() : "Bluetooth did not start on the dongle");
            s_scanning = false;
            return;
        }
        s_held = true;
        s_scan_hold = true;
    }
    lx_bt_speaker_wait_page();
    lx_bt_pads_cancel_open();
    ble_scan_start();
    if (esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, SCAN_S, 0) != ESP_OK) {
        set_err("scan failed");
        scan_done_job(NULL);
    }
}

bool lx_bt_speaker_scan(void)
{
    if (!lx_bt_usb_present()) return false;
    if (s_scanning) return true;
    s_nfound = 0;
    s_scanning = true;
    set_err("");
    lx_bt_post(scan_job, NULL);
    return true;
}

int lx_bt_speaker_results(lx_bt_found_t *out, int max)
{
    if (s_scanning) return -1;
    int n = s_nfound < max ? s_nfound : max;
    memcpy(out, s_found, n * sizeof(*out));
    return n;
}

typedef struct { uint8_t addr[6]; char name[48]; } pick_t;
static pick_t s_pick;

static void pick_job(void *a)
{
    (void)a;
    if (s_scanning) esp_bt_gap_cancel_discovery();
    if (s_linked && memcmp(s_addr, s_pick.addr, 6)) esp_a2d_source_disconnect(s_addr);
    memcpy(s_addr, s_pick.addr, 6);
    snprintf(s_name, sizeof(s_name), "%s", s_pick.name);
    s_saved = s_on = true;
    cfg_save();
    s_state = LX_BT_SPK_RETRY;
    if (!s_linked) connect_job(NULL);
}

void lx_bt_speaker_pick(const uint8_t addr[6], const char *name)
{
    memcpy(s_pick.addr, addr, 6);
    if (name && name[0]) snprintf(s_pick.name, sizeof(s_pick.name), "%s", name);
    else snprintf(s_pick.name, sizeof(s_pick.name), "%02X:%02X:%02X:%02X:%02X:%02X",
                  addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
    lx_bt_post(pick_job, NULL);
}

static void enable_job(void *a)
{
    bool on = (bool)(intptr_t)a;
    if (!s_saved) return;
    s_on = on;
    cfg_save();
    if (on) {
        s_state = LX_BT_SPK_RETRY;
        s_fails = s_wait = 0;
        connect_job(NULL);
    } else {
        s_state = LX_BT_SPK_OFF;
        release_job(NULL);
    }
}

void lx_bt_speaker_enable(bool on) { lx_bt_post(enable_job, (void *)(intptr_t)on); }
bool lx_bt_speaker_enabled(void)   { return s_saved && s_on; }

static void forget_job(void *a)
{
    (void)a;
    if (!s_saved) return;
    if (lx_bt_running() && lx_bt_classic()) esp_bt_gap_remove_bond_device(s_addr);
    s_on = false;
    s_saved = false;
    s_name[0] = 0;
    cfg_save();
    s_state = LX_BT_SPK_OFF;
    release_job(NULL);
}

void lx_bt_speaker_forget(void) { lx_bt_post(forget_job, NULL); }

lx_bt_spk_state_t lx_bt_speaker_state(void) { return s_state; }
const char *lx_bt_speaker_name(void)  { return s_saved ? s_name : ""; }
const char *lx_bt_speaker_error(void) { return s_err; }

#endif
