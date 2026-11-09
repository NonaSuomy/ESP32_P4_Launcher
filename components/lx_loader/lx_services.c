/*
 * Launcher services exported to PAPP apps (SYS, MEM, FS; the others in
 * lx_svc_*.c) with per-run
 * resource tracking, so a run can always be torn down without leaks.
 *
 * Every service entry point is bracketed by SVC_ENTER/SVC_LEAVE. The App
 * Manager only force-deletes an app task while that counter is zero, so an
 * app is never killed while it holds a launcher/driver lock (heap, VFS, ...).
 */
#include "lx_services.h"

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp.h"
#include "lx_comp.h"
#include "lx_audio.h"
#include "lx_input.h"
#include "lx_ui.h"

static const char *TAG = "lx_svc";

#define LX_ABI_VERSION  ((2u << 16) | 0u)     /* 2.0 preview */

static lx_app_log_sink_t s_log_sink;
volatile uint32_t lx_svc_busy_count;
volatile bool lx_svc_exit_flag;
static SemaphoreHandle_t s_track_lock;


/* ── Allocation tracking: open-addressing hash set of live pointers ─────── */

static uintptr_t *s_ptrs;      /* 0 = empty, 1 = tombstone */
static uint32_t   s_ptr_cap;   /* power of two             */
static uint32_t   s_ptr_used;  /* live + tombstones        */
static uint32_t   s_ptr_live;

#define PTR_EMPTY 0u
#define PTR_TOMB  1u

static uint32_t ptr_hash(uintptr_t p)
{
    uint32_t h = (uint32_t)p >> 3;
    h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15;
    return h;
}

static bool ptr_rehash(uint32_t new_cap)
{
    uintptr_t *old = s_ptrs;
    uint32_t old_cap = s_ptr_cap;
    uintptr_t *tbl = heap_caps_calloc(new_cap, sizeof(uintptr_t), MALLOC_CAP_SPIRAM);
    if (!tbl) return false;
    s_ptrs = tbl; s_ptr_cap = new_cap; s_ptr_used = 0; s_ptr_live = 0;
    for (uint32_t i = 0; i < old_cap; i++) {
        uintptr_t p = old[i];
        if (p > PTR_TOMB) {
            uint32_t j = ptr_hash(p) & (new_cap - 1);
            while (tbl[j] != PTR_EMPTY) j = (j + 1) & (new_cap - 1);
            tbl[j] = p; s_ptr_used++; s_ptr_live++;
        }
    }
    heap_caps_free(old);
    return true;
}

static void track_add(void *ptr)
{
    if (!ptr) return;
    xSemaphoreTake(s_track_lock, portMAX_DELAY);
    if ((s_ptr_used + 1) * 4 > s_ptr_cap * 3) {
        uint32_t cap = s_ptr_live * 4 > s_ptr_cap ? s_ptr_cap * 2 : s_ptr_cap;
        if (!ptr_rehash(cap < 1024 ? 1024 : cap)) {
            xSemaphoreGive(s_track_lock);
            ESP_LOGE(TAG, "alloc tracker full; %p untracked", ptr);
            return;
        }
    }
    uint32_t j = ptr_hash((uintptr_t)ptr) & (s_ptr_cap - 1);
    while (s_ptrs[j] > PTR_TOMB) j = (j + 1) & (s_ptr_cap - 1);
    if (s_ptrs[j] == PTR_EMPTY) s_ptr_used++;
    s_ptrs[j] = (uintptr_t)ptr;
    s_ptr_live++;
    xSemaphoreGive(s_track_lock);
}

static bool track_remove(void *ptr)
{
    if (!ptr || !s_ptrs) return false;
    bool found = false;
    xSemaphoreTake(s_track_lock, portMAX_DELAY);
    uint32_t j = ptr_hash((uintptr_t)ptr) & (s_ptr_cap - 1);
    for (uint32_t n = 0; n < s_ptr_cap && s_ptrs[j] != PTR_EMPTY; n++) {
        if (s_ptrs[j] == (uintptr_t)ptr) {
            s_ptrs[j] = PTR_TOMB;
            s_ptr_live--;
            found = true;
            break;
        }
        j = (j + 1) & (s_ptr_cap - 1);
    }
    xSemaphoreGive(s_track_lock);
    return found;
}

/* ── File tracking ──────────────────────────────────────────────────────── */

#define MAX_FILES 32
static FILE *s_files[MAX_FILES];
static void *s_file_bufs[MAX_FILES];

/* SD reads into a buffer that isn't DMA-aligned go one sector at a time through
 * a bounce buffer (1.5 MB/s instead of 15), and stdio's own buffer isn't aligned.
 * The buffer must outlive the file: free it after fclose. NULL keeps the default. */
void *lx_file_fast_io(FILE *f)
{
    void *buf = heap_caps_aligned_alloc(64, LX_FILE_BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (buf && setvbuf(f, buf, _IOFBF, LX_FILE_BUF_SIZE) != 0) {
        heap_caps_free(buf);
        buf = NULL;
    }
    return buf;
}

static bool file_track(FILE *f)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (!s_files[i]) { s_files[i] = f; s_file_bufs[i] = lx_file_fast_io(f); return true; }
    return false;
}

/* ── SYS ────────────────────────────────────────────────────────────────── */

static void sys_log_write(const char *s, size_t len)
{
    SVC_ENTER();
    if (s_log_sink && s && len) s_log_sink(s, len);
    SVC_LEAVE();
}

static int sys_log_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) sys_log_write(buf, n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
    return n;
}

static int64_t sys_time_us(void)
{
    return esp_timer_get_time();
}

static void sys_delay_ms(uint32_t ms)
{
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t ? t : (ms ? 1 : 0));
}

static bool sys_exit_requested(void)
{
    return lx_svc_exit_flag;
}

static const char *sys_board_id(void)
{
    return bsp_info()->id;
}

static bool sys_local_time(papp_datetime_t *out)
{
    time_t now = time(NULL);
    if (now < 1700000000) return false;
    struct tm t;
    localtime_r(&now, &t);
    *out = (papp_datetime_t){ .year = (int16_t)(t.tm_year + 1900), .month = (uint8_t)(t.tm_mon + 1),
                              .day = (uint8_t)t.tm_mday, .hour = (uint8_t)t.tm_hour,
                              .minute = (uint8_t)t.tm_min, .second = (uint8_t)t.tm_sec,
                              .weekday = (uint8_t)t.tm_wday };
    return true;
}

static bool sys_utc_time(int64_t *seconds)
{
    time_t now = time(NULL);
    if (now < 1700000000) return false;
    *seconds = (int64_t)now;
    return true;
}

static const papp_if_sys_t s_if_sys = {
    .hdr = { PAPP_IF_SYS, PAPP_IF_SYS_VER, sizeof(papp_if_sys_t) },
    .abi_version    = LX_ABI_VERSION,
    .log_write      = sys_log_write,
    .log_vprintf    = sys_log_vprintf,
    .time_us        = sys_time_us,
    .delay_ms       = sys_delay_ms,
    .exit_requested = sys_exit_requested,
    .board_id       = sys_board_id,
    .local_time     = sys_local_time,
    .utc_time       = sys_utc_time,
};

/* ── MEM ────────────────────────────────────────────────────────────────── */

static uint32_t mem_idf_caps(uint32_t caps, size_t size)
{
    uint32_t c = MALLOC_CAP_8BIT;
    if (caps & PAPP_MEM_FAST)       c |= MALLOC_CAP_INTERNAL;
    else if (caps & PAPP_MEM_LARGE) c |= MALLOC_CAP_SPIRAM;
    else if (size >= 1024)          c |= MALLOC_CAP_SPIRAM;   /* default policy: big → PSRAM */
    else                            c |= MALLOC_CAP_INTERNAL;
    if (caps & PAPP_MEM_DMA)        c |= MALLOC_CAP_DMA;
    return c;
}

static void *mem_alloc(size_t size, uint32_t caps, size_t align)
{
    SVC_ENTER();
    uint32_t c = mem_idf_caps(caps, size);
    if (caps & PAPP_MEM_DMA) {
        if (align < 64) align = 64;
        size = (size + 63) & ~(size_t)63;    /* PPA/DMA want whole cache lines */
    }
    void *p = align > 4 ? heap_caps_aligned_alloc(align, size, c) : heap_caps_malloc(size, c);
    if (!p && !(caps & (PAPP_MEM_FAST | PAPP_MEM_LARGE))) {
        c ^= MALLOC_CAP_SPIRAM | MALLOC_CAP_INTERNAL;   /* no preference: try the other pool */
        p = align > 4 ? heap_caps_aligned_alloc(align, size, c) : heap_caps_malloc(size, c);
    }
    track_add(p);
    SVC_LEAVE();
    return p;
}

static void *mem_realloc(void *ptr, size_t size)
{
    SVC_ENTER();
    if (ptr && !track_remove(ptr)) {
        ESP_LOGW(TAG, "realloc of unknown pointer %p", ptr);
    }
    uint32_t c = MALLOC_CAP_8BIT | (size >= 1024 ? MALLOC_CAP_SPIRAM : 0);
    void *p = heap_caps_realloc(ptr, size, c);
    if (!p && size >= 1024) p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    if (p) track_add(p);
    else if (ptr && size) track_add(ptr);    /* realloc failed: old block still owned */
    SVC_LEAVE();
    return p;
}

static void mem_free(void *ptr)
{
    if (!ptr) return;
    SVC_ENTER();
    if (track_remove(ptr)) heap_caps_free(ptr);
    else ESP_LOGW(TAG, "free of unknown pointer %p ignored", ptr);
    SVC_LEAVE();
}

static size_t mem_free_size(uint32_t caps)
{
    return heap_caps_get_free_size(mem_idf_caps(caps, (caps & PAPP_MEM_FAST) ? 0 : 1024));
}

static const papp_if_mem_t s_if_mem = {
    .hdr = { PAPP_IF_MEM, PAPP_IF_MEM_VER, sizeof(papp_if_mem_t) },
    .alloc     = mem_alloc,
    .realloc   = mem_realloc,
    .free      = mem_free,
    .free_size = mem_free_size,
};

/* ── FS ─────────────────────────────────────────────────────────────────── */

static papp_file_t *fs_open(const char *path, const char *mode)
{
    SVC_ENTER();
    FILE *f = fopen(path, mode);
    if (f && !file_track(f)) {
        fclose(f);
        f = NULL;
        ESP_LOGE(TAG, "too many open files");
    }
    SVC_LEAVE();
    return (papp_file_t *)f;
}

static int fs_close(papp_file_t *f)
{
    if (!f) return -1;
    SVC_ENTER();
    int r = -1;
    for (int i = 0; i < MAX_FILES; i++) {
        if (s_files[i] != (FILE *)f) continue;
        r = fclose((FILE *)f);
        heap_caps_free(s_file_bufs[i]);
        s_files[i] = NULL;
        s_file_bufs[i] = NULL;
        break;
    }
    SVC_LEAVE();
    return r;
}

static size_t fs_read(papp_file_t *f, void *buf, size_t len)
{
    SVC_ENTER();
    size_t n = fread(buf, 1, len, (FILE *)f);
    SVC_LEAVE();
    return n;
}

static size_t fs_write(papp_file_t *f, const void *buf, size_t len)
{
    SVC_ENTER();
    size_t n = fwrite(buf, 1, len, (FILE *)f);
    SVC_LEAVE();
    return n;
}

static int fs_seek(papp_file_t *f, long off, int whence)
{
    SVC_ENTER();
    int r = fseek((FILE *)f, off, whence);
    SVC_LEAVE();
    return r;
}

static long fs_tell(papp_file_t *f)
{
    SVC_ENTER();
    long r = ftell((FILE *)f);
    SVC_LEAVE();
    return r;
}

static long fs_size(const char *path)
{
    struct stat st;
    SVC_ENTER();
    long r = stat(path, &st) == 0 ? (long)st.st_size : -1;
    SVC_LEAVE();
    return r;
}

static int fs_remove(const char *path)
{
    SVC_ENTER();
    int r = remove(path);
    SVC_LEAVE();
    return r;
}

static int fs_mkdir(const char *path)
{
    SVC_ENTER();
    int r = mkdir(path, 0775);
    SVC_LEAVE();
    return r;
}

static const papp_if_fs_t s_if_fs = {
    .hdr = { PAPP_IF_FS, PAPP_IF_FS_VER, sizeof(papp_if_fs_t) },
    .open   = fs_open,
    .close  = fs_close,
    .read   = fs_read,
    .write  = fs_write,
    .seek   = fs_seek,
    .tell   = fs_tell,
    .size   = fs_size,
    .remove = fs_remove,
    .mkdir  = fs_mkdir,
};

/* ── Registry & run bookkeeping ─────────────────────────────────────────── */

static const papp_iface_hdr_t *const s_registry[] = {
    &s_if_sys.hdr,
    &s_if_mem.hdr,
    &s_if_fs.hdr,
    &lx_if_disp.hdr,
    &lx_if_audi.hdr,
    &lx_if_inpt.hdr,
    &lx_if_thrd.hdr,
    &lx_if_ui.hdr,
    &lx_if_net.hdr,
#if CONFIG_BT_NIMBLE_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE
    &lx_if_ble.hdr,
#endif
};

void lx_svc_init(lx_app_log_sink_t log_sink)
{
    s_log_sink = log_sink;
    s_track_lock = xSemaphoreCreateMutex();
    lx_svc_net_init();
    lx_svc_ble_init();
}

const papp_iface_hdr_t *lx_svc_find(uint32_t id, uint16_t min_version)
{
    for (size_t i = 0; i < sizeof(s_registry) / sizeof(s_registry[0]); i++) {
        if (s_registry[i]->id == id)
            return s_registry[i]->version >= min_version ? s_registry[i] : NULL;
    }
    return NULL;
}

void lx_svc_begin_run(void)
{
    lx_svc_exit_flag = false;
    lx_overlay_reset();
    lx_input_flush();           /* the app sees only input from its own run */
    lx_svc_busy_count = 0;
    memset(s_files, 0, sizeof(s_files));
    memset(s_file_bufs, 0, sizeof(s_file_bufs));
    s_ptrs = NULL; s_ptr_cap = 0; s_ptr_used = 0; s_ptr_live = 0;
}

void lx_svc_end_run(uint32_t *leaked_allocs, uint32_t *leaked_bytes, uint32_t *leaked_files)
{
    uint32_t na = 0, nb = 0, nf = 0;
    for (uint32_t i = 0; i < s_ptr_cap; i++) {
        if (s_ptrs[i] > PTR_TOMB) {
            nb += heap_caps_get_allocated_size((void *)s_ptrs[i]);
            heap_caps_free((void *)s_ptrs[i]);
            na++;
        }
    }
    heap_caps_free(s_ptrs);
    s_ptrs = NULL; s_ptr_cap = 0; s_ptr_used = 0; s_ptr_live = 0;

    for (int i = 0; i < MAX_FILES; i++) {
        if (s_files[i]) {
            fclose(s_files[i]);
            heap_caps_free(s_file_bufs[i]);
            s_files[i] = NULL;
            s_file_bufs[i] = NULL;
            nf++;
        }
    }
    lx_comp_destroy_all();      /* surfaces, streams and sync objects go */
    lx_audio_close_all();       /* with the app; not counted as leaks    */
    lx_thrd_end_run();
    lx_svc_net_end_run();       /* after the threads: none of them is in a call */
    lx_svc_ble_end_run();       /* advertising, connections, GATT table, host */
    lx_ui_reset();              /* a browser the app was killed in */
    *leaked_allocs = na; *leaked_bytes = nb; *leaked_files = nf;
}

void lx_svc_request_exit(void)
{
    lx_svc_exit_flag = true;
}

uint32_t lx_svc_busy(void)
{
    return lx_svc_busy_count;
}
