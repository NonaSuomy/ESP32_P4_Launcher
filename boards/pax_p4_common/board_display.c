// SPDX-License-Identifier: MIT
#include "board_internal.h"
#include "board_pins.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <string.h>
static const char *TAG = "pax_display";
static esp_lcd_panel_handle_t panel;
static uint16_t *fb[2];
static SemaphoreHandle_t done, vsync;
static portMUX_TYPE flip_lock = portMUX_INITIALIZER_UNLOCKED;
static int visible, target, edges;
static i2c_master_dev_handle_t control;
static esp_err_t control_write(uint8_t reg, uint8_t val) {
  uint8_t bytes[] = {reg, val};
  esp_err_t e = ESP_FAIL;
  for (int n = 0; n < 5; n++) {
    e = i2c_master_transmit(control, bytes, 2, 100);
    if (e == ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(10));
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return e;
}
static bool IRAM_ATTR refresh(esp_lcd_panel_handle_t p,
                              esp_lcd_dpi_panel_event_data_t *e, void *ctx) {
  (void)p;
  (void)e;
  (void)ctx;
  BaseType_t wake = pdFALSE;
  xSemaphoreGiveFromISR(vsync, &wake);
  portENTER_CRITICAL_ISR(&flip_lock);
  if (edges && --edges == 0) {
    visible = target;
    xSemaphoreGiveFromISR(done, &wake);
  }
  portEXIT_CRITICAL_ISR(&flip_lock);
  return wake == pdTRUE;
}
esp_err_t bsp_display_init(void) {
  if (panel)
    return ESP_OK;
#if BOARD_AIO
  ledc_timer_config_t timer = {.speed_mode = LEDC_LOW_SPEED_MODE,
                               .duty_resolution = LEDC_TIMER_10_BIT,
                               .timer_num = LEDC_TIMER_0,
                               .freq_hz = 30000,
                               .clk_cfg = LEDC_AUTO_CLK};
  ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "PWM timer");
  ledc_channel_config_t pwm = {.gpio_num = BOARD_LCD_BL_GPIO,
                               .speed_mode = LEDC_LOW_SPEED_MODE,
                               .channel = LEDC_CHANNEL_0,
                               .timer_sel = LEDC_TIMER_0,
                               .duty = 0};
  ESP_RETURN_ON_ERROR(ledc_channel_config(&pwm), TAG, "backlight");
  gpio_set_level(BOARD_LCD_RST_GPIO, 0);
  gpio_set_direction(BOARD_LCD_RST_GPIO, GPIO_MODE_OUTPUT);
  vTaskDelay(pdMS_TO_TICKS(20));
  gpio_set_level(BOARD_LCD_RST_GPIO, 1);
  vTaskDelay(pdMS_TO_TICKS(120));
#else
  i2c_device_config_t dev = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                             .device_address = 0x45,
                             .scl_speed_hz = 100000};
  ESP_RETURN_ON_ERROR(
      i2c_master_bus_add_device(board_i2c_bus(), &dev, &control), TAG,
      "panel control");
  const uint8_t sequence[][2] = {{0x86, 0},   {0x85, 0},    {0x83, 0},
                                 {0x81, 4},   {0x82, 0x80}, {0x83, 1},
                                 {0x86, 0x80}};
  for (unsigned i = 0; i < sizeof(sequence) / sizeof(sequence[0]); i++) {
    ESP_RETURN_ON_ERROR(control_write(sequence[i][0], sequence[i][1]), TAG,
                        "panel power");
    if (i == 1)
      vTaskDelay(pdMS_TO_TICKS(30));
    if (i == 5)
      vTaskDelay(pdMS_TO_TICKS(80));
  }
#endif
  esp_lcd_dsi_bus_handle_t bus;
  esp_lcd_dsi_bus_config_t bus_cfg = {.bus_id = 0,
                                      .num_data_lanes = BOARD_DSI_LANES,
                                      .phy_clk_src =
                                          MIPI_DSI_PHY_CLK_SRC_DEFAULT,
                                      .lane_bit_rate_mbps = BOARD_DSI_MBPS};
  ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "DSI bus");
#if BOARD_AIO
  esp_lcd_panel_io_handle_t io;
  esp_lcd_dbi_io_config_t dbi = {
      .virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi, &io), TAG, "DBI");
  const uint8_t commands[][2] = {{0xB2, 0x10}, {0x80, 0x8B}, {0x81, 0x78},
                                 {0x82, 0x84}, {0x83, 0x88}, {0x84, 0xA8},
                                 {0x85, 0xE3}, {0x86, 0x88}};
  for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
    ESP_RETURN_ON_ERROR(
        esp_lcd_panel_io_tx_param(io, commands[i][0], &commands[i][1], 1), TAG,
        "panel command");
  // RGB565 and native mounting; compositor supplies the 180-degree rotation.
  uint8_t format = 0x55;
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x3a, &format, 1), TAG,
                      "pixel format");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0), TAG,
                      "sleep out");
  vTaskDelay(pdMS_TO_TICKS(120));
  ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(io, 0x29, NULL, 0), TAG,
                      "display on");
#endif
  esp_lcd_dpi_panel_config_t dpi = {
      .virtual_channel = 0,
      .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
      .dpi_clock_freq_mhz = BOARD_PCLK,
      .in_color_format = LCD_COLOR_FMT_RGB565,
#if BOARD_AIO
      .out_color_format = LCD_COLOR_FMT_RGB565,
#else
      .out_color_format = LCD_COLOR_FMT_RGB888,
      .flags.disable_lp = 1,
#endif
      .num_fbs = 2,
      .video_timing = {.h_size = BOARD_LCD_H_RES,
                       .v_size = BOARD_LCD_V_RES,
                       .hsync_pulse_width = BOARD_HPW,
                       .hsync_back_porch = BOARD_HBP,
                       .hsync_front_porch = BOARD_HFP,
                       .vsync_pulse_width = BOARD_VPW,
                       .vsync_back_porch = BOARD_VBP,
                       .vsync_front_porch = BOARD_VFP}};
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_dpi(bus, &dpi, &panel), TAG, "DPI");
  ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_get_frame_buffer(
                          panel, 2, (void **)&fb[0], (void **)&fb[1]),
                      TAG, "buffers");
  size_t bytes = BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2;
  for (int i = 0; i < 2; i++) {
    memset(fb[i], 0, bytes);
    ESP_RETURN_ON_ERROR(
        esp_cache_msync(fb[i], bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M), TAG,
        "clear");
  }
  done = xSemaphoreCreateBinary();
  vsync = xSemaphoreCreateBinary();
  if (!done || !vsync)
    return ESP_ERR_NO_MEM;
  esp_lcd_dpi_panel_event_callbacks_t cb = {.on_refresh_done = refresh};
  ESP_RETURN_ON_ERROR(
      esp_lcd_dpi_panel_register_event_callbacks(panel, &cb, NULL), TAG, "ISR");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "start DPI");
#if !BOARD_AIO
  ESP_RETURN_ON_ERROR(control_write(0x83, 0x0d), TAG, "touch reset");
  vTaskDelay(pdMS_TO_TICKS(10));
  ESP_RETURN_ON_ERROR(control_write(0x83, 0x0f), TAG, "release touch");
  vTaskDelay(pdMS_TO_TICKS(300));
#endif
  bsp_display_backlight(100);
  board_set_cap(BSP_CAP_DISPLAY);
  return ESP_OK;
}
int bsp_display_fb_count(void) { return panel ? 2 : 0; }
uint16_t *bsp_display_fb(int i) {
  return panel && i >= 0 && i < 2 ? fb[i] : NULL;
}
int bsp_display_visible(void) {
  int n;
  portENTER_CRITICAL(&flip_lock);
  n = visible;
  portEXIT_CRITICAL(&flip_lock);
  return n;
}
esp_err_t bsp_display_show(int i) {
  if (!panel || i < 0 || i > 1)
    return ESP_ERR_INVALID_ARG;
  if (!bsp_display_wait_flip(100))
    return ESP_ERR_TIMEOUT;
  size_t bytes = BOARD_LCD_H_RES * BOARD_LCD_V_RES * 2;
  ESP_RETURN_ON_ERROR(
      esp_cache_msync(fb[i], bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M), TAG,
      "flush");
  xSemaphoreTake(done, 0);
  portENTER_CRITICAL(&flip_lock);
  target = i;
  edges = 2;
  portEXIT_CRITICAL(&flip_lock);
  esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, 0, BOARD_LCD_H_RES,
                                            BOARD_LCD_V_RES, fb[i]);
  if (err != ESP_OK) {
    portENTER_CRITICAL(&flip_lock);
    edges = 0;
    portEXIT_CRITICAL(&flip_lock);
  }
  return err;
}
bool bsp_display_wait_flip(uint32_t ms) {
  int n;
  portENTER_CRITICAL(&flip_lock);
  n = edges;
  portEXIT_CRITICAL(&flip_lock);
  return !n || xSemaphoreTake(done, pdMS_TO_TICKS(ms)) == pdTRUE;
}
bool bsp_display_wait_vsync(uint32_t ms) {
  if (!vsync)
    return false;
  xSemaphoreTake(vsync, 0);
  return xSemaphoreTake(vsync, pdMS_TO_TICKS(ms)) == pdTRUE;
}
void bsp_display_backlight(int percent) {
  if (percent < 0)
    percent = 0;
  if (percent > 100)
    percent = 100;
#if BOARD_AIO
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023 * percent / 100);
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
#else
  if (control)
    control_write(0x86, 255 * percent / 100);
#endif
}
