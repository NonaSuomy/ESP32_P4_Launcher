/* Shared state between the board's driver files. Not part of the BSP contract. */
#pragma once

#include "bsp.h"
#include "driver/i2c_master.h"

i2c_master_bus_handle_t board_i2c_bus(void);
void board_set_cap(uint32_t cap);
int board_imu_read(int axis);      /* board_imu.c: 0..2 -> 0..65535, -1 if no accelerometer */
void board_touch_reset(void);      /* GT911: reset through the I/O expander, address 0x14 */

/* The Tab5 ships with one of three panels, told apart by the touch controller on I2C
 * (board_display.c): ILI9881C + GT911 at 0x5D/0x14, or ST7123 / ST7121 with built-in touch at 0x55. */
typedef enum { BOARD_PANEL_ILI9881C, BOARD_PANEL_ST7123, BOARD_PANEL_ST7121 } board_panel_t;
board_panel_t board_panel(void);
const char *board_panel_name(void);
