/*
 * DevLink — remote development service over USB-Serial-JTAG.
 * Protocol: docs/ARCHITECTURE.md §7.8 and devlink_proto.h; host side: tools/papp_dev.py.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the configured DevLink transport(s), hooks ESP_LOG output, and
 * starts the command task(s). Call after SD mount and lx_app_init(). */
/* First thing in app_main: keep the boot log for DL_BOOTLOG */
void lx_devlink_early_init(void);
esp_err_t lx_devlink_start(void);
/* Stops (true) and restarts (false) DevLink while an app has USB-C as a device of its own (lx_usbd). */
void lx_devlink_pause(bool pause);
/* True when the network transport is compiled in; keep Wi-Fi available while
 * apps run so a host can continue to monitor or stop them over TCP. */
bool lx_devlink_network_enabled(void);

/* Counts changes to /sd/apps made over DevLink (put, rm, mkdir, rename): the home screen rescans */
uint32_t lx_devlink_app_changes(void);

/* Log sink for app output (pass to lx_app_init). */
void lx_devlink_app_log(const char *text, size_t len);

/* App exit notification (pass to lx_app_init). */
void lx_devlink_app_exited(const char *path, int code, bool forced,
                           uint32_t leaked_allocs, uint32_t leaked_files);

#ifdef __cplusplus
}
#endif
