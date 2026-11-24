/*
 * BSP display — JD9165 1024x600 MIPI-DSI panel (2 lanes), landscape, double-buffered DPI.
 * V1 panel: init table and timing from the ESP32_P4_Internet_Radio project.
 * 2026 "New Panel" (every v3-chip board, some v1 ones; detect_panel): table and timing from Guition's
 * JC1060P470C_I_W_Y New_Panel demo (esp_lcd_jd9165.c), as EspControl uses them, plus COLMOD 0x55
 * (without it: faint vertical lines; 0x77 gives a flickering bottom edge). The V1 settings on it
 * give a white screen with a vertical noise band.
 */
#include "board_internal.h"
#include "board_pins.h"

#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_jd9165.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bsp_disp";

#define NUM_FBS 2

#define CMD(c, ...) { (c), (uint8_t[]){ __VA_ARGS__ }, sizeof((uint8_t[]){ __VA_ARGS__ }), 0 }
#define CMD_DELAY(c, ms) { (c), (uint8_t[]){ 0x00 }, 0, (ms) }

static const jd9165_lcd_init_cmd_t s_init_new[] = {
    CMD(0x30, 0x00), CMD(0xF7, 0x49, 0x61, 0x02, 0x00), CMD(0x30, 0x01), CMD(0x04, 0x0C), CMD(0x05, 0x08),
    CMD(0x0B, 0x11), CMD(0x20, 0x04), CMD(0x1F, 0x05), CMD(0x23, 0x38), CMD(0x28, 0x18), CMD(0x29, 0x29),
    CMD(0x2A, 0x01), CMD(0x2B, 0x29), CMD(0x2C, 0x01),
    CMD(0x30, 0x02), CMD(0x00, 0x05), CMD(0x01, 0x22), CMD(0x02, 0x08), CMD(0x03, 0x12), CMD(0x04, 0x16),
    CMD(0x05, 0x64), CMD(0x06, 0x00), CMD(0x07, 0x00), CMD(0x08, 0x78), CMD(0x09, 0x00), CMD(0x0A, 0x04),
    CMD(0x0B, 0x16, 0x17, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x07, 0x09),
    CMD(0x0C, 0x09, 0x1E, 0x1E, 0x1C, 0x1C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x0D, 0x0A, 0x05, 0x0B, 0x0D, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x06, 0x08),
    CMD(0x0E, 0x08, 0x1F, 0x1F, 0x1D, 0x1D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x0F, 0x0A, 0x05, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1D, 0x1D, 0x1F),
    CMD(0x10, 0x1F, 0x08, 0x08, 0x06, 0x06, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x11, 0x16, 0x17, 0x0D, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x1C, 0x1C, 0x1E),
    CMD(0x12, 0x1E, 0x09, 0x09, 0x07, 0x07, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x13, 0x00, 0x00, 0x00, 0x00), CMD(0x14, 0x00, 0x00, 0x41, 0x41), CMD(0x15, 0x00, 0x00, 0x00, 0x00),
    CMD(0x17, 0x00), CMD(0x18, 0x85), CMD(0x19, 0x06, 0x09), CMD(0x1A, 0x05, 0x08), CMD(0x1B, 0x0A, 0x04),
    CMD(0x26, 0x00), CMD(0x27, 0x00),
    CMD(0x30, 0x06),
    CMD(0x12, 0x3F, 0x25, 0x27, 0x35, 0x1D, 0x1B, 0x1B, 0x1A, 0x18, 0x0A, 0x2A, 0x21, 0x19, 0x30),
    CMD(0x13, 0x3F, 0x26, 0x27, 0x35, 0x1E, 0x1C, 0x1C, 0x1A, 0x18, 0x0B, 0x2A, 0x21, 0x19, 0x30),
    CMD(0x30, 0x0A), CMD(0x02, 0x4F), CMD(0x0B, 0x40), CMD(0x30, 0x0D), CMD(0x0D, 0x04), CMD(0x10, 0x05),
    CMD(0x11, 0x0C), CMD(0x12, 0x05), CMD(0x13, 0x0C), CMD(0x30, 0x00),
    CMD(0x3A, 0x55),        /* COLMOD 16-bit, as the RGB565 link: without it faint vertical lines; EspControl sends one too */
    CMD_DELAY(0x11, 120),   /* SLPOUT */
    CMD_DELAY(0x29, 20),    /* DISPON */
};

static const jd9165_lcd_init_cmd_t s_init_v1[] = {
    CMD(0x30, 0x00), CMD(0xF7, 0x49, 0x61, 0x02, 0x00), CMD(0x30, 0x01), CMD(0x04, 0x0C), CMD(0x05, 0x00),
    CMD(0x06, 0x00), CMD(0x0B, 0x11), CMD(0x17, 0x00), CMD(0x20, 0x04), CMD(0x1F, 0x05), CMD(0x23, 0x00),
    CMD(0x25, 0x19), CMD(0x28, 0x18), CMD(0x29, 0x04), CMD(0x2A, 0x01), CMD(0x2B, 0x04), CMD(0x2C, 0x01),
    CMD(0x30, 0x02), CMD(0x01, 0x22), CMD(0x03, 0x12), CMD(0x04, 0x00), CMD(0x05, 0x64), CMD(0x0A, 0x08),
    CMD(0x0B, 0x0A, 0x1A, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x06, 0x08, 0x1F, 0x1D),
    CMD(0x0C, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x0D, 0x16, 0x1B, 0x0B, 0x0D, 0x0D, 0x11, 0x10, 0x07, 0x09, 0x1E, 0x1C),
    CMD(0x0E, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x0F, 0x16, 0x1B, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1C, 0x1E, 0x09, 0x07),
    CMD(0x10, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x11, 0x0A, 0x1A, 0x0D, 0x0B, 0x0D, 0x11, 0x10, 0x1D, 0x1F, 0x08, 0x06),
    CMD(0x12, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D, 0x0D),
    CMD(0x14, 0x00, 0x00, 0x11, 0x11), CMD(0x18, 0x99), CMD(0x30, 0x06),
    CMD(0x12, 0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29),
    CMD(0x13, 0x36, 0x2C, 0x2E, 0x3C, 0x38, 0x35, 0x35, 0x32, 0x2E, 0x1D, 0x2B, 0x21, 0x16, 0x29),
    CMD(0x30, 0x0A), CMD(0x02, 0x4F), CMD(0x0B, 0x40), CMD(0x12, 0x3E), CMD(0x13, 0x78), CMD(0x30, 0x0D),
    CMD(0x0D, 0x04), CMD(0x10, 0x0C), CMD(0x11, 0x0C), CMD(0x12, 0x0C), CMD(0x13, 0x0C), CMD(0x30, 0x00),
    CMD_DELAY(0x11, 120),   /* SLPOUT */
    CMD_DELAY(0x29, 20),    /* DISPON */
};

typedef struct {
    const char *name;
    const jd9165_lcd_init_cmd_t *init;
    size_t n_init;
    int rst_gpio;
    int pclk_mhz, hpw, hbp, hfp, vpw, vbp, vfp;
} panel_t;

static const panel_t k_panel_v1 __attribute__((unused)) = {   /* v3 builds: never detected */
    "V1", s_init_v1, sizeof(s_init_v1) / sizeof(s_init_v1[0]), BOARD_LCD_RST_GPIO_V1,
    54, 40, 160, 160, 10, 23, 12,
};
static const panel_t k_panel_new = {
    "new", s_init_new, sizeof(s_init_new) / sizeof(s_init_new[0]), BOARD_LCD_RST_GPIO_NEW,
    52, 24, 136, 160, 2, 21, 12,
};

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fbs[NUM_FBS];
static int s_visible;
static volatile bool s_flip_pending;
static volatile bool s_scan_off;       /* bsp_display_scanout(false) */
static int s_backlight = 100;
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
        .freq_hz = 20000,
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

/*
 * Which panel: both are JD9165s and read the same (zero) IDs, but they reset on different pins: the V1
 * panel on GPIO 27, the 2026 "New Panel" on GPIO 0. With GPIO 0 held low the new panel stops answering
 * (Read Display ID times out); the V1 panel keeps answering. A v1.x-chip board can have either; the
 * v3.x-chip boards (BOARD_CHIP_V3) all have the new one.
 */
static const panel_t *detect_panel(esp_lcd_panel_io_handle_t io)
{
#ifdef BOARD_CHIP_V3
    (void)io;
    return &k_panel_new;
#else
    /* Both reset pins high, then a pause: the panel needs it before its table, after a warm restart
     * too (it stays blank otherwise). gpio_config, not just set_direction: GPIO 0 is an LP pin. */
    const int pins[2] = { BOARD_LCD_RST_GPIO_V1, BOARD_LCD_RST_GPIO_NEW };
    for (int i = 0; i < 2; i++) gpio_reset_pin(pins[i]);
    gpio_config_t rc = { .pin_bit_mask = (1ULL << pins[0]) | (1ULL << pins[1]), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rc);
    for (int i = 0; i < 2; i++) gpio_set_level(pins[i], 1);
    vTaskDelay(pdMS_TO_TICKS(150));
    /* After a software restart the running panel keeps answering even with its reset held low, so
     * only a power-on detects; other resets reuse the answer kept in RTC memory */
    static RTC_NOINIT_ATTR uint32_t s_detected;
    if (esp_reset_reason() != ESP_RST_POWERON && (s_detected & ~1u) == 0x9165A5A0u) {
        const panel_t *pn = (s_detected & 1) ? &k_panel_new : &k_panel_v1;
        gpio_reset_pin(pn == &k_panel_new ? BOARD_LCD_RST_GPIO_V1 : BOARD_LCD_RST_GPIO_NEW);
        return pn;
    }
    uint8_t id[3];
    const panel_t *pn = &k_panel_v1;
    if (esp_lcd_panel_io_rx_param(io, 0x04, id, 3) != ESP_OK) {
        ESP_LOGW(TAG, "panel does not answer: V1 settings");
    } else {
        gpio_set_level(BOARD_LCD_RST_GPIO_NEW, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        esp_log_level_set("lcd.dsi", ESP_LOG_NONE);     /* the timeout is the answer here */
        if (esp_lcd_panel_io_rx_param(io, 0x04, id, 3) != ESP_OK) pn = &k_panel_new;
        esp_log_level_set("lcd.dsi", ESP_LOG_INFO);
        gpio_set_level(BOARD_LCD_RST_GPIO_NEW, 1);
    }
    gpio_reset_pin(pn == &k_panel_new ? BOARD_LCD_RST_GPIO_V1 : BOARD_LCD_RST_GPIO_NEW);   /* not ours */
    s_detected = 0x9165A5A0u | (pn == &k_panel_new);
    return pn;
#endif
}

esp_err_t bsp_display_init(void)
{
    if (s_panel) return ESP_OK;
    backlight_init();

    /* MIPI D-PHY power */
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = BOARD_DSI_PHY_LDO_CHAN, .voltage_mv = BOARD_DSI_PHY_LDO_MV };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "DSI PHY LDO");

    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_cfg = JD9165_PANEL_BUS_DSI_2CH_CONFIG();
    bus_cfg.lane_bit_rate_mbps = 750;           /* both panels */
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "DSI bus");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi_cfg = JD9165_PANEL_IO_DBI_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &io), TAG, "DBI io");

    const panel_t *pn = detect_panel(io);
    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = pn->pclk_mhz,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
        .num_fbs = NUM_FBS,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_pulse_width = pn->hpw,
            .hsync_back_porch = pn->hbp,
            .hsync_front_porch = pn->hfp,
            .vsync_pulse_width = pn->vpw,
            .vsync_back_porch = pn->vbp,
            .vsync_front_porch = pn->vfp,
        },
    };
    jd9165_vendor_config_t vendor = {
        .init_cmds = pn->init,
        .init_cmds_size = pn->n_init,
        .mipi_config = { .dsi_bus = bus, .dpi_config = &dpi_cfg },
    };
    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = pn->rst_gpio,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_jd9165(io, &dev_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(120));     /* JD9165 needs 120 ms after reset before the init table */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");

    void *fb0 = NULL, *fb1 = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, NUM_FBS, &fb0, &fb1), TAG, "fbs");
    s_fbs[0] = fb0;
#if NUM_FBS > 1
    s_fbs[1] = fb1;
#endif

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

    ESP_LOGI(TAG, "JD9165 %dx%d (%s panel) up, fb0=%p fb1=%p", BOARD_LCD_H_RES, BOARD_LCD_V_RES,
             pn->name,
             fb0, fb1);
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
    if (s_scan_off) s_flip_pending = false;     /* nothing is scanned out */
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
    if (s_scan_off) {
        vTaskDelay(pdMS_TO_TICKS(16));
        return true;
    }
    xSemaphoreTake(s_vsync, 0);     /* drop a stale edge */
    return xSemaphoreTake(s_vsync, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void bsp_display_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s_backlight = percent;
    if (s_scan_off) return;                     /* comes back with the scan-out */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023 * percent / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

/* The DSI host's pattern generator takes over and the bridge stops pulling pixels, so the frame DMA
 * stops reading PSRAM; turning it back on resumes the frame buffers. Backlight off meanwhile. */
esp_err_t bsp_display_scanout(bool on)
{
    if (!s_panel) return ESP_ERR_INVALID_STATE;
    if (on != s_scan_off) return ESP_OK;
    if (!on) {
        int bl = s_backlight;
        bsp_display_backlight(0);
        s_backlight = bl;
        s_scan_off = true;
        return esp_lcd_dpi_panel_set_pattern(s_panel, MIPI_DSI_PATTERN_BAR_HORIZONTAL);
    }
    esp_err_t e = esp_lcd_dpi_panel_set_pattern(s_panel, MIPI_DSI_PATTERN_NONE);
    s_scan_off = false;
    xSemaphoreTake(s_vsync, 0);
    bsp_display_backlight(s_backlight);
    return e;
}
