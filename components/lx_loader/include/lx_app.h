/*
 * App Manager + .papp v2 loader.
 *
 * Loads a relocatable .papp from the SD card into PSRAM, maps it executable,
 * resolves its interface imports, runs it in its own task and tears everything
 * down when it exits (or is force-stopped). One app at a time.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "papp_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LX_APP_IDLE = 0,
    LX_APP_RUNNING,
    LX_APP_STOPPING,
} lx_app_state_t;

/* Receives everything the app logs (SYS.log_*). */
typedef void (*lx_app_log_sink_t)(const char *text, size_t len);

/* Called from the App Manager task after teardown. */
typedef void (*lx_app_exit_cb_t)(const char *path, int code, bool forced,
                                 uint32_t leaked_allocs, uint32_t leaked_files);

/* Context of the app that was running when the device last panicked. */
typedef struct {
    char     path[128];
    uint32_t exec_base;   /* runtime address of image offset 0 */
    uint32_t image_size;  /* image + bss                        */
    uint32_t fast_base;   /* runtime address of the FAST region */
    uint32_t fast_size;   /* FAST image + bss (0 = none)        */
} lx_app_crash_ctx_t;

esp_err_t lx_app_init(lx_app_log_sink_t log_sink, lx_app_exit_cb_t exit_cb);

/* First thing at boot: takes the P4's slow low-power RAM out of the heap, so no internal
 * allocation can land there; apps get it only with PAPP_MEM_LPRAM (lx_lpram.c). */
void lx_lpram_claim(void);

/* Load and start an app. Returns when the app is running (or failed to load,
 * with a human-readable reason in err). argv[0] is set to path. */
esp_err_t lx_app_launch(const char *path, int argc, const char *const *argv,
                        char *err, size_t err_len);

/* Ask the running app to exit; force-stop it after grace_ms. Non-blocking. */
esp_err_t lx_app_stop(uint32_t grace_ms);

/* HOME during a run: asks for the in-game menu, which the app's next present
 * opens. False when it can't come up (the app is in a launcher screen, or never
 * presented since the last request): then the caller stops the app. */
bool lx_app_overlay_request(void);

lx_app_state_t lx_app_state(void);
const char    *lx_app_path(void);          /* "" when idle */
uint32_t       lx_app_exec_base(void);     /* 0 when idle  */

/* Reads an app's manifest (name, id, version) without loading the app. */
esp_err_t lx_app_read_manifest(const char *path, papp_manifest_t *out);

/* True if the previous reset was a crash while an app was running. */
bool lx_app_last_crash(lx_app_crash_ctx_t *out);

#ifdef __cplusplus
}
#endif
