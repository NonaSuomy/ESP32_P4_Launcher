/*
 * The RetroESP32 "odroid" platform layer (display, input, audio, settings,
 * SD card, system, app_common) re-implemented on the PAPP v2 SDK, so the
 * emulators' run files and cores build unchanged.
 *
 * Display: every write_frame_* goes to a launcher surface of the frame's own
 * size, scaled to fit by the compositor (no PPA calls here). The last frame
 * presented is kept, so display_get_emu_buffer() can give the emulators'
 * in-game menus a 320x240 copy to draw over, as on the reference board.
 * Input: P1 (board pad + first USB pad). MENU = X and VOLUME = Y unless the
 * emulator sets odroid_input_xy_menu_disable; touching the left / right edge
 * of the screen gives MENU / VOLUME in every emulator (reference zones).
 * Audio: one stereo S16 stream at the emulator's rate; the 5-step volume is
 * the stream's gain and is kept in /sd/odroid/data/volume.
 */
#include "papp/papp.h"
#include "odroid_menu.h"

#include <ctype.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "odroid_audio.h"
#include "odroid_display.h"
#include "odroid_input.h"
#include "odroid_sdcard.h"
#include "odroid_settings.h"
#include "odroid_system.h"
#include "app_common.h"
#include "st7701_lcd.h"

#define EMU_W        320
#define EMU_H        240
#define TOUCH_MENU_X 170      /* landscape x < 170: MENU (reference: portrait y < 170) */
#define TOUCH_VOL_X  630      /* landscape x > 630: VOLUME */
#define VOLUME_FILE  "/sd/odroid/data/volume"

/* ── ESP-IDF helpers ───────────────────────────────────────────────────── */

const char *esp_err_to_name(esp_err_t code) { return code == ESP_OK ? "ESP_OK" : "ESP_ERR"; }
int64_t esp_timer_get_time(void)            { return papp_time_us(); }
uint32_t esp_get_free_heap_size(void)       { return papp_mem_free_size(PAPP_MEM_ANY); }
uint32_t esp_get_minimum_free_heap_size(void) { return papp_mem_free_size(PAPP_MEM_ANY); }
void esp_restart(void)                      { app_return_to_launcher(); }

void odroid_papp_log(char level, const char *tag, const char *fmt, ...)
{
    if (level == 'I') return;                    /* keep the log readable */
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    papp_log("%c %s: %s\n", level, tag, buf);
}

static uint32_t mem_caps(uint32_t caps, size_t *align)
{
    uint32_t c = PAPP_MEM_ANY;
    if (caps & MALLOC_CAP_SPIRAM)   c |= PAPP_MEM_LARGE;
    if (caps & MALLOC_CAP_INTERNAL) c |= PAPP_MEM_FAST;
    if (caps & MALLOC_CAP_DMA) {
        c |= PAPP_MEM_DMA;
        if (*align < 64) *align = 64;
    }
    return c;
}

void *heap_caps_aligned_alloc(size_t align, size_t size, uint32_t caps)
{
    uint32_t c = mem_caps(caps, &align);
    void *p = papp_alloc(size, c, align);
    if (!p && (c & PAPP_MEM_FAST)) p = papp_alloc(size, c & ~PAPP_MEM_FAST, align);   /* internal full: PSRAM */
    return p;
}

void *heap_caps_malloc(size_t size, uint32_t caps)              { return heap_caps_aligned_alloc(0, size, caps); }
void  heap_caps_free(void *p)                                   { papp_free(p); }
void  heap_caps_aligned_free(void *p)                           { papp_free(p); }
void *heap_caps_realloc(void *p, size_t size, uint32_t caps)    { return papp_realloc(p, size); }

void *heap_caps_aligned_calloc(size_t align, size_t n, size_t size, uint32_t caps)
{
    size_t total;
    if (__builtin_mul_overflow(n, size, &total)) return NULL;
    void *p = heap_caps_aligned_alloc(align, total, caps);
    if (p) memset(p, 0, total);
    return p;
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)    { return heap_caps_aligned_calloc(0, n, size, caps); }

size_t heap_caps_get_free_size(uint32_t caps)
{
    size_t a = 0;
    return papp_mem_free_size(mem_caps(caps, &a));
}

size_t heap_caps_get_largest_free_block(uint32_t caps)  { return heap_caps_get_free_size(caps); }
size_t heap_caps_get_minimum_free_size(uint32_t caps)   { return heap_caps_get_free_size(caps); }
void heap_caps_print_heap_info(uint32_t caps)           { }

const char *SD_BASE_PATH = "/sd";      /* RetroESP32 app_common: SD mount point */

/* ── app_common ────────────────────────────────────────────────────────── */

static char s_rom_path[256];
static jmp_buf *s_exit_jb;

void odroid_papp_set_rom(const char *path, jmp_buf *exit_jb)
{
    snprintf(s_rom_path, sizeof(s_rom_path), "%s", path);
    s_exit_jb = exit_jb;
}

void app_init(void)            { }
void app_check_safe_boot(void) { }

int app_get_rom_path(char *buf, int len)
{
    if (!s_rom_path[0]) return -1;
    snprintf(buf, len, "%s", s_rom_path);
    return 0;
}

int odroid_papp_exit_code;          /* emu_main: PAPP_EXIT_RELAUNCH when started from the browser */

void app_return_to_launcher(void)
{
    /* From the main thread, unwind to emu_main (cleanup, back to the browser
     * flow); from an emulator task, end the app (the launcher cleans up). */
    if (s_exit_jb && !papp_thread_self()) longjmp(*s_exit_jb, 1);
    papp_exit(odroid_papp_exit_code);
}

/* ── Display ───────────────────────────────────────────────────────────── */

static papp_surface_t *s_surf;
static int s_sw, s_sh;
static uint16_t *s_last;            /* buffer of the last frame presented (surface memory) */
static int s_last_stride;
static uint16_t *s_emu;             /* 320x240 copy for the in-game menus */
static papp_mutex_t *s_disp_mx;

static void disp_lock(void)
{
    if (!s_disp_mx) s_disp_mx = papp_mutex_create(true);
    papp_mutex_lock(s_disp_mx);
}

static void disp_unlock(void) { papp_mutex_unlock(s_disp_mx); }

/* Locks a surface buffer of w x h (creating / resizing the surface). */
static uint16_t *frame_begin(int w, int h, int *stride)
{
    if (s_surf && (w != s_sw || h != s_sh)) {
        papp_disp_surface_destroy(s_surf);
        s_surf = NULL;
    }
    if (!s_surf) {
        papp_surface_cfg_t cfg = { .width = w, .height = h, .format = PAPP_PIXFMT_RGB565,
                                   .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
        s_surf = papp_disp_surface_create(&cfg);
        s_sw = w;
        s_sh = h;
        papp_log("odroid: %dx%d surface %s\n", w, h, s_surf ? "created" : "FAILED");
        if (!s_surf) return NULL;
    }
    uint16_t *px = papp_disp_lock(s_surf, stride);
    s_last = px;
    s_last_stride = *stride;
    return px;
}

/* No copy: the PPA reads the core's frame directly (stride must equal width). */
static bool present_direct(const uint16_t *src, int w, int h)
{
    int stride;
    if (!frame_begin(w, h, &stride) || papp_disp_present_ext(s_surf, src) != 0) return false;
    s_last = (uint16_t *)src;
    s_last_stride = w;
    return true;
}

static void frame_end(void)
{
    if (s_surf) papp_disp_present(s_surf);
}

static void present_rgb565(const uint16_t *src, int w, int h, int src_stride, bool swap)
{
    if (src && !swap && src_stride == w && present_direct(src, w, h)) return;
    int stride;
    uint16_t *dst = frame_begin(w, h, &stride);
    if (!dst || !src) return;
    for (int y = 0; y < h; y++, dst += stride, src += src_stride) {
        if (!swap) memcpy(dst, src, (size_t)w * 2);
        else for (int x = 0; x < w; x++) dst[x] = (uint16_t)((src[x] >> 8) | (src[x] << 8));
    }
    frame_end();
}

void ili9341_write_frame_rgb565_custom(const uint16_t *buf, uint16_t w, uint16_t h, float scale, bool swap)
{
    disp_lock();
    present_rgb565(buf, w, h, w, swap);
    disp_unlock();
}

void ili9341_write_frame_rgb565_ex(const uint16_t *buf, bool swap) { ili9341_write_frame_rgb565_custom(buf, EMU_W, EMU_H, 2.0f, swap); }
void ili9341_write_frame_rgb565(const uint16_t *buf)               { ili9341_write_frame_rgb565_custom(buf, EMU_W, EMU_H, 2.0f, false); }
void ili9341_write_frame_gb(uint16_t *buf, int scale)              { ili9341_write_frame_rgb565_custom(buf, 160, 144, 2.0f, false); }
void ili9341_write_frame_lynx(const uint16_t *buf)                 { ili9341_write_frame_rgb565_custom(buf, 160, 102, 2.0f, false); }

void ili9341_write_frame_sms(uint8_t *buf, uint16_t color[], uint8_t gg, uint8_t scale)
{
    disp_lock();
    /* Game Gear: the 160x144 LCD is VDP lines 24..167, columns 48..207 */
    int w = gg ? 160 : 256, h = gg ? 144 : 192, stride_in = 256, xoff = gg ? 48 : 0, yoff = gg ? 24 : 0, stride;
    uint16_t *dst = frame_begin(w, h, &stride);
    if (dst && buf) {
        for (int y = 0; y < h; y++, dst += stride) {
            const uint8_t *s = buf + (y + yoff) * stride_in + xoff;
            for (int x = 0; x < w; x++) dst[x] = color[s[x] & 0x1F];
        }
        frame_end();
    }
    disp_unlock();
}

void ili9341_write_frame_prosystem(uint8_t *buf, uint16_t *pal)
{
    disp_lock();
    int stride;
    uint16_t *dst = frame_begin(EMU_W, EMU_H, &stride);
    if (dst && buf) {
        for (int y = 0; y < EMU_H; y++, dst += stride, buf += EMU_W)
            for (int x = 0; x < EMU_W; x++) dst[x] = pal[buf[x]];
        frame_end();
    }
    disp_unlock();
}

void ili9341_write_frame_nes(uint8_t *buf, uint16_t *pal, uint8_t scale)
{
    disp_lock();
    int stride;
    uint16_t *dst = frame_begin(256, 224, &stride);
    if (dst && buf) {
        for (int y = 0; y < 224; y++, dst += stride, buf += 256)
            for (int x = 0; x < 256; x++) {
                uint16_t c = pal[buf[x]];
                dst[x] = (uint16_t)((c >> 8) | (c << 8));        /* reference NES palette is byte-swapped */
            }
        frame_end();
    }
    disp_unlock();
}

void ili9341_write_frame_c64(uint8_t *buf, uint16_t *pal)
{
    disp_lock();
    int stride;
    uint16_t *dst = frame_begin(384, 272, &stride);
    if (dst && buf) {
        for (int y = 0; y < 272; y++, dst += stride, buf += 384)
            for (int x = 0; x < 384; x++) dst[x] = pal[buf[x]];
        frame_end();
    }
    disp_unlock();
}

/* The menus draw over a 320x240 copy of the last frame (nearest-scaled to fit). */
uint16_t *display_get_emu_buffer(void)
{
    disp_lock();
    if (!s_emu) s_emu = papp_alloc(EMU_W * EMU_H * 2, PAPP_MEM_LARGE, 64);
    if (s_emu && s_last != s_emu) {             /* else it already holds the last frame */
        memset(s_emu, 0, EMU_W * EMU_H * 2);
        if (s_last && s_sw && s_sh) {
            int sw = s_sw, sh = s_sh;
            int dw = EMU_W, dh = sh * EMU_W / sw;
            if (dh > EMU_H) { dh = EMU_H; dw = sw * EMU_H / sh; }
            int ox = (EMU_W - dw) / 2, oy = (EMU_H - dh) / 2;
            for (int y = 0; y < dh; y++) {
                const uint16_t *src = s_last + (y * sh / dh) * s_last_stride;
                uint16_t *d = s_emu + (oy + y) * EMU_W + ox;
                for (int x = 0; x < dw; x++) d[x] = src[x * sw / dw];
            }
        }
    }
    disp_unlock();
    return s_emu;
}

void display_emu_flush(void)
{
    disp_lock();
    if (s_emu) present_rgb565(s_emu, EMU_W, EMU_H, EMU_W, false);
    disp_unlock();
}

void ili9341_clear(uint16_t color)
{
    disp_lock();
    if (s_surf) {
        int stride;
        uint16_t *dst = frame_begin(s_sw, s_sh, &stride);
        if (dst) {
            for (int y = 0; y < s_sh; y++)
                for (int x = 0; x < s_sw; x++) dst[y * stride + x] = color;
            frame_end();
        }
    }
    disp_unlock();
}

void ili9341_init(void)                                  { }
void ili9341_write_frame_rectangleLE(int x, int y, int w, int h, const uint16_t *d) { }
void display_flush(void)                                 { }
void display_flush_force(void)                           { }
void display_set_scale(float sx, float sy)               { }
bool is_backlight_initialized(void)                      { return true; }
uint16_t *display_get_framebuffer(void)                  { return NULL; }
void display_lcd_draw_raw(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t *d) { }
void ili9341_poweroff(void)                              { }
void ili9341_prepare(void)                               { }
void odroid_display_lock(void)                           { disp_lock(); }
void odroid_display_unlock(void)                         { disp_unlock(); }
void odroid_display_lock_gb_display(void)                { disp_lock(); }
void odroid_display_unlock_gb_display(void)              { disp_unlock(); }
void odroid_display_lock_nes_display(void)               { disp_lock(); }
void odroid_display_unlock_nes_display(void)             { disp_unlock(); }
void odroid_display_lock_sms_display(void)               { disp_lock(); }
void odroid_display_unlock_sms_display(void)             { disp_unlock(); }
void odroid_display_show_sderr(int e)                    { papp_log("odroid: SD card error %d\n", e); }
void odroid_display_show_hourglass(void)                 { }
void odroid_display_show_splash(void)                    { }
void odroid_display_drain_spi(void)                      { }
esp_err_t st7701_lcd_draw_to_fb(int x, int y, int w, int h, const uint16_t *d) { return ESP_OK; }

void odroid_papp_display_close(void)
{
    if (s_surf) papp_disp_surface_destroy(s_surf);
    s_surf = NULL;
    s_last = NULL;
}

/* ── Input ─────────────────────────────────────────────────────────────── */

volatile int odroid_paddle_adc_raw = -1;
bool odroid_input_xy_menu_disable;
bool odroid_input_touch_buttons_disable;

static int s_touch_zone;              /* -1 MENU, 1 VOLUME, 0 none (held while touched) */
static int64_t s_touch_polled;

static void poll_touch(void)
{
    int64_t now = papp_time_us();
    if (now - s_touch_polled < 30000) return;            /* the I2C read is not free */
    s_touch_polled = now;
    papp_touch_point_t p;
    const papp_if_disp_t *d = papp_if_disp();
    s_touch_zone = 0;
    if (d && d->hdr.version >= 2 && d->touch_read(NULL, &p, 1) > 0) {
        papp_disp_info_t info = { .width = 800 };
        papp_disp_info(&info);
        int w = info.width;                             /* the zones were drawn for 800 wide */
        if (p.x < TOUCH_MENU_X * w / 800) s_touch_zone = -1;
        else if (p.x > TOUCH_VOL_X * w / 800) s_touch_zone = 1;
    }
}

void odroid_input_gamepad_read(odroid_gamepad_state *st)
{
    memset(st, 0, sizeof(*st));
    papp_gamepad_t g;
    uint32_t b = 0;
    if (papp_input_gamepad(0, &g)) {
        b = g.buttons;
        if (g.ly < -16000) b |= PAPP_BTN_UP;
        if (g.ly > 16000)  b |= PAPP_BTN_DOWN;
        if (g.lx < -16000) b |= PAPP_BTN_LEFT;
        if (g.lx > 16000)  b |= PAPP_BTN_RIGHT;
    }
    st->values[ODROID_INPUT_UP]     = !!(b & PAPP_BTN_UP);
    st->values[ODROID_INPUT_DOWN]   = !!(b & PAPP_BTN_DOWN);
    st->values[ODROID_INPUT_LEFT]   = !!(b & PAPP_BTN_LEFT);
    st->values[ODROID_INPUT_RIGHT]  = !!(b & PAPP_BTN_RIGHT);
    st->values[ODROID_INPUT_SELECT] = !!(b & PAPP_BTN_SELECT);
    st->values[ODROID_INPUT_START]  = !!(b & PAPP_BTN_START);
    st->values[ODROID_INPUT_A]      = !!(b & PAPP_BTN_A);
    st->values[ODROID_INPUT_B]      = !!(b & PAPP_BTN_B);
    st->values[ODROID_INPUT_X]      = !!(b & PAPP_BTN_X);
    st->values[ODROID_INPUT_Y]      = !!(b & PAPP_BTN_Y);
    st->values[ODROID_INPUT_L]      = !!(b & (PAPP_BTN_L1 | PAPP_BTN_L2));
    st->values[ODROID_INPUT_R]      = !!(b & (PAPP_BTN_R1 | PAPP_BTN_R2));
    if (!odroid_input_touch_buttons_disable) {
        poll_touch();
        st->values[ODROID_INPUT_MENU]   = s_touch_zone < 0;
        st->values[ODROID_INPUT_VOLUME] = s_touch_zone > 0;
    }
    if (!odroid_input_xy_menu_disable) {
        st->values[ODROID_INPUT_MENU]   |= st->values[ODROID_INPUT_X];
        st->values[ODROID_INPUT_VOLUME] |= st->values[ODROID_INPUT_Y];
    }
    int pv = papp_input_analog(PAPP_ANALOG_PADDLE);
    odroid_paddle_adc_raw = pv < 0 ? -1 : pv >> 4;
}

odroid_gamepad_state odroid_input_read_raw(void)
{
    odroid_gamepad_state s;
    odroid_input_gamepad_read(&s);
    return s;
}

void odroid_input_gamepad_init(void)                       { }
void odroid_input_battery_level_init(void)                 { }
void odroid_input_battery_monitor_enabled_set(bool e)      { }
void odroid_paddle_adc_init(void)                          { }
bool odroid_input_gpio_pad_detected(void)                  { return false; }
bool odroid_input_usb_gamepad_connected(void)              { papp_gamepad_t g; return papp_input_gamepad(0, &g); }
bool odroid_input_usb_map_exists(void)                     { return false; }
void odroid_input_usb_map_load(void)                       { }
bool odroid_input_usb_map_save(const odroid_usb_map_t *m)  { return false; }
void odroid_input_usb_map_get(odroid_usb_map_t *m)         { memset(m, 0, sizeof(*m)); }
void odroid_input_usb_map_set(const odroid_usb_map_t *m)   { }

void odroid_input_battery_level_read(odroid_battery_state *s)
{
    s->millivolts = 4000;
    s->percentage = 100;
    s->charging = false;
}

/* ── Audio ─────────────────────────────────────────────────────────────── */

static papp_audio_stream_t *s_stream;
static int s_rate;
static int s_volume = -1;

static int volume_load(void)
{
    if (s_volume < 0) {
        s_volume = ODROID_VOLUME_LEVEL4;
        FILE *f = fopen(VOLUME_FILE, "r");
        if (f) {
            int v;
            if (fscanf(f, "%d", &v) == 1 && v >= 0 && v < ODROID_VOLUME_LEVEL_COUNT) s_volume = v;
            fclose(f);
        }
    }
    return s_volume;
}

/* The launcher's master volume sets the level (odroid_emu_volume); the odroid
 * levels the cores used to step through are no longer applied */
static void volume_apply(void)
{
    if (s_stream) papp_audio_set_gain(s_stream, 100);
}

void odroid_audio_init(int rate)
{
    if (s_stream && rate == s_rate) return;
    if (s_stream) papp_audio_close(s_stream);
    papp_audio_cfg_t cfg = { .rate = rate, .channels = 2, .format = PAPP_AUDIO_S16 };
    s_stream = papp_audio_open(&cfg);
    s_rate = rate;
    volume_apply();
}

void odroid_audio_terminate(void)
{
    if (s_stream) {
        /* Discard any queued tail so audio from the exiting emulator does
         * not leak into the launcher or the next app. */
        papp_audio_clear(s_stream);
        papp_audio_close(s_stream);
    }
    s_stream = NULL;
    s_rate = 0;
}

void odroid_audio_submit(short *stereo, int frames)
{
    if (s_stream && frames > 0) papp_audio_write(s_stream, stereo, frames, 1000);
}

/* Sound waiting to be played, in ms (-1: no stream). Low = the emulation is falling behind. */
int odroid_audio_queued_ms(void)
{
    if (!s_stream || !s_rate) return -1;
    papp_audio_stats_t st;
    papp_audio_stats(s_stream, &st);
    return (int)((uint64_t)st.queued_frames * 1000 / (uint32_t)s_rate);
}

void odroid_audio_submit_zero(void)           { }
int  odroid_audio_sample_rate_get(void)       { return s_rate; }
odroid_volume_level odroid_audio_volume_get(void) { return (odroid_volume_level)volume_load(); }

void odroid_audio_volume_set(int level)
{
    if (level < 0) level = 0;
    if (level >= ODROID_VOLUME_LEVEL_COUNT) level = ODROID_VOLUME_LEVEL_COUNT - 1;
    s_volume = level;
    volume_apply();
}

void odroid_audio_volume_change(void)
{
    odroid_audio_volume_set((volume_load() + 1) % ODROID_VOLUME_LEVEL_COUNT);
    odroid_settings_Volume_set(s_volume);
}

/* ── In-game menu (odroid_menu.h) ──────────────────────────────────────── */

/* Waits (up to 1 s) until no button is held and the screen isn't touched, so the
 * press that opened or closed a dialog reaches neither the dialog nor the game */
static void wait_release(void)
{
    const papp_if_disp_t *d = papp_if_disp();
    for (int t = 0; t < 100; t++) {
        papp_gamepad_t g;
        papp_touch_point_t p;
        bool held = papp_input_gamepad(0, &g) && g.buttons;
        if (!held && d && d->hdr.version >= 2 && d->touch_read(NULL, &p, 1) > 0) held = true;
        if (!held) break;
        papp_delay_ms(10);
    }
    s_touch_zone = 0;
    s_touch_polled = 0;
}

static void dialog_begin(void)
{
    wait_release();
    if (s_stream) papp_audio_pause(s_stream, true);
}

static void dialog_end(void)
{
    if (s_stream) papp_audio_pause(s_stream, false);
    wait_release();
}

emu_menu_action_t odroid_emu_menu(const char *note, unsigned flags)
{
    const char *items[7];
    emu_menu_action_t ids[7];
    int n = 0;
    bool can_save = !(flags & EMU_MENU_NO_SAVE), saved = can_save && (flags & EMU_MENU_HAS_SAVE);
    items[n] = "Resume game";       ids[n++] = EMU_MENU_RESUME;
    if (can_save) { items[n] = "Save game";       ids[n++] = EMU_MENU_SAVE; }
    if (saved)    { items[n] = "Load saved game"; ids[n++] = EMU_MENU_LOAD; }
    if (!(flags & EMU_MENU_NO_RESTART)) { items[n] = "Restart game"; ids[n++] = EMU_MENU_RESTART; }
    if (saved)    { items[n] = "Delete save";     ids[n++] = EMU_MENU_DELETE; }
    int volume = n;
    items[n] = "Volume";            ids[n++] = EMU_MENU_RESUME;
    items[n] = "Exit game";         ids[n++] = EMU_MENU_EXIT;

    char title[48];
    if (note) snprintf(title, sizeof(title), "Game menu - %s", note);
    else snprintf(title, sizeof(title), "Game menu");
    dialog_begin();
    int k, at = 0;
    while ((k = papp_ui_menu(title, items, n, at)) == volume) {
        papp_ui_volume();
        at = volume;                                    /* back on Volume */
    }
    dialog_end();
    return k < 0 ? EMU_MENU_RESUME : ids[k];
}

void odroid_emu_volume(void)
{
    dialog_begin();
    papp_ui_volume();
    dialog_end();
}

/* ── Settings ──────────────────────────────────────────────────────────── */

static int s_start_action;
static uint8_t s_scale_off;

static char *dup_part(const char *path, bool keep_dir, bool keep_ext)
{
    if (!path) return NULL;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char *s = strdup(keep_dir ? path : base);
    if (s && !keep_ext) {
        char *dot = strrchr(s, '.');
        if (dot && dot != s) *dot = 0;
    }
    return s;
}

char *odroid_util_GetFileName(const char *path)                    { return dup_part(path, false, true); }
char *odroid_util_GetFileNameWithoutExtension(const char *path)    { return dup_part(path, false, false); }

char *odroid_util_GetFileExtenstion(const char *path)
{
    const char *dot = path ? strrchr(path, '.') : NULL;
    return dot ? strdup(dot + 1) : NULL;
}

int32_t odroid_settings_Volume_get(void) { return volume_load(); }

void odroid_settings_Volume_set(int32_t v)
{
    odroid_audio_volume_set(v);
    mkdir("/sd/odroid", 0775);
    mkdir("/sd/odroid/data", 0775);
    FILE *f = fopen(VOLUME_FILE, "w");
    if (f) {
        fprintf(f, "%d\n", s_volume);
        fclose(f);
    }
}

int32_t odroid_settings_Backlight_get(void)                  { return 4; }
void    odroid_settings_Backlight_set(int32_t v)             { }
char   *odroid_settings_RomFilePath_get(void)                { return s_rom_path[0] ? strdup(s_rom_path) : NULL; }
void    odroid_settings_RomFilePath_set(const char *p)       { snprintf(s_rom_path, sizeof(s_rom_path), "%s", p ? p : ""); }
int32_t odroid_settings_DataSlot_get(void)                   { return 0; }
void    odroid_settings_DataSlot_set(int32_t v)              { }
int32_t odroid_settings_AppSlot_get(void)                    { return 0; }
void    odroid_settings_AppSlot_set(int32_t v)               { }
int32_t odroid_settings_VRef_get(void)                       { return 1100; }
void    odroid_settings_VRef_set(int32_t v)                  { }
ODROID_START_ACTION odroid_settings_StartAction_get(void)    { return (ODROID_START_ACTION)s_start_action; }
void    odroid_settings_StartAction_set(ODROID_START_ACTION v) { s_start_action = v; }
ODROID_AUDIO_SINK odroid_settings_AudioSink_get(void)        { return ODROID_AUDIO_SINK_SPEAKER; }
void    odroid_settings_AudioSink_set(ODROID_AUDIO_SINK v)   { }
uint8_t odroid_settings_ScaleDisabled_get(ODROID_SCALE_DISABLE s)         { return (s_scale_off & s) ? 1 : 0; }
void    odroid_settings_ScaleDisabled_set(ODROID_SCALE_DISABLE s, uint8_t v) { s_scale_off = v ? s_scale_off | s : s_scale_off & ~s; }

/* ── SD card (always mounted by the launcher) ──────────────────────────── */

esp_err_t odroid_sdcard_open(const char *base)   { return ESP_OK; }
esp_err_t odroid_sdcard_close(void)              { return ESP_OK; }
bool odroid_sdcard_is_mounted(void)              { return true; }

size_t odroid_sdcard_get_filesize(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (size_t)st.st_size : 0;
}

size_t odroid_sdcard_copy_file_to_memory(const char *path, void *ptr)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = 0, r;
    while ((r = fread((char *)ptr + n, 1, 64 * 1024, f)) > 0) n += r;
    fclose(f);
    return n;
}

char *odroid_sdcard_create_savefile_path(const char *base, const char *name)
{
    size_t len = strlen(base) + strlen(name) + 16;
    char *s = malloc(len);
    if (s) snprintf(s, len, "%s/odroid/data/%s", base, name);
    return s;
}

/* select() as a sleep (spectemu's frame timing); no sockets in apps */
int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv)
{
    if (tv) papp_delay_ms((uint32_t)(tv->tv_sec * 1000 + tv->tv_usec / 1000));
    return 0;
}

/* Directory access: the SDK has no directory listing yet (Atari 800's H:
 * device is the only user); report "not found". */
DIR *opendir(const char *path)       { errno = ENOENT; return NULL; }
struct dirent *readdir(DIR *d)       { return NULL; }
int closedir(DIR *d)                 { return 0; }
int rmdir(const char *path)          { errno = ENOSYS; return -1; }

/* ── System ────────────────────────────────────────────────────────────── */

void  odroid_system_init(void)              { }
void  odroid_system_application_set(int s)  { }
void *odroid_system_get_i2c_bus(void)       { return NULL; }
void  odroid_system_sleep(void)             { }
void  odroid_system_led_set(int v)          { }
