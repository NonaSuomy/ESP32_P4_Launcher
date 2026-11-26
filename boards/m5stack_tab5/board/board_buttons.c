/*
 * No buttons or paddle on this board: input is USB (and touch). The battery (a 2-cell pack
 * read by an INA226 at 0x41) is board_power.c's.
 * The accelerometer (board_imu.c) is offered as three analog inputs.
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
    if (ch >= BSP_ANALOG_ACCEL_X && ch <= BSP_ANALOG_ACCEL_Z) return board_imu_read(ch - BSP_ANALOG_ACCEL_X);
    return -1;
}

/* bsp_battery_mv: board_power.c */
