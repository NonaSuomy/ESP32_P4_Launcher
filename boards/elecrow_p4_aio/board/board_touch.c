/*
 * GT911 capacitive touch over the shared I2C bus, polled. Reset on BOARD_TP_RST with INT held
 * low first (address 0x5D), as Elecrow's code does. Raw points are in the controller's
 * configured resolution, panel native orientation (landscape here); BOARD_LCD_FLIP turns them.
 */
#include "board_internal.h"
#include "board_pins.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define GT911_ADDR_A      0x5D
#define GT911_ADDR_B      0x14
#define GT911_REG_CONFIG  0x8047    /* config version, then x_max u16, y_max u16 */
#define GT911_REG_PID     0x8140
#define GT911_REG_STATUS  0x814E
#define GT911_REG_POINTS  0x814F
#define GT911_POINT_SIZE  8

static i2c_master_dev_handle_t s_dev;
static uint16_t s_raw_w = BOARD_LCD_H_RES, s_raw_h = BOARD_LCD_V_RES;
static bsp_touch_point_t s_last[BSP_TOUCH_MAX_POINTS];   /* last report: the GT911 only */
static int s_last_n;                                     /* flags data when it changes  */

static esp_err_t gt_read(uint16_t reg, uint8_t *buf, size_t len)
{
    uint8_t addr[2] = { reg >> 8, reg & 0xFF };
    return i2c_master_transmit_receive(s_dev, addr, 2, buf, len, 50);
}

static esp_err_t gt_write8(uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = { reg >> 8, reg & 0xFF, val };
    return i2c_master_transmit(s_dev, buf, 3, 50);
}

esp_err_t bsp_touch_init(void)
{
    if (s_dev) return ESP_OK;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;

    /* Reset with INT low: the GT911 then answers at 0x5D */
    gpio_config_t io = { .pin_bit_mask = (1ULL << BOARD_TP_RST) | (1ULL << BOARD_TP_INT), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(BOARD_TP_INT, 0);
    gpio_set_level(BOARD_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_direction(BOARD_TP_INT, GPIO_MODE_INPUT);
    vTaskDelay(pdMS_TO_TICKS(60));

    /* Talk to each address rather than probing (a probe can upset the controller) */
    static const uint16_t addrs[] = { GT911_ADDR_A, GT911_ADDR_B };
    uint8_t pid[4] = { 0 };
    uint8_t cfg[5] = { 0 };
    uint16_t addr = 0;
    for (size_t i = 0; i < sizeof(addrs) / sizeof(addrs[0]) && !addr; i++) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 400000,
        };
        if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) continue;
        if (gt_read(GT911_REG_PID, pid, sizeof(pid)) == ESP_OK && gt_read(GT911_REG_CONFIG, cfg, sizeof(cfg)) == ESP_OK) {
            addr = addrs[i];
        } else {
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
        }
    }
    if (!addr) {
        ESP_LOGW(TAG, "GT911 not found on I2C");
        return ESP_ERR_NOT_FOUND;
    }
    uint16_t w = cfg[1] | cfg[2] << 8, h = cfg[3] | cfg[4] << 8;
    if (w && h) { s_raw_w = w; s_raw_h = h; }
    ESP_LOGI(TAG, "GT%.4s at 0x%02X, config v%u, %ux%u", (char *)pid, addr, cfg[0], s_raw_w, s_raw_h);

    board_set_cap(BSP_CAP_TOUCH);
    return ESP_OK;
}

int bsp_touch_read(bsp_touch_point_t *pts, int max)
{
    if (!s_dev) return -1;
    uint8_t status;
    if (gt_read(GT911_REG_STATUS, &status, 1) != ESP_OK) return -1;
    if (!(status & 0x80)) goto out;     /* nothing new since the last report */

    int n = status & 0x0F;
    if (n > BSP_TOUCH_MAX_POINTS) n = BSP_TOUCH_MAX_POINTS;
    uint8_t raw[BSP_TOUCH_MAX_POINTS * GT911_POINT_SIZE];
    esp_err_t err = n ? gt_read(GT911_REG_POINTS, raw, n * GT911_POINT_SIZE) : ESP_OK;
    gt_write8(GT911_REG_STATUS, 0);
    if (err != ESP_OK) return -1;

    const bsp_info_t *b = bsp_info();
    for (int i = 0; i < n; i++) {
        const uint8_t *p = raw + i * GT911_POINT_SIZE;
        /* raw -> panel native pixels */
        int px = (p[1] | p[2] << 8) * b->display_w / s_raw_w;
        int py = (p[3] | p[4] << 8) * b->display_h / s_raw_h;
        if (px >= b->display_w) px = b->display_w - 1;
        if (py >= b->display_h) py = b->display_h - 1;
#if BOARD_LCD_FLIP
        px = b->display_w - 1 - px;     /* panel scanned the other way round */
        py = b->display_h - 1 - py;
#endif
        /* panel -> landscape: inverse of the mapping documented in bsp.h */
        int x = px, y = py;
        if (b->display_rot == 270)     { x = py; y = b->display_w - 1 - px; }
        else if (b->display_rot == 90) { x = b->display_h - 1 - py; y = px; }
        s_last[i] = (bsp_touch_point_t){ .x = x, .y = y, .size = p[5] | p[6] << 8, .id = p[0] };
    }
    s_last_n = n;
out:
    n = s_last_n < max ? s_last_n : max;
    memcpy(pts, s_last, n * sizeof(*pts));
    return n;
}
