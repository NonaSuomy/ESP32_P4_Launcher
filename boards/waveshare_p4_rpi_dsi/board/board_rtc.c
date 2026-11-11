/*
 * No real-time clock on this board: the time comes from NTP or DevLink after each power-on.
 */
#include "board_internal.h"

esp_err_t bsp_rtc_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_rtc_get(int64_t *utc)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t bsp_rtc_set(int64_t utc)
{
    return ESP_ERR_NOT_SUPPORTED;
}
