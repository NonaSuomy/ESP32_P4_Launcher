/* Small helpers shared by launcher components. */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#define LX_LAUNCHER_VERSION  "0.36.1"
#define LX_ABI_STRING        "2.0-preview"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard CRC-32 (IEEE 802.3, same as zlib/binascii.crc32).
 * Chain calls by passing the previous result as `crc`; start with 0. */
uint32_t lx_crc32(uint32_t crc, const void *data, size_t len);

/* Initialize before starting services that write to the SD card. The try-lock
 * serializes multi-call mutations across the web file manager and DevLink. */
bool lx_common_init(void);
void lx_sd_write_lock(void);
bool lx_sd_write_try_lock(void);
void lx_sd_write_unlock(void);

/* Call while holding the SD write lock. Replaces a closed staging file without
 * unlinking the old destination first; attempts to restore it if commit fails. */
int lx_sd_replace_file(const char *staging_path, const char *destination_path);

#ifdef __cplusplus
}
#endif
