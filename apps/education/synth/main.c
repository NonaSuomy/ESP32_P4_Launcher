/*
 * Synth and oscilloscope (education): see what a sound looks like.
 *
 * A sound is the air pushed back and forth. The speaker gets a list of numbers, 32000 every
 * second, each one how far to push it. The synth makes those numbers:
 *   - pitch = how many times a second the shape repeats (A4 = 440 Hz; one octave up = 2x),
 *   - timbre = the shape: a sine is pure, square and saw sound buzzy (they hold many harmonics),
 *     noise has no repeating shape at all.
 * The scope (left) draws the numbers as they go to the speaker; the spectrum (right) shows
 * which frequencies are in them (a Fourier transform): a sine is one bar, a saw a whole row.
 * Echo adds what was played 300 ms ago, a little quieter.
 *
 * Touch the keys (several at once make a chord). Pad: A / B / Y play C / E / G, L1 / R1
 * change the shape, UP / DOWN the octave, X exits. USB keyboard: A S D F G H J K = white keys,
 * W E T Y U = black keys.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "edu_ui.h"

PAPP_MANIFEST(.id = "edu.synth", .name = "Synth", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define RATE 32000
#define BLOCK 256                       /* frames per write: 8 ms */
#define VOICES 8
#define SCOPE_N 2048                    /* recent output, for the scope and the spectrum */
#define FFT_N 512
#define ECHO_N (RATE * 3 / 10)

enum { W_SINE, W_SQUARE, W_SAW, W_TRIANGLE, W_NOISE, WAVES };
static const char *const wave_name[WAVES] = { "Sine", "Square", "Saw", "Triangle", "Noise" };
enum { ID_WAVE = 1, ID_OCT_DOWN = 10, ID_OCT_UP, ID_ECHO, ID_KEY = 20 };

/* Screen layout */
#define SC_X 20                         /* scope */
#define SC_Y 112
#define SC_W 448
#define SC_H 150
#define SP_X 480                        /* spectrum */
#define SP_W 300
#define KB_X 22                         /* keyboard: 2 octaves, 14 white keys */
#define KB_Y 330
#define KB_H 134
#define WK_W 54
#define BK_W 34
#define BK_H 82

typedef struct {
    volatile bool gate;                 /* key held */
    volatile uint32_t step;             /* phase step per sample (2^32 = one cycle) */
    volatile int key;                   /* which key holds it, -1 free */
    uint32_t phase;
    float env;                          /* 0..1: rises fast when held, falls when let go */
} voice_t;

static voice_t s_voice[VOICES];
static volatile int s_wave = W_SAW;
static volatile bool s_echo, s_stop;
static int s_octave = 4;                /* the left C is C4 (middle C) */
static int16_t s_scope[SCOPE_N];        /* written by the audio thread, read by the main one */
static volatile uint32_t s_scope_pos;
static int16_t *s_echo_buf;
static uint32_t s_echo_pos, s_noise = 0x12345u;
static float s_sine[1024];
static bool s_quit;

/* ── Sound ─────────────────────────────────────────────────────────────── */

static float key_freq(int key)          /* key 0 = C of s_octave; 12 keys an octave */
{
    int midi = 12 * (s_octave + 1) + key;           /* C4 = 60 */
    return 440.0f * powf(2.0f, (midi - 69) / 12.0f);
}

static float wave(int w, uint32_t phase)
{
    float t = phase * (1.0f / 4294967296.0f);       /* 0..1 along the cycle */
    switch (w) {
    case W_SINE: return s_sine[phase >> 22];
    case W_SQUARE: return t < 0.5f ? 0.6f : -0.6f;
    case W_SAW: return (2 * t - 1) * 0.7f;
    case W_TRIANGLE: return (t < 0.5f ? 4 * t - 1 : 3 - 4 * t);
    default:
        s_noise ^= s_noise << 13;
        s_noise ^= s_noise >> 17;
        s_noise ^= s_noise << 5;
        return ((int32_t)s_noise >> 16) * (0.6f / 32768.0f);
    }
}

static int audio_thread(void *arg)
{
    papp_audio_stream_t *a = arg;
    static int16_t out[BLOCK];
    while (!s_stop && !papp_exit_requested()) {
        int w = s_wave;
        for (int i = 0; i < BLOCK; i++) {
            float mix = 0;
            for (int v = 0; v < VOICES; v++) {
                voice_t *p = &s_voice[v];
                if (p->gate) p->env += (1.0f - p->env) * 0.004f;    /* attack about 8 ms */
                else if (p->env > 0) p->env = p->env < 0.0005f ? 0 : p->env * 0.9993f;  /* release */
                if (p->env <= 0) continue;
                mix += wave(w, p->phase) * p->env;
                p->phase += p->step;
            }
            mix *= 0.3f;
            if (s_echo) {                                   /* the sound of 300 ms ago, at 45% */
                mix += s_echo_buf[s_echo_pos] * (0.45f / 32767.0f);
            }
            if (mix > 1) mix = 1;
            if (mix < -1) mix = -1;
            int16_t v16 = (int16_t)(mix * 32000);
            s_echo_buf[s_echo_pos] = s_echo ? v16 : 0;
            s_echo_pos = (s_echo_pos + 1) % ECHO_N;
            out[i] = v16;
            s_scope[(s_scope_pos + i) % SCOPE_N] = v16;
        }
        s_scope_pos = (s_scope_pos + BLOCK) % SCOPE_N;
        papp_audio_write(a, out, BLOCK, 200);
    }
    return 0;
}

/* A key goes down / up: find it a voice, or let its voice go */
static void key_down(int key)
{
    for (int v = 0; v < VOICES; v++)
        if (s_voice[v].gate && s_voice[v].key == key) return;
    int best = 0;                                   /* a free voice (the quietest), else voice 0 */
    float quiet = 2;
    for (int v = 0; v < VOICES; v++)
        if (!s_voice[v].gate && s_voice[v].env < quiet) { quiet = s_voice[v].env; best = v; }
    voice_t *p = &s_voice[best];
    p->key = key;
    p->step = (uint32_t)(key_freq(key) / RATE * 4294967296.0f);
    p->gate = true;
}

static void key_up(int key)
{
    for (int v = 0; v < VOICES; v++)
        if (s_voice[v].key == key) s_voice[v].gate = false;
}

/* ── Spectrum: a 512-point FFT of the latest output ────────────────────── */

static float s_re[FFT_N], s_im[FFT_N], s_mag[FFT_N / 2], s_hann[FFT_N];

static void fft(float *re, float *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {            /* bit-reversed order */
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {        /* butterflies */
        float a = -2 * (float)M_PI / len;
        float wr = cosf(a), wi = sinf(a);
        for (int i = 0; i < n; i += len) {
            float cr = 1, ci = 0;
            for (int k = 0; k < len / 2; k++) {
                int p = i + k, q = p + len / 2;
                float tr = re[q] * cr - im[q] * ci, ti = re[q] * ci + im[q] * cr;
                re[q] = re[p] - tr; im[q] = im[p] - ti;
                re[p] += tr; im[p] += ti;
                float nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

static void spectrum(void)
{
    uint32_t end = s_scope_pos;
    for (int i = 0; i < FFT_N; i++) {
        s_re[i] = s_scope[(end + SCOPE_N - FFT_N + i) % SCOPE_N] * s_hann[i] * (1.0f / 32768.0f);
        s_im[i] = 0;
    }
    fft(s_re, s_im, FFT_N);
    for (int i = 0; i < FFT_N / 2; i++) {
        float m = sqrtf(s_re[i] * s_re[i] + s_im[i] * s_im[i]);
        float db = 20 * log10f(m + 1e-6f);          /* decibels: each 20 = 10x louder */
        float v = (db + 48) / 54;                   /* -48 dB .. +6 dB -> 0..1 */
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        s_mag[i] = s_mag[i] * 0.6f + v * 0.4f;      /* a little smoothing */
    }
}

/* ── Screen ────────────────────────────────────────────────────────────── */

static const int black_after[5] = { 0, 1, 3, 4, 5 };    /* black keys follow white keys C D F G A */
static const int white_semi[7] = { 0, 2, 4, 5, 7, 9, 11 };
static const int black_semi[5] = { 1, 3, 6, 8, 10 };

/* Which key (0..23) is at a point of the keyboard, -1 none */
static int key_at(int x, int y)
{
    if (x < KB_X || x >= KB_X + 14 * WK_W || y < KB_Y || y >= KB_Y + KB_H) return -1;
    if (y < KB_Y + BK_H)
        for (int o = 0; o < 2; o++)
            for (int b = 0; b < 5; b++) {
                int bx = KB_X + (o * 7 + black_after[b] + 1) * WK_W - BK_W / 2;
                if (x >= bx && x < bx + BK_W) return o * 12 + black_semi[b];
            }
    int w = (x - KB_X) / WK_W;
    return (w / 7) * 12 + white_semi[w % 7];
}

static bool s_down[24];                 /* keys held, from touch, pad and keyboard */

static void draw_keyboard(void)
{
    for (int w = 0; w < 14; w++) {
        int key = (w / 7) * 12 + white_semi[w % 7];
        ui_rrect(KB_X + w * WK_W + 1, KB_Y, WK_W - 2, KB_H, 6, s_down[key] ? UI_RGB(0x7FC8FF) : UI_RGB(0xE8EDF5));
        if (w % 7 == 0) {
            char n[8];
            snprintf(n, sizeof(n), "C%d", s_octave + w / 7);
            ui_text_c(ui_small, KB_X + w * WK_W + WK_W / 2, KB_Y + KB_H - 18, n, UI_RGB(0x5A6478));
        }
    }
    for (int o = 0; o < 2; o++)
        for (int b = 0; b < 5; b++) {
            int key = o * 12 + black_semi[b];
            int bx = KB_X + (o * 7 + black_after[b] + 1) * WK_W - BK_W / 2;
            ui_rrect(bx, KB_Y, BK_W, BK_H, 5, s_down[key] ? C_BLUE : UI_RGB(0x161B26));
        }
}

static void draw_scope(void)
{
    ui_rrect(SC_X, SC_Y, SC_W, SC_H, 12, UI_RGB(0x04140C));
    int stride;
    uint16_t *px = ui_pixels(&stride);
    int mid = SC_Y + SC_H / 2;
    for (int x = SC_X + 8; x < SC_X + SC_W - 8; x++) px[mid * stride + x] = UI_RGB(0x14402A);
    for (int x = SC_X + 8 + 64; x < SC_X + SC_W - 8; x += 64)    /* grid: 64 samples = 2 ms */
        for (int y = SC_Y + 6; y < SC_Y + SC_H - 6; y += 3) px[y * stride + x] = UI_RGB(0x14402A);

    /* trigger: start where the wave crosses zero going up, so a steady note stands still */
    uint32_t end = s_scope_pos;
    int n = SC_W - 16, start = (int)(end + SCOPE_N - n - 600) % SCOPE_N;
    for (int i = 0; i < 560; i++) {
        int a = s_scope[(start + i) % SCOPE_N], b = s_scope[(start + i + 1) % SCOPE_N];
        if (a < 0 && b >= 0) { start = (start + i + 1) % SCOPE_N; break; }
    }
    int py = mid;
    for (int i = 0; i < n; i++) {
        int y = mid - s_scope[(start + i) % SCOPE_N] * (SC_H / 2 - 8) / 32768;
        int x = SC_X + 8 + i, y0 = py < y ? py : y, y1 = py < y ? y : py;
        for (int yy = y0; yy <= y1; yy++) px[yy * stride + x] = UI_RGB(0x46F08C);
        py = y;
    }
    ui_text_l(ui_small, SC_X + 12, SC_Y + 16, "SCOPE   2 ms a line", UI_RGB(0x2E9A60));
}

static void draw_spectrum(void)
{
    ui_rrect(SP_X, SC_Y, SP_W, SC_H, 12, UI_RGB(0x0B1226));
    /* bins 0..71 = 0..4.5 kHz (62.5 Hz each), 4 px a bar */
    int base = SC_Y + SC_H - 26;
    for (int i = 1; i < 72; i++) {
        int h = (int)(s_mag[i] * (SC_H - 52));
        if (h > 0) ui_fill(SP_X + 6 + i * 4, base - h, 3, h, ui_mix(C_BLUE, C_ORANGE, (int)(s_mag[i] * 256)));
    }
    ui_text_l(ui_small, SP_X + 12, SC_Y + 16, "SPECTRUM", UI_RGB(0x5A78B0));
    ui_text_l(ui_small, SP_X + 10, base + 13, "0", C_FAINT);
    ui_text_c(ui_small, SP_X + 10 + 32 * 4, base + 13, "2 kHz", C_FAINT);
    ui_text_r(ui_small, SP_X + SP_W - 8, base + 13, "4.5", C_FAINT);
}

static void draw(papp_surface_t *s)
{
    ui_begin(s);
    static const char *const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    char status[96] = "Touch the keys";
    int held = -1, nheld = 0;
    for (int k = 0; k < 24; k++)
        if (s_down[k]) { if (held < 0) held = k; nheld++; }
    if (held >= 0)
        snprintf(status, sizeof(status), "%s%d  %d Hz%s     %s", names[held % 12], s_octave + held / 12,
                 (int)(key_freq(held) + 0.5f), nheld > 1 ? "  (+ more)" : "", wave_name[s_wave]);
    ui_bar("Synth", NULL, 0, 0, status, held >= 0 ? C_GREEN : C_DIM, false);
    draw_scope();
    draw_spectrum();

    for (int w = 0; w < WAVES; w++) {                   /* the shape */
        int x = 20 + w * 92;
        bool on = s_wave == w;
        ui_rrect(x, 274, 86, 44, 22, ui_pressed_color(ID_WAVE + w, on ? C_BLUE : C_RAISED));
        ui_text_c(ui_small, x + 43, 296, wave_name[w], on ? C_TEXT : C_DIM);
        ui_hit(x, 274, 86, 44, ID_WAVE + w, UI_TAP);
    }
    char oct[16];                                       /* octave */
    snprintf(oct, sizeof(oct), "Octave %d", s_octave);
    ui_rrect(484, 274, 44, 44, 22, ui_pressed_color(ID_OCT_DOWN, C_RAISED));
    ui_text_c(ui_med, 506, 294, "-", C_TEXT);
    ui_hit(484, 274, 44, 44, ID_OCT_DOWN, UI_TAP);
    ui_text_c(ui_small, 580, 296, oct, C_TEXT);
    ui_rrect(632, 274, 44, 44, 22, ui_pressed_color(ID_OCT_UP, C_RAISED));
    ui_text_c(ui_med, 654, 294, "+", C_TEXT);
    ui_hit(632, 274, 44, 44, ID_OCT_UP, UI_TAP);
    ui_rrect(690, 274, 90, 44, 22, ui_pressed_color(ID_ECHO, s_echo ? C_GREEN : C_RAISED));
    ui_text_c(ui_small, 735, 296, "Echo", s_echo ? C_TEXT : C_DIM);
    ui_hit(690, 274, 90, 44, ID_ECHO, UI_TAP);

    draw_keyboard();
    ui_end(s);
}

static void all_up(void)
{
    for (int k = 0; k < 24; k++) {
        key_up(k);
        s_down[k] = false;                          /* still held: starts again in the new octave */
    }
}

static void act(int id, int x)
{
    (void)x;
    if (id >= ID_WAVE && id < ID_WAVE + WAVES) s_wave = id - ID_WAVE;
    else if (id == ID_OCT_DOWN && s_octave > 1) { all_up(); s_octave--; }
    else if (id == ID_OCT_UP && s_octave < 6) { all_up(); s_octave++; }
    else if (id == ID_ECHO) s_echo = !s_echo;
    else if (id == UI_ID_CLOSE) s_quit = true;
}

/* USB keyboard: the row A S D F G H J K is C D E F G A B C, W E T Y U the black keys */
static const struct { uint16_t usage; uint8_t key; } kbd_map[] = {
    { 0x04, 0 }, { 0x16, 2 }, { 0x07, 4 }, { 0x09, 5 }, { 0x0A, 7 }, { 0x0B, 9 }, { 0x0D, 11 }, { 0x0E, 12 },
    { 0x1A, 1 }, { 0x08, 3 }, { 0x17, 6 }, { 0x1C, 8 }, { 0x18, 10 },
};

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    papp_surface_t *s = ui_open();
    if (!s) return 1;
    for (int i = 0; i < 1024; i++) s_sine[i] = sinf(i * 2 * (float)M_PI / 1024) * 0.8f;
    for (int i = 0; i < FFT_N; i++) s_hann[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (FFT_N - 1));
    for (int v = 0; v < VOICES; v++) s_voice[v].key = -1;
    s_echo_buf = papp_alloc(ECHO_N * 2, PAPP_MEM_LARGE, 4);
    papp_audio_cfg_t cfg = { .rate = RATE, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 40 };
    papp_audio_stream_t *a = papp_audio_open(&cfg);
    if (!a || !s_echo_buf) {
        papp_log("no audio\n");
        papp_free(s_echo_buf);
        papp_disp_surface_destroy(s);
        return 1;
    }
    memset(s_echo_buf, 0, ECHO_N * 2);
    papp_thread_cfg_t tc = { .name = "synth", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 1 };
    papp_thread_t *t = papp_thread_create(audio_thread, a, &tc);
    if (!t) return 1;

    uint32_t prev = 0xFFFFFFFFu;
    while (!papp_exit_requested() && !s_quit) {
        papp_gamepad_t gp = { 0 };
        papp_input_gamepad(0, &gp);
        uint32_t press = gp.buttons & ~prev;
        prev = gp.buttons;
        if (press & PAPP_BTN_X) break;
        if (press & PAPP_BTN_L1) s_wave = (s_wave + WAVES - 1) % WAVES;
        if (press & PAPP_BTN_R1) s_wave = (s_wave + 1) % WAVES;
        if (press & PAPP_BTN_UP) act(ID_OCT_UP, 0);
        if (press & PAPP_BTN_DOWN) act(ID_OCT_DOWN, 0);

        bool want[24] = { 0 };                          /* every key held right now */
        papp_touch_point_t pts[5];
        int n = papp_disp_touch(s, pts, 5);
        for (int i = 0; i < n; i++) {
            int k = key_at(pts[i].x, pts[i].y);
            if (k >= 0) want[k] = true;
        }
        if (gp.buttons & PAPP_BTN_A) want[0] = true;    /* C E G: a chord */
        if (gp.buttons & PAPP_BTN_B) want[4] = true;
        if (gp.buttons & PAPP_BTN_Y) want[7] = true;
        for (size_t i = 0; i < sizeof(kbd_map) / sizeof(kbd_map[0]); i++)
            if (papp_input_key_down(kbd_map[i].usage)) want[kbd_map[i].key] = true;
        for (int k = 0; k < 24; k++) {
            if (want[k] && !s_down[k]) key_down(k);
            if (!want[k] && s_down[k]) key_up(k);
            s_down[k] = want[k];
        }

        ui_touch(s, act);
        spectrum();
        draw(s);
    }
    s_stop = true;
    papp_thread_join(t, PAPP_WAIT_FOREVER, NULL);
    papp_audio_close(a);
    papp_free(s_echo_buf);
    papp_disp_surface_destroy(s);
    return 0;
}
