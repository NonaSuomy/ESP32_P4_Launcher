#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_8BIT   (1u << 0)
#define MALLOC_CAP_DMA    (1u << 2)
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)

#ifdef PAX_APP_SIDE
#ifdef __cplusplus
extern "C" {
#endif
void *heap_caps_malloc(size_t n, uint32_t caps);
void heap_caps_free(void *p);
#ifdef PAPP_NATIVE_PAPP2
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
#endif
#ifdef __cplusplus
}
#endif
/* PAX ROM loading deliberately prefers complete in-memory regions.  The
 * launcher allocator has more than enough PSRAM for the compact test set;
 * this estimate prevents the legacy streamer from switching to FATFS. */
static inline size_t heap_caps_get_free_size(uint32_t caps)
{
    (void)caps;
    return 32u * 1024u * 1024u;
}
static inline size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return 32u * 1024u * 1024u;
}
#else
static inline void *heap_caps_malloc(size_t n, uint32_t caps)
{
    (void)caps;
    return malloc(n);
}
#endif
#if !defined(PAPP_NATIVE_PAPP2)
static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    (void)caps;
    return calloc(n, size);
}
#endif
#ifndef PAX_APP_SIDE
static inline void heap_caps_free(void *p) { free(p); }
#endif

/* Implemented by the PAX syscall shim so the returned pointer can be
 * aligned in the launcher's PSRAM allocator and reclaimed with the normal
 * tracked-allocation cleanup path. */
#ifdef __cplusplus
extern "C" {
#endif
void *heap_caps_aligned_alloc(size_t alignment, size_t size, uint32_t caps);
#ifdef __cplusplus
}
#endif

static inline void *heap_caps_aligned_calloc(size_t alignment, size_t n,
                                             size_t size, uint32_t caps)
{
    size_t total = n * size;
    void *p = heap_caps_aligned_alloc(alignment, total, caps);
    if (p) __builtin_memset(p, 0, total);
    return p;
}
