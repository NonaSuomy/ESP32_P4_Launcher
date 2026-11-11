/*
 * PAPP v1 compatibility host: runs a RetroESP32_P4 "PSRAM app" (v1, entry
 * app_entry(const app_services_t *)) as a v2 app. Every v1 service is
 * implemented with the v2 SDK, so the game's own glue (papp_main.c,
 * papp_syscalls.c, i_video/SDL shims, ...) builds unchanged.
 *
 * Differences from the v1 launcher, by design:
 *  - display_write_frame_custom: the frame goes to a v2 surface of its own
 *    size, scaled to fit by the compositor (the v1 scale factor is ignored).
 *  - Input: P1 = board pad + first USB pad. MENU comes from V1_MENU_BUTTON
 *    (default X) or a tap on the left edge of the screen; the right edge (or
 *    V1_VOLUME_BUTTON, default none) opens the launcher's volume overlay.
 *    Only the thread that draws handles the edges: games read input from
 *    other threads too (Doom's exit watchdog), and an overlay opened there
 *    was drawn over at once by the game's next frame.
 *  - HOME (launcher exit request) sets the game's own exit flag, which its
 *    glue already polls (renamed to v1_exit_requested at compile time).
 *  - task_delete of another task can't kill it (v2 threads end themselves);
 *    leftovers are stopped by the launcher when the app exits.
 *  - File services use the SDK FS interface directly (the game's newlib glue
 *    sits on top of them, so stdio must not be used here).
 */
#include "papp/papp.h"

#include <stdarg.h>
#include <string.h>

#define PAPP_APP_SIDE 1
#include "psram_app.h"

#ifndef V1_MENU_BUTTON
#define V1_MENU_BUTTON   PAPP_BTN_X
#endif
#ifndef V1_VOLUME_BUTTON
#define V1_VOLUME_BUTTON 0
#endif
#define EDGE_ZONE_PX     96          /* left / right screen edge touch zones */

#ifndef V1_STACK
#define V1_STACK 32768
#endif
PAPP_MANIFEST(.id = V1_ID, .name = V1_NAME, .version = PAPP_APP_VERSION, .main_stack = V1_STACK);

int app_entry(const app_services_t *svc);
extern volatile int v1_exit_requested;       /* the game's flag (see build) */

/* ── Display ───────────────────────────────────────────────────────────── */

static papp_surface_t *s_surf;
static int s_sw, s_sh;
static papp_mutex_t *s_disp_mx;
static papp_thread_t *s_draw_thread;         /* the thread that presents frames */
static bool s_drawn;

static papp_surface_t *surface_for(int w, int h)
{
    if (s_surf && (w != s_sw || h != s_sh)) {
        papp_disp_surface_destroy(s_surf);
        s_surf = NULL;
    }
    if (!s_surf) {
        papp_surface_cfg_t cfg = { .width = w, .height = h, .format = PAPP_PIXFMT_RGB565,
                                   .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
        s_surf = papp_disp_surface_create(&cfg);
        s_sw = w;
        s_sh = h;
    }
    return s_surf;
}

static void d_write_frame_custom(const uint16_t *buf, uint16_t w, uint16_t h, float scale, bool swap)
{
    papp_surface_t *s = surface_for(w, h);
    if (!s || !buf) return;
    s_draw_thread = papp_thread_self();
    s_drawn = true;
    if (!swap) {                            /* no copy: the PPA reads the game's buffer */
        papp_disp_present_ext(s, buf);
        return;
    }
    int stride;
    uint16_t *dst = papp_disp_lock(s, &stride);
    for (int y = 0; y < h; y++, dst += stride, buf += w) {
        for (int x = 0; x < w; x++) dst[x] = (uint16_t)((buf[x] >> 8) | (buf[x] << 8));
    }
    papp_disp_present(s);
}

static void d_write_frame_rgb565(const uint16_t *buf) { d_write_frame_custom(buf, 320, 240, 0, false); }

static void d_clear(uint16_t color)
{
    if (!s_surf) return;
    int stride;
    uint16_t *dst = papp_disp_lock(s_surf, &stride);
    for (int y = 0; y < s_sh; y++)
        for (int x = 0; x < s_sw; x++) dst[y * stride + x] = color;
    papp_disp_present(s_surf);
}

static uint16_t *d_get_fb(void)               { return NULL; }
static void d_flush(void)                     { }
static void d_set_scale(float sx, float sy)   { }
static void d_write_rect(int x, int y, int w, int h, const uint16_t *data) { }
static void d_lock(void)                      { if (s_disp_mx) papp_mutex_lock(s_disp_mx); }
static void d_unlock(void)                    { if (s_disp_mx) papp_mutex_unlock(s_disp_mx); }

/* ── Audio ─────────────────────────────────────────────────────────────── */

static papp_audio_stream_t *s_stream;
static int s_rate;

static void a_init(int rate)
{
    if (s_stream && rate == s_rate) return;
    if (s_stream) papp_audio_close(s_stream);
    papp_audio_cfg_t cfg = { .rate = rate, .channels = 2, .format = PAPP_AUDIO_S16 };
    s_stream = papp_audio_open(&cfg);
    s_rate = rate;
}

static void a_submit(short *stereo, int frames)
{
    if (s_stream && frames > 0) papp_audio_write(s_stream, stereo, frames, 1000);
}

/* ── Input ─────────────────────────────────────────────────────────────── */

static bool s_touch_was_down;

static int edge_tap(void)                      /* -1 left edge, 1 right edge, 0 none */
{
    papp_touch_point_t p;
    const papp_if_disp_t *d = papp_if_disp();
    bool down = d && d->hdr.version >= 2 && d->touch_read(NULL, &p, 1) > 0;
    int r = 0;
    if (down && !s_touch_was_down) {
        papp_disp_info_t info;
        papp_disp_info(&info);
        if (p.x < EDGE_ZONE_PX) r = -1;
        else if (p.x >= info.width - EDGE_ZONE_PX) r = 1;
    }
    s_touch_was_down = down;
    return r;
}

static void i_read(papp_gamepad_state_t *st)
{
    static bool menu_touch;            /* hold MENU for a few reads after an edge tap */
    static int menu_hold;
    memset(st, 0, sizeof(*st));
    if (papp_exit_requested()) v1_exit_requested = true;

    papp_gamepad_t g;
    uint32_t b = 0;
    if (papp_input_gamepad(0, &g)) {
        b = g.buttons;
        if (g.ly < -16000) b |= PAPP_BTN_UP;
        if (g.ly > 16000)  b |= PAPP_BTN_DOWN;
        if (g.lx < -16000) b |= PAPP_BTN_LEFT;
        if (g.lx > 16000)  b |= PAPP_BTN_RIGHT;
    }
    st->values[PAPP_INPUT_UP]     = !!(b & PAPP_BTN_UP);
    st->values[PAPP_INPUT_DOWN]   = !!(b & PAPP_BTN_DOWN);
    st->values[PAPP_INPUT_LEFT]   = !!(b & PAPP_BTN_LEFT);
    st->values[PAPP_INPUT_RIGHT]  = !!(b & PAPP_BTN_RIGHT);
    st->values[PAPP_INPUT_SELECT] = !!(b & PAPP_BTN_SELECT);
    st->values[PAPP_INPUT_START]  = !!(b & PAPP_BTN_START);
    st->values[PAPP_INPUT_A]      = !!(b & PAPP_BTN_A);
    st->values[PAPP_INPUT_B]      = !!(b & PAPP_BTN_B);
    st->values[PAPP_INPUT_X]      = !!(b & PAPP_BTN_X) && !(V1_MENU_BUTTON & PAPP_BTN_X) && !(V1_VOLUME_BUTTON & PAPP_BTN_X);
    st->values[PAPP_INPUT_Y]      = !!(b & PAPP_BTN_Y) && !(V1_MENU_BUTTON & PAPP_BTN_Y) && !(V1_VOLUME_BUTTON & PAPP_BTN_Y);
    st->values[PAPP_INPUT_L]      = !!(b & (PAPP_BTN_L1 | PAPP_BTN_L2));
    st->values[PAPP_INPUT_R]      = !!(b & (PAPP_BTN_R1 | PAPP_BTN_R2));

    bool draws = !s_drawn || papp_thread_self() == s_draw_thread;
    int edge = draws ? edge_tap() : 0;
    if (edge < 0) { menu_touch = true; menu_hold = 6; }
    if (draws && menu_hold > 0 && --menu_hold == 0) menu_touch = false;
    st->values[PAPP_INPUT_MENU] = (V1_MENU_BUTTON && (b & V1_MENU_BUTTON)) || menu_touch;

    static uint32_t prev;
    if (!draws) return;
    bool vol = edge > 0 || (V1_VOLUME_BUTTON && (b & V1_VOLUME_BUTTON) && !(prev & V1_VOLUME_BUTTON));
    prev = b;
    if (vol) papp_ui_volume();
}

/* ── Files: the SDK FS interface (the game's stdio sits on these) ──────── */

static void *f_open(const char *path, const char *mode)   { return papp_fs() ? papp_fs()->open(path, mode) : NULL; }
static int f_close(void *f)                                { return papp_fs()->close(f); }

static size_t f_read(void *ptr, size_t size, size_t n, void *f)
{
    if (!size) return 0;
    return papp_fs()->read(f, ptr, size * n) / size;
}

static size_t f_write(const void *ptr, size_t size, size_t n, void *f)
{
    if (!size) return 0;
    return papp_fs()->write(f, ptr, size * n) / size;
}

static int  f_seek(void *f, long off, int whence)          { return papp_fs()->seek(f, off, whence); }
static long f_tell(void *f)                                { return papp_fs()->tell(f); }

/* ── Memory ────────────────────────────────────────────────────────────── */

static void *m_alloc(size_t n)                 { return papp_alloc(n, PAPP_MEM_ANY, 0); }
static void *m_realloc(void *p, size_t n)      { return papp_realloc(p, n); }
static void  m_free(void *p)                   { papp_free(p); }

static void *m_calloc(size_t c, size_t n)
{
    size_t total;
    if (__builtin_mul_overflow(c, n, &total)) return NULL;
    void *p = papp_alloc(total, PAPP_MEM_ANY, 0);
    if (p) memset(p, 0, total);
    return p;
}

static void *m_caps_alloc(size_t n, uint32_t caps)
{
    uint32_t c = PAPP_MEM_ANY;
    size_t align = 0;
    if (caps & PAPP_MEM_CAP_SPIRAM)   c |= PAPP_MEM_LARGE;
    if (caps & PAPP_MEM_CAP_INTERNAL) c |= PAPP_MEM_FAST;
    if (caps & PAPP_MEM_CAP_DMA)      { c |= PAPP_MEM_DMA; align = 64; }
    void *p = papp_alloc(n, c, align);
    if (!p && (c & PAPP_MEM_FAST)) p = papp_alloc(n, PAPP_MEM_ANY, align);     /* fall back to PSRAM */
    return p;
}

/* ── System ────────────────────────────────────────────────────────────── */

static int s_vprintf(const char *fmt, va_list ap)
{
    return papp_if_sys()->log_vprintf(fmt, ap);
}

static int s_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = s_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

static void    s_delay(int ms)      { if (papp_exit_requested()) v1_exit_requested = true; papp_delay_ms(ms > 0 ? ms : 0); }
static int64_t s_time(void)         { return papp_time_us(); }

static char s_rom_path[256];
static char   *set_rom_get(void)              { return s_rom_path; }
static void    set_rom_set(const char *p)     { strncpy(s_rom_path, p ? p : "", sizeof(s_rom_path) - 1); }
static int32_t set_vol_get(void)              { return 60; }
static void    set_vol_set(int32_t v)         { }
static int32_t set_bri_get(void)              { return 100; }
static void    set_bri_set(int32_t v)         { }

/* ── Tasks → threads ───────────────────────────────────────────────────── */

typedef struct { void (*fn)(void *); void *arg; } tramp_t;

static int task_tramp(void *p)
{
    tramp_t t = *(tramp_t *)p;
    papp_free(p);
    t.fn(t.arg);                      /* FreeRTOS-style: normally never returns */
    return 0;
}

static int t_create(void (*fn)(void *), const char *name, uint32_t stack, void *arg, int prio,
                    void *out_handle, int core)
{
    tramp_t *t = papp_alloc(sizeof(*t), PAPP_MEM_FAST, 0);
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    papp_thread_cfg_t cfg = {
        .name = name,
        .stack_size = stack,
        .prio = prio >= 7 ? PAPP_PRIO_HIGH : prio <= 2 ? PAPP_PRIO_LOW : PAPP_PRIO_NORMAL,
        .core = (core == 0 || core == 1) ? core : -1,
    };
    papp_thread_t *th = papp_thread_create(task_tramp, t, &cfg);
    if (!th) {
        papp_free(t);
        return -1;
    }
    if (out_handle) *(papp_thread_t **)out_handle = th;
    return 0;                          /* the v1 launcher returned 0 on success (games check != 0) */
}

static void t_delete(void *h)
{
    if (!h || h == papp_thread_self()) papp_thread_exit(0);
    /* Another task: v2 can't kill it; it ends itself or with the app */
}

/* ── Graphics helpers ──────────────────────────────────────────────────── */

static uint16_t *g_png(const char *path, uint16_t *w, uint16_t *h) { return NULL; }

static int g_sprite_blit(uint16_t *fb, uint32_t fb_w, uint32_t fb_h, uint32_t x, uint32_t y,
                         const uint16_t *sp, uint32_t sw, uint32_t sh, uint16_t key)
{
    for (uint32_t j = 0; j < sh && y + j < fb_h; j++)
        for (uint32_t i = 0; i < sw && x + i < fb_w; i++) {
            uint16_t c = sp[j * sw + i];
            if (c != key) fb[(y + j) * fb_w + x + i] = c;
        }
    return 0;
}

static int g_fb_copy(const uint16_t *src, uint16_t *dst, uint32_t w, uint32_t h)
{
    memcpy(dst, src, (size_t)w * h * 2);
    return 0;
}

static int touch(int *x, int *y)
{
    papp_touch_point_t p;
    const papp_if_disp_t *d = papp_if_disp();
    if (!d || d->hdr.version < 2 || d->touch_read(NULL, &p, 1) <= 0) return 0;
    if (x) *x = p.x;
    if (y) *y = p.y;
    return 1;
}

static int paddle(void)
{
    int v = papp_input_analog(PAPP_ANALOG_PADDLE);
    return v < 0 ? -1 : v >> 4;            /* 0..65535 -> 12-bit like v1 */
}

/* ── Entry ─────────────────────────────────────────────────────────────── */

static const app_services_t s_svc = {
    .abi_version = PAPP_ABI_VERSION,
    .display_get_framebuffer = d_get_fb,
    .display_get_emu_buffer = d_get_fb,
    .display_flush = d_flush,
    .display_emu_flush = d_flush,
    .display_clear = d_clear,
    .display_set_scale = d_set_scale,
    .display_write_frame_rgb565 = d_write_frame_rgb565,
    .display_write_frame_custom = d_write_frame_custom,
    .display_write_rect = d_write_rect,
    .display_lock = d_lock,
    .display_unlock = d_unlock,
    .audio_init = a_init,
    .audio_submit = a_submit,
    .input_gamepad_read = i_read,
    .file_open = f_open,
    .file_close = f_close,
    .file_read = f_read,
    .file_write = f_write,
    .file_seek = f_seek,
    .file_tell = f_tell,
    .mem_alloc = m_alloc,
    .mem_calloc = m_calloc,
    .mem_realloc = m_realloc,
    .mem_free = m_free,
    .mem_caps_alloc = m_caps_alloc,
    .log_printf = s_printf,
    .log_vprintf = s_vprintf,
    .delay_ms = s_delay,
    .get_time_us = s_time,
    .settings_rom_path_get = set_rom_get,
    .settings_rom_path_set = set_rom_set,
    .settings_volume_get = set_vol_get,
    .settings_volume_set = set_vol_set,
    .settings_brightness_get = set_bri_get,
    .settings_brightness_set = set_bri_set,
    .task_create = t_create,
    .task_delete = t_delete,
    .png_load_rgb565 = g_png,
    .sprite_blit = g_sprite_blit,
    .fb_copy = g_fb_copy,
    .touch_read = touch,
    .paddle_read = paddle,
};

int main(int argc, char **argv)
{
    s_disp_mx = papp_mutex_create(true);
    int r = app_entry(&s_svc);
    if (s_stream) papp_audio_close(s_stream);
    if (s_surf) papp_disp_surface_destroy(s_surf);
    return r;
}
