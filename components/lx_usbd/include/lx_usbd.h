/* lx_usbd: the board's USB device port as an app's USB HID device (papp_abi.h, HIDD). */
#pragma once
#include "papp_abi.h"

extern const papp_if_hidd_t lx_if_hidd;
/* At the end of an app's run: the port goes back to the launcher's USB serial/JTAG link. */
void lx_usbd_end_run(void);
