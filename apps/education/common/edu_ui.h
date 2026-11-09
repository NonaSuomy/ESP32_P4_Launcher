/*
 * edu_ui — a small touch UI for the education apps (built in by apps/education/edubuild.py).
 *
 * Draw into an 800x480 surface (the launcher scales it to the screen) with anti-aliased shapes
 * and Montserrat text, register touch targets while drawing, and let ui_touch() call your
 * act(id, x) when one is tapped. Also: a log panel, and the shared "remote" state (mode, level,
 * LED, message) with its Control screen, used by BLE Remote and Wi-Fi Remote.
 *
 *   papp_surface_t *s = ui_open();
 *   while (!papp_exit_requested()) {
 *       ui_touch(s, act);                       // calls act(id, x) for taps, drags, repeats
 *       ui_begin(s);
 *       ui_bar("My app", tabs, 2, tab, "status", C_GREEN, false);
 *       ui_button(40, 120, 200, 56, "Press me", C_BLUE, MY_BUTTON);
 *       ui_end(s);
 *   }
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "papp/papp.h"

#define UI_W 800
#define UI_H 480

#define UI_RGB(h) PAPP_RGB565(((h) >> 16) & 255, ((h) >> 8) & 255, (h) & 255)
#define C_BG      UI_RGB(0x081226)
#define C_PANEL   UI_RGB(0x0E1A30)
#define C_RAISED  UI_RGB(0x16274A)
#define C_BORDER  UI_RGB(0x24406E)
#define C_BLUE    UI_RGB(0x1E6FFF)
#define C_TEXT    UI_RGB(0xFFFFFF)
#define C_DIM     UI_RGB(0x8FA3BF)
#define C_FAINT   UI_RGB(0x4A5C7A)
#define C_GREEN   UI_RGB(0x3DDC84)
#define C_ORANGE  UI_RGB(0xF5A623)
#define C_RED     UI_RGB(0xD9453B)
#define C_IN      UI_RGB(0x7CE0A8)
#define C_OUT     UI_RGB(0x7FB4FF)

typedef struct ui_font ui_font_t;
extern const ui_font_t *const ui_small;      /* 23 px line */
extern const ui_font_t *const ui_med;        /* 33 px line */
extern const ui_font_t *const ui_large;      /* 55 px line */

/* ── Frame ── */
papp_surface_t *ui_open(void);               /* the 800x480 surface; NULL: no display */
void ui_begin(papp_surface_t *s);            /* lock, clear to C_BG, forget the touch targets */
void ui_end(papp_surface_t *s);              /* present (waits for vsync) */

/* ── Shapes (x, y = top left unless noted) ── */
uint16_t ui_mix(uint16_t a, uint16_t b, int t);                  /* t 0..256 = a..b */
void ui_fill(int x, int y, int w, int h, uint16_t c);
void ui_rrect(int x, int y, int w, int h, int r, uint16_t c);
void ui_disc(float cx, float cy, float r, uint16_t c, int alpha); /* alpha 0..255 */
void ui_line(float x0, float y0, float x1, float y1, float width, uint16_t c);

/* ── Text: cy = vertical centre of capitals; ui_text_n takes the line top ── */
int  ui_text_w(const ui_font_t *f, const char *s);
int  ui_line_h(const ui_font_t *f);
int  ui_line_top(const ui_font_t *f, int cy);
int  ui_text_n(const ui_font_t *f, int x, int top, const char *s, int n, uint16_t c, int x_max);
void ui_text_l(const ui_font_t *f, int x, int cy, const char *s, uint16_t c);
void ui_text_c(const ui_font_t *f, int cx, int cy, const char *s, uint16_t c);
void ui_text_r(const ui_font_t *f, int x, int cy, const char *s, uint16_t c);
int  ui_text_wrap(const ui_font_t *f, int x, int top, int w, const char *s, uint16_t c, int max_lines);

/* ── Touch targets: ids 1..199 are yours; 200+ belong to edu_ui ── */
enum { UI_TAP, UI_DRAG, UI_REPEAT };         /* act on release / while held with x / on press + repeat */
void     ui_hit(int x, int y, int w, int h, int id, int kind);
uint16_t ui_pressed_color(int id, uint16_t c);   /* lighter while the finger is on it */
void     ui_button(int x, int y, int w, int h, const char *label, uint16_t bg, int id);
void     ui_toggle(int x, int y, bool on, int id);
/* Reads the touch panel and calls act(id, x): call once per frame, before drawing */
void     ui_touch(papp_surface_t *s, void (*act)(int id, int x));
void     ui_touch_reset(void);               /* ignore the touch until released (after a menu) */

/* Title bar with tabs and a close button (64 px) and a status line under it (to y 104).
 * Tapping a tab calls act(UI_ID_TAB + i); the close button act(UI_ID_CLOSE). */
enum { UI_ID_TAB = 200, UI_ID_CLOSE = 210 };
void ui_bar(const char *title, const char *const *tabs, int ntabs, int tab, const char *status,
            uint16_t dot, bool blink);

/* ── Log panel: dir 0 = in (green), 1 = out (blue), 2 = info (grey) ── */
void ui_log_bytes(int dir, const char *p, int n);    /* raw bytes: a line without newline continues */
void ui_log(int dir, const char *fmt, ...) __attribute__((format(printf, 2, 3)));   /* one line, also papp_log */
void ui_log_clear(void);
void ui_log_draw(int x, int y, int w, int h, const char *empty);

/* ── The remote's state and its Control screen (y 112..464) ── */
#define REMOTE_MSG_MAX 32
typedef struct {
    uint8_t mode;                            /* 0 Idle, 1 Run, 2 Test */
    uint8_t level;                           /* 0..100 */
    uint8_t led;                             /* 0 / 1 */
    char    msg[REMOTE_MSG_MAX + 1];
    char    source[48];                      /* "Last change: ..." */
    uint32_t rev;                            /* +1 on every change */
} remote_t;
extern remote_t remote;
extern const char *const remote_mode_name[3];
enum { REMOTE_MODE = 1, REMOTE_LEVEL = 2, REMOTE_LED = 4, REMOTE_MSG = 8 };   /* what changed */

void remote_changed(int what, const char *who);   /* rev++, "Last change: who" */
void remote_draw(const char *hint);               /* hint: the line under the controls */
int  remote_act(int id, int x);                   /* touch: what changed (0: not a remote id) */
int  remote_pad(uint32_t press);                  /* UP/DOWN mode, LEFT/RIGHT level, A LED */
