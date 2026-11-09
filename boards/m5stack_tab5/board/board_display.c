/*
 * BSP display â€” Tab5 panel, 720x1280 portrait MIPI-DSI (2 lanes), double-buffered DPI.
 * Tab5 units ship with one of three panels, told apart by their touch controller:
 * ILI9881C + GT911, or ST7123 / ST7121 with built-in touch at 0x55 (its firmware version
 * register says which). Timings and init tables from M5Stack's BSP (M5Tab5-UserDemo).
 */
#include "board_internal.h"
#include "board_pins.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_ili9881c.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ili9881c_tab5_init.h"
#include "st712x_tab5_init.h"

static const char *TAG = "bsp_disp";

#define NUM_FBS 2

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fbs[NUM_FBS];
static int s_visible;
static volatile bool s_flip_pending;
static SemaphoreHandle_t s_vsync;

static bool IRAM_ATTR on_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata,
                                      void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync, &woken);
    return woken == pdTRUE;
}

static void backlight_init(void)
{
    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&t);
    ledc_channel_config_t c = {
        .gpio_num = BOARD_LCD_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
    };
    ledc_channel_config(&c);
}

/* â”€â”€ Which panel â”€â”€ */

static int s_panel_type = -1;

board_panel_t board_panel(void)
{
    if (s_panel_type >= 0) return s_panel_type;
    s_panel_type = BOARD_PANEL_ILI9881C;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus || i2c_master_probe(bus, 0x5D, 50) == ESP_OK || i2c_master_probe(bus, 0x14, 50) == ESP_OK ||
        i2c_master_probe(bus, 0x55, 50) != ESP_OK)
        return s_panel_type;                    /* GT911 (or nothing found: the original panel) */
    s_panel_type = BOARD_PANEL_ST7123;          /* M5Stack's fallback for 0x55 too */
    i2c_device_config_t cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = 0x55, .scl_speed_hz = 100000 };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &cfg, &dev) == ESP_OK) {
        uint8_t reg[2] = { 0, 0 }, fw = 0;      /* firmware version: 1 = ST7121, 3 = ST7123 */
        if (i2c_master_transmit_receive(dev, reg, 2, &fw, 1, 50) == ESP_OK) {
            if (fw == 1) s_panel_type = BOARD_PANEL_ST7121;
            ESP_LOGI(TAG, "touch at 0x55, firmware version %u", fw);
        }
        i2c_master_bus_rm_device(dev);
    }
    return s_panel_type;
}

const char *board_panel_name(void)
{
    static const char *const names[] = { "ILI9881C", "ST7123", "ST7121" };
    return names[board_panel()];
}

/* ST7123 / ST7121: a plain DPI panel, set up by commands over DBI before the video starts */
static esp_err_t st712x_init(esp_lcd_dsi_bus_handle_t dsi, esp_lcd_panel_io_handle_t io, bool st7121)
{
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 70,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = NUM_FBS,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_pulse_width = 2,
            .hsync_back_porch = 40,
            .hsync_front_porch = 40,
            .vsync_pulse_width = st7121 ? 20 : 2,
            .vsync_back_porch = st7121 ? 24 : 8,
            .vsync_front_porch = st7121 ? 200 : 220,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(dsi, &dpi_cfg, &s_panel), TAG, "DPI panel");
    if (st7121) {                               /* reset is on the I/O expander: a software reset */
        esp_lcd_panel_io_tx_param(io, 0x01, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(120));
    } else {
        uint8_t id[3] = { 0 };
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_lcd_panel_io_rx_param(io, 0x04, id, 3);
        ESP_LOGI(TAG, "ST7123 ID %02X %02X %02X", id[0], id[1], id[2]);
    }
    const tab5_panel_cmd_t *cmds = st7121 ? tab5_st7121_init : tab5_st7123_init;
    int n = st7121 ? sizeof(tab5_st7121_init) / sizeof(*cmds) : sizeof(tab5_st7123_init) / sizeof(*cmds);
    for (int i = 0; i < n; i++) {
        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, cmds[i].cmd, cmds[i].data, cmds[i].data_bytes), TAG,
                            "command %02X", cmds[i].cmd);
        vTaskDelay(pdMS_TO_TICKS(cmds[i].delay_ms + (st7121 ? 5 : 0)));
    }
    return esp_lcd_panel_init(s_panel);         /* starts the video stream */
}

static esp_err_t ili9881c_init(esp_lcd_dsi_bus_handle_t dsi, esp_lcd_panel_io_handle_t io)
{
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel = 0,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = 60,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = NUM_FBS,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_back_porch = 140,
            .hsync_pulse_width = 40,
            .hsync_front_porch = 40,
            .vsync_back_porch = 20,
            .vsync_pulse_width = 4,
            .vsync_front_porch = 20,
        },
    };
    ili9881c_vendor_config_t vendor = {
        .init_cmds = tab5_lcd_ili9881c_specific_init_code_default,
        .init_cmds_size = sizeof(tab5_lcd_ili9881c_specific_init_code_default) /
                          sizeof(tab5_lcd_ili9881c_specific_init_code_default[0]),
        .mipi_config = { .dsi_bus = dsi, .dpi_config = &dpi_cfg, .lane_num = 2 },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = -1,                   /* reset is on I/O expander 1, released in bsp_init */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ili9881c(io, &dev_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    esp_lcd_panel_disp_on_off(s_panel, true);
    return ESP_OK;
}

esp_err_t bsp_display_init(void)
{
    if (s_panel) return ESP_OK;
    board_panel_t type = board_panel();
    backlight_init();

    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = BOARD_DSI_PHY_LDO_CHAN, .voltage_mv = BOARD_DSI_PHY_LDO_MV };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "DSI PHY LDO");

    esp_lcd_dsi_bus_handle_t dsi;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = 0,       /* esp_lcd picks: PLL_F20M on v1.x, XTAL on v3.x (the legacy source aborts there) */
        .lane_bit_rate_mbps = type == BOARD_PANEL_ILI9881C ? BOARD_DSI_LANE_MBPS : 965,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &dsi), TAG, "DSI bus");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi_cfg = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(dsi, &dbi_cfg, &io), TAG, "DBI io");

    if (type == BOARD_PANEL_ILI9881C) ESP_RETURN_ON_ERROR(ili9881c_init(dsi, io), TAG, "ILI9881C");
    else ESP_RETURN_ON_ERROR(st712x_init(dsi, io, type == BOARD_PANEL_ST7121), TAG, "%s", board_panel_name());

    void *fb0 = NULL, *fb1 = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, NUM_FBS, &fb0, &fb1), TAG, "fbs");
    s_fbs[0] = fb0;
    s_fbs[1] = fb1;

    s_vsync = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_refresh_done = on_refresh_done };
    esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL);

    size_t bytes = BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2;
    for (int i = 0; i < NUM_FBS; i++) {
        memset(s_fbs[i], 0, bytes);
        esp_cache_msync(s_fbs[i], bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    bsp_display_show(0);
    bsp_display_backlight(100);
    board_set_cap(BSP_CAP_DISPLAY);

    ESP_LOGI(TAG, "%s %dx%d up, fb0=%p fb1=%p", board_panel_name(), BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fbs[0], s_fbs[1]);
    return ESP_OK;
}

int bsp_display_fb_count(void)
{
    return s_panel ? NUM_FBS : 0;
}

uint16_t *bsp_display_fb(int index)
{
    return (s_panel && index >= 0 && index < NUM_FBS) ? s_fbs[index] : NULL;
}

esp_err_t bsp_display_show(int index)
{
    if (!s_panel || index < 0 || index >= NUM_FBS) return ESP_ERR_INVALID_ARG;
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fbs[index]);
    if (err == ESP_OK) {
        s_visible = index;
        xSemaphoreTake(s_vsync, 0);
        s_flip_pending = true;
    }
    return err;
}

bool bsp_display_wait_flip(uint32_t timeout_ms)
{
    if (!s_flip_pending) return true;
    bool ok = xSemaphoreTake(s_vsync, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    if (ok) s_flip_pending = false;
    return ok;
}

int bsp_display_visible(void)
{
    return s_visible;
}

bool bsp_display_wait_vsync(uint32_t timeout_ms)
{
    if (!s_vsync) return false;
    xSemaphoreTake(s_vsync, 0);
    return xSemaphoreTake(s_vsync, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void bsp_display_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023 * percent / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
