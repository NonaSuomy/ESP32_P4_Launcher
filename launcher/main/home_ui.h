/*
 * What the launcher's other screens (store.c) use from the home screen (home.c):
 * palette, drawing helpers, the gamepad / keyboard focus, and going home.
 * All of it runs on the home task.
 */
#pragma once
#include <stdbool.h>
#include "lvgl.h"

#define APPS_DIR      "/sd/apps"

/* Palette (from the design mock-up) */
#define C_BG_TOP      lv_color_hex(0x0B1630)
#define C_BG_BOTTOM   lv_color_hex(0x050A14)
#define C_PANEL       lv_color_hex(0x0E1A30)
#define C_BORDER      lv_color_hex(0x24406E)
#define C_ACCENT      lv_color_hex(0x3BC9F5)
#define C_BLUE        lv_color_hex(0x1E6FFF)
#define C_TEXT        lv_color_hex(0xFFFFFF)
#define C_DIM         lv_color_hex(0x8FA3BF)
#define C_GREEN       lv_color_hex(0x3DDC84)
#define C_FOCUS       lv_color_hex(0x1B3A70)
#define C_ORANGE      lv_color_hex(0xF5A623)
#define C_RED         lv_color_hex(0xD9453B)

/* The layout is drawn for 800x480: on a bigger screen right-hand items move by
 * ui_dx(), bottom ones by ui_dy(), and full-width ones grow by them */
int ui_dx(void);
int ui_dy(void);

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h, int radius);
lv_obj_t *ui_button(lv_obj_t *parent, int x, int y, int w, const char *text, lv_event_cb_t cb);   /* 44 high */
void ui_focus_style(lv_obj_t *o, int radius);               /* the bright ring of a focused item */
/* An app icon: the PNG src ("S:/sd/...", scaled to size) or a coloured tile with the
 * initial of name; key picks the colour */
lv_obj_t *ui_icon(lv_obj_t *parent, const char *src, const char *key, const char *name, int size);
lv_obj_t *ui_screen(void);                                  /* empty screen with the background */
void ui_message(const char *title, const char *text);      /* with OK; then store_focus() or home */

/* Gamepad / keyboard focus: a list of items, directions go to the nearest one */
void ui_focus_clear(void);
void ui_focus_add(lv_obj_t *o);
void ui_focus_set(lv_obj_t *o);
lv_obj_t *ui_focus_get(void);
void ui_focus_drop(void);                                   /* nothing focused (ring off) */

void show_home(void);
void home_launch(const char *path);                         /* starts the .papp from the home task */
bool home_show_dev(void);                                   /* Settings -> Show developer apps */
