/* Shared state between the board's driver files. Not part of the BSP contract. */
#pragma once

#include "bsp.h"
#include "driver/i2c_master.h"

i2c_master_bus_handle_t board_i2c_bus(void);
void board_set_cap(uint32_t cap);
