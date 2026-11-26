/*
 * The P4's low-power RAM (LP SRAM at 0x50108000, 32 KB) is kept away from ordinary allocations.
 *
 * IDF adds it to the heap as internal RAM (CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP), last in line: once
 * the main internal SRAM can't fit a request, an internal allocation lands there. The HP CPU reaches it
 * uncached over the LP bus, about 50 times slower than SRAM, so an app's FAST buffer or thread stack could
 * end up there and run at a fraction of its speed (#38).
 *
 * So the launcher takes every free piece of it out of the heap: at boot, and again before each app starts,
 * because IDF's own start-up can hold some of it and give it back later (on the Guition boards the main
 * task's stack is there, and returns to the heap when app_main ends). Pieces of 1 KB or more become small
 * private heaps that apps reach only by asking: papp_alloc(size, PAPP_MEM_LPRAM, align), for data that is
 * rarely touched. Smaller pieces just stay taken.
 */
#include "lx_services.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "multi_heap.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "lx_lpram";

#define MAX_POOLS  6
#define MIN_POOL   1024

typedef struct { multi_heap_handle_t heap; uintptr_t lo, hi; } pool_t;
static pool_t s_pool[MAX_POOLS];
static int s_npool;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;     /* shared by the pools' heaps */

static void sweep(bool boot)
{
    size_t taken = 0;
    for (int i = 0; i < 32; i++) {                  /* every free piece, largest first */
        size_t n = heap_caps_get_largest_free_block(MALLOC_CAP_RTCRAM);
        if (n < 8) break;
        void *p = NULL;
        while (n >= 8 && !(p = heap_caps_malloc(n, MALLOC_CAP_RTCRAM))) n -= 4;   /* the heap's own overhead */
        if (!p) break;
        taken += n;
        if (n < MIN_POOL || s_npool == MAX_POOLS) continue;       /* stays taken, unused */
        multi_heap_handle_t h = multi_heap_register(p, n);
        if (!h) continue;
        multi_heap_set_lock(h, &s_lock);
        s_pool[s_npool++] = (pool_t){ h, (uintptr_t)p, (uintptr_t)p + n };
    }
    if (boot || taken)
        ESP_LOGI(TAG, "low-power RAM: took %u bytes out of the heap; %u free for PAPP_MEM_LPRAM in %d pieces",
                 (unsigned)taken, (unsigned)lx_lpram_free_size(), s_npool);
}

void lx_lpram_claim(void) { sweep(true); }
void lx_lpram_sweep(void) { sweep(false); }

static pool_t *pool_of(const void *p)
{
    for (int i = 0; i < s_npool; i++)
        if ((uintptr_t)p >= s_pool[i].lo && (uintptr_t)p < s_pool[i].hi) return &s_pool[i];
    return NULL;
}

bool lx_lpram_owns(const void *p)
{
    return p && pool_of(p);
}

void *lx_lpram_alloc(size_t size, size_t align)
{
    if (!size) return NULL;
    for (int i = 0; i < s_npool; i++) {
        multi_heap_handle_t h = s_pool[i].heap;
        void *p = align > 4 ? multi_heap_aligned_alloc(h, size, align) : multi_heap_malloc(h, size);
        if (p) return p;
    }
    return NULL;
}

/* Grows or shrinks in its own piece; else moves to another one */
void *lx_lpram_realloc(void *p, size_t size)
{
    pool_t *pl = pool_of(p);
    if (!pl) return NULL;
    void *q = multi_heap_realloc(pl->heap, p, size);
    if (q || !size) return q;
    size_t old = multi_heap_get_allocated_size(pl->heap, p);
    q = lx_lpram_alloc(size, 4);
    if (q) {
        memcpy(q, p, old < size ? old : size);
        multi_heap_free(pl->heap, p);
    }
    return q;
}

void lx_lpram_free(void *p)
{
    pool_t *pl = pool_of(p);
    if (pl) multi_heap_free(pl->heap, p);
}

size_t lx_lpram_size(const void *p)
{
    pool_t *pl = pool_of(p);
    return pl ? multi_heap_get_allocated_size(pl->heap, (void *)p) : 0;
}

size_t lx_lpram_free_size(void)
{
    size_t n = 0;
    for (int i = 0; i < s_npool; i++) n += multi_heap_free_size(s_pool[i].heap);
    return n;
}
