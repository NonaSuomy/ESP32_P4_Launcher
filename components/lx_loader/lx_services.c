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
#include "lx_usbd.h"

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include "esp_random.h"
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_mmu_map.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "bsp.h"
#include "lx_comp.h"
#include "lx_audio.h"
#include "lx_input.h"
#include "lx_ui.h"
#include "lx_zip.h"

static const char *TAG = "lx_svc";

#define LX_ABI_VERSION  ((2u << 16) | 0u)     /* 2.0 preview */

static lx_app_log_sink_t s_log_sink;
volatile uint32_t lx_svc_busy_count;
volatile bool lx_svc_exit_flag;
static SemaphoreHandle_t s_track_lock;


/* â”€â”€ Allocation tracking: open-addressing hash set of live pointers â”€â”€â”€â”€â”€â”€â”€ */

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

/* â”€â”€ File tracking â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

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

static bool file_track(FILE *f, bool fast_io)
{
    for (int i = 0; i < MAX_FILES; i++)
        if (!s_files[i]) { s_files[i] = f; s_file_bufs[i] = fast_io ? lx_file_fast_io(f) : NULL; return true; }
    return false;
}

/* â”€â”€ SYS â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

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

static int sys_chip_rev(void)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    return ci.revision;              /* major * 100 + minor */
}

static void sys_random(void *buf, size_t len)
{
    if (buf && len) esp_fill_random(buf, len);
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
    .chip_rev       = sys_chip_rev,
    .random         = sys_random,
};

/* â”€â”€ MEM â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

static uint32_t mem_idf_caps(uint32_t caps, size_t size)
{
    uint32_t c = MALLOC_CAP_8BIT;
    if (caps & PAPP_MEM_FAST)       c |= MALLOC_CAP_INTERNAL;
    else if (caps & PAPP_MEM_LARGE) c |= MALLOC_CAP_SPIRAM;
    else if (size >= 1024)          c |= MALLOC_CAP_SPIRAM;   /* default policy: big â†’ PSRAM */
    else                            c |= MALLOC_CAP_INTERNAL;
    if (caps & PAPP_MEM_DMA)        c |= MALLOC_CAP_DMA;
    return c;
}

static void *mem_alloc(size_t size, uint32_t caps, size_t align)
{
    SVC_ENTER();
    if (caps & PAPP_MEM_LPRAM) {                /* only on request: slow, rarely-touched data */
        void *lp = (caps & PAPP_MEM_DMA) ? NULL : lx_lpram_alloc(size, align);
        track_add(lp);
        SVC_LEAVE();
        return lp;
    }
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
    void *p;
    if (lx_lpram_owns(ptr)) {                   /* stays in low-power RAM */
        p = lx_lpram_realloc(ptr, size);
    } else {
        uint32_t c = MALLOC_CAP_8BIT | (size >= 1024 ? MALLOC_CAP_SPIRAM : 0);
        p = heap_caps_realloc(ptr, size, c);
        if (!p && size >= 1024) p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    }
    if (p) track_add(p);
    else if (ptr && size) track_add(ptr);    /* realloc failed: old block still owned */
    SVC_LEAVE();
    return p;
}

static void mem_free(void *ptr)
{
    if (!ptr) return;
    SVC_ENTER();
    if (!track_remove(ptr)) ESP_LOGW(TAG, "free of unknown pointer %p ignored", ptr);
    else if (lx_lpram_owns(ptr)) lx_lpram_free(ptr);
    else heap_caps_free(ptr);
    SVC_LEAVE();
}

static size_t mem_free_size(uint32_t caps)
{
    if (caps & PAPP_MEM_LPRAM) return lx_lpram_free_size();
    return heap_caps_get_free_size(mem_idf_caps(caps, (caps & PAPP_MEM_FAST) ? 0 : 1024));
}

/* Code memory (MEM v2) for JITs: a PSRAM block mapped a second time with the EXEC
 * capability, like the app image. The app uses only the executable alias (writes
 * work there too, as they do for its own .data). Caches are tagged by virtual
 * address: before the block goes back to the heap, the alias's dirty lines are
 * written back and dropped, so they can't land on the next owner's data later. */
#define CODE_PAGE   0x10000
#define MAX_CODE    64
typedef struct { void *buf, *exec; size_t size; } code_blk_t;
static code_blk_t s_code[MAX_CODE];

/* esp_mmu_map / unmap turn the caches off for a moment: the caller's stack must be
 * in internal RAM. App threads may have PSRAM stacks, so those calls run on a
 * short-lived helper task with an internal stack, and the caller waits. */
typedef struct { void (*fn)(void *); void *arg; SemaphoreHandle_t done; } internal_call_t;

static void internal_call_task(void *p)
{
    internal_call_t *c = p;
    c->fn(c->arg);
    xSemaphoreGive(c->done);
    vTaskDelete(NULL);
}

static void run_on_internal_stack(void (*fn)(void *), void *arg)
{
    int here;
    if (esp_ptr_internal(&here)) {
        fn(arg);
        return;
    }
    internal_call_t c = { fn, arg, xSemaphoreCreateBinary() };
    if (c.done && xTaskCreate(internal_call_task, "lx_mmu", 4096, &c, uxTaskPriorityGet(NULL), NULL) == pdPASS)
        xSemaphoreTake(c.done, portMAX_DELAY);
    else
        ESP_LOGE(TAG, "no memory for the mapping task");
    if (c.done) vSemaphoreDelete(c.done);
}

static void code_unmap(void *p)
{
    code_blk_t *b = p;
    esp_cache_msync(b->exec, b->size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                                      ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    esp_mmu_unmap(b->exec);
}

static void code_release(code_blk_t *b)
{
    run_on_internal_stack(code_unmap, b);
    heap_caps_free(b->buf);
    memset(b, 0, sizeof(*b));
}

static void code_map(void *p)
{
    code_blk_t *b = p;
    esp_paddr_t paddr;
    mmu_target_t target;
    b->exec = NULL;
    if (esp_mmu_vaddr_to_paddr(b->buf, &paddr, &target) != ESP_OK ||
        esp_mmu_map(paddr, b->size, MMU_TARGET_PSRAM0, MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ | MMU_MEM_CAP_32BIT,
                    ESP_MMU_MMAP_FLAG_PADDR_SHARED, &b->exec) != ESP_OK) {
        b->exec = NULL;
        return;
    }
    /* nothing of the heap's may stay dirty in the cache under the old address */
    esp_cache_msync(b->buf, b->size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE |
                                     ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}

static void *mem_code_alloc(size_t size)
{
    if (!size) return NULL;
    SVC_ENTER();
    void *ret = NULL;
    size = (size + CODE_PAGE - 1) & ~(size_t)(CODE_PAGE - 1);
    code_blk_t *b = NULL;
    for (int i = 0; i < MAX_CODE && !b; i++)
        if (!s_code[i].buf) b = &s_code[i];
    void *buf = b ? heap_caps_aligned_alloc(CODE_PAGE, size, MALLOC_CAP_SPIRAM) : NULL;
    if (buf) {
        code_blk_t nb = { buf, NULL, size };
        run_on_internal_stack(code_map, &nb);
        if (nb.exec) {
            *b = nb;
            ret = nb.exec;
        } else {
            heap_caps_free(buf);
        }
    }
    if (!ret) ESP_LOGW(TAG, "code_alloc(%u) failed", (unsigned)size);
    SVC_LEAVE();
    return ret;
}

static void mem_code_free(void *code)
{
    if (!code) return;
    SVC_ENTER();
    for (int i = 0; i < MAX_CODE; i++)
        if (s_code[i].exec == code) {
            code_release(&s_code[i]);
            break;
        }
    SVC_LEAVE();
}

static int mem_code_sync(void *code, size_t size)
{
    uintptr_t a = (uintptr_t)code & ~(uintptr_t)63, e = ((uintptr_t)code + size + 63) & ~(uintptr_t)63;
    if (!code || !size) return PAPP_E_ARG;
    SVC_ENTER();
    int rc = PAPP_E_ARG;
    for (int i = 0; i < MAX_CODE; i++) {
        uintptr_t b = (uintptr_t)s_code[i].exec;
        if (b && a >= b && e <= b + s_code[i].size) {
            esp_cache_msync((void *)a, e - a, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
            esp_cache_msync((void *)a, e - a, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
            __asm__ volatile("fence.i" ::: "memory");
            rc = PAPP_E_OK;
            break;
        }
    }
    /* Executable internal SRAM (the app's FAST region, e.g. a JIT's hot code buffer): same sync.
     * Launchers up to 0.29.1 return PAPP_E_ARG here, so apps can test for it. */
    if (rc != PAPP_E_OK && esp_ptr_internal((void *)a) && esp_ptr_internal((void *)(e - 1)) &&
        esp_ptr_executable((void *)a) && esp_ptr_executable((void *)(e - 1))) {
        esp_cache_msync((void *)a, e - a, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        esp_cache_msync((void *)a, e - a, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
        __asm__ volatile("fence.i" ::: "memory");
        rc = PAPP_E_OK;
    }
    SVC_LEAVE();
    return rc;
}

static const papp_if_mem_t s_if_mem = {
    .hdr = { PAPP_IF_MEM, PAPP_IF_MEM_VER, sizeof(papp_if_mem_t) },
    .alloc     = mem_alloc,
    .realloc   = mem_realloc,
    .free      = mem_free,
    .free_size = mem_free_size,
    .code_alloc = mem_code_alloc,
    .code_free  = mem_code_free,
    .code_sync  = mem_code_sync,
};

/* â”€â”€ Zipped ROMs â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */
/* An app opening "dir/Zelda.nes" that isn't there, with "dir/Zelda.zip" holding a .nes member, gets
 * the member: unpacked into PSRAM and served as an in-memory file, or, when it is too large to hold
 * twice (GBA pages its ROM from the open file), unpacked once into ZIP_CACHE and opened from there.
 * The cache keeps only the latest ROM. */

#define ZIP_CACHE      "/sd/launcher/zipcache"
#define ZIP_CACHE_SRC  ZIP_CACHE "/source.txt"

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   pos;
} memfile_t;

static ssize_t mf_read(void *c, char *buf, size_t n)
{
    memfile_t *m = c;
    if (m->pos >= m->size) return 0;
    if (n > m->size - m->pos) n = m->size - m->pos;
    memcpy(buf, m->data + m->pos, n);
    m->pos += n;
    return (ssize_t)n;
}

static int mf_seek(void *c, off_t *off, int whence)
{
    memfile_t *m = c;
    off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)m->pos : (off_t)m->size;
    off_t to = base + *off;
    if (to < 0) return -1;
    m->pos = (size_t)to;
    *off = to;
    return 0;
}

static int mf_close(void *c)
{
    memfile_t *m = c;
    heap_caps_free(m->data);
    heap_caps_free(m);
    return 0;
}

static bool zip_to_cache(const char *path, const char *zip, const lx_zip_member_t *m, char *out, size_t len)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, len, ZIP_CACHE "/%s", base);
    struct stat zs, cs;
    if (stat(zip, &zs) != 0) return false;
    char want[704], have[704] = "";
    snprintf(want, sizeof(want), "%s|%s|%08lx|%lu|%ld\n", out, zip, (unsigned long)m->crc,
             (unsigned long)m->usize, (long)zs.st_mtime);
    FILE *s = fopen(ZIP_CACHE_SRC, "r");
    if (s) { if (!fgets(have, sizeof(have), s)) have[0] = 0; fclose(s); }
    if (strcmp(want, have) == 0 && stat(out, &cs) == 0 && cs.st_size == (off_t)m->usize) return true;

    mkdir("/sd/launcher", 0775);
    mkdir(ZIP_CACHE, 0775);
    DIR *d = opendir(ZIP_CACHE);                /* only the latest ROM stays */
    if (d) {
        struct dirent *de;
        char p[300];
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.') continue;
            snprintf(p, sizeof(p), ZIP_CACHE "/%s", de->d_name);
            remove(p);
        }
        closedir(d);
    }
    int64_t t0 = esp_timer_get_time();
    if (lx_zip_unpack_file(zip, m, out) != 0) return false;
    ESP_LOGI(TAG, "%s: %lu bytes unpacked to the card in %lld ms", m->name, (unsigned long)m->usize,
             (esp_timer_get_time() - t0) / 1000);
    s = fopen(ZIP_CACHE_SRC, "w");
    if (s) { fputs(want, s); fclose(s); }
    return true;
}

static bool zip_in_ram(const char *path, const lx_zip_member_t *m)
{
    const char *dot = strrchr(path, '.');
    if (dot && (strcasecmp(dot, ".gba") == 0 || strcasecmp(dot, ".agb") == 0)) return false;
    size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    return m->usize <= 12u * 1024 * 1024 && (size_t)m->usize * 2 + 4u * 1024 * 1024 <= big;
}

/* NULL when path isn't a zipped ROM; sets *fast_io to whether the file wants a fast I/O buffer */
static FILE *zip_open(const char *path, bool *fast_io)
{
    char zip[300];
    lx_zip_member_t m;
    if (!lx_zip_for(path, zip, sizeof(zip), &m)) return NULL;
    if (zip_in_ram(path, &m)) {
        int64_t t0 = esp_timer_get_time();
        memfile_t *mf = heap_caps_calloc(1, sizeof(*mf), MALLOC_CAP_SPIRAM);
        if (mf) mf->data = lx_zip_unpack_mem(zip, &m);
        FILE *f = NULL;
        if (mf && mf->data) {
            mf->size = m.usize;
            f = fopencookie(mf, "rb", (cookie_io_functions_t){ .read = mf_read, .seek = mf_seek, .close = mf_close });
        }
        if (!f && mf) { heap_caps_free(mf->data); heap_caps_free(mf); }
        if (f) {
            setvbuf(f, NULL, _IONBF, 0);        /* reads go straight to memcpy */
            *fast_io = false;
            ESP_LOGI(TAG, "%s: %s unpacked into memory (%lu bytes, %lld ms)", zip, m.name,
                     (unsigned long)m.usize, (esp_timer_get_time() - t0) / 1000);
        }
        return f;
    }
    char cached[300];
    if (!zip_to_cache(path, zip, &m, cached, sizeof(cached))) return NULL;
    *fast_io = true;
    return fopen(cached, "rb");
}

static bool read_only(const char *mode)
{
    return mode && mode[0] == 'r' && !strchr(mode, '+');
}

/* â”€â”€ FS â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

static papp_file_t *fs_open(const char *path, const char *mode)
{
    SVC_ENTER();
    FILE *f = fopen(path, mode);
    bool fast_io = true;
    if (!f && read_only(mode)) f = zip_open(path, &fast_io);
    if (f && !file_track(f, fast_io)) {
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
    if (r < 0) {                                /* a zipped ROM under its virtual name */
        char zip[300];
        lx_zip_member_t m;
        if (lx_zip_for(path, zip, sizeof(zip), &m)) r = (long)m.usize;
    }
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

/* FS v2: stat, rename, rmdir and directories. Directories are tracked like files and
 * closed at the end of the run. */
static DIR *s_dirs[PAPP_FS_MAX_DIRS];

static int fs_stat(const char *path, papp_stat_t *out)
{
    struct stat st;
    SVC_ENTER();
    int r = stat(path, &st);
    if (r == 0 && out) {
        memset(out, 0, sizeof(*out));
        out->is_dir = S_ISDIR(st.st_mode);
        out->size = out->is_dir ? 0 : (int64_t)st.st_size;
        out->mtime = (int64_t)st.st_mtime;
    }
    SVC_LEAVE();
    return r == 0 ? 0 : -1;
}

static int fs_rename(const char *from, const char *to)
{
    SVC_ENTER();
    int r = rename(from, to);
    SVC_LEAVE();
    return r == 0 ? 0 : -1;
}

static int fs_rmdir(const char *path)
{
    SVC_ENTER();
    int r = rmdir(path);
    SVC_LEAVE();
    return r == 0 ? 0 : -1;
}

static papp_dir_t *fs_opendir(const char *path)
{
    SVC_ENTER();
    DIR *d = NULL;
    int slot = -1;
    for (int i = 0; i < PAPP_FS_MAX_DIRS && slot < 0; i++)
        if (!s_dirs[i]) slot = i;
    if (slot >= 0 && (d = opendir(path)) != NULL) s_dirs[slot] = d;
    SVC_LEAVE();
    return (papp_dir_t *)d;
}

static int dir_slot(papp_dir_t *d)
{
    for (int i = 0; d && i < PAPP_FS_MAX_DIRS; i++)
        if (s_dirs[i] == (DIR *)d) return i;
    return -1;
}

static int fs_readdir(papp_dir_t *d, papp_dirent_t *out)
{
    if (dir_slot(d) < 0 || !out) return -1;
    SVC_ENTER();
    int r = 0;
    struct dirent *e;
    while ((e = readdir((DIR *)d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        memset(out, 0, sizeof(*out));
        strlcpy(out->name, e->d_name, sizeof(out->name));
        out->is_dir = e->d_type == DT_DIR;
        r = 1;
        break;
    }
    SVC_LEAVE();
    return r;
}

static void fs_closedir(papp_dir_t *d)
{
    int i = dir_slot(d);
    if (i < 0) return;
    SVC_ENTER();
    closedir(s_dirs[i]);
    s_dirs[i] = NULL;
    SVC_LEAVE();
}

static int fs_volume_info(const char *mount_path, papp_fs_volume_info_t *out)
{
    if (!mount_path || !out) return -1;
    uint8_t fat_type = 0;
    memset(out, 0, sizeof(*out));
    SVC_ENTER();
    esp_err_t err = esp_vfs_fat_info(mount_path, &out->total_bytes, &out->free_bytes);
    if (err == ESP_OK) err = esp_vfs_fat_get_type(mount_path, &fat_type);
    if (err == ESP_OK) {
        switch (fat_type) {
        case FS_FAT12: out->type = PAPP_FS_TYPE_FAT12; break;
        case FS_FAT16: out->type = PAPP_FS_TYPE_FAT16; break;
        case FS_FAT32: out->type = PAPP_FS_TYPE_FAT32; break;
        case FS_EXFAT: out->type = PAPP_FS_TYPE_EXFAT; break;
        default: out->type = PAPP_FS_TYPE_UNKNOWN; break;
        }
    }
    SVC_LEAVE();
    return err == ESP_OK ? 0 : -1;
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
    .stat     = fs_stat,
    .rename   = fs_rename,
    .rmdir    = fs_rmdir,
    .opendir  = fs_opendir,
    .readdir  = fs_readdir,
    .closedir = fs_closedir,
    .volume_info = fs_volume_info,
};

/* â”€â”€ Registry & run bookkeeping â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

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
#if (CONFIG_BT_NIMBLE_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE) || (CONFIG_BT_BLUEDROID_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID)
    &lx_if_ble.hdr,
#endif
    &lx_if_usbh.hdr,
    &lx_if_pwr.hdr,
    &lx_if_hidd.hdr,
    &lx_if_irq.hdr,
    &lx_if_codc.hdr,
};

void lx_svc_init(lx_app_log_sink_t log_sink)
{
    s_log_sink = log_sink;
    s_track_lock = xSemaphoreCreateMutex();
    lx_svc_codec_init();
    lx_svc_net_init();
    lx_svc_ble_init();
    lx_svc_irq_init();
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
    lx_svc_irq_begin_run();
}

void lx_svc_end_run(uint32_t *leaked_allocs, uint32_t *leaked_bytes, uint32_t *leaked_files)
{
    uint32_t na = 0, nb = 0, nf = 0;
    lx_svc_irq_end_run();       /* first: no interrupt may run app code or touch its memory after this */
    for (uint32_t i = 0; i < s_ptr_cap; i++) {
        if (s_ptrs[i] > PTR_TOMB) {
            void *p = (void *)s_ptrs[i];
            if (lx_lpram_owns(p)) {
                nb += lx_lpram_size(p);
                lx_lpram_free(p);
            } else {
                nb += heap_caps_get_allocated_size(p);
                heap_caps_free(p);
            }
            na++;
        }
    }
    heap_caps_free(s_ptrs);
    s_ptrs = NULL; s_ptr_cap = 0; s_ptr_used = 0; s_ptr_live = 0;
    for (int i = 0; i < MAX_CODE; i++)
        if (s_code[i].buf) {
            nb += s_code[i].size;
            code_release(&s_code[i]);
            na++;
        }

    for (int i = 0; i < PAPP_FS_MAX_DIRS; i++)
        if (s_dirs[i]) { closedir(s_dirs[i]); s_dirs[i] = NULL; nf++; }
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
    lx_svc_usbh_end_run();      /* USB devices the app left open */
    lx_svc_pwr_end_run();       /* charging back on if the app stopped it */
    lx_svc_codec_end_run();     /* JPEG engines and their buffers */
    lx_usbd_end_run();          /* USB-C back to the launcher's USB link */
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
