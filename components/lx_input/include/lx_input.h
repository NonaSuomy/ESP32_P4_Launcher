/*
 * Input manager (ARCHITECTURE §7.5, first cut).
 *
 * Sources (USB HID gamepads/keyboards, board buttons, DevLink injection)
 * feed logical devices: up to PAPP_MAX_PLAYERS gamepads in the standard
 * layout and one merged keyboard. Changes become events in one queue that the
 * running app drains through INPT. HOME (or Select+Start held 1 s) is a system
 * chord and goes to the launcher. Board buttons (BSP) merge into player 1.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "papp_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LX_INPUT_SYS_HOME = 1,      /* HOME pressed, or Select+Start held 1 s */
} lx_input_sys_t;

typedef void (*lx_input_sys_cb_t)(lx_input_sys_t ev);

/* Starts the USB HID host when the board has BSP_CAP_USB_HOST. */
esp_err_t lx_input_init(lx_input_sys_cb_t sys_cb);

/* Consumers */
bool     lx_input_gamepad(int player, papp_gamepad_t *out);
bool     lx_input_key_down(uint16_t usage);
bool     lx_input_keyboard_attached(void);          /* a USB keyboard or the board's own */
uint16_t lx_input_modifiers(void);
bool     lx_input_poll_event(papp_input_event_t *out);
void     lx_input_flush(void);
/* One-line summary of connected devices, for launcher screens. */
void     lx_input_describe(char *buf, size_t len);
/* PAPP_ANALOG_*: 0..65535, -1 if the board has no such input. */
int      lx_input_analog(int ch);

/* Sources: gamepads */
int  lx_input_pad_attach(const char *name, uint16_t vid, uint16_t pid);   /* -> player or -1 */
void lx_input_pad_update(int player, uint32_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                         uint16_t lt, uint16_t rt);
void lx_input_pad_detach(int player);

/* Button mapping, per pad model (VID:PID), as text on the SD card: your own in
 * /sd/config/gamepads_mine.txt, else the shared /sd/config/gamepads.txt (lx_padmap.c).
 * Bits are PAPP_BTN_ positions. set_map also clears a pre-0.23 copy in NVS (flash):
 * call it from a task with an internal stack, never from an app thread. */
#define LX_INPUT_MAP_BITS 20
int       lx_input_pad_first(void);                 /* first real pad, or -1 */
uint32_t  lx_input_pad_raw(int player);             /* buttons before the mapping */
esp_err_t lx_input_pad_set_map(int player, const uint8_t *map);   /* NULL: remove yours (database or default) */
void      lx_input_pad_reload_maps(void);           /* the files changed: apply them to the connected pads */

/* Sources: keyboards (boot report: modifiers + up to 6 usages) */
void lx_input_kbd_attach(void);
void lx_input_kbd_report(uint8_t mods, const uint8_t keys[6]);
void lx_input_kbd_detach(void);

/* DevLink injection: press for hold_ms, then release */
void lx_input_inject_buttons(int player, uint32_t buttons, uint32_t hold_ms);
void lx_input_inject_key(uint16_t usage, uint16_t mods, uint32_t hold_ms);

#ifdef __cplusplus
}
#endif
