/*
 * FT6336 capacitive touch on the shared I2C bus (0x38), polled. The controller reports
 * portrait panel pixels (320x480, the orientation Waveshare's BSP draws in); they are
 * mapped here to the landscape frame the panel is scanned in, then to the virtual
 * 768x512 screen (see board_display.c).
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

#define FT_REG_TD_STATUS  0x02      /* number of points, then 6 bytes per point */
#define FT_REG_CHIP_ID    0xA3
#define FT_POINT_SIZE     6
#define FT_MAX_POINTS     2

static i2c_master_dev_handle_t s_dev;

static esp_err_t ft_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 50);
}

esp_err_t bsp_touch_init(void)
{
    if (s_dev) return ESP_OK;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;

    /* Reset pulse, as Waveshare's BSP does (active low) */
    gpio_config_t io = { .pin_bit_mask = 1ULL << BOARD_TP_RST, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    gpio_set_level(BOARD_TP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_TP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(300));

    if (i2c_master_probe(bus, BOARD_TP_ADDR, 50) != ESP_OK) {
        ESP_LOGW(TAG, "FT6336 not found on I2C");
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TP_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev), TAG, "add device");

    uint8_t id = 0;
    ft_read(FT_REG_CHIP_ID, &id, 1);
    ESP_LOGI(TAG, "FT6x36 at 0x%02X, chip id 0x%02X", BOARD_TP_ADDR, id);
    board_set_cap(BSP_CAP_TOUCH);
    return ESP_OK;
}

int bsp_touch_read(bsp_touch_point_t *pts, int max)
{
    if (!s_dev) return -1;
    uint8_t raw[1 + FT_MAX_POINTS * FT_POINT_SIZE];
    if (ft_read(FT_REG_TD_STATUS, raw, sizeof(raw)) != ESP_OK) return -1;

    int n = raw[0] & 0x0F;
    if (n > FT_MAX_POINTS) n = 0;       /* 0x0F etc.: no valid report */
    if (n > max) n = max;
    const bsp_info_t *b = bsp_info();
    for (int i = 0; i < n; i++) {
        const uint8_t *p = raw + 1 + i * FT_POINT_SIZE;
        int tx = (p[0] & 0x0F) << 8 | p[1];         /* portrait 0..319 */
        int ty = (p[2] & 0x0F) << 8 | p[3];         /* portrait 0..479 */
        if (tx >= BOARD_TP_RAW_W) tx = BOARD_TP_RAW_W - 1;
        if (ty >= BOARD_TP_RAW_H) ty = BOARD_TP_RAW_H - 1;
        /* portrait -> landscape, matching the panel's MADCTL in board_display.c */
        int x = ty, y = BOARD_TP_RAW_W - 1 - tx;
#if BOARD_LCD_FLIP_180
        x = BOARD_LCD_H_RES - 1 - x;
        y = BOARD_LCD_V_RES - 1 - y;
#endif
        /* panel pixels -> the virtual screen the launcher draws in */
        x = x * b->display_w / BOARD_LCD_H_RES;
        y = y * b->display_h / BOARD_LCD_V_RES;
        pts[i] = (bsp_touch_point_t){ .x = x, .y = y, .size = p[4], .id = p[2] >> 4 };
    }
    return n;
}
