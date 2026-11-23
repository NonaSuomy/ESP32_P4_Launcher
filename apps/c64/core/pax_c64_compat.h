/* Small compatibility surface used by the Frodo core's PAX build. */
#pragma once

#include "psram_app.h"

#ifndef MAX_SAVE_STATES_MEMORY
#define MAX_SAVE_STATES_MEMORY 1
#endif

#define KEY_SPC     0x20
#define KEY_CUD     129
#define KEY_F5      130
#define KEY_F3      131
#define KEY_F1      132
#define KEY_F7      133
#define KEY_CLR     134
#define KEY_DEL     135
#define KEY_SHL     136
#define KEY_SHR     137
#define KEY_HOM     138
#define KEY_R_S     139
#define KEY_COMM    140
#define KEY_CTL     141
#define KEY_BAK     142
#define KEY_POUND   143
#define KEY_SLO     144
#define KEY_RESTORE 145

#define JOY1_UP     (1 << 8)
#define JOY1_DOWN   (2 << 8)
#define JOY1_LEFT   (3 << 8)
#define JOY1_RIGHT  (4 << 8)
#define JOY1_BTN    (5 << 8)
#define JOY2_UP     (6 << 8)
#define JOY2_DOWN   (7 << 8)
#define JOY2_LEFT   (8 << 8)
#define JOY2_RIGHT  (9 << 8)
#define JOY2_BTN    (10 << 8)

#ifdef __cplusplus
extern "C" {
#endif

extern const app_services_t *_pax_svc;
FILE *_tmpfile(void);

/* The original ODROID-GO build used these hooks for its launcher and
 * multiplayer layer.  A PAX has one local machine and the launcher owns the
 * close control, so the hooks remain as tiny ABI-compatible stubs. */
static inline char mp_isMultiplayer(void) { return 0; }
void c64_beforeStart(void);
void c64_started(void);

#ifdef __cplusplus
}
#endif
