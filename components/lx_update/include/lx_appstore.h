/*
 * Online app catalogue: apps.json on the repository's "apps" release lists the
 * published apps (tools/make_apps_release.py). The launcher shows it and installs
 * or updates an app on the SD card. Both calls run in the background; follow them
 * with lx_appstore_state(). A /sd/update/apps_url.txt holding one URL replaces the
 * catalogue address, to test against another server.
 */
#pragma once
#include <stdbool.h>
#include "esp_err.h"

#define LX_APPSTORE_MAX 64
#define LX_APPSTORE_COMMUNITY "Community"     /* folder of the community apps */

typedef struct {
    char id[32];
    char name[32];
    char version[16];
    char folder[24];            /* where a new install goes; "" = main page */
    char dir[24];               /* /sd/apps/[<folder>/]<dir>/<file> */
    char file[32];
    char url[192];
    char icon[192];             /* "" = none */
    char sha256[65];
    long size;
    bool community;             /* shared without source, not reviewed (after the project's apps) */
    char author[32];            /* community apps */
    char description[124];      /* one line for the Store */
    char category[16];          /* emulators, games, tools, productivity; "" = not given */
} lx_appstore_app_t;

typedef enum {
    LX_APPSTORE_IDLE,
    LX_APPSTORE_BUSY,
    LX_APPSTORE_DONE,           /* catalogue read, or app installed */
    LX_APPSTORE_FAILED,         /* reason: lx_appstore_error() */
} lx_appstore_state_t;

/* Reads the catalogue, then saves each app's icon to lx_appstore_icon_path() if it
 * is not on the card yet (lx_appstore_percent() follows the icons) */
void lx_appstore_fetch(void);
int lx_appstore_count(void);
const lx_appstore_app_t *lx_appstore_app(int i);     /* changes while a fetch runs: copy it when DONE */

/* Downloads app i to papp_path (checked against its SHA-256, then put in place of
 * the old file) and its icon to icon_path. folder_icon_path, when not NULL and not
 * on the card yet, gets the icon of the app's folder. Directories are created. */
void lx_appstore_install(int i, const char *papp_path, const char *icon_path, const char *folder_icon_path);

lx_appstore_state_t lx_appstore_state(void);
void lx_appstore_reset(void);           /* result read: back to IDLE */
int lx_appstore_percent(void);
const char *lx_appstore_error(void);

/* The last fetch replaced /sd/config/gamepads.txt (the controller mapping database) */
bool lx_appstore_gamepads_updated(void);

/* Where the catalogue icon of app a is kept on the card ("" when it has none) */
const char *lx_appstore_icon_path(const lx_appstore_app_t *a, char *buf, int len);

/* a > b for "x.y.z" versions */
bool lx_appstore_newer(const char *a, const char *b);
