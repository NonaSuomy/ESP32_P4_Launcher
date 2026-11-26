/*
 * BSP display — EK79007 1024x600 MIPI-DSI panel (2 lanes), landscape, double-buffered DPI.
 * Timing, lane rate and the scan-direction pins from Elecrow's ESP-IDF lessons; the panel
 * driver's own init table (esp_lcd_ek79007 default).
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
#include "esp_lcd_ek79007.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

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
        .freq_hz = 30000,
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

esp_err_t bsp_display_init(void)
{
    if (s_panel) return ESP_OK;
    backlight_init();

    /* Scan direction: Elecrow's levels give an upright picture in the kit's case */
    gpio_config_t dir = {
        .pin_bit_mask = (1ULL << BOARD_LCD_UPDN_GPIO) | (1ULL << BOARD_LCD_SHLR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&dir);
    gpio_set_level(BOARD_LCD_UPDN_GPIO, BOARD_LCD_FLIP ? 1 : 0);
    gpio_set_level(BOARD_LCD_SHLR_GPIO, BOARD_LCD_FLIP ? 0 : 1);

    /* MIPI D-PHY power */
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = BOARD_DSI_PHY_LDO_CHAN, .voltage_mv = BOARD_DSI_PHY_LDO_MV };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "DSI PHY LDO");

    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_cfg = EK79007_PANEL_BUS_DSI_2CH_CONFIG();
    bus_cfg.lane_bit_rate_mbps = BOARD_DSI_LANE_MBPS;
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "DSI bus");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi_cfg = EK79007_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &io), TAG, "DBI io");

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = BOARD_PCLK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = NUM_FBS,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_pulse_width = BOARD_HPW,
            .hsync_back_porch = BOARD_HBP,
            .hsync_front_porch = BOARD_HFP,
            .vsync_pulse_width = BOARD_VPW,
            .vsync_back_porch = BOARD_VBP,
            .vsync_front_porch = BOARD_VFP,
        },
    };
    ek79007_vendor_config_t vendor = {
        .mipi_config = { .dsi_bus = bus, .dpi_config = &dpi_cfg, .lane_num = 2 },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = BOARD_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ek79007(io, &dev_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");

    void *fb0 = NULL, *fb1 = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, NUM_FBS, &fb0, &fb1), TAG, "fbs");
    s_fbs[0] = fb0;
    s_fbs[1] = fb1;

    s_vsync = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_refresh_done = on_refresh_done };
    esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL);

    /* Black in both buffers before the backlight comes on (no white flash) */
    size_t bytes = BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2;
    for (int i = 0; i < NUM_FBS; i++) {
        memset(s_fbs[i], 0, bytes);
        esp_cache_msync(s_fbs[i], bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    bsp_display_show(0);
    bsp_display_backlight(100);
    board_set_cap(BSP_CAP_DISPLAY);

    ESP_LOGI(TAG, "EK79007 %dx%d up, fb0=%p fb1=%p", BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_fbs[0], s_fbs[1]);
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
    /* Passing one of the driver's own framebuffers makes it switch scan-out to
     * that buffer (after a cache write-back) instead of copying. */
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
    xSemaphoreTake(s_vsync, 0);     /* drop a stale edge */
    return xSemaphoreTake(s_vsync, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void bsp_display_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023 * percent / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
