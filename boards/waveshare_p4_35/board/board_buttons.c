/*
 * No buttons or paddle on this board: input is USB (and touch). The battery is a 2-cell
 * pack read by an INA226 (0x41); not reported yet, as the launcher expects one Li-ion cell.
 */
#include "board_internal.h"

esp_err_t bsp_buttons_init(void)
{
    return ESP_ERR_NOT_FOUND;
}

uint32_t bsp_buttons_read(void)
{
    return 0;
}

int bsp_analog_read(int ch)
{
    return -1;
}

int bsp_battery_mv(void)
{
    return -1;
}
