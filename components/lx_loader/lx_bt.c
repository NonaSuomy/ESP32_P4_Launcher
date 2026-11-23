/*
 * Bluetooth core: see lx_bt.h. Owns Bluedroid's start and stop and the choice of controller
 * (USB dongle in lx_bt_usb.c, or the C6 here). Every start and stop runs on the "lx_bt" task.
 */
#include "sdkconfig.h"
#include "lx_bt.h"

#if CONFIG_BT_BLUEDROID_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID

#include <string.h>
#include "lx_bt_priv.h"
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

static const char *TAG = "lx_bt";

static QueueHandle_t s_jobq;
static TaskHandle_t  s_task;
static volatile int  s_users;
static volatile bool s_running, s_classic;
static volatile uint32_t s_gen;
static bool s_c6_failed, s_ctrl_init;

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
    ESP_LOGI(TAG, "Bluedroid up on %s", dongle ? lx_bt_usb_name() : "the C6 (BLE only)");
    if (dongle) lx_bt_speaker_stack_up();
    return 1;
}

static void stop_stack(void)
{
    if (!s_running) return;
    lx_bt_ble_stack_down();
    if (s_classic) lx_bt_speaker_stack_down();
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
    if (user == LX_BT_USER_APP) s_c6_failed = false;      /* the next app may try again */
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
    lx_bt_usb_init();
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

#endif
