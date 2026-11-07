/*
 * Clock — big clock, stopwatch and countdown timer. Time and time zone come from
 * the launcher (NTP or DevLink).
 *   X / START  mode menu
 *   Stopwatch  A start/stop, B reset
 *   Timer      UP/DOWN minutes, A start/stop, B reset
 * Touch (works without a pad): hold anywhere for the menu; in the clock a tap opens it too.
 * Stopwatch and timer: tap the left half to start/stop, the right half to reset; a stopped
 * timer's top band sets the minutes (left -1, right +1). Hints are drawn in those places.
 */
#include <math.h>
#include <string.h>
#include "papp/papp.h"

PAPP_MANIFEST(.id = "tool.clock", .name = "Clock", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define W 400
#define H 240

enum { MODE_CLOCK, MODE_STOPWATCH, MODE_TIMER };

#define HOLD_US   600000          /* touch held this long = menu */
#define BAND_H    56              /* timer: minutes band at the top */

static uint16_t *s_px;
static int s_stride;

static void rect(int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++) s_px[j * s_stride + i] = c;
}

/* Seven-segment digit in a box of w x h */
static void digit(int x, int y, int w, int h, int t, int d, uint16_t c)
{
    static const uint8_t seg[10] = { 0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F };
    uint8_t s = d >= 0 && d <= 9 ? seg[d] : 0x40;
    int mid = y + (h - t) / 2;
    if (s & 0x01) rect(x + t, y, w - 2 * t, t, c);                     /* top */
    if (s & 0x02) rect(x + w - t, y + t, t, (h - 3 * t) / 2, c);       /* upper right */
    if (s & 0x04) rect(x + w - t, mid + t, t, (h - 3 * t) / 2, c);     /* lower right */
    if (s & 0x08) rect(x + t, y + h - t, w - 2 * t, t, c);             /* bottom */
    if (s & 0x10) rect(x, mid + t, t, (h - 3 * t) / 2, c);             /* lower left */
    if (s & 0x20) rect(x, y + t, t, (h - 3 * t) / 2, c);               /* upper left */
    if (s & 0x40) rect(x + t, mid, w - 2 * t, t, c);                   /* middle */
}

/* "12:34" style: pairs of digits separated by colons, centred at y */
static void digits(const int *v, int pairs, int y, int w, int h, int t, uint16_t c)
{
    int gap = t * 2, colon = t * 3;
    int total = pairs * (2 * w + t) + (pairs - 1) * colon;
    int x = (W - total) / 2;
    for (int p = 0; p < pairs; p++) {
        digit(x, y, w, h, t, v[p] / 10, c);
        digit(x + w + t, y, w, h, t, v[p] % 10, c);
        x += 2 * w + t;
        if (p + 1 < pairs) {
            rect(x + t, y + h / 3, t, t, c);
            rect(x + t, y + 2 * h / 3 - t, t, t, c);
            x += colon;
        }
    }
    (void)gap;
}

/* Touch hints: play / pause, reset, minus / plus, centred at (cx, cy) */
static void hint_play(int cx, int cy, bool running, uint16_t c)
{
    if (running) {
        rect(cx - 9, cy - 10, 6, 20, c);
        rect(cx + 3, cy - 10, 6, 20, c);
    } else {
        for (int i = 0; i < 20; i++) rect(cx - 8, cy - 10 + i, 1 + (i < 10 ? i : 19 - i) * 18 / 10, 1, c);
    }
}

static void hint_reset(int cx, int cy, uint16_t c)
{
    rect(cx - 9, cy - 9, 18, 18, c);
}

static void hint_sign(int cx, int cy, bool plus, uint16_t c)
{
    rect(cx - 10, cy - 2, 20, 5, c);
    if (plus) rect(cx - 2, cy - 10, 5, 20, c);
}

static void beep(papp_audio_stream_t *a)
{
    if (!a) return;
    static int16_t pcm[1600];
    for (int i = 0; i < 1600; i++)
        pcm[i] = (int16_t)(9000.0f * sinf((float)i * 2.0f * 3.14159f * 880.0f / 16000.0f));
    papp_audio_write(a, pcm, 1600, 0);
}

int main(int argc, char **argv)
{
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) return 1;
    papp_audio_cfg_t ac = { .rate = 16000, .channels = 1, .format = PAPP_AUDIO_S16 };
    papp_audio_stream_t *audio = papp_audio_open(&ac);

    const uint16_t bg = PAPP_RGB565(6, 12, 28), fg = PAPP_RGB565(120, 200, 255);
    const uint16_t dim = PAPP_RGB565(70, 90, 130), on = PAPP_RGB565(120, 255, 170);
    int mode = MODE_CLOCK, timer_min = 5;
    int64_t sw_base = 0, sw_start = 0, tm_left = 5 * 60 * 1000000LL, tm_last = 0;
    bool sw_run = false, tm_run = false;
    uint32_t prev = 0;
    bool down = false, held = false, wait_release = false;
    int64_t down_at = 0;
    int tx = 0, ty = 0;

    while (!papp_exit_requested()) {
        papp_gamepad_t pad = { 0 };
        papp_input_gamepad(0, &pad);
        uint32_t press = pad.buttons & ~prev;
        prev = pad.buttons;
        int64_t now = papp_time_us();

        /* Touch: a short tap acts where it lands; holding opens the menu */
        papp_touch_point_t tp;
        bool touch = papp_disp_touch(s, &tp, 1) > 0;
        bool tap = false, menu = false;
        if (wait_release) {
            wait_release = touch;               /* the touch that closed the menu */
        } else if (touch) {
            if (!down) { down = true; held = false; down_at = now; }
            tx = tp.x;
            ty = tp.y;
            if (!held && now - down_at >= HOLD_US) { held = true; menu = true; }
        } else if (down) {
            down = false;
            tap = !held;
        }
        if (tap && mode == MODE_CLOCK) menu = true;

        if (menu || (press & (PAPP_BTN_X | PAPP_BTN_START))) {
            static const char *const items[] = { "Clock", "Stopwatch", "Timer", "Exit" };
            int r = papp_ui_menu("Clock", items, 4, mode);
            if (r == 3) break;
            if (r >= 0) mode = r;
            prev = 0xFFFFFFFFu;             /* ignore what closed the menu until released */
            down = false;
            wait_release = true;
            continue;
        }
        if (tap) {                              /* the same as a button, by where it landed */
            bool left = tx < W / 2;
            if (mode == MODE_TIMER && !tm_run && ty < BAND_H) press |= left ? PAPP_BTN_DOWN : PAPP_BTN_UP;
            else press |= left ? PAPP_BTN_A : PAPP_BTN_B;
        }
        if (mode == MODE_STOPWATCH) {
            if (press & PAPP_BTN_A) {
                if (sw_run) sw_base += now - sw_start;
                else sw_start = now;
                sw_run = !sw_run;
            }
            if (press & PAPP_BTN_B) { sw_run = false; sw_base = 0; }
        } else if (mode == MODE_TIMER) {
            if (!tm_run) {
                if (press & PAPP_BTN_UP) timer_min = timer_min >= 99 ? 99 : timer_min + 1;
                if (press & PAPP_BTN_DOWN) timer_min = timer_min <= 1 ? 1 : timer_min - 1;
                if (press & (PAPP_BTN_UP | PAPP_BTN_DOWN)) tm_left = timer_min * 60 * 1000000LL;
            }
            if (press & PAPP_BTN_A) {
                if (tm_left <= 0) tm_left = timer_min * 60 * 1000000LL;
                tm_run = !tm_run;
                tm_last = now;
            }
            if (press & PAPP_BTN_B) { tm_run = false; tm_left = timer_min * 60 * 1000000LL; }
        }
        if (tm_run) {
            tm_left -= now - tm_last;
            tm_last = now;
            if (tm_left <= 0) {
                tm_left = 0;
                tm_run = false;
                for (int i = 0; i < 3; i++) beep(audio);
            }
        }

        s_px = papp_disp_lock(s, &s_stride);
        rect(0, 0, W, H, bg);
        if (mode == MODE_CLOCK) {
            papp_datetime_t t;
            if (papp_local_time(&t)) {
                int hm[3] = { t.hour, t.minute, t.second };
                digits(hm, 3, 60, 44, 90, 7, fg);
                int date[3] = { t.day, t.month, t.year % 100 };
                digits(date, 3, 176, 18, 36, 3, dim);
            } else {
                int none[1] = { -1 };
                (void)none;
                for (int i = 0; i < 4; i++) rect(100 + i * 52, 110, 40, 8, dim);
            }
        } else if (mode == MODE_STOPWATCH) {
            int64_t us = sw_base + (sw_run ? now - sw_start : 0);
            int cs = (int)(us / 10000 % 100), sec = (int)(us / 1000000);
            int v[3] = { sec / 60 % 100, sec % 60, cs };
            digits(v, 3, 75, 44, 90, 7, sw_run ? on : fg);
            hint_play(W / 4, H - 30, sw_run, dim);
            hint_reset(3 * W / 4, H - 30, dim);
        } else {
            int sec = (int)((tm_left + 999999) / 1000000);
            int v[2] = { sec / 60, sec % 60 };
            digits(v, 2, 70, 60, 100, 9, tm_run ? on : tm_left == 0 ? PAPP_RGB565(255, 90, 90) : fg);
            hint_play(W / 4, H - 30, tm_run, dim);
            hint_reset(3 * W / 4, H - 30, dim);
            if (!tm_run) {
                hint_sign(W / 4, BAND_H / 2, false, dim);
                hint_sign(3 * W / 4, BAND_H / 2, true, dim);
            }
        }
        papp_disp_present(s);
    }
    if (audio) papp_audio_close(audio);
    papp_disp_surface_destroy(s);
    return 0;
}
