/*
 * Test aid: a board without a Bluetooth dongle poses as a BLE gamepad (HID over GATT on its C6),
 * for testing the launcher's Bluetooth gamepads (lx_bt_pads.c) on another board without a real
 * controller in hand. Only in a launcher built with -DLX_BT_TESTPAD=1; such a board runs no BLE
 * apps (the pad owns the GATT server).
 *
 * It advertises as "LX Test Pad XXXX" (gamepad appearance, HID service) until a host connects, is
 * encrypted and subscribes to the input report (its CCCD write; esp_hidd itself would notify before
 * that, which floods the link while the host still reads the service: its answers queue behind the
 * reports), then SETTLE_MS later sends report 1 every PERIOD_MS, like a pad that streams. It
 * plays for PLAY_S (X sweeps 0..255, button 1 held half of every second), then rests for REST_S
 * (everything centred, still sent): the launcher calls a missing speaker only while nobody plays.
 * It logs what it sent every 10 s.
 */
#include "sdkconfig.h"

#if defined(LX_BT_TESTPAD) && LX_BT_TESTPAD && CONFIG_BT_BLUEDROID_ENABLED && CONFIG_BT_GATTS_ENABLE

#include "lx_bt_priv.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_hidd.h"
#include "esp_hidd_gatts.h"

static const char *TAG = "testpad";

#define PERIOD_MS 15
#define SETTLE_MS 300
#define PLAY_S    50
#define REST_S    40

/* report 1: 16 buttons, hat (4 bits) + 4 padding, X Y Z Rz 0..255 */
static const uint8_t k_map[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07, 0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14,
    0x75, 0x04, 0x95, 0x01, 0x81, 0x42, 0x75, 0x04, 0x95, 0x01, 0x81, 0x03, 0x65, 0x00,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x04,
    0x81, 0x02, 0xC0,
};

static esp_hid_raw_report_map_t s_maps[] = { { k_map, sizeof(k_map) } };
static char s_name[24] = "LX Test Pad";
static esp_hid_device_config_t s_cfg = {
    .vendor_id = 0x303A, .product_id = 0x4C58, .version = 0x0100,
    .device_name = s_name, .manufacturer_name = "ESP32-P4 Launcher", .serial_number = "test",
    .report_maps = s_maps, .report_maps_len = 1,
};

static esp_hidd_dev_t *s_dev;
static volatile bool s_conn;
static int s_auth_fails;
static volatile bool s_enc, s_subscribed;
static volatile int64_t s_send_from;   /* esp_timer time sending starts (0 = not encrypted + subscribed yet) */
static uint32_t s_n, s_sent, s_fail;
static esp_timer_handle_t s_tick;

static void adv_start(void)
{
    uint8_t adv[31];
    int n = 0;
    adv[n++] = 2; adv[n++] = 0x01; adv[n++] = 0x06;                            /* flags */
    adv[n++] = 3; adv[n++] = 0x19; adv[n++] = 0xC4; adv[n++] = 0x03;           /* appearance: gamepad */
    adv[n++] = 3; adv[n++] = 0x03; adv[n++] = 0x12; adv[n++] = 0x18;           /* HID service */
    int l = (int)strlen(s_name);
    if (l > 31 - n - 2) l = 31 - n - 2;
    adv[n++] = (uint8_t)(l + 1); adv[n++] = 0x09;
    memcpy(adv + n, s_name, l);
    n += l;
    esp_ble_gap_config_adv_data_raw(adv, n);
}

static void gap_cb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t *p)
{
    switch (ev) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT: {
        esp_ble_adv_params_t ap = {
            .adv_int_min = 0x20, .adv_int_max = 0x30, .adv_type = ADV_TYPE_IND,
            .own_addr_type = BLE_ADDR_TYPE_PUBLIC, .channel_map = ADV_CHNL_ALL,
            .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
        };
        esp_ble_gap_start_advertising(&ap);
        break;
    }
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        ESP_LOGI(TAG, "advertising as \"%s\" (%s)", s_name, p->adv_start_cmpl.status ? "failed" : "ok");
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, true);
        break;
    case ESP_GAP_BLE_NC_REQ_EVT:
        esp_ble_confirm_reply(p->ble_security.key_notif.bd_addr, true);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "pairing / encryption %s (0x%x)", p->ble_security.auth_cmpl.success ? "ok" : "failed",
                 p->ble_security.auth_cmpl.fail_reason);
        if (p->ble_security.auth_cmpl.success) {
            s_enc = true;
            if (s_subscribed && !s_send_from) s_send_from = esp_timer_get_time() + SETTLE_MS * 1000LL;
            s_auth_fails = 0;
        } else if (++s_auth_fails >= 2) {
            /* the host keeps failing (it lost its keys and pairs, which a bonded peer refuses): drop
             * ours, as a pad put in pairing mode would */
            s_auth_fails = 0;
            esp_ble_remove_bond_device(p->ble_security.auth_cmpl.bd_addr);
            ESP_LOGW(TAG, "two failures in a row: keys dropped, the host can pair again");
        }
        break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "connection interval %.2f ms", p->update_conn_params.conn_int * 1.25);
        break;
    default:
        break;
    }
}

static void hidd_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        adv_start();
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "host connected");
        s_conn = true;
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "host disconnected: advertising again");
        s_conn = false;
        s_enc = s_subscribed = false;
        s_send_from = 0;
        adv_start();
        break;
    default:
        break;
    }
}

static void tick(void *a)
{
    (void)a;
    s_n++;
    if (!s_conn || !s_dev || !s_send_from || esp_timer_get_time() < s_send_from) return;
    uint32_t ms = s_n * PERIOD_MS;
    uint8_t r[7];
    bool play = (ms / 1000) % (PLAY_S + REST_S) < PLAY_S;
    uint16_t b = play && (ms % 1000) < 500 ? 0x0001 : 0;
    r[0] = (uint8_t)b; r[1] = (uint8_t)(b >> 8);
    r[2] = 0x08;                        /* hat: centre (null) */
    r[3] = play ? (uint8_t)s_sent : 128;    /* X: counter while playing */
    r[4] = 128; r[5] = 128; r[6] = 128;
    if (esp_hidd_dev_input_set(s_dev, 0, 1, r, sizeof(r)) == ESP_OK) s_sent++;
    else s_fail++;
    if ((s_n * PERIOD_MS) % 10000 < PERIOD_MS)
        ESP_LOGI(TAG, "%s, %u reports sent, %u refused", play ? "playing" : "resting", (unsigned)s_sent, (unsigned)s_fail);
}

/* esp_hidd's GATT server, plus: a write of 01 00 to a CCCD = the host subscribed (the launcher writes
 * it on every connection) */
static void gatts_cb(esp_gatts_cb_event_t ev, esp_gatt_if_t gif, esp_ble_gatts_cb_param_t *p)
{
    if (ev == ESP_GATTS_WRITE_EVT && p->write.len == 2 && (p->write.value[0] & 1) && !s_subscribed) {
        s_subscribed = true;
        ESP_LOGI(TAG, "host subscribed");
        if (s_enc && !s_send_from) s_send_from = esp_timer_get_time() + SETTLE_MS * 1000LL;
    }
    esp_hidd_gatts_event_handler(ev, gif, p);
}

static void start_job(void *a)
{
    (void)a;
    if (!lx_bt_acquire(LX_BT_USER_PADS, false, NULL)) {
        ESP_LOGE(TAG, "Bluetooth did not start");
        return;
    }
    const uint8_t *ba = esp_bt_dev_get_address();
    if (ba) snprintf(s_name, sizeof(s_name), "LX Test Pad %02X%02X", ba[4], ba[5]);
    esp_ble_gap_set_device_name(s_name);
    esp_ble_auth_req_t auth = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t io = ESP_IO_CAP_NONE;
    uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK, ksize = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &io, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &ksize, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, 1);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, 1);
    esp_ble_gatts_register_callback(gatts_cb);
    esp_err_t e = esp_hidd_dev_init(&s_cfg, ESP_HID_TRANSPORT_BLE, hidd_cb, &s_dev);
    if (e != ESP_OK) { ESP_LOGE(TAG, "HID device: %s", esp_err_to_name(e)); return; }
    const esp_timer_create_args_t ta = { .callback = tick, .name = "testpad" };
    if (esp_timer_create(&ta, &s_tick) == ESP_OK) esp_timer_start_periodic(s_tick, PERIOD_MS * 1000);
    ESP_LOGI(TAG, "BLE test gamepad up: %s", s_name);
}

void lx_bt_testpad_init(void)
{
    ESP_LOGW(TAG, "test build: this board poses as a BLE gamepad");
    lx_bt_ble_handlers(LX_BT_BLE_TEST, gap_cb, NULL);
    lx_bt_post(start_job, NULL);
}

#else

void lx_bt_testpad_init(void);
void lx_bt_testpad_init(void) {}

#endif
