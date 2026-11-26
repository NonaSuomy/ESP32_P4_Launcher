/*
 * App Manager + .papp v2 loader.
 *
 * Load:  header → SLOW image (PSRAM, 64 KB pages) + FAST image (executable
 *        internal SRAM, or PSRAM after SLOW if it doesn't fit) → CRC →
 *        relocations → imports → D-cache writeback → executable MMU alias →
 *        I-cache invalidate.
 * Run:   dedicated task (internal or PSRAM stack per manifest).
 * Exit:  app returns (or a thread calls exit) → App Manager stops the main
 *        task and every app thread, frees every tracked resource, unmaps,
 *        reports via exit_cb.
 *
 * The executable alias is the mechanism proven in RetroESP32_P4
 * (psram_app_loader.c); what is new is that the image is relocated to
 * wherever the alias lands instead of assuming 0x4A000000.
 */
#include "lx_app.h"
#include "lx_services.h"
#include "lx_common.h"
#include "papp_abi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_mmu_map.h"
#include "esp_cache.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "lx_app";

#define MMU_PAGE_SIZE       0x10000
#define CACHE_LINE          64
#define ALIGN_UP(x, a)      (((x) + (a) - 1) & ~((a) - 1))
#define DEFAULT_STACK       (16 * 1024)
#define MAX_INTERNAL_STACK  (32 * 1024)
#define MAX_IMAGE_SIZE      (24 * 1024 * 1024)
#define APP_TASK_PRIO       5
#define APP_TASK_CORE       0

typedef struct {
    char            path[128];
    papp2_header_t  hdr;
    uint8_t        *buf;          /* data-side view of the image  */
    size_t          alloc;
    void           *exec;         /* executable alias             */
    uint8_t        *fast_buf;     /* FAST region, data-side view  */
    uint8_t        *fast_exec;    /* FAST region, runtime address */
    uint8_t        *fast_sram;    /* internal SRAM block (or NULL: FAST lives in PSRAM) */
    void           *test_pad;     /* CONFIG_LX_APP_TEST_BASE_SHIFT_PAGES mapping */
    void           *test_pad_buf;
    StaticTask_t   *tcb;
    StackType_t    *stack;
    TaskHandle_t    task;
    int             argc;
    char          **argv;         /* one block: pointers + strings */
} app_t;

typedef enum { MSG_EXITED, MSG_STOP } msg_type_t;
typedef struct { msg_type_t type; int code; uint32_t grace_ms; uint32_t run; } msg_t;

static app_t                    *s_app;
static volatile lx_app_state_t   s_state;
static QueueHandle_t             s_queue;
static lx_app_exit_cb_t          s_exit_cb;
static volatile bool              s_relaunching;  /* between teardown and the relaunch */
static char                      s_cur_path[128];
static volatile uint32_t         s_run;          /* tags MSG_EXITED so a late one can't end the next run */

/* Survives a panic reboot so the next boot can say which app crashed and where. */
#define CRASH_MAGIC 0xC0DEDEADu
typedef struct { uint32_t magic; lx_app_crash_ctx_t ctx; } crash_rec_t;
static __NOINIT_ATTR crash_rec_t s_crash_rec;
static lx_app_crash_ctx_t s_last_crash;
static bool s_has_last_crash;

/* ── Loading ────────────────────────────────────────────────────────────── */

static esp_err_t fail(char *err, size_t len, esp_err_t code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, len, fmt, ap);
    va_end(ap);
    ESP_LOGE(TAG, "%s", err);
    return code;
}

static inline uint32_t rd32(const uint8_t *p)  { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void     wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

/* Cross-region auipc deltas, so PCREL_LO12 records can find their HI part
 * (the packer emits each HI right before its LOs). */
#define PCREL_RING 32
typedef struct {
    struct { uint32_t key, delta; } e[PCREL_RING];
    uint32_t pos;
} pcrel_ring_t;

static esp_err_t apply_relocs(app_t *a, const papp2_reloc_t *r, uint32_t n, pcrel_ring_t *ring,
                              char *err, size_t err_len)
{
    const papp2_header_t *h = &a->hdr;
    const uint32_t base = (uint32_t)(uintptr_t)a->exec;
    const uint32_t fast_base = (uint32_t)(uintptr_t)a->fast_exec;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t off = PAPP2_RELOC_OFF(&r[i]);
        uint32_t type = PAPP2_RELOC_TYPE(&r[i]);
        bool site_fast = r[i].off_type & PAPP2_RELOC_SITE_FAST;
        uint32_t val = r[i].value & ~PAPP2_RELOC_VAL_FAST;
        bool val_fast = r[i].value & PAPP2_RELOC_VAL_FAST;
        if (off + 4 > (site_fast ? h->fast_image_size : h->image_size) ||
            (type <= PAPP2_R_PCREL_HI20 &&
             val > (val_fast ? h->fast_image_size + h->fast_bss_size : h->image_size + h->bss_size)))
            return fail(err, err_len, ESP_ERR_INVALID_SIZE, "reloc %lu out of range", (unsigned long)i);

        uint8_t *site = (site_fast ? a->fast_buf : a->buf) + off;
        uint32_t pc = (site_fast ? fast_base : base) + off;
        uint32_t v = (val_fast ? fast_base : base) + val;
        uint32_t insn = rd32(site);

        switch (type) {
        case PAPP2_R_PCREL_HI20:
            v -= pc;
            ring->e[ring->pos % PCREL_RING].key = off | (site_fast ? PAPP2_RELOC_VAL_FAST : 0);
            ring->e[ring->pos++ % PCREL_RING].delta = v;
            type = PAPP2_R_HI20;
            break;
        case PAPP2_R_PCREL_LO12_I:
        case PAPP2_R_PCREL_LO12_S: {
            uint32_t k = 0;
            while (k < PCREL_RING && ring->e[k].key != r[i].value) k++;
            if (k == PCREL_RING)
                return fail(err, err_len, ESP_ERR_INVALID_ARG, "reloc %lu: low part without its auipc",
                            (unsigned long)i);
            v = ring->e[k].delta;
            type = type == PAPP2_R_PCREL_LO12_I ? PAPP2_R_LO12_I : PAPP2_R_LO12_S;
            break;
        }
        case PAPP2_R_DIFF32: {
            uint32_t shift = (fast_base - PAPP2_FAST_LINK_BASE) - base;   /* FAST moved this much more */
            wr32(site, insn + (r[i].value == 1 ? shift : -shift));
            continue;
        }
        }

        uint32_t lo = v & 0xFFF;
        switch (type) {
        case PAPP2_R_ABS32:
            insn = v;
            break;
        case PAPP2_R_HI20:
            insn = (insn & 0xFFF) | ((v + 0x800) & 0xFFFFF000u);
            break;
        case PAPP2_R_LO12_I:
            insn = (insn & 0x000FFFFFu) | (lo << 20);
            break;
        case PAPP2_R_LO12_S:
            insn = (insn & 0x01FFF07Fu) | ((lo >> 5) << 25) | ((lo & 0x1F) << 7);
            break;
        default:
            return fail(err, err_len, ESP_ERR_NOT_SUPPORTED, "unknown reloc type %lu", (unsigned long)type);
        }
        wr32(site, insn);
    }
    return ESP_OK;
}

static esp_err_t resolve_imports(app_t *a, char *err, size_t err_len)
{
    const papp2_header_t *h = &a->hdr;
    if (h->imports_count == 0) return ESP_OK;
    if ((h->imports_off & 3) ||
        h->imports_off + h->imports_count * sizeof(papp_import_t) > h->image_size)
        return fail(err, err_len, ESP_ERR_INVALID_SIZE, "bad import table");

    papp_import_t *imp = (papp_import_t *)(a->buf + h->imports_off);
    for (uint32_t i = 0; i < h->imports_count; i++) {
        const papp_iface_hdr_t *iface = lx_svc_find(imp[i].id, imp[i].min_version);
        if (!iface && !(imp[i].flags & PAPP_IMPORT_OPTIONAL)) {
            char id[5];
            memcpy(id, &imp[i].id, 4);
            id[4] = 0;
            return fail(err, err_len, ESP_ERR_NOT_SUPPORTED,
                        "app requires interface '%s' v%u, not provided by this launcher",
                        id, imp[i].min_version);
        }
        imp[i].iface = iface;
    }
    return ESP_OK;
}

static void app_free(app_t *a)
{
    if (!a) return;
    if (a->exec)  esp_mmu_unmap(a->exec);
    if (a->test_pad) esp_mmu_unmap(a->test_pad);
    if (a->test_pad_buf) heap_caps_free(a->test_pad_buf);
    if (a->buf)   heap_caps_free(a->buf);
    if (a->fast_sram) heap_caps_free(a->fast_sram);
    if (a->stack) heap_caps_free(a->stack);
    if (a->tcb)   heap_caps_free(a->tcb);
    free(a->argv);
    free(a);
}

static esp_err_t load_image(app_t *a, FILE *f, char *err, size_t err_len)
{
    papp2_header_t *h = &a->hdr;

    if (fread(h, 1, sizeof(*h), f) != sizeof(*h))
        return fail(err, err_len, ESP_ERR_INVALID_SIZE, "file too short");
    if (h->magic != PAPP2_MAGIC)
        return fail(err, err_len, ESP_ERR_INVALID_ARG, "not a PAPP v2 image (magic %08lx)",
                    (unsigned long)h->magic);
    if (h->fmt_version != PAPP2_FMT_VERSION || h->header_size != PAPP2_HEADER_SIZE)
        return fail(err, err_len, ESP_ERR_INVALID_VERSION, "unsupported format version %u",
                    h->fmt_version);
    if (h->image_size == 0 || h->image_size + h->bss_size > MAX_IMAGE_SIZE ||
        h->entry_off >= h->image_size || h->fast_image_size + h->fast_bss_size > MAX_IMAGE_SIZE ||
        (h->fast_image_size | h->fast_bss_size) & 3)
        return fail(err, err_len, ESP_ERR_INVALID_SIZE, "bad image sizes");

    /* FAST region: executable internal SRAM if it fits the per-app budget,
     * otherwise right after SLOW in the same PSRAM block (correct, slower). */
    size_t fast_total = ALIGN_UP(h->fast_image_size + h->fast_bss_size, CACHE_LINE);
    size_t slow_total = ALIGN_UP(h->image_size + h->bss_size, CACHE_LINE);
    if (fast_total && fast_total <= CONFIG_LX_APP_FAST_MAX_KB * 1024u)
        a->fast_sram = heap_caps_aligned_alloc(CACHE_LINE, fast_total,
                                               MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL);   /* not 8BIT: heap refuses EXEC+8BIT */
    if (fast_total && !a->fast_sram)
        ESP_LOGW(TAG, "FAST region (%u B) goes to PSRAM: %s", (unsigned)fast_total,
                 fast_total > CONFIG_LX_APP_FAST_MAX_KB * 1024u ? "over CONFIG_LX_APP_FAST_MAX_KB"
                                                                : "no executable internal SRAM free");

    a->alloc = ALIGN_UP(slow_total + (a->fast_sram ? 0 : fast_total), MMU_PAGE_SIZE);
    a->buf = heap_caps_aligned_alloc(MMU_PAGE_SIZE, a->alloc, MALLOC_CAP_SPIRAM);
    if (!a->buf)
        return fail(err, err_len, ESP_ERR_NO_MEM, "no PSRAM for %u byte image", (unsigned)a->alloc);
    a->fast_buf = a->fast_sram ? a->fast_sram : a->buf + slow_total;

    int64_t t0 = esp_timer_get_time();
    if (fread(a->buf, 1, h->image_size, f) != h->image_size ||
        fread(a->fast_buf, 1, h->fast_image_size, f) != h->fast_image_size)
        return fail(err, err_len, ESP_ERR_INVALID_SIZE, "short read on image");
    memset(a->buf + h->image_size, 0, slow_total - h->image_size);
    if (fast_total) memset(a->fast_buf + h->fast_image_size, 0, fast_total - h->fast_image_size);
    if (!a->fast_sram) memset(a->buf + slow_total + fast_total, 0, a->alloc - slow_total - fast_total);
    int64_t t1 = esp_timer_get_time();

    if (lx_crc32(lx_crc32(0, a->buf, h->image_size), a->fast_buf, h->fast_image_size) != h->image_crc32)
        return fail(err, err_len, ESP_ERR_INVALID_CRC, "image CRC mismatch (corrupt file?)");

    /* Relocations stream from the file in fixed-size batches; they need the
     * runtime base, so map the executable alias first. */
    esp_paddr_t paddr = 0;
    mmu_target_t target = MMU_TARGET_PSRAM0;
    esp_err_t e = esp_mmu_vaddr_to_paddr(a->buf, &paddr, &target);
    if (e != ESP_OK) return fail(err, err_len, e, "vaddr_to_paddr failed");
#if CONFIG_LX_APP_TEST_BASE_SHIFT_PAGES > 0
    /* Occupy virtual space first so the exec alias lands at a different base */
    {
        size_t pad = CONFIG_LX_APP_TEST_BASE_SHIFT_PAGES * MMU_PAGE_SIZE;
        a->test_pad_buf = heap_caps_aligned_alloc(MMU_PAGE_SIZE, pad, MALLOC_CAP_SPIRAM);
        esp_paddr_t pad_paddr;
        if (a->test_pad_buf && esp_mmu_vaddr_to_paddr(a->test_pad_buf, &pad_paddr, &target) == ESP_OK)
            esp_mmu_map(pad_paddr, pad, MMU_TARGET_PSRAM0, MMU_MEM_CAP_READ | MMU_MEM_CAP_32BIT,
                        ESP_MMU_MMAP_FLAG_PADDR_SHARED, &a->test_pad);
    }
#endif
    e = esp_mmu_map(paddr, a->alloc, MMU_TARGET_PSRAM0,
                    MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ | MMU_MEM_CAP_32BIT,
                    ESP_MMU_MMAP_FLAG_PADDR_SHARED, &a->exec);
    if (e != ESP_OK) {
        a->exec = NULL;
        return fail(err, err_len, e, "esp_mmu_map(exec) failed: %s", esp_err_to_name(e));
    }
    a->fast_exec = a->fast_sram ? a->fast_sram : (uint8_t *)a->exec + slow_total;

    enum { BATCH = 512 };
    pcrel_ring_t ring;
    memset(&ring, 0xFF, sizeof(ring));      /* no valid key is all ones */
    ring.pos = 0;
    papp2_reloc_t *batch = malloc(BATCH * sizeof(papp2_reloc_t));
    if (!batch) return fail(err, err_len, ESP_ERR_NO_MEM, "no memory for relocations");
    uint32_t crc = 0;
    for (uint32_t done = 0; done < h->relocs_count;) {
        uint32_t n = h->relocs_count - done;
        if (n > BATCH) n = BATCH;
        if (fread(batch, sizeof(papp2_reloc_t), n, f) != n) {
            free(batch);
            return fail(err, err_len, ESP_ERR_INVALID_SIZE, "short read on relocations");
        }
        crc = lx_crc32(crc, batch, n * sizeof(papp2_reloc_t));
        e = apply_relocs(a, batch, n, &ring, err, err_len);
        if (e != ESP_OK) { free(batch); return e; }
        done += n;
    }
    free(batch);
    if (crc != h->relocs_crc32)
        return fail(err, err_len, ESP_ERR_INVALID_CRC, "relocation CRC mismatch");

    e = resolve_imports(a, err, err_len);
    if (e != ESP_OK) return e;

    esp_cache_msync(a->buf, a->alloc, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    esp_cache_msync(a->exec, a->alloc, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
    if (a->fast_sram) {
        /* Internal SRAM is reached through the L1 caches too: write the data
         * side back, drop stale instruction lines (both cores) */
        esp_cache_msync(a->fast_sram, fast_total, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
        esp_cache_msync(a->fast_sram, fast_total, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
        __asm__ volatile("fence.i" ::: "memory");
    }
    int64_t t2 = esp_timer_get_time();

    ESP_LOGI(TAG, "loaded %s: image=%lu bss=%lu relocs=%lu imports=%lu exec=%p (read %lld ms, fixup %lld ms)",
             a->path, (unsigned long)h->image_size, (unsigned long)h->bss_size,
             (unsigned long)h->relocs_count, (unsigned long)h->imports_count, a->exec,
             (t1 - t0) / 1000, (t2 - t1) / 1000);
    if (fast_total)
        ESP_LOGI(TAG, "FAST region: %lu + bss %lu B at %p (%s)", (unsigned long)h->fast_image_size,
                 (unsigned long)h->fast_bss_size, a->fast_exec, a->fast_sram ? "internal SRAM" : "PSRAM");
    return ESP_OK;
}

static const papp_manifest_t *app_manifest(const app_t *a)
{
    const papp2_header_t *h = &a->hdr;
    if (h->manifest_off == 0 || (h->manifest_off & 3) ||
        h->manifest_off + sizeof(papp_manifest_t) > h->image_size)
        return NULL;
    const papp_manifest_t *m = (const papp_manifest_t *)(a->buf + h->manifest_off);
    return m->magic == PAPP_MANIFEST_MAGIC ? m : NULL;
}

/* ── Running ────────────────────────────────────────────────────────────── */

static void app_task(void *arg)
{
    app_t *a = arg;
    papp_start_fn_t start = (papp_start_fn_t)((uint8_t *)a->exec + a->hdr.entry_off);
    /* Register before any app code runs: a launcher task on this core with a
     * lower priority is preempted inside xTaskCreate, before it could do so */
    lx_thrd_begin_run(xTaskGetCurrentTaskHandle());
    lx_app_post_exit(start(a->argc, a->argv));
    for (;;) vTaskSuspend(NULL);    /* the App Manager deletes us (and may resume us meanwhile) */
}

void lx_app_post_exit(int code)
{
    msg_t m = { .type = MSG_EXITED, .code = code, .run = s_run };
    xQueueSend(s_queue, &m, portMAX_DELAY);
}

static char **build_argv(const char *path, int argc, const char *const *argv, int *out_argc)
{
    size_t bytes = (argc + 2) * sizeof(char *) + strlen(path) + 1;
    for (int i = 0; i < argc; i++) bytes += strlen(argv[i]) + 1;
    char **v = malloc(bytes);
    if (!v) return NULL;
    char *s = (char *)(v + argc + 2);
    v[0] = s; strcpy(s, path); s += strlen(path) + 1;
    for (int i = 0; i < argc; i++) {
        v[i + 1] = s; strcpy(s, argv[i]); s += strlen(argv[i]) + 1;
    }
    v[argc + 1] = NULL;
    *out_argc = argc + 1;
    return v;
}

esp_err_t lx_app_launch(const char *path, int argc, const char *const *argv,
                        char *err, size_t err_len)
{
    if (s_state != LX_APP_IDLE)
        return fail(err, err_len, ESP_ERR_INVALID_STATE, "an app is already running");

    lx_lpram_sweep();           /* low-power RAM IDF gave back (main task stack): never for the app's own memory */
    app_t *a = calloc(1, sizeof(*a));
    if (!a) return fail(err, err_len, ESP_ERR_NO_MEM, "out of memory");
    strlcpy(a->path, path, sizeof(a->path));

    FILE *f = fopen(path, "rb");
    if (!f) { free(a); return fail(err, err_len, ESP_ERR_NOT_FOUND, "cannot open %s", path); }
    void *iobuf = lx_file_fast_io(f);
    esp_err_t e = load_image(a, f, err, err_len);
    fclose(f);
    heap_caps_free(iobuf);
    if (e != ESP_OK) { app_free(a); return e; }

    const papp_manifest_t *mf = app_manifest(a);
    uint32_t stack = (mf && mf->main_stack) ? mf->main_stack : DEFAULT_STACK;
    bool psram_stack = stack > MAX_INTERNAL_STACK || (mf && (mf->flags & PAPP_MF_STACK_PSRAM));
    if (mf) ESP_LOGI(TAG, "app '%s' (%s) v%s, stack %lu (%s)", mf->name, mf->id, mf->version,
                     (unsigned long)stack, psram_stack ? "PSRAM" : "internal");

    a->argv = build_argv(path, argc, argv, &a->argc);
    a->stack = heap_caps_malloc(stack, psram_stack ? MALLOC_CAP_SPIRAM : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    a->tcb = heap_caps_calloc(1, sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!a->argv || !a->stack || !a->tcb) {
        app_free(a);
        return fail(err, err_len, ESP_ERR_NO_MEM, "no memory for app stack (%lu bytes)", (unsigned long)stack);
    }

    lx_svc_begin_run();
    s_run++;
    snprintf(s_cur_path, sizeof(s_cur_path), "%s", a->path);
    s_app = a;
    s_state = LX_APP_RUNNING;

    s_crash_rec.magic = CRASH_MAGIC;
    strlcpy(s_crash_rec.ctx.path, a->path, sizeof(s_crash_rec.ctx.path));
    s_crash_rec.ctx.exec_base = (uint32_t)(uintptr_t)a->exec;
    s_crash_rec.ctx.image_size = a->hdr.image_size + a->hdr.bss_size;
    s_crash_rec.ctx.fast_base = (uint32_t)(uintptr_t)a->fast_exec;
    s_crash_rec.ctx.fast_size = a->hdr.fast_image_size + a->hdr.fast_bss_size;

    a->task = xTaskCreateStaticPinnedToCore(app_task, "papp_main", stack / sizeof(StackType_t),
                                            a, APP_TASK_PRIO, a->stack, a->tcb, APP_TASK_CORE);
    return ESP_OK;
}

bool lx_app_fast_sram(uintptr_t *base, size_t *size)
{
    app_t *a = s_app;
    if (!a || !a->fast_sram) return false;
    *base = (uintptr_t)a->fast_sram;
    *size = ALIGN_UP(a->hdr.fast_image_size + a->hdr.fast_bss_size, CACHE_LINE);
    return true;
}

/* ── Teardown (App Manager task) ────────────────────────────────────────── */

/* Delete the app's tasks (main + threads), but never while one of them is
 * inside a service call (it might hold the heap or VFS lock) or still on the
 * other core's CPU (the busy count could still change, and the kernel would
 * finish the delete after we free the TCB and stack). Returns false if they
 * could not be stopped. */
static bool kill_app_tasks(app_t *a, int *threads)
{
    for (int tries = 0; tries < 400; tries++) {
        vTaskSuspend(a->task);
        lx_thrd_suspend_all(true);
        lx_task_wait_off_cpu(a->task);
        if (lx_svc_busy() == 0) {
            *threads = lx_thrd_kill_all();
            vTaskDelete(a->task);
            return true;
        }
        lx_thrd_suspend_all(false);
        vTaskResume(a->task);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

static void teardown(int code, bool forced)
{
    app_t *a = s_app;
    int threads = 0;
    lx_svc_request_exit();          /* blocking service calls (audio write) give up */
    if (!kill_app_tasks(a, &threads)) {
        ESP_LOGE(TAG, "app stuck inside a service call; rebooting");
        esp_restart();
    }
    if (threads) ESP_LOGW(TAG, "stopped %d app thread(s) still running at exit", threads);

    uint32_t na, nb, nf;
    lx_svc_end_run(&na, &nb, &nf);
    if (na || nf)
        ESP_LOGW(TAG, "reclaimed %lu allocations (%lu bytes) and %lu files left by the app",
                 (unsigned long)na, (unsigned long)nb, (unsigned long)nf);

    char path[sizeof(a->path)];
    strlcpy(path, a->path, sizeof(path));
    app_free(a);
    s_app = NULL;
    s_cur_path[0] = 0;
    s_crash_rec.magic = 0;
    s_state = LX_APP_IDLE;

    ESP_LOGI(TAG, "%s exited with %d%s; free: internal %u, PSRAM %u", path, code,
             forced ? " (forced)" : "",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (s_exit_cb) s_exit_cb(path, code, forced, na, nf);
}

/* PAPP_EXIT_RELAUNCH: start the app again (no arguments). lx_app_state()
 * keeps reporting RUNNING meanwhile, so the home screen doesn't take over. */
static void relaunch(const char *path)
{
    char err[96];
    ESP_LOGI(TAG, "relaunching %s", path);
    if (lx_app_launch(path, 0, NULL, err, sizeof(err)) != ESP_OK)
        ESP_LOGE(TAG, "relaunch failed: %s", err);
}

static void manager_task(void *arg)
{
    msg_t m;
    for (;;) {
        xQueueReceive(s_queue, &m, portMAX_DELAY);
        if (!s_app || (m.type == MSG_EXITED && m.run != s_run)) continue;
        if (m.type == MSG_EXITED) {
            if (m.code == PAPP_EXIT_RELAUNCH) {
                char path[sizeof(s_app->path)];
                strlcpy(path, s_app->path, sizeof(path));
                s_relaunching = true;
                teardown(m.code, false);
                relaunch(path);
                s_relaunching = false;
            } else {
                teardown(m.code, false);
            }
        } else if (m.type == MSG_STOP) {
            s_state = LX_APP_STOPPING;
            lx_svc_request_exit();
            msg_t r;
            TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(m.grace_ms);
            bool exited = false;
            for (;;) {
                TickType_t now = xTaskGetTickCount();
                if ((int32_t)(until - now) <= 0 ||
                    xQueueReceive(s_queue, &r, until - now) != pdTRUE) break;
                if (r.type == MSG_EXITED && r.run == s_run) { exited = true; break; }
            }
            teardown(exited ? r.code : -1, !exited);
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t lx_app_init(lx_app_log_sink_t log_sink, lx_app_exit_cb_t exit_cb)
{
    esp_reset_reason_t rr = esp_reset_reason();
    if (s_crash_rec.magic == CRASH_MAGIC &&
        (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT)) {
        s_last_crash = s_crash_rec.ctx;
        s_last_crash.path[sizeof(s_last_crash.path) - 1] = 0;
        s_has_last_crash = true;
        ESP_LOGW(TAG, "previous reset: crash in %s (exec base 0x%08lx)", s_last_crash.path,
                 (unsigned long)s_last_crash.exec_base);
    }
    s_crash_rec.magic = 0;

    s_exit_cb = exit_cb;
    lx_svc_init(log_sink);
    s_queue = xQueueCreate(4, sizeof(msg_t));
    BaseType_t ok = xTaskCreatePinnedToCore(manager_task, "app_mgr", 4096, NULL, 6, NULL, 1);
    return (s_queue && ok == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t lx_app_stop(uint32_t grace_ms)
{
    if (s_state != LX_APP_RUNNING) return ESP_ERR_INVALID_STATE;
    msg_t m = { .type = MSG_STOP, .grace_ms = grace_ms };
    return xQueueSend(s_queue, &m, 0) == pdTRUE ? ESP_OK : ESP_FAIL;
}

lx_app_state_t lx_app_state(void)
{
    if (s_relaunching) return LX_APP_RUNNING;
    return s_state;
}

const char *lx_app_path(void)
{
    return s_cur_path;          /* a copy: the app record is freed at exit, under readers' feet */
}

uint32_t lx_app_exec_base(void)
{
    return s_app ? (uint32_t)(uintptr_t)s_app->exec : 0;
}

esp_err_t lx_app_read_manifest(const char *path, papp_manifest_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return ESP_ERR_NOT_FOUND;
    papp2_header_t h;
    esp_err_t e = ESP_ERR_INVALID_ARG;
    if (fread(&h, 1, sizeof(h), f) == sizeof(h) && h.magic == PAPP2_MAGIC && h.manifest_off &&
        h.manifest_off + sizeof(*out) <= h.image_size &&
        fseek(f, sizeof(h) + h.manifest_off, SEEK_SET) == 0 &&
        fread(out, 1, sizeof(*out), f) == sizeof(*out) && out->magic == PAPP_MANIFEST_MAGIC) {
        out->id[sizeof(out->id) - 1] = 0;
        out->name[sizeof(out->name) - 1] = 0;
        out->version[sizeof(out->version) - 1] = 0;
        e = ESP_OK;
    }
    fclose(f);
    return e;
}

bool lx_app_last_crash(lx_app_crash_ctx_t *out)
{
    if (s_has_last_crash && out) *out = s_last_crash;
    return s_has_last_crash;
}
