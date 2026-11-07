/*
 * FT5x06 capacitive touch at I2C 0x38 on the shared bus. Polled (no interrupt line); the ATTINY88
 * releases its reset in bsp_display_init. It reports in panel pixels (800x480), mirrored in both
 * axes on this panel; the panel is landscape, so that is all the mapping there is.
 */
#include "board_internal.h"
#include "board_pins.h"

#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define FT_REG_DEVICE_MODE  0x00
#define FT_REG_TD_STATUS    0x02    /* points down, then 6 bytes per point */
#define FT_REG_G_MODE       0xA4    /* 0 = polling */
#define FT_REG_FIRMWARE     0xA6
#define FT_REG_VENDOR       0xA8
#define FT_POINT_SIZE       6
#define FT_EVENT_UP         1

static i2c_master_dev_handle_t s_dev;

static esp_err_t ft_read(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, len, 50);
}

static esp_err_t ft_write8(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, 2, 50);
}

esp_err_t bsp_touch_init(void)
{
    if (s_dev) return ESP_OK;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) return ESP_ERR_INVALID_STATE;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_TOUCH_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) return ESP_FAIL;

    /* Just out of reset: give the controller up to 300 ms to answer */
    uint8_t id[3] = { 0 };
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < 6 && err != ESP_OK; i++) {
        if (i) vTaskDelay(pdMS_TO_TICKS(50));
        err = ft_read(FT_REG_FIRMWARE, id, 1);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "FT5x06 not found on I2C");
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    ft_read(FT_REG_VENDOR, id + 1, 1);
    ft_write8(FT_REG_DEVICE_MODE, 0);   /* normal operation */
    ft_write8(FT_REG_G_MODE, 0);        /* polling, no interrupt pulses */
    ESP_LOGI(TAG, "FT5x06 at 0x%02X, firmware 0x%02X, vendor 0x%02X", BOARD_TOUCH_ADDR, id[0], id[1]);

    board_set_cap(BSP_CAP_TOUCH);
    return ESP_OK;
}

int bsp_touch_read(bsp_touch_point_t *pts, int max)
{
    if (!s_dev) return -1;
    uint8_t raw[1 + BSP_TOUCH_MAX_POINTS * FT_POINT_SIZE];
    if (ft_read(FT_REG_TD_STATUS, raw, sizeof(raw)) != ESP_OK) return -1;

    int n = raw[0] & 0x0F;
    if (n > BSP_TOUCH_MAX_POINTS) n = 0;    /* 0x0F: no valid data */
    const bsp_info_t *b = bsp_info();
    int out = 0;
    for (int i = 0; i < n && out < max; i++) {
        const uint8_t *p = raw + 1 + i * FT_POINT_SIZE;
        if (p[0] >> 6 == FT_EVENT_UP) continue;
        int x = (p[0] & 0x0F) << 8 | p[1];
        int y = (p[2] & 0x0F) << 8 | p[3];
        if (x >= b->display_w) x = b->display_w - 1;
        if (y >= b->display_h) y = b->display_h - 1;
        pts[out++] = (bsp_touch_point_t){
            .x = b->display_w - 1 - x,
            .y = b->display_h - 1 - y,
            .size = p[4],
            .id = p[2] >> 4,
        };
    }
    return out;
}
