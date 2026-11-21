/*
 * The clip-on Tab5 Keyboard (M5Stack): an I2C key controller at 0x6D on the EXT socket (G0 SDA, G1 SCL), a bus
 * of its own (I2C port 0; the board's devices are on port 1). Looked for every 0.7 s while it is away, checked every 3 s while it is there (it can be
 * clipped on and off at any time), and read whenever the input task polls: the count of queued key events, then
 * the events, one byte each. Its interrupt line is not used. tab5kbd.h turns the events into keyboard reports.
 *
 * Set up as M5's own keyboard firmware documents: key mode (0x10 = 0), which reports each key going down and up,
 * the event queue emptied (0x02 = 0), interrupt configuration 0x07. Its two LEDs are left to the keyboard.
 */
#include "board_internal.h"
#include "tab5kbd.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "bsp_keyboard";

#define KBD_I2C_PORT      0            /* the board's own bus is port 1 */
#define KBD_I2C_SDA       0
#define KBD_I2C_SCL       1
#define KBD_ADDR          0x6D
#define KBD_REG_INT_CFG   0x00
#define KBD_REG_EVENT_NUM 0x02
#define KBD_REG_MODE      0x10
#define KBD_REG_EVENT     0x20
#define KBD_REG_FIRMWARE  0xFE
#define KBD_PROBE_MS      700
#define KBD_CHECK_MS      3000

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static bool s_present;
static int64_t s_next_probe, s_next_check;
static tab5kbd_t s_kbd;

/* The EXT socket's I2C bus (G0/G1): made once and kept, as the keyboard can be clipped on at any time. */
static i2c_master_bus_handle_t ext_bus(void)
{
    if (s_bus) return s_bus;
    i2c_master_bus_config_t cfg = {
        .i2c_port = KBD_I2C_PORT, .sda_io_num = KBD_I2C_SDA, .scl_io_num = KBD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,              /* keep an empty socket quiet */
    };
    if (i2c_new_master_bus(&cfg, &s_bus) != ESP_OK) s_bus = NULL;
    return s_bus;
}

static bool reg_write(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = { reg, v };
    return i2c_master_transmit(s_dev, b, 2, 20) == ESP_OK;
}

static bool reg_read(uint8_t reg, uint8_t *v)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, v, 1, 20) == ESP_OK;
}

static bool answers(i2c_master_bus_handle_t bus)
{
    return i2c_master_probe(bus, KBD_ADDR, 20) == ESP_OK;
}

static void found(i2c_master_bus_handle_t bus)
{
    if (!s_dev) {
        i2c_device_config_t dev = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = KBD_ADDR, .scl_speed_hz = 400000 };
        if (i2c_master_bus_add_device(bus, &dev, &s_dev) != ESP_OK) { s_dev = NULL; return; }
    }
    uint8_t fw = 0;
    if (!reg_write(KBD_REG_MODE, 0) || !reg_write(KBD_REG_EVENT_NUM, 0) || !reg_write(KBD_REG_INT_CFG, 0x07) ||
        !reg_read(KBD_REG_FIRMWARE, &fw)) {
        ESP_LOGW(TAG, "0x%02X answers on the EXT socket but did not take the keyboard's set-up", KBD_ADDR);
        return;
    }
    tab5kbd_reset(&s_kbd);
    s_present = true;
    ESP_LOGI(TAG, "Tab5 Keyboard: firmware %u, at 0x%02X on G0/G1", fw, KBD_ADDR);
}

int bsp_keyboard_read(bool *present, uint8_t *mods, uint8_t keys[6])
{
    tab5kbd_report_t r;
    if (tab5kbd_take(&s_kbd, &r)) goto give;            /* reports from the last read go out first, in order */
    i2c_master_bus_handle_t bus = ext_bus();
    if (bus) {
        int64_t now = esp_timer_get_time() / 1000;
        if (!s_present) {
            if (now >= s_next_probe) {
                s_next_probe = now + KBD_PROBE_MS;
                if (answers(bus)) found(bus);
            }
        } else {
            if (now >= s_next_check) {
                s_next_check = now + KBD_CHECK_MS;
                if (!answers(bus)) {
                    ESP_LOGI(TAG, "Tab5 Keyboard: gone");
                    tab5kbd_release_all(&s_kbd);           /* whatever was held comes up */
                    s_present = false;
                }
            }
            uint8_t n = 0;
            if (s_present && reg_read(KBD_REG_EVENT_NUM, &n) && n > 0 && n <= 32)
                for (int i = 0; i < n; i++) {
                    uint8_t v;
                    if (!reg_read(KBD_REG_EVENT, &v) || v == 0xFF) break;   /* 0xFF: the queue ran dry */
                    tab5kbd_event(&s_kbd, v, (uint32_t)now);
                }
        }
    }
    *present = s_present;
    if (!tab5kbd_take(&s_kbd, &r)) return 0;
give:
    *present = s_present;
    *mods = r.mods;
    memcpy(keys, r.keys, 6);
    return 1;
}
