/*
 * BSP display — ST7796 320x480 SPI panel (80 MHz), scanned in landscape (MADCTL MV), PWM backlight.
 * Init sequence and polarity as in Waveshare's BSP (esp_lcd_st7796, BGR, inverted colours).
 *
 * The panel has its own GRAM and no scan-out from our memory, so the double-buffered contract
 * of bsp.h is emulated. The two framebuffers are a 768x512 virtual screen in PSRAM (the
 * launcher's layout needs that much room); bsp_display_show() hands one to a push task, which
 * scales it by 0.625 with the PPA into a 480x320 panel frame, then streams that to the panel in
 * bands through two internal DMA bounce buffers, byte-swapping on the way (the panel takes
 * big-endian RGB565). "Flip done" = the push no longer reads the buffer the compositor wants to
 * draw into, which is as soon as the PPA pass is over.
 * A panel frame is 300 KiB, about 31 ms on the wire: ~30 fps at most, and it may tear.
 */
#include "board_internal.h"
#include "board_pins.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "driver/ppa.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "bsp_disp";

#define NUM_FBS      2
#define BAND_LINES   40                                   /* 480x40x2 = 37.5 KiB per band */
#define BAND_PIXELS  (BOARD_LCD_H_RES * BAND_LINES)
#define FB_BYTES     ((size_t)BOARD_VIRT_W * BOARD_VIRT_H * 2)
#define OUT_BYTES    ((size_t)BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2)
#define SCALE        ((float)BOARD_LCD_H_RES / BOARD_VIRT_W)     /* 0.625 = 10/16, exact for the PPA */

#define EV_PUSH_DONE (1 << 0)

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fbs[NUM_FBS];
static uint16_t *s_band[2];
static uint16_t *s_out;                                   /* scaled panel frame */
static ppa_client_handle_t s_ppa;
static SemaphoreHandle_t s_band_free;                     /* counts bounce buffers not on the wire */
static EventGroupHandle_t s_ev;
static TaskHandle_t s_push_task;
static volatile int s_visible;
static volatile int s_pending = -1;                       /* next buffer to push */
static volatile int s_reading = -1;                       /* buffer the push task reads now */

static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_band_free, &woken);
    return woken == pdTRUE;
}

static inline void swap_copy(uint16_t *dst, const uint16_t *src, size_t pixels)
{
    const uint32_t *s = (const uint32_t *)src;
    uint32_t *d = (uint32_t *)dst;
    for (size_t i = 0; i < pixels / 2; i++) {
        uint32_t v = s[i];
        d[i] = ((v & 0x00FF00FFu) << 8) | ((v & 0xFF00FF00u) >> 8);
    }
}

static void push_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        int idx = s_pending;
        if (idx < 0) continue;
        s_pending = -1;
        s_reading = idx;
        ppa_srm_oper_config_t op = {
            .in = {
                .buffer = s_fbs[idx],
                .pic_w = BOARD_VIRT_W, .pic_h = BOARD_VIRT_H,
                .block_w = BOARD_VIRT_W, .block_h = BOARD_VIRT_H,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .out = {
                .buffer = s_out,
                .buffer_size = OUT_BYTES,
                .pic_w = BOARD_LCD_H_RES, .pic_h = BOARD_LCD_V_RES,
                .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
            },
            .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
            .scale_x = SCALE,
            .scale_y = SCALE,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &op);
        s_reading = -1;          /* the framebuffer is free for the compositor again */
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "PPA scale failed: %s", esp_err_to_name(err));
            xEventGroupSetBits(s_ev, EV_PUSH_DONE);
            continue;
        }
        /* the PPA wrote s_out by DMA: drop stale cache lines before the CPU reads it */
        esp_cache_msync(s_out, OUT_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        const uint16_t *fb = s_out;
        int b = 0;
        for (int y = 0; y < BOARD_LCD_V_RES; y += BAND_LINES) {
            int lines = BOARD_LCD_V_RES - y < BAND_LINES ? BOARD_LCD_V_RES - y : BAND_LINES;
            xSemaphoreTake(s_band_free, portMAX_DELAY);
            swap_copy(s_band[b], fb + (size_t)y * BOARD_LCD_H_RES, (size_t)lines * BOARD_LCD_H_RES);
            esp_lcd_panel_draw_bitmap(s_panel, 0, y, BOARD_LCD_H_RES, y + lines, s_band[b]);
            b ^= 1;
        }
        xSemaphoreTake(s_band_free, portMAX_DELAY);    /* wait for the wire to drain */
        xSemaphoreTake(s_band_free, portMAX_DELAY);
        xSemaphoreGive(s_band_free);
        xSemaphoreGive(s_band_free);
        xEventGroupSetBits(s_ev, EV_PUSH_DONE);
    }
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

esp_err_t bsp_display_init(void)
{
    if (s_panel) return ESP_OK;
    backlight_init();

    for (int i = 0; i < NUM_FBS; i++) {
        s_fbs[i] = heap_caps_aligned_calloc(128, 1, FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_fbs[i]) return ESP_ERR_NO_MEM;
    }
    s_out = heap_caps_aligned_calloc(128, 1, OUT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_out) return ESP_ERR_NO_MEM;
    ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_cfg, &s_ppa), TAG, "PPA client");
    for (int i = 0; i < 2; i++) {
        s_band[i] = heap_caps_aligned_alloc(64, BAND_PIXELS * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (!s_band[i]) return ESP_ERR_NO_MEM;
    }
    s_band_free = xSemaphoreCreateCounting(2, 2);
    s_ev = xEventGroupCreate();

    spi_bus_config_t bus = {
        .sclk_io_num = BOARD_LCD_CLK,
        .mosi_io_num = BOARD_LCD_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BAND_PIXELS * 2,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BOARD_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "SPI bus");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = BOARD_LCD_DC,
        .cs_gpio_num = BOARD_LCD_CS,
        .pclk_hz = BOARD_LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 3,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_done,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BOARD_LCD_SPI_HOST, &io_cfg, &io),
                        TAG, "panel io");

    esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = BOARD_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(io, &dev_cfg, &s_panel), TAG, "panel");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "init");
    esp_lcd_panel_invert_color(s_panel, true);
    /* Waveshare's portrait setup is MX; landscape is MV (or MV+MX+MY, 180 degrees round) */
    esp_lcd_panel_swap_xy(s_panel, true);
#if BOARD_LCD_FLIP_180
    esp_lcd_panel_mirror(s_panel, true, true);
#else
    esp_lcd_panel_mirror(s_panel, false, false);
#endif

    xTaskCreatePinnedToCore(push_task, "lcd_push", 4096, NULL, configMAX_PRIORITIES - 4, &s_push_task, 1);

    /* Clear the panel before the backlight comes on */
    s_visible = 0;
    bsp_display_show(0);
    bsp_display_wait_vsync(200);
    esp_lcd_panel_disp_on_off(s_panel, true);
    bsp_display_backlight(100);
    board_set_cap(BSP_CAP_DISPLAY);

    ESP_LOGI(TAG, "ST7796 %dx%d up (SPI %d MHz), virtual %dx%d, fb0=%p fb1=%p", BOARD_LCD_H_RES,
             BOARD_LCD_V_RES, BOARD_LCD_PCLK_HZ / 1000000, BOARD_VIRT_W, BOARD_VIRT_H, s_fbs[0], s_fbs[1]);
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
    xEventGroupClearBits(s_ev, EV_PUSH_DONE);
    s_visible = index;
    s_pending = index;
    xTaskNotifyGive(s_push_task);
    return ESP_OK;
}

int bsp_display_visible(void)
{
    return s_visible;
}

bool bsp_display_wait_vsync(uint32_t timeout_ms)
{
    if (!s_panel) return false;
    return xEventGroupWaitBits(s_ev, EV_PUSH_DONE, pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms)) & EV_PUSH_DONE;
}

bool bsp_display_wait_flip(uint32_t timeout_ms)
{
    if (!s_panel) return false;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    /* the hidden buffer is free once no push reads it */
    while (s_reading == (s_visible ^ 1)) {
        if ((int32_t)(end - xTaskGetTickCount()) <= 0) return false;
        vTaskDelay(1);
    }
    return true;
}

void bsp_display_backlight(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023 * percent / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
