/*
 * The kit's button module: a resistor ladder on GPIO16 (ADC1 channel 0), read as calibrated millivolts.
 * Thresholds from NonaSuomy's measurements on the AIO (#2); Elecrow's code agrees on left (1.5-1.9 V),
 * right (2.0-2.3 V), down / "middle" (2.4-2.65 V) and up (2.7-3.0 V). The lowest steps are two buttons at once.
 * No real-time clock or battery measurement on this board.
 */
#include "board_internal.h"
#include "board_pins.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "buttons";

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;

esp_err_t bsp_buttons_init(void)
{
    if (s_adc) return ESP_OK;
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    esp_err_t err = adc_oneshot_new_unit(&unit, &s_adc);
    if (err != ESP_OK) return err;
    adc_oneshot_chan_cfg_t ch = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    err = adc_oneshot_config_channel(s_adc, BOARD_KEY_ADC_CHAN, &ch);
    if (err == ESP_OK) {
        adc_cali_curve_fitting_config_t cal = {
            .unit_id = ADC_UNIT_1,
            .chan = BOARD_KEY_ADC_CHAN,
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        err = adc_cali_create_scheme_curve_fitting(&cal, &s_cali);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "button ladder ADC: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return err;
    }
    board_set_cap(BSP_CAP_GPIO_PAD);
    return ESP_OK;
}

uint32_t bsp_buttons_read(void)
{
    int raw, mv;
    if (!s_adc || adc_oneshot_read(s_adc, BOARD_KEY_ADC_CHAN, &raw) != ESP_OK) return 0;
    if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) return 0;
    if (mv < 1300) return 0;            /* below the ladder's lowest step: nothing pressed */
    if (mv < 1480) return BSP_BTN_DOWN | BSP_BTN_LEFT;
    if (mv < 1600) return BSP_BTN_UP | BSP_BTN_LEFT;
    if (mv < 1750) return BSP_BTN_LEFT;
    if (mv < 1950) return BSP_BTN_DOWN | BSP_BTN_RIGHT;
    if (mv < 2100) return BSP_BTN_UP | BSP_BTN_RIGHT;
    if (mv < 2400) return BSP_BTN_RIGHT;
    if (mv < 2750) return BSP_BTN_DOWN;
    if (mv < 3100) return BSP_BTN_UP;
    return 0;                           /* released: the ladder idles near 3.3 V */
}

int bsp_analog_read(int ch)
{
    (void)ch;
    return -1;
}

int bsp_battery_mv(void)
{
    return -1;
}

esp_err_t bsp_rtc_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_rtc_get(int64_t *utc)
{
    (void)utc;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_rtc_set(int64_t utc)
{
    (void)utc;
    return ESP_ERR_NOT_SUPPORTED;
}
