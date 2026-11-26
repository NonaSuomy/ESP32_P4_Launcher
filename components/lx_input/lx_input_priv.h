#pragma once

#include "esp_err.h"

esp_err_t lx_input_usb_start(void);
esp_err_t lx_input_xinput_start(void);     /* after usb_start: the USB host is installed */

/* Button mappings as text on the SD card (lx_padmap.c) */
#include <stdint.h>
#define LX_PADMAP_DIR "/sd/config"
enum { LX_PADMAP_NONE, LX_PADMAP_MINE, LX_PADMAP_DB };
int       lx_padmap_load(uint16_t vid, uint16_t pid, uint8_t *map);     /* -> LX_PADMAP_ */
esp_err_t lx_padmap_save(uint16_t vid, uint16_t pid, const char *name, const uint8_t *map);  /* NULL: remove */

/* Gamepad report decoding (lx_pad_decode.c), shared by USB and Bluetooth pads */
#include <stdbool.h>
#include <stddef.h>
#include "lx_hid_parse.h"
typedef struct {
    uint32_t b;
    int16_t  lx, ly, rx, ry;
    uint16_t lt, rt;
} lx_pad_state_t;
uint32_t lx_pad_hat(uint8_t hat);                   /* 0..7 clockwise from up -> PAPP_BTN_ d-pad */
uint32_t lx_pad_axes_dpad(uint8_t x, uint8_t y, uint8_t lo, uint8_t hi);
uint32_t lx_pad_ps_buttons(uint8_t hat_btn, uint8_t b2, uint8_t b3);
void     lx_pad_ps4(const uint8_t *p, lx_pad_state_t *s);   /* DS4 report 0x01 layout, p[0] = id, 10 bytes */
void     lx_pad_ps5(const uint8_t *p, lx_pad_state_t *s);   /* DualSense USB report layout, 11 bytes */
bool     lx_pad_layout(const hid_layout_t *l, const uint8_t *p, size_t len, lx_pad_state_t *out);
