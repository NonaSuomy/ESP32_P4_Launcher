/*
 * Launcher UI screens (ARCHITECTURE §6.2 `UI`): the content browser and a
 * message box, drawn by the launcher over whatever is on the display.
 *
 * The browser lists cfg->root (folders + files with matching extensions),
 * with Favorites and Recent tabs (kept in /sd/launcher/, shared by all apps),
 * a live search filter (physical keyboard, or an on-screen one), and a
 * per-ROM menu: Run, or Resume / Restart / Delete save when a save exists,
 * plus Favorite. Gamepad, keyboard and touch all work.
 *
 * Calls block until the user is done or *abort becomes true. They run on the
 * caller's task; `guard` (lx_ui_set_guard) brackets every stretch of work that
 * holds a lock (display, files), so the App Manager never kills an app task
 * in the middle of one.
 */
#pragma once

#include <stdbool.h>
#include "papp_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

void lx_ui_set_guard(void (*enter)(void), void (*leave)(void));

int  lx_ui_pick_file(const papp_pick_cfg_t *cfg, papp_pick_result_t *out, volatile bool *abort);
void lx_ui_message(const char *title, const char *text, volatile bool *abort);
/* A list of choices over the current picture; returns the index picked, or -1
 * (B / tap outside / abort). At most LX_UI_MENU_MAX items. */
#define LX_UI_MENU_MAX 7
int  lx_ui_menu(const char *title, const char *const *items, int n, int initial, volatile bool *abort);
/* The launcher's volume over the current picture: left/right adjust, B/Y close,
 * closes by itself after 3 s; saved for the next boot. Returns the level. */
int  lx_ui_volume(volatile bool *abort);

/* The Wi-Fi picker: scan, choose, password keyboard, join; the network joined is
 * saved (lx_net_connect). Returns 1 connected, 0 cancelled, -1 abort. The caller
 * holds the radio (an app: lx_svc_net_hold). */
int  lx_ui_wifi(volatile bool *abort);

/* Frees what an interrupted call left behind (app killed while browsing). */
void lx_ui_reset(void);

#ifdef __cplusplus
}
#endif
