/*
 * Import table + thin C API over the launcher interfaces.
 * The loader fills .iface in each import before _papp_start runs.
 */
#include "papp/papp.h"

__attribute__((section(".papp_imports"), used))
papp_import_t __papp_imports[] = {
    { PAPP_IF_SYS, 1, PAPP_IMPORT_REQUIRED, 0 },     /* v2 for papp_local_time, v3 for papp_utc_time */
    { PAPP_IF_MEM, PAPP_IF_MEM_VER, PAPP_IMPORT_REQUIRED, 0 },
    { PAPP_IF_FS,  PAPP_IF_FS_VER,  PAPP_IMPORT_OPTIONAL, 0 },
    { PAPP_IF_DISP, PAPP_IF_DISP_VER, PAPP_IMPORT_OPTIONAL, 0 },
    { PAPP_IF_AUDI, PAPP_IF_AUDI_VER, PAPP_IMPORT_OPTIONAL, 0 },
    { PAPP_IF_INPT, 1, PAPP_IMPORT_OPTIONAL, 0 },          /* v2 functions checked at call */
    { PAPP_IF_THRD, PAPP_IF_THRD_VER, PAPP_IMPORT_OPTIONAL, 0 },
    { PAPP_IF_UI, 1, PAPP_IMPORT_OPTIONAL, 0 },              /* v2 functions checked at call */
    { PAPP_IF_NET, 1, PAPP_IMPORT_OPTIONAL, 0 },             /* launcher 0.29+ */
    { PAPP_IF_BLE, 1, PAPP_IMPORT_OPTIONAL, 0 },             /* launcher 0.29+, boards with BLE */
};

#define SYS ((const papp_if_sys_t *)__papp_imports[0].iface)
#define MEM ((const papp_if_mem_t *)__papp_imports[1].iface)
#define FS  ((const papp_if_fs_t *)__papp_imports[2].iface)
#define DISP ((const papp_if_disp_t *)__papp_imports[3].iface)
#define AUDI ((const papp_if_audi_t *)__papp_imports[4].iface)
#define INPT ((const papp_if_inpt_t *)__papp_imports[5].iface)
#define THRD ((const papp_if_thrd_t *)__papp_imports[6].iface)
#define UI   ((const papp_if_ui_t *)__papp_imports[7].iface)
#define NET  ((const papp_if_net_t *)__papp_imports[8].iface)
#define BLE  ((const papp_if_ble_t *)__papp_imports[9].iface)

const papp_if_sys_t *papp_if_sys(void) { return SYS; }
const papp_if_mem_t *papp_if_mem(void) { return MEM; }
const papp_if_fs_t  *papp_fs(void)     { return FS; }

void papp_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    SYS->log_vprintf(fmt, ap);
    va_end(ap);
}

int64_t     papp_time_us(void)        { return SYS->time_us(); }
void        papp_delay_ms(uint32_t ms) { SYS->delay_ms(ms); }
bool        papp_exit_requested(void) { return SYS->exit_requested(); }
const char *papp_board_id(void)       { return SYS->board_id(); }
uint32_t    papp_launcher_abi(void)   { return SYS->abi_version; }
bool        papp_local_time(papp_datetime_t *out)
{
    return SYS->hdr.version >= 2 && SYS->local_time(out);
}

bool        papp_utc_time(int64_t *seconds)
{
    return SYS->hdr.version >= 3 && SYS->utc_time(seconds);
}

void  *papp_alloc(size_t size, uint32_t caps, size_t align) { return MEM->alloc(size, caps, align); }
void  *papp_realloc(void *ptr, size_t size)                 { return MEM->realloc(ptr, size); }
void   papp_free(void *ptr)                                 { MEM->free(ptr); }
size_t papp_mem_free_size(uint32_t caps)                    { return MEM->free_size(caps); }

const papp_if_disp_t *papp_if_disp(void) { return DISP; }

bool papp_disp_info(papp_disp_info_t *out)
{
    if (!DISP) return false;
    DISP->info(out);
    return true;
}

papp_surface_t *papp_disp_surface_create(const papp_surface_cfg_t *cfg)
{
    return DISP ? DISP->surface_create(cfg) : NULL;
}

void  papp_disp_surface_destroy(papp_surface_t *s)            { DISP->surface_destroy(s); }
void *papp_disp_lock(papp_surface_t *s, int *stride_px)       { return DISP->lock(s, stride_px); }
int   papp_disp_present_ext(papp_surface_t *s, const void *pixels) { return DISP->present_ext(s, pixels, 0); }
int   papp_disp_present(papp_surface_t *s)                    { return DISP->present(s, 0); }
int   papp_disp_touch(papp_surface_t *s, papp_touch_point_t *pts, int max)
{
    return DISP->touch_read(s, pts, max);
}

const papp_if_audi_t *papp_if_audi(void) { return AUDI; }

uint32_t papp_audio_output_rate(void) { return AUDI ? AUDI->output_rate() : 0; }

papp_audio_stream_t *papp_audio_open(const papp_audio_cfg_t *cfg)
{
    return AUDI ? AUDI->open(cfg) : NULL;
}

void   papp_audio_close(papp_audio_stream_t *s)                { AUDI->close(s); }
size_t papp_audio_write(papp_audio_stream_t *s, const void *data, size_t frames, uint32_t timeout_ms)
{
    return AUDI->write(s, data, frames, timeout_ms);
}
void   papp_audio_pause(papp_audio_stream_t *s, bool pause)    { AUDI->pause(s, pause); }
void   papp_audio_set_gain(papp_audio_stream_t *s, int percent) { AUDI->set_gain(s, percent); }
void   papp_audio_clear(papp_audio_stream_t *s)                { AUDI->clear(s); }
void   papp_audio_stats(papp_audio_stream_t *s, papp_audio_stats_t *out) { AUDI->stats(s, out); }

const papp_if_inpt_t *papp_if_inpt(void) { return INPT; }

bool papp_input_gamepad(int player, papp_gamepad_t *out)
{
    if (INPT) return INPT->gamepad(player, out);
    *out = (papp_gamepad_t){ 0 };
    return false;
}

bool     papp_input_key_down(uint16_t usage)          { return INPT && INPT->key_down(usage); }
uint16_t papp_input_modifiers(void)                   { return INPT ? INPT->modifiers() : 0; }
bool     papp_input_poll_event(papp_input_event_t *e) { return INPT && INPT->poll_event(e); }
void     papp_input_flush(void)                       { if (INPT) INPT->flush(); }

int papp_input_analog(int ch)
{
    return INPT && INPT->hdr.version >= 2 ? INPT->analog(ch) : -1;
}

/* ── Threads ────────────────────────────────────────────────────────────── */

const papp_if_thrd_t *papp_if_thrd(void) { return THRD; }

/* Frees the C library's per-thread state when a thread ends (libc_glue.c). */
void papp_rt_thread_end(void) __attribute__((weak));

typedef struct { papp_thread_fn_t fn; void *arg; } tramp_t;

static int thread_tramp(void *p)
{
    tramp_t t = *(tramp_t *)p;
    papp_free(p);
    int code = t.fn(t.arg);
    if (papp_rt_thread_end) papp_rt_thread_end();
    return code;
}

papp_thread_t *papp_thread_create(papp_thread_fn_t fn, void *arg, const papp_thread_cfg_t *cfg)
{
    if (!THRD || !fn) return NULL;
    tramp_t *t = papp_alloc(sizeof(*t), PAPP_MEM_FAST, 0);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    papp_thread_t *h = THRD->thread_create(cfg, thread_tramp, t);
    if (!h) papp_free(t);
    return h;
}

bool papp_thread_join(papp_thread_t *t, uint32_t timeout_ms, int *exit_code)
{
    return THRD && THRD->thread_join(t, timeout_ms, exit_code);
}

void papp_thread_exit(int code)
{
    if (THRD && THRD->thread_self()) {
        if (papp_rt_thread_end) papp_rt_thread_end();
        THRD->thread_exit(code);
    }
    papp_exit(code);                       /* main thread: ends the app */
}

papp_thread_t *papp_thread_self(void) { return THRD ? THRD->thread_self() : NULL; }
void           papp_yield(void)       { if (THRD) THRD->yield(); else SYS->delay_ms(0); }

papp_mutex_t *papp_mutex_create(bool recursive)          { return THRD ? THRD->mutex_create(recursive) : NULL; }
void          papp_mutex_destroy(papp_mutex_t *m)        { if (THRD) THRD->mutex_destroy(m); }
void          papp_mutex_lock(papp_mutex_t *m)           { THRD->mutex_lock(m, PAPP_WAIT_FOREVER); }
bool          papp_mutex_trylock(papp_mutex_t *m, uint32_t timeout_ms) { return THRD->mutex_lock(m, timeout_ms); }
void          papp_mutex_unlock(papp_mutex_t *m)         { THRD->mutex_unlock(m); }

papp_sem_t *papp_sem_create(uint32_t initial, uint32_t max) { return THRD ? THRD->sem_create(initial, max) : NULL; }
void        papp_sem_destroy(papp_sem_t *s)                 { if (THRD) THRD->sem_destroy(s); }
bool        papp_sem_take(papp_sem_t *s, uint32_t timeout_ms) { return THRD->sem_take(s, timeout_ms); }
void        papp_sem_give(papp_sem_t *s)                    { THRD->sem_give(s); }

papp_queue_t *papp_queue_create(uint32_t length, uint32_t item_size)
{
    return THRD ? THRD->queue_create(length, item_size) : NULL;
}
void     papp_queue_destroy(papp_queue_t *q)                               { if (THRD) THRD->queue_destroy(q); }
bool     papp_queue_send(papp_queue_t *q, const void *item, uint32_t timeout_ms) { return THRD->queue_send(q, item, timeout_ms); }
bool     papp_queue_recv(papp_queue_t *q, void *item, uint32_t timeout_ms)       { return THRD->queue_recv(q, item, timeout_ms); }
uint32_t papp_queue_count(papp_queue_t *q)                                 { return THRD->queue_count(q); }

/* ── UI ─────────────────────────────────────────────────────────────────── */

const papp_if_ui_t *papp_if_ui(void) { return UI; }

int papp_ui_pick_file(const papp_pick_cfg_t *cfg, papp_pick_result_t *out)
{
    if (!UI) {
        out->action = PAPP_PICK_CANCEL;
        return PAPP_PICK_CANCEL;
    }
    return UI->pick_file(cfg, out);
}

void papp_ui_message(const char *title, const char *text)
{
    if (UI) UI->message(title, text);
    else papp_log("%s: %s\n", title, text);
}

int papp_ui_menu(const char *title, const char *const *items, int n, int initial)
{
    return UI && UI->hdr.version >= 2 ? UI->menu(title, items, n, initial) : -1;
}

int papp_ui_volume(void)
{
    return UI && UI->hdr.version >= 3 ? UI->volume() : -1;
}

int papp_ui_wifi(void)
{
    return UI && UI->hdr.version >= 4 ? UI->wifi() : PAPP_E_UNSUPPORTED;
}

/* ── NET ────────────────────────────────────────────────────────────────── */

const papp_if_net_t *papp_net(void) { return NET; }

/* ── BLE ────────────────────────────────────────────────────────────────── */

const papp_if_ble_t *papp_ble(void) { return BLE; }
