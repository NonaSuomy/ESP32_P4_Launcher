#define PAPP_APP_SIDE 1
#include "papp/papp.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

PAPP_MANIFEST(.id="tool.ble_input_test", .name="BLE Input Test",
              .version=PAPP_APP_VERSION, .main_stack=32768);

#define WIDTH 1024
#define HEIGHT 600
#define MAX_NOTIFY 16

static const papp_if_ble_t *ble;
static papp_surface_t *surface;
static lv_display_t *display;
static lv_indev_t *pointer;
static uint16_t *canvas;
static lv_obj_t *status_label, *device_label, *report_label, *detail_label;
static papp_ble_conn_t connection = PAPP_BLE_CONN_NONE;
static uint16_t notify_handles[MAX_NOTIFY];
static unsigned notify_count;
static bool connected, scanning, rescan, quitting;
static uint32_t scan_count, report_count;
static char status_text[128] = "Starting Bluetooth LE…";
static char device_text[96] = "Waiting for PAX BLE Remote";
static char report_text[160] = "No HID input received yet";
static char detail_text[200] = "Enable BLE Remote on another P4 to test keyboard, mouse and gamepad reports.";

static uint32_t ticks(void) { return (uint32_t)(papp_time_us() / 1000); }
static void flush(lv_display_t *d, const lv_area_t *area, uint8_t *pixels) {
    (void)area;
    papp_disp_present_ext(surface, pixels);
    lv_display_flush_ready(d);
}
static void read_touch(lv_indev_t *in, lv_indev_data_t *data) {
    (void)in;
    papp_touch_point_t point;
    if (papp_disp_touch(surface, &point, 1) > 0) {
        data->point.x = point.x;
        data->point.y = point.y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else data->state = LV_INDEV_STATE_RELEASED;
}
static void refresh_event(lv_event_t *event) {
    (void)event;
    rescan = true;
}
static void close_event(lv_event_t *event) {
    (void)event;
    quitting = true;
}
static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y,
                            const lv_font_t *font, lv_color_t color) {
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}
static void build_ui(void) {
    lv_obj_t *root = lv_screen_active();
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x081827), 0);
    lv_obj_set_style_text_color(root, lv_color_hex(0xe7f3ff), 0);
    make_label(root, "BLE Input Test", 40, 24, &lv_font_montserrat_28, lv_color_hex(0x70d8ff));
    make_label(root, "Connect another P4 running PAX BLE Remote. Incoming HID reports appear below.",
               42, 72, &lv_font_montserrat_14, lv_color_hex(0xa9bfd0));
    status_label = make_label(root, status_text, 42, 126, &lv_font_montserrat_20, lv_color_hex(0xffd166));
    device_label = make_label(root, device_text, 42, 184, &lv_font_montserrat_20, lv_color_hex(0xffffff));
    lv_obj_set_width(device_label, 900);
    report_label = make_label(root, report_text, 42, 260, &lv_font_montserrat_28, lv_color_hex(0x8af0a5));
    lv_obj_set_width(report_label, 930);
    detail_label = make_label(root, detail_text, 42, 350, &lv_font_montserrat_14, lv_color_hex(0xa9bfd0));
    lv_obj_set_width(detail_label, 920);
    lv_obj_t *scan = lv_button_create(root);
    lv_obj_set_pos(scan, 42, 500);
    lv_obj_set_size(scan, 220, 58);
    lv_obj_set_style_bg_color(scan, lv_color_hex(0x185b7b), 0);
    lv_obj_add_event_cb(scan, refresh_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_text = lv_label_create(scan);
    lv_label_set_text(scan_text, "Scan Again");
    lv_obj_center(scan_text);
    lv_obj_t *close = lv_button_create(root);
    lv_obj_set_pos(close, 780, 500);
    lv_obj_set_size(close, 200, 58);
    lv_obj_set_style_bg_color(close, lv_color_hex(0x344458), 0);
    lv_obj_add_event_cb(close, close_event, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_text = lv_label_create(close);
    lv_label_set_text(close_text, "Close");
    lv_obj_center(close_text);
}
static void ui_update(void) {
    if (status_label) lv_label_set_text(status_label, status_text);
    if (device_label) lv_label_set_text(device_label, device_text);
    if (report_label) lv_label_set_text(report_label, report_text);
    if (detail_label) lv_label_set_text(detail_label, detail_text);
}
static void report_received(const papp_ble_event_t *event) {
    char formatted[160];
    const uint8_t *d = event->data;
    const unsigned n = event->len;
    report_count++;
    if (n == 6 && d[0] == 3) {
        unsigned buttons = (unsigned)d[1] | ((unsigned)d[2] << 8);
        snprintf(formatted, sizeof(formatted), "GAMEPAD  buttons %04X  hat %u  X %d  Y %d",
                 buttons, d[3], (int8_t)d[4], (int8_t)d[5]);
        snprintf(detail_text, sizeof(detail_text), "Report %lu · GATT handle %u · %u bytes",
                 (unsigned long)report_count, (unsigned)event->handle, n);
    } else if (n >= 5 && d[0] == 2) {
        snprintf(formatted, sizeof(formatted), "MOUSE  buttons %02X  X %d  Y %d  wheel %d",
                 d[1], (int8_t)d[2], (int8_t)d[3], (int8_t)d[4]);
        snprintf(detail_text, sizeof(detail_text), "Report %lu · GATT handle %u · %u bytes",
                 (unsigned long)report_count, (unsigned)event->handle, n);
    } else if (n >= 9 && d[0] == 1) {
        snprintf(formatted, sizeof(formatted), "KEYBOARD  modifiers %02X  key usages %02X %02X %02X %02X %02X %02X",
                 d[1], d[3], d[4], d[5], d[6], d[7], d[8]);
        snprintf(detail_text, sizeof(detail_text), "Report %lu · GATT handle %u · report ID 1",
                 (unsigned long)report_count, (unsigned)event->handle);
    } else {
        snprintf(formatted, sizeof(formatted), "HID data (%u bytes): %02X %02X %02X %02X %02X %02X",
                 n, n > 0 ? d[0] : 0, n > 1 ? d[1] : 0, n > 2 ? d[2] : 0,
                 n > 3 ? d[3] : 0, n > 4 ? d[4] : 0, n > 5 ? d[5] : 0);
        snprintf(detail_text, sizeof(detail_text), "Report %lu · GATT handle %u · raw notification",
                 (unsigned long)report_count, (unsigned)event->handle);
    }
    snprintf(report_text, sizeof(report_text), "%s", formatted);
}
static void disconnect_remote(void) {
    if (connection != PAPP_BLE_CONN_NONE) ble->disconnect(connection);
    connection = PAPP_BLE_CONN_NONE;
    connected = false;
    notify_count = 0;
}
static void start_scan(void) {
    if (!ble || !ble->available()) {
        snprintf(status_text, sizeof(status_text), "Bluetooth LE is unavailable on this device.");
        snprintf(detail_text, sizeof(detail_text), "Check that the launcher firmware and C6 BLE co-processor support PAPP2 BLE.");
        ui_update();
        papp_delay_ms(1000);
        return;
    }
    if (connected) disconnect_remote();
    if (scanning) ble->scan_stop();
    scanning = false;
    papp_ble_scan_cfg_t cfg = { .active = 1, .filter_duplicates = 1, .duration_ms = 0 };
    int rc = ble->scan_start(&cfg);
    scanning = rc == 0;
    snprintf(status_text, sizeof(status_text), rc == 0 ? "Scanning for PAX BLE Remote…" : "BLE scan failed (%d)", rc);
    snprintf(device_text, sizeof(device_text), "Nearby BLE advertisements: %lu", (unsigned long)scan_count);
    ui_update();
}
static bool has_hid_service(papp_ble_found_svc_t *services, int count) {
    for (int i = 0; i < count; i++)
        if (services[i].uuid.len == 2 && services[i].uuid.u[0] == 0x12 && services[i].uuid.u[1] == 0x18)
            return true;
    return false;
}
static void connect_remote(const papp_ble_addr_t *address) {
    ble->scan_stop();
    scanning = false;
    snprintf(status_text, sizeof(status_text), "Found PAX BLE Remote; connecting…");
    ui_update();
    int rc = ble->connect(address, 8000, &connection);
    if (rc != 0) {
        snprintf(status_text, sizeof(status_text), "Connection failed (%d); retrying scan…", rc);
        connection = PAPP_BLE_CONN_NONE;
        return;
    }
    connected = true;
    snprintf(status_text, sizeof(status_text), "Connected · discovering HID reports…");
    ui_update();
    papp_ble_found_svc_t services[PAPP_BLE_MAX_SVCS];
    papp_ble_found_chr_t chars[PAPP_BLE_MAX_CHRS];
    int svc_count = 0, char_count = 0;
    rc = ble->discover(connection, services, PAPP_BLE_MAX_SVCS, &svc_count,
                       chars, PAPP_BLE_MAX_CHRS, &char_count, 12000);
    if (rc != 0 || !has_hid_service(services, svc_count)) {
        snprintf(status_text, sizeof(status_text), "Connected, but the HID service was not found.");
        snprintf(detail_text, sizeof(detail_text), "Discovery result %d · %d service(s) · %d characteristic(s)",
                 rc, svc_count, char_count);
        return;
    }
    notify_count = 0;
    for (int i = 0; i < char_count && notify_count < MAX_NOTIFY; i++) {
        if (!chars[i].cccd || !(chars[i].flags & (PAPP_BLE_NOTIFY | PAPP_BLE_INDICATE))) continue;
        rc = ble->subscribe(connection, chars[i].cccd,
                            (chars[i].flags & PAPP_BLE_NOTIFY) ? 1 : 2, 5000);
        if (rc == 0) notify_handles[notify_count++] = chars[i].handle;
    }
    snprintf(status_text, sizeof(status_text), "Connected · subscribed to %u HID report(s)", notify_count);
    snprintf(device_text, sizeof(device_text), "PAX BLE Remote · address %02X:%02X:%02X:%02X:%02X:%02X",
             address->a[5], address->a[4], address->a[3], address->a[2], address->a[1], address->a[0]);
    snprintf(detail_text, sizeof(detail_text), "Tap a key or move the remote's mouse/gamepad controls to see live reports.");
}
static void process_event(const papp_ble_event_t *event) {
    if (event->type == PAPP_BLE_EV_ADV && !connected) {
        scan_count++;
        int name_len = 0;
        const uint8_t *name = papp_ble_ad_find(event->data, event->len, 0x09, &name_len);
        if (!name) name = papp_ble_ad_find(event->data, event->len, 0x08, &name_len);
        if (name && name_len == (int)strlen("PAX BLE Remote") &&
            memcmp(name, "PAX BLE Remote", (size_t)name_len) == 0) {
            connect_remote(&event->addr);
        } else if ((scan_count % 8) == 0) {
            snprintf(device_text, sizeof(device_text), "Scanning… %lu BLE advertisements seen", (unsigned long)scan_count);
        }
    } else if (event->type == PAPP_BLE_EV_NOTIFY && connected) {
        report_received(event);
    } else if (event->type == PAPP_BLE_EV_SCAN_DONE) {
        scanning = false;
    } else if (event->type == PAPP_BLE_EV_DISCONNECTED && event->conn == connection) {
        connection = PAPP_BLE_CONN_NONE;
        connected = false;
        notify_count = 0;
        snprintf(status_text, sizeof(status_text), "Remote disconnected; scanning again…");
    }
}
static int app_run(void) {
    papp_disp_info_t info;
    if (!papp_disp_info(&info)) return -1;
    papp_surface_cfg_t cfg = { .width = WIDTH, .height = HEIGHT,
                               .format = PAPP_PIXFMT_RGB565, .scale = PAPP_SCALE_FIT_ASPECT,
                               .buffers = 1 };
    surface = papp_disp_surface_create(&cfg);
    if (!surface) return -2;
    canvas = papp_alloc(WIDTH * HEIGHT * sizeof(uint16_t), PAPP_MEM_LARGE | PAPP_MEM_DMA, 64);
    if (!canvas) { papp_disp_surface_destroy(surface); surface = NULL; return -3; }
    lv_init();
    lv_tick_set_cb(ticks);
    display = lv_display_create(WIDTH, HEIGHT);
    lv_display_set_default(display);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_buffers(display, canvas, NULL, WIDTH * HEIGHT * sizeof(uint16_t), LV_DISPLAY_RENDER_MODE_FULL);
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_touch);
    lv_indev_set_display(pointer, display);
    build_ui();
    ble = papp_ble();
    start_scan();
    while (!quitting && !papp_exit_requested()) {
        if (rescan) {
            rescan = false;
            scan_count = 0;
            start_scan();
        }
        papp_ble_event_t event;
        if (ble && ble->wait_event(&event, 40)) {
            process_event(&event);
            ui_update();
        } else if (!connected && !scanning) {
            static uint32_t last_scan_restart;
            uint32_t now = ticks();
            if (now - last_scan_restart > 500) {
                last_scan_restart = now;
                start_scan();
            }
        }
        lv_timer_handler();
        papp_delay_ms(8);
    }
    if (ble && scanning) ble->scan_stop();
    if (connected) disconnect_remote();
    lv_indev_delete(pointer);
    lv_display_delete(display);
    lv_deinit();
    papp_free(canvas);
    papp_disp_surface_destroy(surface);
    return 0;
}

int main(void) { return app_run(); }
