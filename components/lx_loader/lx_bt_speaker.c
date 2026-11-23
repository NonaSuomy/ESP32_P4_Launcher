/*
 * Bluetooth speaker: Bluedroid's A2DP source on a USB dongle (the C6 is BLE only), fed by the
 * launcher's audio mixer (lx_audio_set_tap) while the speaker plays, so apps need no change.
 *
 * The speaker picked in Settings is saved in /sd/config/bt_speaker.txt (addr=, name=, on=). While
 * it is on and a dongle is plugged in, the launcher keeps the stack up and connects, again every
 * RETRY_MS when the speaker is off or out of range. Pairing: Secure Simple Pairing without input
 * (speakers have none), or legacy PIN 0000. Bonds stay in Bluedroid's NVS store.
 *
 * Audio: the mixer runs at the board's rate (48 kHz); A2DP (SBC) runs at the rate the speaker
 * picked, usually 44.1 kHz. The tap resamples into a ring that Bluedroid's A2DP task drains through
 * the data callback. The two clocks drift a little: a full ring drops input, an empty one plays
 * silence. The master volume is applied in software (the board's codec gets silence).
 */
#include "sdkconfig.h"

#if CONFIG_BT_BLUEDROID_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID

#include <stdio.h>
#include <string.h>
#include "lx_bt_priv.h"
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
#define RETRY_MS   10000
#define SCAN_S     10                       /* inquiry length: units of 1.28 s, 8 = ~10 s */
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
static volatile bool s_linked, s_held, s_scanning, s_scan_hold;
static char     s_err[128];
static TimerHandle_t s_retry;

static lx_bt_found_t s_found[LX_BT_MAX_FOUND];
static volatile int  s_nfound;

/* Ring of S16 stereo frames at the A2DP rate: the mixer's tap writes, the A2DP task reads */
static int16_t *s_ring;
static uint32_t s_cap, s_lim;
static volatile uint32_t s_head, s_tail;
static volatile int s_rate = 44100;
static uint32_t s_step, s_frac = 0x10000;    /* 16.16 mixer frames per A2DP frame */
static int16_t  s_prev[2], s_cur[2];
static uint32_t s_dropped, s_short;
static volatile bool s_playing;

/* ── Saved speaker ──────────────────────────────────────────────────────── */

static void cfg_save(void)
{
    if (!s_saved) { remove(CFG_FILE); return; }
    FILE *f = fopen(CFG_FILE, "w");
    if (!f) { ESP_LOGW(TAG, "cannot write " CFG_FILE); return; }
    fprintf(f, "addr=%02X:%02X:%02X:%02X:%02X:%02X\nname=%s\non=%d\n",
            s_addr[0], s_addr[1], s_addr[2], s_addr[3], s_addr[4], s_addr[5], s_name, s_on ? 1 : 0);
    fclose(f);
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
    if (!s_saved || !s_on || s_linked || s_state == LX_BT_SPK_CONNECTING) return;
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
    s_state = LX_BT_SPK_CONNECTING;
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

static void retry_tick(TimerHandle_t t)
{
    (void)t;
    if (s_on && s_saved && !s_linked && s_state != LX_BT_SPK_CONNECTING && lx_bt_usb_present())
        lx_bt_post(connect_job, NULL);
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
            s_linked = true;
            s_state = LX_BT_SPK_CONNECTED;
            set_err("");
            ESP_LOGI(TAG, "connected to %s", s_name);
            lx_bt_post(start_job, NULL);
        } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            bool was = s_linked;
            s_linked = false;
            audio_off();
            s_state = s_on && s_saved ? LX_BT_SPK_RETRY : LX_BT_SPK_OFF;
            if (was) ESP_LOGI(TAG, "disconnected (%s)", p->conn_stat.disc_rsn == ESP_A2D_DISC_RSN_NORMAL ? "normal" : "link lost");
            else set_err("speaker not found: is it on and in pairing mode?");
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
    if (cod) s_found[k].audio = audio;
}

static void scan_done_job(void *a)
{
    (void)a;
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
        if (p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && s_scanning) lx_bt_post(scan_done_job, NULL);
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) ESP_LOGI(TAG, "paired with %s", p->auth_cmpl.device_name);
        else ESP_LOGW(TAG, "pairing failed: %d", p->auth_cmpl.stat);
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
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    if (s_on && s_saved) lx_bt_post(connect_job, NULL);
}

void lx_bt_speaker_stack_down(void)
{
    audio_off();
    if (s_linked) esp_a2d_source_disconnect(s_addr);
    esp_a2d_source_deinit();
    s_linked = false;
    s_held = false;
    s_scanning = s_scan_hold = false;
    s_state = s_on && s_saved ? (lx_bt_usb_present() ? LX_BT_SPK_RETRY : LX_BT_SPK_NO_DONGLE) : LX_BT_SPK_OFF;
}

void lx_bt_speaker_dongle(bool present)
{
    if (present && s_on && s_saved) lx_bt_post(connect_job, NULL);
    if (!present && s_on && s_saved) s_state = LX_BT_SPK_NO_DONGLE;
}

void lx_bt_speaker_init(void)
{
    cfg_load();
    s_state = s_on && s_saved ? LX_BT_SPK_NO_DONGLE : LX_BT_SPK_OFF;
    s_retry = xTimerCreate("bt_spk", pdMS_TO_TICKS(RETRY_MS), pdTRUE, NULL, retry_tick);
    if (s_retry) xTimerStart(s_retry, 0);
    if (s_saved) ESP_LOGI(TAG, "saved speaker %s (%s)", s_name, s_on ? "on" : "off");
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
