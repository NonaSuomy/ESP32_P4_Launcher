/* Shared state between the board's driver files. Not part of the BSP contract. */
#pragma once

#include "bsp.h"
#include "driver/i2c_master.h"

i2c_master_bus_handle_t board_i2c_bus(void);
void board_set_cap(uint32_t cap);
/* The board is a JC1060P470C_I_W_Y (known once bsp_display_init has picked the panel: the new panel
 * comes on _Y boards only): other audio amplifier pin */
bool board_is_y(void);
