/*
 * Launcher self-update (A/B). A launcher.bin in /sd/update is flashed to the
 * next OTA slot. A new version must call lx_update_confirm() once it works; if
 * it crashes first, the bootloader returns to the previous one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#define LX_UPDATE_FILE  "/sd/update/launcher.bin"
#define LX_UPDATE_DONE  "/sd/update/launcher.done"

/* Mark the running image good (no-op unless it is on probation). */
void lx_update_confirm(void);

/* True when the running image came back from a failed update this boot. */
bool lx_update_rolled_back(void);

/* True if LX_UPDATE_FILE is a launcher for this chip with another version. */
bool lx_update_available(char *version, size_t len);

/* Flash the file and select it for the next boot; progress in percent. The
 * caller restarts. The file is renamed first, so it can't be offered again. */
esp_err_t lx_update_install(void (*progress)(int percent));

/* Online update from the newest GitHub release. Both calls run in the background;
 * follow them with lx_update_online_state(). READY means the checked file is in
 * LX_UPDATE_FILE, for lx_update_install(). */
typedef enum {
    LX_ONLINE_IDLE,
    LX_ONLINE_BUSY,
    LX_ONLINE_LATEST,       /* nothing newer */
    LX_ONLINE_AVAILABLE,    /* lx_update_online_version() is newer */
    LX_ONLINE_READY,
    LX_ONLINE_FAILED,       /* reason: lx_update_online_error() */
    LX_ONLINE_C6_READY,     /* the Wi-Fi chip firmware is in LX_UPDATE_C6_FILE */
} lx_online_state_t;

/* The release's Wi-Fi chip (ESP32-C6) firmware for this board, from the last check:
 * its version, "" if the release has none. Downloaded by lx_update_online_download_c6(). */
#define LX_UPDATE_C6_FILE  "/sd/update/c6.bin"
const char *lx_update_online_c6_version(void);
void lx_update_online_download_c6(void);

void lx_update_online_check(void);
void lx_update_online_download(void);
lx_online_state_t lx_update_online_state(void);
void lx_update_online_reset(void);          /* result read: back to IDLE */
int lx_update_online_percent(void);
const char *lx_update_online_version(void);
const char *lx_update_online_error(void);
