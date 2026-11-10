/*
 * Mandelbrot race (education): the same picture drawn four ways, each one timed.
 *
 * The Mandelbrot set: for each pixel take the point c of the plane, start at z = 0 and repeat
 * z = z * z + c. If z stays small the point is in the set (black); otherwise the colour says
 * how many steps it took to escape. Every pixel is independent, so the work splits easily.
 *
 *   1 core           one thread does all the rows.
 *   2 cores          two threads, one on each core of the P4, take every other row: about 2x.
 *   2 cores + FAST   the same, with the inner loop placed in internal SRAM (PAPP_FAST) instead
 *                    of PSRAM. It only helps when the code doesn't stay in the cache: this
 *                    loop is small, so expect little or no change. That is a lesson too.
 *   Fixed point      2 cores using whole numbers (scaled by 2^27) instead of float. On the P4,
 *                    which has a floating-point unit, float is the faster one.
 *
 * Tap the picture to zoom in there (2x); Zoom out goes back. Race all runs the four in a row.
 * Deep in (zoom 4096 and more) float runs out of digits and the picture turns blocky; fixed
 * point, with 27 bits after the point, stays sharp further: speed is not everything.
 *
 * Worth knowing: the workers run at the same priority as the main thread. At a higher one the
 * worker on the main thread's core would start at once and keep the main thread from starting
 * the second worker: the "2 cores" run would take as long as 1 core.
 *
 * Pad: A race all, R1 zoom in (middle), B / L1 zoom out, LEFT / RIGHT pick a way, Y run it,
 * X exits.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "edu_ui.h"

PAPP_MANIFEST(.id = "edu.mandel", .name = "Mandelbrot Race", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define IW 512                          /* the picture */
#define IH 352
#define IX 20
#define IY 112
#define SIDE_X 548
#define SIDE_W 232
#define MAX_IT 255

enum { M_ONE, M_TWO, M_FAST, M_FIXED, METHODS };
static const char *const method_name[METHODS] = { "1 core", "2 cores", "2 cores + FAST", "Fixed point" };
enum { ID_METHOD = 1, ID_RACE = 10, ID_ZOOM_OUT, ID_RESET };

static uint16_t *s_img;                 /* IW x IH, PSRAM */
static uint16_t s_pal[MAX_IT + 1];
static PAPP_FAST_DATA uint16_t s_pal_fast[MAX_IT + 1];
static double s_cx = -0.6, s_cy = 0, s_scale = 3.2 / IW;     /* centre and size of a pixel */
static int s_method = M_TWO, s_zoom = 1;
static int64_t s_time[METHODS];         /* last time of each, us (0: not run at this view) */
static bool s_quit, s_busy;
static int s_pending = -1;              /* a run asked for by a button: done after this frame */
static bool s_race;

/* ── The inner loop, three times ───────────────────────────────────────── */

typedef struct { int y0, step; float x0, y0f, d; } job_t;

/* One row in float: the same code in PSRAM (normal) and in internal SRAM (PAPP_FAST) */
#define ROW_FLOAT(NAME, ATTR, PAL)                                               \
    static ATTR void NAME(uint16_t *out, float cx0, float cy, float d)           \
    {                                                                            \
        for (int x = 0; x < IW; x++) {                                           \
            float cr = cx0 + x * d, zr = 0, zi = 0, r2 = 0, i2 = 0;              \
            int it = 0;                                                          \
            while (it < MAX_IT && r2 + i2 < 4.0f) {                              \
                zi = 2 * zr * zi + cy;                                           \
                zr = r2 - i2 + cr;                                               \
                r2 = zr * zr;                                                    \
                i2 = zi * zi;                                                    \
                it++;                                                            \
            }                                                                    \
            out[x] = PAL[it];                                                    \
        }                                                                        \
    }
ROW_FLOAT(row_float, __attribute__((noinline)), s_pal)
ROW_FLOAT(row_float_fast, PAPP_FAST, s_pal_fast)

/* Fixed point: numbers times 2^27 in 32 bits (-16 .. 16), products in 64 bits */
#define FIX 27
static void __attribute__((noinline)) row_fixed(uint16_t *out, float cx0f, float cyf, float df)
{
    int32_t cx0 = (int32_t)(cx0f * (1 << FIX)), cy = (int32_t)(cyf * (1 << FIX)), d = (int32_t)(df * (1 << FIX));
    const int32_t four = 4 << FIX;
    for (int x = 0; x < IW; x++) {
        int32_t cr = cx0 + x * d, zr = 0, zi = 0, r2 = 0, i2 = 0;
        int it = 0;
        while (it < MAX_IT && r2 + i2 < four) {
            zi = (int32_t)(((int64_t)zr * zi) >> (FIX - 1)) + cy;
            zr = r2 - i2 + cr;
            r2 = (int32_t)(((int64_t)zr * zr) >> FIX);
            i2 = (int32_t)(((int64_t)zi * zi) >> FIX);
            it++;
        }
        out[x] = s_pal[it];
    }
}

static int s_run_method;

static int worker(void *arg)
{
    const job_t *j = arg;
    for (int y = j->y0; y < IH && !papp_exit_requested(); y += j->step) {
        float cy = j->y0f + y * j->d;
        uint16_t *out = &s_img[y * IW];
        if (s_run_method == M_FAST) row_float_fast(out, j->x0, cy, j->d);
        else if (s_run_method == M_FIXED) row_fixed(out, j->x0, cy, j->d);
        else row_float(out, j->x0, cy, j->d);
    }
    return 0;
}

/* Draws the picture with method m and returns the time it took, in us */
static int64_t run(int m)
{
    int n = m == M_ONE ? 1 : 2;
    s_run_method = m;
    job_t job[2];
    float x0 = (float)(s_cx - IW / 2 * s_scale), y0 = (float)(s_cy - IH / 2 * s_scale), d = (float)s_scale;
    papp_thread_t *t[2] = { NULL, NULL };
    int64_t t0 = papp_time_us();
    for (int i = 0; i < n; i++) {
        job[i] = (job_t){ i, n, x0, y0, d };
        papp_thread_cfg_t cfg = { .name = "mandel", .stack_size = 4096, .prio = PAPP_PRIO_NORMAL, .core = (int8_t)i };
        t[i] = papp_thread_create(worker, &job[i], &cfg);
        if (!t[i]) worker(&job[i]);     /* no threads: do it here */
    }
    for (int i = 0; i < n; i++)
        if (t[i]) papp_thread_join(t[i], PAPP_WAIT_FOREVER, NULL);
    return papp_time_us() - t0;
}

static void make_palette(void)
{
    for (int i = 0; i < MAX_IT; i++) {          /* dark blue -> cyan -> white -> orange, repeating */
        float t = (i % 64) / 64.0f;
        float r, g, b;
        if (t < 0.33f) { float k = t / 0.33f; r = 0.0f; g = 0.1f + 0.6f * k; b = 0.35f + 0.6f * k; }
        else if (t < 0.66f) { float k = (t - 0.33f) / 0.33f; r = k; g = 0.7f + 0.3f * k; b = 0.95f; }
        else { float k = (t - 0.66f) / 0.34f; r = 1.0f; g = 1.0f - 0.45f * k; b = 0.95f - 0.9f * k; }
        s_pal[i] = PAPP_RGB565((int)(r * 255), (int)(g * 255), (int)(b * 255));
    }
    s_pal[MAX_IT] = 0;                          /* in the set */
    memcpy(s_pal_fast, s_pal, sizeof(s_pal));
}

static void new_view(void)
{
    memset(s_time, 0, sizeof(s_time));          /* times only compare at the same view */
}

static void act(int id, int x)
{
    (void)x;
    if (s_busy) return;
    if (id >= ID_METHOD && id < ID_METHOD + METHODS) {
        s_method = id - ID_METHOD;
        s_pending = s_method;
    } else if (id == ID_RACE) {
        s_race = true;
    } else if (id == ID_ZOOM_OUT && s_zoom > 1) {
        s_scale *= 2;
        s_zoom /= 2;
        new_view();
        s_pending = s_method;
    } else if (id == ID_RESET) {
        s_cx = -0.6; s_cy = 0; s_scale = 3.2 / IW; s_zoom = 1;
        new_view();
        s_pending = s_method;
    } else if (id == UI_ID_CLOSE) {
        s_quit = true;
    }
}

/* Zoom in 2x around a point of the picture */
static void zoom_in(int px, int py)
{
    if (s_busy || s_zoom >= 1 << 16) return;
    s_cx += (px - IW / 2) * s_scale;
    s_cy += (py - IH / 2) * s_scale;
    s_scale /= 2;
    s_zoom *= 2;
    new_view();
    s_pending = s_method;
}

/* A tap on the picture: zoom in there */
static void touch_image(papp_surface_t *s)
{
    static bool down, inside;
    static papp_touch_point_t last;
    papp_touch_point_t p;
    bool now = papp_disp_touch(s, &p, 1) > 0;
    if (now) {
        if (!down) inside = p.x >= IX && p.x < IX + IW && p.y >= IY && p.y < IY + IH;
        last = p;
    } else if (down && inside) {
        zoom_in(last.x - IX, last.y - IY);
    }
    down = now;
}

static void draw(papp_surface_t *s, const char *busy)
{
    ui_begin(s);
    char status[96];
    if (busy) snprintf(status, sizeof(status), "Drawing with %s ...", busy);
    else snprintf(status, sizeof(status), "Zoom %dx     Tap the picture to zoom in     Max %d steps", s_zoom, MAX_IT);
    ui_bar("Mandelbrot Race", NULL, 0, 0, status, busy ? C_ORANGE : C_GREEN, busy != NULL);

    int stride;
    uint16_t *px = ui_pixels(&stride);
    for (int y = 0; y < IH; y++) memcpy(&px[(IY + y) * stride + IX], &s_img[y * IW], IW * 2);

    ui_rrect(SIDE_X, IY, SIDE_W, IH, 18, C_PANEL);
    int x = SIDE_X + 14, w = SIDE_W - 28;
    int64_t worst = 1;
    for (int i = 0; i < METHODS; i++)
        if (s_time[i] > worst) worst = s_time[i];
    for (int i = 0; i < METHODS; i++) {           /* one row per way: button, bar, time, speed-up */
        int y = 124 + i * 62;
        bool sel = s_method == i;
        ui_hit(x - 6, y - 4, w + 12, 58, ID_METHOD + i, UI_TAP);
        ui_rrect(x - 6, y - 4, w + 12, 58, 10, ui_pressed_color(ID_METHOD + i, sel ? C_RAISED : C_PANEL));
        ui_text_l(ui_small, x, y + 10, method_name[i], sel ? C_TEXT : C_DIM);
        char t[32] = "-";                         /* line 1: speed-up; line 2: bar + time */
        if (s_time[i] && s_time[M_ONE]) {
            int x10 = (int)((s_time[M_ONE] * 10 + s_time[i] / 2) / s_time[i]);  /* no %f: newlib keeps its buffers */
            snprintf(t, sizeof(t), "x%d.%d", x10 / 10, x10 % 10);
            ui_text_r(ui_small, x + w, y + 10, t, C_TEXT);
        }
        int bar = w - 76;
        snprintf(t, sizeof(t), s_time[i] ? "%d ms" : "-", (int)(s_time[i] / 1000));
        ui_text_r(ui_small, x + w, y + 35, t, s_time[i] ? C_TEXT : C_FAINT);
        int bw = s_time[i] ? (int)(s_time[i] * bar / worst) : 0;
        ui_rrect(x, y + 28, bar, 14, 7, C_BG);
        if (bw > 0) ui_rrect(x, y + 28, bw < 14 ? 14 : bw, 14, 7, i == M_FIXED ? C_ORANGE : i == M_ONE ? C_RED : C_GREEN);
    }
    ui_button(x, 378, w, 38, "Race all", C_BLUE, ID_RACE);
    const int bw = w / 2 - 4;                     /* two small buttons: the small font fits */
    ui_rrect(x, 424, bw, 32, 16, ui_pressed_color(ID_ZOOM_OUT, C_RAISED));
    ui_text_c(ui_small, x + bw / 2, 440, "Zoom out", s_zoom > 1 ? C_TEXT : C_FAINT);
    ui_hit(x, 424, bw, 32, ID_ZOOM_OUT, UI_TAP);
    ui_rrect(x + w - bw, 424, bw, 32, 16, ui_pressed_color(ID_RESET, C_RAISED));
    ui_text_c(ui_small, x + w - bw / 2, 440, "Reset", C_TEXT);
    ui_hit(x + w - bw, 424, bw, 32, ID_RESET, UI_TAP);
    ui_end(s);
}

/* Shows "drawing" first, so the screen says what's happening during the run */
static void timed_run(papp_surface_t *s, int m)
{
    s_busy = true;
    draw(s, method_name[m]);
    draw(s, method_name[m]);                    /* both buffers */
    s_time[m] = run(m);
    papp_log("%s: %d ms (zoom %d)\n", method_name[m], (int)(s_time[m] / 1000), s_zoom);
    s_busy = false;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    papp_surface_t *s = ui_open();
    s_img = papp_alloc(IW * IH * 2, PAPP_MEM_LARGE, 64);
    if (!s || !s_img) return 1;
    memset(s_img, 0, IW * IH * 2);
    make_palette();
    s_race = true;                              /* start with a race */

    uint32_t prev = 0xFFFFFFFFu;
    while (!papp_exit_requested() && !s_quit) {
        papp_gamepad_t gp = { 0 };
        papp_input_gamepad(0, &gp);
        uint32_t press = gp.buttons & ~prev;
        prev = gp.buttons;
        if (press & PAPP_BTN_X) break;
        if (press & PAPP_BTN_A) s_race = true;
        if (press & (PAPP_BTN_B | PAPP_BTN_L1)) act(ID_ZOOM_OUT, 0);
        if (press & PAPP_BTN_R1) zoom_in(IW / 2, IH / 2);
        if (press & PAPP_BTN_LEFT) s_method = (s_method + METHODS - 1) % METHODS;
        if (press & PAPP_BTN_RIGHT) s_method = (s_method + 1) % METHODS;
        if (press & PAPP_BTN_Y) s_pending = s_method;

        touch_image(s);
        ui_touch(s, act);
        if (s_race) {
            s_race = false;
            for (int m = 0; m < METHODS && !papp_exit_requested(); m++) timed_run(s, m);
        } else if (s_pending >= 0) {
            int m = s_pending;
            s_pending = -1;
            timed_run(s, m);
        }
        draw(s, NULL);
    }
    papp_free(s_img);
    papp_disp_surface_destroy(s);
    return 0;
}
