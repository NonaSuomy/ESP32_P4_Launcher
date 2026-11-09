/*
 * BSP display — RPi 7" V1-style 5" 800x480 panel behind a TC358762 DSI-to-DPI bridge, landscape.
 * An ATTINY88 at I2C 0x45 switches the panel power, the bridge/touch resets and the backlight PWM.
 * Bring-up ported from the esp32-scummvm port (esp32p4-ev branch, its forked function_ev_board BSP),
 * which follows embenix ESP32-P4-DSI-Support-Hub rpi-7inch-touch-display-v1.c and Linux tc358762.c.
 *
 * The bridge only shows a clean picture in RGB888 (RGB565 wraps ~200 px sideways: a TC358762 FIFO
 * phase issue that no porch setting fixes). The BSP contract is RGB565, so the launcher draws into
 * two RGB565 shadow buffers and bsp_display_show() has the PPA convert one into the hidden RGB888
 * scan-out buffer of the same index before switching to it. The PPA's 565 -> 888 expansion already
 * gives the panel's byte order (no R/B swap, as the esp32-scummvm game path found).
 */
#include "board_internal.h"
#include "board_pins.h"

#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "driver/ppa.h"
#include "hal/mipi_dsi_hal.h"
#include "hal/mipi_dsi_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "bsp_disp";

#define NUM_FBS 2

/* DPI timing that the bridge syncs to (embenix): 26 MHz / (1058 x 526) = about 46.7 Hz */
#define PANEL_PCLK_MHZ  26
#define PANEL_HSW       2
#define PANEL_HBP       46
#define PANEL_HFP       210
#define PANEL_VSW       20
#define PANEL_VBP       4
#define PANEL_VFP       22

/* ATTINY88 registers */
#define ATTINY_REG_PORTA        0x81
#define ATTINY_REG_PORTB        0x82
#define ATTINY_REG_PORTC        0x83
#define ATTINY_REG_PWM          0x86
#define ATTINY_REG_ADDR_L       0x8c
#define ATTINY_REG_ADDR_H       0x8d
#define ATTINY_REG_DATA_H       0x90
#define ATTINY_REG_DATA_L       0x91
#define ATTINY_PA_LCD_LR        BIT(2)
#define ATTINY_PB_LCD_MAIN      BIT(7)
#define ATTINY_PC_LED_EN        BIT(0)
#define ATTINY_PC_RST_TP_N      BIT(1)
#define ATTINY_PC_RST_LCD_N     BIT(2)
#define ATTINY_PC_RST_BRIDGE_N  BIT(3)

/* TC358762 registers */
#define TC_PPI_STARTPPI         0x0104
#define TC_PPI_LPTXTIMECNT      0x0114
#define TC_PPI_D0S_ATMR         0x0144
#define TC_PPI_D1S_ATMR         0x0148
#define TC_PPI_D0S_CLRSIPOCOUNT 0x0164
#define TC_PPI_D1S_CLRSIPOCOUNT 0x0168
#define TC_DSI_STARTDSI         0x0204
#define TC_DSI_LANEENABLE       0x0210
#define TC_LCDCTRL              0x0420
#define TC_LCD_HS_HBP           0x0424
#define TC_LCD_HDISP_HFP        0x0428
#define TC_LCD_VS_VBP           0x042c
#define TC_LCD_VDISP_VFP        0x0430
#define TC_SPICMR               0x0450
#define TC_SYSCTRL              0x0464

/* Private layout of esp_lcd_dsi_bus_t (esp_lcd/dsi/mipi_dsi_priv.h, checked against IDF 5.5.2):
 * the bridge needs the HAL for non-burst video, a continuous HS clock and generic long writes. */
typedef struct {
    int bus_id;
    mipi_dsi_hal_context_t hal;
} dsi_bus_priv_t;

static i2c_master_dev_handle_t s_attiny;
static esp_lcd_panel_handle_t s_panel;
static uint8_t *s_scan[NUM_FBS];        /* RGB888, scanned out by the DPI */
static uint16_t *s_fbs[NUM_FBS];        /* RGB565, what the launcher draws into */
static ppa_client_handle_t s_ppa_srm, s_ppa_blend;
static uint8_t *s_fg_a4;                /* blend foreground: A4, all 0 = fully transparent */
static bool s_use_blend;                /* convert with the blend engine (set if it matches SRM) */
static int s_visible;
static volatile bool s_flip_pending;
static SemaphoreHandle_t s_vsync;

#define FB_BYTES   (BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2)
#define SCAN_BYTES (BOARD_LCD_H_RES * BOARD_LCD_V_RES * 3)

/* RGB565 buffer -> RGB888 scan-out buffer, whole frame. The blend engine is a plain pixel pipe;
 * the SRM engine is built for scaling and works in blocks (about 18 ms for 800x480 here). Blend
 * reads a foreground too: a transparent A4 one, so the output is the background as is. */
static esp_err_t convert(ppa_client_handle_t engine, const uint16_t *src, uint8_t *dst)
{
    if (engine == s_ppa_blend) {
        ppa_blend_oper_config_t op = {
            .in_bg = {
                .buffer = src,
                .pic_w = BOARD_LCD_H_RES, .pic_h = BOARD_LCD_V_RES,
                .block_w = BOARD_LCD_H_RES, .block_h = BOARD_LCD_V_RES,
                .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
            },
            .in_fg = {
                .buffer = s_fg_a4,
                .pic_w = BOARD_LCD_H_RES, .pic_h = BOARD_LCD_V_RES,
                .block_w = BOARD_LCD_H_RES, .block_h = BOARD_LCD_V_RES,
                .blend_cm = PPA_BLEND_COLOR_MODE_A4,
            },
            .out = {
                .buffer = dst,
                .buffer_size = SCAN_BYTES,
                .pic_w = BOARD_LCD_H_RES, .pic_h = BOARD_LCD_V_RES,
                .blend_cm = PPA_BLEND_COLOR_MODE_RGB888,
            },
            .bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
            .bg_alpha_fix_val = 255,
            .fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE,
            .fg_alpha_fix_val = 0,
            .mode = PPA_TRANS_MODE_BLOCKING,
        };
        return ppa_do_blend(engine, &op);
    }
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src,
            .pic_w = BOARD_LCD_H_RES, .pic_h = BOARD_LCD_V_RES,
            .block_w = BOARD_LCD_H_RES, .block_h = BOARD_LCD_V_RES,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst,
            .buffer_size = SCAN_BYTES,
            .pic_w = BOARD_LCD_H_RES, .pic_h = BOARD_LCD_V_RES,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB888,
        },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(engine, &op);
}

/* Use the blend engine only if it gives the same bytes as SRM (whose output order the esp32-scummvm
 * port confirmed on this panel): a test pattern through both, compared. Runs before the backlight. */
static void pick_engine(void)
{
    uint8_t *ref = heap_caps_aligned_alloc(128, SCAN_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!ref) return;
    uint16_t *src = s_fbs[1];
    for (int i = 0; i < BOARD_LCD_H_RES * BOARD_LCD_V_RES; i++) src[i] = (uint16_t)(i * 40503u >> 7);
    esp_cache_msync(src, FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    int64_t t0 = esp_timer_get_time();
    esp_err_t e1 = convert(s_ppa_srm, src, ref);
    int64_t t1 = esp_timer_get_time();
    esp_err_t e2 = convert(s_ppa_blend, src, s_scan[1]);
    int64_t t2 = esp_timer_get_time();
    esp_cache_msync(ref, SCAN_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    esp_cache_msync(s_scan[1], SCAN_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
    bool same = e1 == ESP_OK && e2 == ESP_OK && memcmp(ref, s_scan[1], SCAN_BYTES) == 0;
    s_use_blend = same;
    ESP_LOGI(TAG, "565->888: SRM %.2f ms, blend %.2f ms (%s) -> %s", (t1 - t0) / 1e3, (t2 - t1) / 1e3,
             e2 != ESP_OK ? esp_err_to_name(e2) : same ? "same bytes" : "DIFFERENT bytes", same ? "blend" : "SRM");
    heap_caps_free(ref);
    memset(src, 0, FB_BYTES);
}

static bool IRAM_ATTR on_refresh_done(esp_lcd_panel_handle_t panel, esp_lcd_dpi_panel_event_data_t *edata,
                                      void *user_ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_vsync, &woken);
    return woken == pdTRUE;
}

static esp_err_t attiny_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_attiny, buf, 2, 50);
}

static void tc_write(esp_lcd_dsi_bus_handle_t bus, uint16_t reg, uint32_t val)
{
    dsi_bus_priv_t *priv = (dsi_bus_priv_t *)bus;
    uint8_t payload[6] = { reg & 0xFF, reg >> 8, val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF, val >> 24 };
    mipi_dsi_hal_host_gen_write_long_packet(&priv->hal, 0, MIPI_DSI_DT_GENERIC_LONG_WRITE, payload, sizeof(payload));
}

/* Panel power on, every reset held (bridge, LCD, touch), backlight off */
static esp_err_t attiny_power_on(void)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BOARD_ATTINY_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(board_i2c_bus(), &dev_cfg, &s_attiny), TAG, "ATTINY device");
    /* After a reset in the middle of an I2C transfer (flashing, a panic) the ATTINY can miss
     * the first writes: free the bus and try again for about a second */
    esp_err_t err = attiny_write(ATTINY_REG_PWM, 0);
    for (int i = 0; err != ESP_OK && i < 10; i++) {
        i2c_master_bus_reset(board_i2c_bus());
        vTaskDelay(pdMS_TO_TICKS(100));
        err = attiny_write(ATTINY_REG_PWM, 0);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "ATTINY not answering");
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_PORTC, 0x00), TAG, "ATTINY PORTC");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_PORTA, ATTINY_PA_LCD_LR), TAG, "ATTINY PORTA");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_PORTB, ATTINY_PB_LCD_MAIN), TAG, "ATTINY PORTB");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_PORTC, ATTINY_PC_LED_EN), TAG, "ATTINY PORTC");
    vTaskDelay(pdMS_TO_TICKS(80));
    return ESP_OK;
}

/* Bridge and LCD out of reset, then SYSPMCTRL (0x047C) = 0 through the ATTINY's register proxy */
static esp_err_t bridge_release(void)
{
    ESP_RETURN_ON_ERROR(attiny_write(ATTINY_REG_PORTC, ATTINY_PC_LED_EN | ATTINY_PC_RST_LCD_N | ATTINY_PC_RST_BRIDGE_N),
                        TAG, "bridge reset release");
    vTaskDelay(pdMS_TO_TICKS(10));
    static const uint8_t wake[][2] = {
        { ATTINY_REG_ADDR_H, 0x04 }, { ATTINY_REG_ADDR_L, 0x7c },
        { ATTINY_REG_DATA_H, 0x00 }, { ATTINY_REG_DATA_L, 0x00 },
    };
    for (size_t i = 0; i < sizeof(wake) / sizeof(wake[0]); i++) {
        ESP_RETURN_ON_ERROR(attiny_write(wake[i][0], wake[i][1]), TAG, "bridge wake");
        vTaskDelay(pdMS_TO_TICKS(8));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

/* Bridge registers as Linux tc358762.c / embenix: 1 lane, RGB888, plain mode timing */
static void bridge_init(esp_lcd_dsi_bus_handle_t bus)
{
    tc_write(bus, TC_DSI_LANEENABLE, BIT(0) | BIT(1));     /* clock + D0 */
    tc_write(bus, TC_PPI_D0S_CLRSIPOCOUNT, 0x05);
    tc_write(bus, TC_PPI_D1S_CLRSIPOCOUNT, 0x05);
    tc_write(bus, TC_PPI_D0S_ATMR, 0x00);
    tc_write(bus, TC_PPI_D1S_ATMR, 0x00);
    tc_write(bus, TC_PPI_LPTXTIMECNT, 0x03);
    tc_write(bus, TC_SPICMR, 0x00);
    tc_write(bus, TC_LCDCTRL, 0x00100150);                 /* VSDELAY | RGB888 | VTGEN */
    tc_write(bus, TC_SYSCTRL, 0x040f);
    tc_write(bus, TC_LCD_HS_HBP, (uint32_t)PANEL_HBP << 16 | PANEL_HSW);
    tc_write(bus, TC_LCD_HDISP_HFP, (uint32_t)PANEL_HFP << 16 | BOARD_LCD_H_RES);
    tc_write(bus, TC_LCD_VS_VBP, (uint32_t)PANEL_VBP << 16 | PANEL_VSW);
    tc_write(bus, TC_LCD_VDISP_VFP, (uint32_t)PANEL_VFP << 16 | BOARD_LCD_V_RES);
    vTaskDelay(pdMS_TO_TICKS(100));
    tc_write(bus, TC_PPI_STARTPPI, 0x01);
    tc_write(bus, TC_DSI_STARTDSI, 0x01);
    vTaskDelay(pdMS_TO_TICKS(100));
}

esp_err_t bsp_display_init(void)
{
    if (s_panel) return ESP_OK;
    if (!board_i2c_bus()) return ESP_ERR_INVALID_STATE;
    /* The ATTINY first, before the DSI PHY comes up */
    ESP_RETURN_ON_ERROR(attiny_power_on(), TAG, "panel power");

    /* MIPI D-PHY power */
    esp_ldo_channel_handle_t ldo = NULL;
    esp_ldo_channel_config_t ldo_cfg = { .chan_id = BOARD_DSI_PHY_LDO_CHAN, .voltage_mv = BOARD_DSI_PHY_LDO_MV };
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &ldo), TAG, "DSI PHY LDO");

    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = BOARD_DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = BOARD_DSI_LANE_MBPS,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "DSI bus");

    /* A DBI IO sets the LP escape clock in the controller; the setting stays after it is deleted */
    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi_cfg = { .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8 };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi_cfg, &io), TAG, "DBI io");
    esp_lcd_panel_io_del(io);

    esp_lcd_dpi_panel_config_t dpi_cfg = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = PANEL_PCLK_MHZ,
        .virtual_channel = 0,
        .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB888,
        .num_fbs = NUM_FBS,
        .video_timing = {
            .h_size = BOARD_LCD_H_RES,
            .v_size = BOARD_LCD_V_RES,
            .hsync_pulse_width = PANEL_HSW,
            .hsync_back_porch = PANEL_HBP,
            .hsync_front_porch = PANEL_HFP,
            .vsync_pulse_width = PANEL_VSW,
            .vsync_back_porch = PANEL_VBP,
            .vsync_front_porch = PANEL_VFP,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(bus, &dpi_cfg, &s_panel), TAG, "DPI panel");

    /* The TC358762 wants non-burst video with sync pulses; esp_lcd_new_panel_dpi() set burst */
    dsi_bus_priv_t *priv = (dsi_bus_priv_t *)bus;
    mipi_dsi_host_ll_dpi_set_video_burst_type(priv->hal.host, MIPI_DSI_LL_VIDEO_NON_BURST_WITH_SYNC_PULSES);
    mipi_dsi_host_ll_dpi_enable_frame_ack(priv->hal.host, false);
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "DPI start");

    /* Continuous HS clock for the bridge's PLL; the data lane still drops to LP in blanking,
     * which is when the register writes below go out */
    mipi_dsi_host_ll_set_clock_lane_state(priv->hal.host, MIPI_DSI_LL_CLOCK_LANE_STATE_HS);
    mipi_dsi_host_ll_enable_cmd_ack(priv->hal.host, false);
    ESP_RETURN_ON_ERROR(bridge_release(), TAG, "bridge");
    bridge_init(bus);
    /* Touch out of reset too (bsp_touch_init follows) */
    attiny_write(ATTINY_REG_PORTC, ATTINY_PC_LED_EN | ATTINY_PC_RST_TP_N | ATTINY_PC_RST_LCD_N | ATTINY_PC_RST_BRIDGE_N);

    void *fb0 = NULL, *fb1 = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(s_panel, NUM_FBS, &fb0, &fb1), TAG, "fbs");
    s_scan[0] = fb0;
    s_scan[1] = fb1;
    for (int i = 0; i < NUM_FBS; i++) {
        s_fbs[i] = heap_caps_aligned_calloc(128, 1, FB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        ESP_RETURN_ON_FALSE(s_fbs[i], ESP_ERR_NO_MEM, TAG, "RGB565 buffers");
    }
    ppa_client_config_t ppa_cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    ESP_RETURN_ON_ERROR(ppa_register_client(&ppa_cfg, &s_ppa_srm), TAG, "PPA SRM");
    size_t a4_bytes = BOARD_LCD_H_RES * BOARD_LCD_V_RES / 2;
    s_fg_a4 = heap_caps_aligned_calloc(128, 1, a4_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    ppa_cfg.oper_type = PPA_OPERATION_BLEND;
    if (s_fg_a4 && ppa_register_client(&ppa_cfg, &s_ppa_blend) == ESP_OK) {
        esp_cache_msync(s_fg_a4, a4_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        pick_engine();
    }

    s_vsync = xSemaphoreCreateBinary();
    esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_refresh_done = on_refresh_done };
    esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, NULL);

    /* Black in every buffer before the backlight comes on (no white flash) */
    for (int i = 0; i < NUM_FBS; i++) {
        memset(s_scan[i], 0, SCAN_BYTES);
        esp_cache_msync(s_scan[i], SCAN_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_cache_msync(s_fbs[i], FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    bsp_display_show(0);
    bsp_display_backlight(100);
    board_set_cap(BSP_CAP_DISPLAY);

    ESP_LOGI(TAG, "TC358762 %dx%d up, RGB888 scan-out %p %p, RGB565 %p %p", BOARD_LCD_H_RES, BOARD_LCD_V_RES,
             s_scan[0], s_scan[1], s_fbs[0], s_fbs[1]);
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
    esp_cache_msync(s_fbs[index], FB_BYTES, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    /* Scan-out buffer `index` is hidden: callers wait for the last flip before drawing into
     * buffer `index`, so the panel is on the other one */
    esp_err_t err = convert(s_use_blend ? s_ppa_blend : s_ppa_srm, s_fbs[index], s_scan[index]);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPA 565->888 failed: %s", esp_err_to_name(err));
        return err;
    }
    /* One of the driver's own framebuffers: it switches scan-out to it instead of copying */
    err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, BOARD_LCD_H_RES, BOARD_LCD_V_RES, s_scan[index]);
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
    if (!s_attiny) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    attiny_write(ATTINY_REG_PWM, 255 * percent / 100);
}
