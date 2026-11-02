// SPDX-License-Identifier: MIT
#include "board_internal.h"
#include "board_pins.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t calibration;
esp_err_t bsp_buttons_init(void) {
#if BOARD_AIO
  adc_oneshot_unit_init_cfg_t cfg = {.unit_id = ADC_UNIT_1};
  esp_err_t e = adc_oneshot_new_unit(&cfg, &adc);
  if (e != ESP_OK)
    return e;
  adc_oneshot_chan_cfg_t ch = {.atten = ADC_ATTEN_DB_12,
                               .bitwidth = ADC_BITWIDTH_DEFAULT};
  e = adc_oneshot_config_channel(adc, ADC_CHANNEL_0, &ch);
  if (e != ESP_OK)
    return e;
  adc_cali_curve_fitting_config_t cal = {.unit_id = ADC_UNIT_1,
                                         .chan = ADC_CHANNEL_0,
                                         .atten = ADC_ATTEN_DB_12,
                                         .bitwidth = ADC_BITWIDTH_DEFAULT};
  e = adc_cali_create_scheme_curve_fitting(&cal, &calibration);
  if (e == ESP_OK)
    board_set_cap(BSP_CAP_GPIO_PAD);
  return e;
#else
  return ESP_ERR_NOT_FOUND;
#endif
}
uint32_t bsp_buttons_read(void) {
#if BOARD_AIO
  int value;
  if (!adc || adc_oneshot_read(adc, ADC_CHANNEL_0, &value) != ESP_OK)
    return 0;
  int mv;
  if (!calibration ||
      adc_cali_raw_to_voltage(calibration, value, &mv) != ESP_OK)
    return 0;
  if (mv < 1480)
    return BSP_BTN_DOWN | BSP_BTN_LEFT;
  if (mv < 1600)
    return BSP_BTN_UP | BSP_BTN_LEFT;
  if (mv < 1750)
    return BSP_BTN_LEFT;
  if (mv < 1950)
    return BSP_BTN_DOWN | BSP_BTN_RIGHT;
  if (mv < 2100)
    return BSP_BTN_UP | BSP_BTN_RIGHT;
  if (mv < 2400)
    return BSP_BTN_RIGHT;
  if (mv < 2750)
    return BSP_BTN_DOWN;
  if (mv < 3100)
    return BSP_BTN_UP;
#endif
  return 0;
}
int bsp_analog_read(int ch) {
  (void)ch;
  return -1;
}
int bsp_battery_mv(void) { return -1; }
esp_err_t bsp_rtc_init(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t bsp_rtc_get(int64_t *utc) {
  (void)utc;
  return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t bsp_rtc_set(int64_t utc) {
  (void)utc;
  return ESP_ERR_NOT_SUPPORTED;
}
