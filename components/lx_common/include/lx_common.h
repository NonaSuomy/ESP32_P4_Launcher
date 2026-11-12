/* Small helpers shared by launcher components. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define LX_LAUNCHER_VERSION  "0.29.4"
#define LX_ABI_STRING        "2.0-preview"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard CRC-32 (IEEE 802.3, same as zlib/binascii.crc32).
 * Chain calls by passing the previous result as `crc`; start with 0. */
uint32_t lx_crc32(uint32_t crc, const void *data, size_t len);

#ifdef __cplusplus
}
#endif
