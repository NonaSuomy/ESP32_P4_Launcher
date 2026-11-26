/*
 * perf_bench — CPU, memory and 3D engine benchmark, to compare boards (chip v1.x vs v3.x).
 * Built and run by tools/perf_bench.py, which also turns the panel's scan-out off for some runs
 * (DevLink "display off": the LCD refresh reads the frame buffer from PSRAM all the time).
 *
 *   args: cpu         CoreMark on 1 core, then on 2 cores (each run >= 10 s)
 *         mem         PSRAM and internal SRAM: write, read, copy (1 and 2 cores), load latency
 *         ppa         the PPA filling and scaling into a 1024x600 surface (needs the display)
 *         3d [mode] [frames]   e3d engine, a fixed scene at 400x240 on 2 cores, `frames` frames
 *                     (default 600). mode: off = into a PSRAM buffer, never shown (default);
 *                     show = through a 2-buffer surface, scaled to the screen. The engine's SIMD
 *                     code differs per chip generation (v1.x: PIE, -march ..._xesppie; v3.x: the
 *                     re-encoded _xespv; a v1.x PIE instruction faults on v3), so this build has
 *                     the engine in C, and perf_bench_pie / perf_bench_espv have its SIMD code
 *         clock       the CPU clock, from the cycle counter
 *
 * Results are log lines "RESULT <name> <value> <unit>". Exit code: 0, or 1 if a check failed.
 */
#include "papp/papp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef PERF_E3D
#include "e3d.h"
#endif
/* The 3D results carry the build's flavour: "" = the engine in plain C, "_simd" = with the SIMD
 * extension of the chip it was built for (tools/perf_bench.py builds one app per flavour) */
#ifndef PERF_3D_SUFFIX
#define PERF_3D_SUFFIX ""
#endif

PAPP_MANIFEST(.id = "test.perfbench", .name = "Perf bench", .version = PAPP_APP_VERSION,
              .main_stack = 24576, .flags = PAPP_MF_HIDDEN);

static int g_fail;

static void result(const char *name, double v, const char *unit)
{
    papp_log("RESULT %s %.3f %s", name, v, unit);
}

static inline uint32_t cycles(void)
{
    uint32_t c;
    __asm__ volatile("csrr %0, cycle" : "=r"(c));
    return c;
}

/* ── clock ─────────────────────────────────────────────────────────────── */

static void bench_clock(void)
{
    uint32_t c0 = cycles();
    int64_t t0 = papp_time_us();
    while (papp_time_us() - t0 < 200000) { }
    uint32_t c1 = cycles();
    int64_t t1 = papp_time_us();
    result("cpu_mhz", (double)(c1 - c0) / (double)(t1 - t0), "MHz");
}

/* ── CoreMark ──────────────────────────────────────────────────────────── */

extern int coremark_main(int argc, char *argv[]);
extern unsigned int default_num_contexts;
extern double cm_iter_per_sec;
extern int cm_valid;
void cm_reset_cores(void);

static void bench_cpu(void)
{
    for (unsigned n = 1; n <= 2; n++) {
        default_num_contexts = n;
        cm_iter_per_sec = 0;
        cm_valid = 0;
        cm_reset_cores();
        char *argv[] = { "coremark", NULL };
        coremark_main(1, argv);
        if (!cm_valid) {
            papp_log("FAIL: CoreMark on %u core(s) did not validate", n);
            g_fail = 1;
        }
        result(n == 1 ? "coremark_1core" : "coremark_2core", cm_iter_per_sec, "iter/s");
    }
}

/* ── Memory ────────────────────────────────────────────────────────────── */

#define PS_BYTES  (8u << 20)    /* well past the caches */
#define IN_BYTES  (64u << 10)

/* Kernels, kept in internal SRAM so instruction fetch never competes with the data */
PAPP_FAST static uint32_t k_read(const uint32_t *p, size_t words)
{
    uint32_t a = 0, b = 0, c = 0, d = 0;
    for (size_t i = 0; i < words; i += 8) {
        a += p[i] ^ p[i + 4];
        b += p[i + 1] ^ p[i + 5];
        c += p[i + 2] ^ p[i + 6];
        d += p[i + 3] ^ p[i + 7];
    }
    return a + b + c + d;
}

PAPP_FAST static void k_write(uint32_t *p, size_t words, uint32_t v)
{
    for (size_t i = 0; i < words; i += 8) {
        p[i] = v; p[i + 1] = v; p[i + 2] = v; p[i + 3] = v;
        p[i + 4] = v; p[i + 5] = v; p[i + 6] = v; p[i + 7] = v;
    }
}

PAPP_FAST static uint32_t k_chase(const uint32_t *p, uint32_t start, int hops)
{
    uint32_t i = start;
    while (hops--) i = p[i];
    return i;
}

static volatile uint32_t g_sink;

/* best MB/s of 3 passes; op 0 read, 1 write, 2 memcpy (src = buf, dst = buf2) */
static double pass_mbs(int op, uint32_t *buf, uint32_t *buf2, size_t bytes, int reps)
{
    double best = 0;
    for (int r = 0; r < 3; r++) {
        int64_t t0 = papp_time_us();
        for (int k = 0; k < reps; k++) {
            if (op == 0) g_sink += k_read(buf, bytes / 4);
            else if (op == 1) k_write(buf, bytes / 4, (uint32_t)k);
            else memcpy(buf2, buf, bytes);
        }
        int64_t dt = papp_time_us() - t0;
        double mbs = (double)bytes * reps / (double)dt;       /* bytes/us = MB/s */
        if (op == 2) mbs *= 2;                                 /* read + write */
        if (mbs > best) best = mbs;
    }
    return best;
}

typedef struct { int op; uint32_t *buf; size_t bytes; int reps; papp_sem_t *go; } par_t;

static int par_worker(void *arg)
{
    par_t *w = arg;
    papp_sem_take(w->go, PAPP_WAIT_FOREVER);
    for (int k = 0; k < w->reps; k++) {
        if (w->op == 0) g_sink += k_read(w->buf, w->bytes / 4);
        else k_write(w->buf, w->bytes / 4, (uint32_t)k);
    }
    return 0;
}

/* Both cores at once, each on its own half: total MB/s */
static double par_mbs(int op, uint32_t *buf, size_t bytes, int reps)
{
    double best = 0;
    for (int r = 0; r < 3; r++) {
        par_t w[2];
        papp_thread_t *t[2];
        papp_sem_t *go = papp_sem_create(0, 2);
        for (int c = 0; c < 2; c++) {
            w[c] = (par_t){ op, buf + c * (bytes / 8), bytes / 2, reps, go };
            /* NORMAL: a HIGH worker on this thread's core would run to its end before the
             * second one is let go */
            papp_thread_cfg_t cfg = { .name = "membw", .stack_size = 4096, .prio = PAPP_PRIO_NORMAL, .core = (int8_t)c };
            t[c] = papp_thread_create(par_worker, &w[c], &cfg);
        }
        if (!go || !t[0] || !t[1]) { papp_log("FAIL: no threads"); g_fail = 1; return 0; }
        papp_delay_ms(5);
        int64_t t0 = papp_time_us();
        papp_sem_give(go);
        papp_sem_give(go);
        for (int c = 0; c < 2; c++) papp_thread_join(t[c], PAPP_WAIT_FOREVER, NULL);
        int64_t dt = papp_time_us() - t0;                 /* both done */
        papp_sem_destroy(go);
        double mbs = (double)bytes * reps / (double)dt;
        if (mbs > best) best = mbs;
    }
    return best;
}

/* ns per dependent load, random cycle over `bytes` */
static double chase_ns(uint32_t *buf, size_t bytes)
{
    size_t n = bytes / 64;                      /* one entry per cache line */
    size_t stride = 16;                         /* words per line */
    uint32_t *order = papp_alloc(n * sizeof(uint32_t), PAPP_MEM_LARGE, 4);
    if (!order) return 0;
    for (size_t i = 0; i < n; i++) order[i] = (uint32_t)i;
    uint32_t seed = 12345;
    for (size_t i = n - 1; i > 0; i--) {
        seed = seed * 1103515245u + 12345u;
        size_t j = (seed >> 8) % (i + 1);
        uint32_t x = order[i]; order[i] = order[j]; order[j] = x;
    }
    for (size_t i = 0; i < n; i++) buf[order[i] * stride] = order[(i + 1) % n] * stride;
    uint32_t start = order[0] * stride;
    papp_free(order);
    int hops = 1 << 20;
    if ((size_t)hops < n * 4) hops = (int)(n * 4);
    g_sink += k_chase(buf, start, hops / 4);   /* warm-up */
    int64_t t0 = papp_time_us();
    g_sink += k_chase(buf, start, hops);
    return (double)(papp_time_us() - t0) * 1000.0 / hops;
}

static void bench_mem(void)
{
    uint32_t *ps = papp_alloc(PS_BYTES, PAPP_MEM_LARGE, 64);
    uint32_t *ps2 = papp_alloc(PS_BYTES / 2, PAPP_MEM_LARGE, 64);
    uint32_t *in = papp_alloc(IN_BYTES, PAPP_MEM_FAST, 64);
    uint32_t *in2 = papp_alloc(IN_BYTES, PAPP_MEM_FAST, 64);
    if (!ps || !ps2 || !in || !in2) {
        papp_log("FAIL: no memory for the buffers");
        g_fail = 1;
        return;
    }
    memset(ps, 1, PS_BYTES);
    memset(in, 1, IN_BYTES);
    result("psram_read",  pass_mbs(0, ps, NULL, PS_BYTES, 2), "MB/s");
    result("psram_write", pass_mbs(1, ps, NULL, PS_BYTES, 2), "MB/s");
    result("psram_copy",  pass_mbs(2, ps, ps2, PS_BYTES / 2, 2), "MB/s");
    result("psram_read_2core",  par_mbs(0, ps, PS_BYTES, 2), "MB/s");
    result("psram_write_2core", par_mbs(1, ps, PS_BYTES, 2), "MB/s");
    result("psram_latency", chase_ns(ps, PS_BYTES), "ns");
    result("sram_read",  pass_mbs(0, in, NULL, IN_BYTES, 256), "MB/s");
    result("sram_write", pass_mbs(1, in, NULL, IN_BYTES, 256), "MB/s");
    result("sram_copy",  pass_mbs(2, in, in2, IN_BYTES, 256), "MB/s");
    result("sram_latency", chase_ns(in, IN_BYTES), "ns");
    papp_free(ps);
    papp_free(ps2);
    papp_free(in);
    papp_free(in2);
}

/* ── PPA ───────────────────────────────────────────────────────────────── */

static void bench_ppa(void)
{
    papp_disp_info_t di;
    papp_disp_info(&di);
    papp_surface_cfg_t cfg = { .width = di.width, .height = di.height, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_NONE, .buffers = 1 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    int sw = di.width / 2, sh = di.height / 2;
    uint16_t *src = papp_alloc((size_t)sw * sh * 2, PAPP_MEM_LARGE | PAPP_MEM_DMA, 64);
    if (!s || !src) {
        papp_log("FAIL: no surface or picture");
        g_fail = 1;
        return;
    }
    for (int i = 0; i < sw * sh; i++) src[i] = (uint16_t)(i * 2654435761u >> 16);
    int stride;
    papp_disp_lock(s, &stride);
    const int n = 100;
    double mpx = (double)di.width * di.height * n / 1e6;
    int64_t t0 = papp_time_us();
    for (int i = 0; i < n; i++) papp_disp_fill(s, 0, 0, di.width, di.height, (uint16_t)(i * 0x0841));
    double dt = (double)(papp_time_us() - t0) / 1e6;
    result("ppa_fill", mpx / dt, "Mpix/s");
    papp_blit_t b = { .pixels = src, .w = (uint16_t)sw, .h = (uint16_t)sh, .format = PAPP_BLIT_RGB565,
                      .scale_x = 512, .scale_y = 512 };
    t0 = papp_time_us();
    for (int i = 0; i < n; i++) papp_disp_blit(s, &b);
    dt = (double)(papp_time_us() - t0) / 1e6;
    result("ppa_scale2x", mpx / dt, "Mpix/s");
    papp_disp_present(s);
    papp_disp_surface_destroy(s);
    papp_free(src);
}

/* ── 3D ────────────────────────────────────────────────────────────────── */

#ifdef PERF_E3D
#define W3 400
#define H3 240
#define NBALL 9

static void bench_3d(bool show, int frames)
{
    papp_surface_t *surf = NULL;
    uint16_t *fb = NULL;
    if (show) {
        papp_surface_cfg_t cfg = { .width = W3, .height = H3, .format = PAPP_PIXFMT_RGB565,
                                   .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
        surf = papp_disp_surface_create(&cfg);
    } else {
        fb = papp_alloc(W3 * H3 * 2, PAPP_MEM_LARGE | PAPP_MEM_DMA, 64);
    }
    e3d_t *e = e3d_create(W3, H3, 2);
    e3d_mesh *floor_m = e3d_mesh_floor(0, -500, 1100, 500, 0, 16, 0xD8D0B8, 0x506070);
    e3d_mesh *ball_m = e3d_mesh_sphere(45, 24, 16, 230, 70, 60);
    e3d_mesh *gem_m = e3d_mesh_sphere(70, 8, 4, 80, 200, 240);
    if ((!surf && !fb) || !e || !floor_m || !ball_m || !gem_m) {
        papp_log("FAIL: 3d: out of memory");
        g_fail = 1;
        return;
    }
    *e3d_cam(e) = (e3d_camera){ .pos = { -160, 300, 0 }, .pitch = -22 };
    e3d_params *par = e3d_params_get(e);
    par->clear_rgb565 = 0x18E7;
    par->shadows = true;
    par->shadow_y = 1;
    par->shadow_dark = 0.4f;
    e3d_object floor_o, ball[NBALL], gem;
    e3d_object_init(&floor_o, floor_m);
    floor_o.flags = E3D_FLOOR;
    for (int i = 0; i < NBALL; i++) {
        e3d_object_init(&ball[i], ball_m);
        e3d_object_color(&ball[i], 60 + 20 * i, 200 - 15 * i, 90 + 15 * (i % 3));
    }
    e3d_object_init(&gem, gem_m);
    gem.flags = E3D_FLAT;

    int64_t t_render = 0, t_geom = 0, t_rast = 0, tris = 0;
    int64_t t0 = papp_time_us();
    for (int f = 0; f < frames && !papp_exit_requested(); f++) {
        float t = (float)f / 30.f;                      /* the same scene on every board */
        *e3d_light_get(e) = (e3d_light){ .pos = { 550 + 300 * cosf(t * 0.6f), 450, 300 * sinf(t * 0.6f) },
                                         .r = 255, .g = 245, .b = 220 };
        for (int i = 0; i < NBALL; i++) {
            e3d_mat m;
            e3d_move(&m, 380 + 140 * (i / 3), 45 + 140 * fabsf(sinf(t * 2.0f + i * 0.7f)), -200 + 200 * (i % 3));
            ball[i].m = m;
        }
        e3d_mat m, spin;
        e3d_rotate(&spin, 0, t * 70, 20);
        e3d_move(&m, 700, 260 + 30 * sinf(t * 1.3f), 0);
        e3d_mul(&m, &spin);
        gem.m = m;

        int stride = W3;
        uint16_t *px = show ? papp_disp_lock(surf, &stride) : fb;
        int64_t r0 = papp_time_us();
        e3d_begin(e, px, stride);
        e3d_draw(e, &floor_o);
        for (int i = 0; i < NBALL; i++) e3d_draw(e, &ball[i]);
        e3d_draw(e, &gem);
        e3d_draw_light(e);
        e3d_end(e);
        t_render += papp_time_us() - r0;
        const e3d_stats *st = e3d_get_stats(e);
        t_geom += st->t_geom_us;
        t_rast += st->t_raster_us;
        tris += st->tris_drawn + st->shadow_tris;
        if (show) papp_disp_present(surf);
    }
    double secs = (double)(papp_time_us() - t0) / 1e6;
    const char *tag = show ? "3d_show" : "3d_off";
    char name[48];
    const char *sx = PERF_3D_SUFFIX;
    snprintf(name, sizeof(name), "%s%s_fps", tag, sx);         result(name, frames / secs, "fps");
    snprintf(name, sizeof(name), "%s%s_render_ms", tag, sx);   result(name, t_render / 1000.0 / frames, "ms");
    snprintf(name, sizeof(name), "%s%s_geom_ms", tag, sx);     result(name, t_geom / 1000.0 / frames, "ms");
    snprintf(name, sizeof(name), "%s%s_raster_ms", tag, sx);   result(name, t_rast / 1000.0 / frames, "ms");
    snprintf(name, sizeof(name), "%s%s_tris", tag, sx);        result(name, (double)tris / frames, "tris");

    e3d_destroy(e);
    e3d_mesh_free(floor_m);
    e3d_mesh_free(ball_m);
    e3d_mesh_free(gem_m);
    if (surf) papp_disp_surface_destroy(surf);
    if (fb) papp_free(fb);
}
#endif

int main(int argc, char **argv)
{
    const char *what = argc > 1 ? argv[1] : "clock";
    if (!strcmp(what, "clock")) bench_clock();
    else if (!strcmp(what, "cpu")) bench_cpu();
    else if (!strcmp(what, "mem")) bench_mem();
    else if (!strcmp(what, "ppa")) bench_ppa();
#ifdef PERF_E3D
    else if (!strcmp(what, "3d"))
        bench_3d(argc > 2 && !strcmp(argv[2], "show"), argc > 3 ? atoi(argv[3]) : 600);
#endif
    else {
        papp_log("unknown test '%s'", what);
        return 2;
    }
    return g_fail;
}
