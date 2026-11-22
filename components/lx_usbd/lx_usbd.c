/*
 * lx_usbd: the Tab5's USB-C as an app's USB HID device (HIDD interface). As the Tab5
 * Multitool's firmware does it (src/t5m/UsbHid.cpp): USB-C is wired to full-speed PHY 0, which the USB
 * serial/JTAG unit (the launcher's DevLink link) owns; start() gives it to the full-speed OTG controller and
 * runs TinyUSB there in device mode with one HID interface; stop() and the app's end give it back.
 *
 * On the P4 the PHY selection, the full-speed OTG controller's reset and clocks, and the serial/JTAG unit's
 * reset are all in the always-on low-power domain: a software reset or a panic leaves them as they were. A
 * panic while the app had the port left the OTG controller running and the serial/JTAG unit half set up, and
 * every boot after it then crashed installing DevLink's driver. So start() leaves a mark in RTC memory, a clean
 * stop() resets the OTG controller and clears it, and a boot that finds the mark (or the PHY still swapped)
 * resets both units and puts the PHY back before anything uses them.
 *
 * Reports wait in a queue and the TinyUSB task sends them one per frame, in order.
 */
#include "lx_usbd.h"
#include "lx_devlink.h"

#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_private/usb_phy.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "soc/lp_clkrst_struct.h"
#include "soc/lp_system_struct.h"
#include "soc/usb_serial_jtag_struct.h"
#include "tusb.h"

static const char *TAG = "lx_usbd";

#define RHPORT          0           /* TinyUSB's port 0 is the P4's full-speed OTG controller */
#define EP_HID          0x81
#define REPORT_DESC_MAX 512

typedef struct {
    uint8_t id, len;
    uint16_t wait_ms;
    uint8_t data[16];
} report_t;

static uint8_t s_report_desc[REPORT_DESC_MAX];
static uint16_t s_report_desc_len;
static uint8_t s_config[TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN];
static char s_manufacturer[32], s_product[32], s_serial[13];
static uint8_t s_boot_id;
static volatile uint8_t s_leds;
static volatile bool s_running;
static usb_phy_handle_t s_phy;
static TaskHandle_t s_task;
static QueueHandle_t s_queue;
static volatile bool s_task_quit;

static tusb_desc_device_t s_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x82F0,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

/* ── USB-C between the serial/JTAG unit and the OTG controller ── */

static void phy_to_otg(void)
{
    LP_SYS.usb_ctrl.sw_hw_usb_phy_sel = 1;
    LP_SYS.usb_ctrl.sw_usb_phy_sel = 1;            /* serial/JTAG -> PHY 1 (not wired), OTG FS -> PHY 0 */
    USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;      /* keep serial/JTAG off PHY 1's pins */
}

static void phy_to_jtag(void)
{
    LP_SYS.usb_ctrl.sw_usb_phy_sel = 0;
    USB_SERIAL_JTAG.conf0.usb_pad_enable = 1;
}

#define DIRTY_MAGIC 0x48494444u          /* "HIDD": the app had the port and did not give it back */
static RTC_NOINIT_ATTR uint32_t s_dirty;

static void reset_otg_fs(void)
{
    LP_AON_CLKRST.hp_usb_clkrst_ctrl1.rst_en_usb_otg11 = 1;   /* USB_WRAP and the DWC FS controller */
    esp_rom_delay_us(10);
    LP_AON_CLKRST.hp_usb_clkrst_ctrl1.rst_en_usb_otg11 = 0;
}

static void restore_on_shutdown(void)
{
    phy_to_jtag();
    reset_otg_fs();
    s_dirty = 0;
}

__attribute__((constructor)) static void restore_at_boot(void)
{
    if (s_dirty != DIRTY_MAGIC && !LP_SYS.usb_ctrl.sw_usb_phy_sel) return;
    reset_otg_fs();
    LP_AON_CLKRST.hp_usb_clkrst_ctrl1.rst_en_usb_device = 1;   /* the serial/JTAG unit, back to its reset state */
    esp_rom_delay_us(10);
    LP_AON_CLKRST.hp_usb_clkrst_ctrl1.rst_en_usb_device = 0;
    USB_SERIAL_JTAG.conf0.phy_sel = 0;
    phy_to_jtag();
    s_dirty = 0;
}

/* ── the TinyUSB task ── */

static void usb_task(void *arg)
{
    (void)arg;
    report_t r;
    bool have = false;
    int64_t last_sent = 0;
    while (!s_task_quit) {
        tud_task_ext(1, false);
        if (!tud_mounted()) {
            xQueueReset(s_queue);
            have = false;
            continue;
        }
        if (!have) have = xQueueReceive(s_queue, &r, 0) == pdTRUE;
        if (!have) continue;
        if (tud_suspended()) {
            tud_remote_wakeup();                   /* a key wakes a sleeping host if it allowed that */
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        if (!tud_hid_ready()) continue;
        if (r.wait_ms && esp_timer_get_time() - last_sent < (int64_t)r.wait_ms * 1000) continue;
        if (tud_hid_get_protocol() == HID_PROTOCOL_BOOT) {
            if (s_boot_id && r.id == s_boot_id) tud_hid_report(0, r.data, r.len);   /* boot: keyboard only, no ID */
        } else {
            tud_hid_report(r.id, r.data, r.len);
        }
        last_sent = esp_timer_get_time();
        have = false;
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

/* ── HIDD ── */

static void hidd_stop(void)
{
    if (!s_running) return;
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(20));
    s_task_quit = true;
    for (int i = 0; i < 50 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(5));
    tusb_deinit(RHPORT);
    if (s_phy) {
        usb_del_phy(s_phy);
        s_phy = NULL;
    }
    phy_to_jtag();
    reset_otg_fs();                                 /* nothing of the app's device left running */
    s_dirty = 0;
    s_running = false;
    lx_devlink_pause(false);
    s_leds = 0;
    ESP_LOGI(TAG, "USB-C back to the launcher");
}

static int hidd_start(const papp_hidd_cfg_t *cfg)
{
#if !CONFIG_IDF_TARGET_ESP32P4
    (void)cfg;
    return PAPP_E_UNSUPPORTED;
#else
    if (!cfg || !cfg->report_desc || !cfg->report_desc_len || cfg->report_desc_len > REPORT_DESC_MAX) return PAPP_E_ARG;
    if (s_running) hidd_stop();
    memcpy(s_report_desc, cfg->report_desc, cfg->report_desc_len);
    s_report_desc_len = cfg->report_desc_len;
    s_boot_id = cfg->boot_keyboard_id;
    snprintf(s_manufacturer, sizeof(s_manufacturer), "%s", cfg->manufacturer ? cfg->manufacturer : "P4 Launcher");
    snprintf(s_product, sizeof(s_product), "%s", cfg->product ? cfg->product : "P4 Launcher app");
    s_device.idVendor = cfg->vid ? cfg->vid : 0x303A;
    s_device.idProduct = cfg->pid ? cfg->pid : 0x82F0;
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_BASE);
    snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const uint8_t config[] = {
        TUD_CONFIG_DESCRIPTOR(1, 1, 0, sizeof(s_config), TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
        TUD_HID_DESCRIPTOR(0, 0, s_boot_id ? HID_ITF_PROTOCOL_KEYBOARD : HID_ITF_PROTOCOL_NONE, 0, EP_HID, 16, 1),
    };
    memcpy(s_config, config, sizeof(s_config));
    /* the report descriptor's length, which the macro took as a constant: bytes 7..8 of the HID descriptor */
    uint8_t *hid = s_config + TUD_CONFIG_DESC_LEN + 9;
    hid[7] = (uint8_t)(s_report_desc_len & 0xFF);
    hid[8] = (uint8_t)(s_report_desc_len >> 8);
    if (s_boot_id) s_config[TUD_CONFIG_DESC_LEN + 6] = HID_SUBCLASS_BOOT;   /* the interface's subclass */

    if (!s_queue) s_queue = xQueueCreate(128, sizeof(report_t));
    if (!s_queue) return PAPP_E_NOMEM;
    xQueueReset(s_queue);
    usb_phy_config_t phy = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
        .otg_speed = USB_PHY_SPEED_FULL,
    };
    if (usb_new_phy(&phy, &s_phy) != ESP_OK) return PAPP_E_UNSUPPORTED;
    static bool hooked;
    if (!hooked) hooked = esp_register_shutdown_handler(restore_on_shutdown) == ESP_OK;
    ESP_LOGI(TAG, "USB-C goes to the app as \"%s\": the launcher's USB link is away until it ends", s_product);
    vTaskDelay(pdMS_TO_TICKS(50));                  /* let that line out first */
    lx_devlink_pause(true);                         /* nothing more for the serial/JTAG unit */
    s_dirty = DIRTY_MAGIC;
    phy_to_otg();
    tusb_rhport_init_t init = { .role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL };
    if (!tusb_init(RHPORT, &init)) {
        usb_del_phy(s_phy);
        s_phy = NULL;
        phy_to_jtag();
        reset_otg_fs();
        s_dirty = 0;
        lx_devlink_pause(false);
        return PAPP_E_IO;
    }
    s_task_quit = false;
    if (xTaskCreatePinnedToCore(usb_task, "lx_usbd", 4096, NULL, 5, &s_task, 0) != pdPASS) {
        tusb_deinit(RHPORT);
        usb_del_phy(s_phy);
        s_phy = NULL;
        phy_to_jtag();
        reset_otg_fs();
        s_dirty = 0;
        lx_devlink_pause(false);
        return PAPP_E_NOMEM;
    }
    s_running = true;
    return PAPP_E_OK;
#endif
}

static int hidd_report(uint8_t id, const void *data, uint8_t len, uint16_t wait_ms)
{
    if (!s_running) return PAPP_E_UNSUPPORTED;
    if (len > 16 || (len && !data)) return PAPP_E_ARG;
    if (!tud_mounted()) return PAPP_E_OK;           /* nobody to tell */
    report_t r = { .id = id, .len = len, .wait_ms = wait_ms };
    if (len) memcpy(r.data, data, len);
    return xQueueSend(s_queue, &r, tud_suspended() ? 0 : pdMS_TO_TICKS(100)) == pdTRUE ? PAPP_E_OK : PAPP_E_LIMIT;
}

static uint8_t hidd_leds(void) { return s_running ? s_leds : 0; }

static int hidd_state(void)
{
    if (!s_running) return 0;
    int s = PAPP_HIDD_RUNNING;
    if (tud_mounted()) s |= PAPP_HIDD_MOUNTED;
    if (tud_suspended()) s |= PAPP_HIDD_SUSPENDED;
    if (tud_mounted() && tud_hid_get_protocol() == HID_PROTOCOL_BOOT) s |= PAPP_HIDD_BOOT;
    return s;
}

const papp_if_hidd_t lx_if_hidd = {
    .hdr = { PAPP_IF_HIDD, PAPP_IF_HIDD_VER, sizeof(papp_if_hidd_t) },
    .start = hidd_start,
    .stop = hidd_stop,
    .report = hidd_report,
    .leds = hidd_leds,
    .state = hidd_state,
};

void lx_usbd_end_run(void)
{
    if (s_running) hidd_stop();
}

/* ── TinyUSB's callbacks ── */

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&s_device; }
const uint8_t *tud_descriptor_configuration_cb(uint8_t index) { (void)index; return s_config; }
const uint8_t *tud_hid_descriptor_report_cb(uint8_t itf) { (void)itf; return s_report_desc; }

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[33];
    const char *s;
    switch (index) {
    case 0: buf[1] = 0x0409; buf[0] = (TUSB_DESC_STRING << 8) | 4; return buf;
    case 1: s = s_manufacturer; break;
    case 2: s = s_product; break;
    case 3: s = s_serial; break;
    default: return NULL;
    }
    size_t n = strlen(s);
    if (n > 32) n = 32;
    for (size_t i = 0; i < n; i++) buf[1 + i] = (uint8_t)s[i];
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return buf;
}

uint16_t tud_hid_get_report_cb(uint8_t itf, uint8_t id, hid_report_type_t type, uint8_t *buf, uint16_t len)
{
    (void)itf; (void)id; (void)type; (void)buf; (void)len;
    return 0;
}

/* Output reports: the keyboard's LEDs (Num 1, Caps 2, Scroll 4), with or without a report ID. */
void tud_hid_set_report_cb(uint8_t itf, uint8_t id, hid_report_type_t type, const uint8_t *buf, uint16_t len)
{
    (void)itf;
    if (type != HID_REPORT_TYPE_OUTPUT || !len) return;
    if (id == 0 && len >= 2 && s_boot_id && buf[0] == s_boot_id) s_leds = buf[1];
    else if (id == 0 || id == s_boot_id) s_leds = buf[0];
}
