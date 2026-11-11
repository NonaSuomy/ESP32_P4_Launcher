/*
 * No buttons, paddle or battery measurement on this board: input is USB (and touch).
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
