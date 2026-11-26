/*
 * codec_test — checks DISP v5 papp_disp_blit_yuv (PPA YUV 4:2:0 -> RGB565) against a CPU model, and the CODC
 * interface (hardware JPEG encode / decode) by a round trip, then times both. Exit code = failed tests.
 *
 * YUV tests use 16x16 blocks of one Y, U and V, and only check pixels well inside a block (the PPA filters
 * chroma and scales smoothly). Colours are allowed a small difference (the PPA's conversion is fixed point).
 * The last frame (a scaled colour test card) stays on screen for `secs` seconds (arg 1, default 3).
 *
 *   python papp-sdk/tools/papp_build.py --name codec_test --src apps/codec_test/main.c --libc
 *   python tools/papp_dev.py --port COM6 deploy build/apps/codec_test/codec_test.papp --timeout 60
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "papp/papp.h"
#include "dec_tests.h"

PAPP_MANIFEST(.id = "test.codec", .name = "Codec test", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define SW 800
#define SH 480

static papp_surface_t *s;
static uint16_t *px;
static int stride;
static int fails, tests;

static uint16_t rgb(int r, int g, int b) { return PAPP_RGB565(r, g, b); }
static int clamp8(float v) { return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5f); }

/* ── YUV ─────────────────────────────────────────────────────────────── */

#define FW 128                          /* the test frame: 8 x 6 blocks of 16 x 16 */
#define FH 96
#define PAD 24                          /* extra bytes per line for the stride tests */

static uint8_t *fy, *fu, *fv;           /* planes with PAD-wide strides */
#define SY (FW + PAD)
#define SUV (FW / 2 + PAD)

static void block_yuv(int bx, int by, int *y, int *u, int *v)
{
    *y = 40 + (bx * 23 + by * 17) % 170;
    *u = 70 + (bx * 41 + by * 13) % 120;        /* moderate colours: nothing clips */
    *v = 75 + (bx * 19 + by * 37) % 110;
}

static void make_frame(void)
{
    for (int r = 0; r < FH; r++)
        for (int c = 0; c < FW; c++) {
            int y, u, v;
            block_yuv(c / 16, r / 16, &y, &u, &v);
            fy[r * SY + c] = (uint8_t)y;
            if (!(r & 1) && !(c & 1)) {
                fu[(r / 2) * SUV + c / 2] = (uint8_t)u;
                fv[(r / 2) * SUV + c / 2] = (uint8_t)v;
            }
        }
}

static void yuv_to_rgb(int y, int u, int v, bool bt709, bool full, int *r, int *g, int *b)
{
    float Y = full ? (float)y : 1.164f * (y - 16), U = (float)(u - 128), V = (float)(v - 128);
    float k = full ? 1.0f : 1.1429f;             /* chroma 16..240 -> full swing */
    U *= k; V *= k;
    if (bt709) { *r = clamp8(Y + 1.5748f * V); *g = clamp8(Y - 0.1873f * U - 0.4681f * V); *b = clamp8(Y + 1.8556f * U); }
    else       { *r = clamp8(Y + 1.402f * V);  *g = clamp8(Y - 0.3441f * U - 0.7141f * V); *b = clamp8(Y + 1.772f * U); }
}

static void paint_background(void)
{
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) px[y * stride + x] = rgb((x * 3) & 255, (y * 5) & 255, ((x + y) * 2) & 255);
}

static int near(uint16_t got, int r, int g, int b, int tol5, int tol6)
{
    int gr = got >> 11, gg = (got >> 5) & 63, gb = got & 31;
    int er = r >> 3, eg = g >> 2, eb = b >> 3;
    return abs(gr - er) <= tol5 && abs(gg - eg) <= tol6 && abs(gb - eb) <= tol5;
}

static void check_yuv(const char *name, int dx, int dy, int scale256, uint8_t flags, int strided)
{
    tests++;
    paint_background();
    uint16_t before_l = px[(dy + 5) * stride + dx - 1], before_t = px[(dy - 1) * stride + dx + 5];
    papp_yuv_t f = { .y = fy, .u = fu, .v = fv, .w = FW, .h = FH, .dx = (int16_t)dx, .dy = (int16_t)dy,
                     .scale_x = (uint16_t)scale256, .scale_y = (uint16_t)scale256, .flags = flags };
    if (strided) { f.stride_y = SY; f.stride_uv = SUV; }
    else {                                   /* the same frame without gaps */
        static uint8_t ty[FW * FH], tu[FW * FH / 4], tv[FW * FH / 4];
        for (int r = 0; r < FH; r++) memcpy(ty + r * FW, fy + r * SY, FW);
        for (int r = 0; r < FH / 2; r++) { memcpy(tu + r * FW / 2, fu + r * SUV, FW / 2); memcpy(tv + r * FW / 2, fv + r * SUV, FW / 2); }
        f.y = ty; f.u = tu; f.v = tv;
        if (strided == 2) {                  /* planes at odd addresses: the launcher's byte path */
            static uint8_t oy[FW * FH + 1], ou[FW * FH / 4 + 1], ov[FW * FH / 4 + 1];
            memcpy(oy + 1, ty, sizeof(ty)); memcpy(ou + 1, tu, sizeof(tu)); memcpy(ov + 1, tv, sizeof(tv));
            f.y = oy + 1; f.u = ou + 1; f.v = ov + 1;
        }
    }
    int64_t t0 = papp_time_us();
    int r = papp_disp_blit_yuv(s, &f);
    int64_t dt = papp_time_us() - t0;
    if (r != 0) { papp_log("FAIL %s: returned %d\n", name, r); fails++; return; }
    int k = scale256 ? scale256 : 256;
    int bs = 16 * k / 256, bad = 0, shown = 0;
    for (int by = 0; by < FH / 16; by++)
        for (int bx = 0; bx < FW / 16; bx++) {
            int y, u, v, er, eg, eb;
            block_yuv(bx, by, &y, &u, &v);
            yuv_to_rgb(y, u, v, flags & PAPP_YUV_BT709, flags & PAPP_YUV_FULL_RANGE, &er, &eg, &eb);
            for (int yy = 3; yy < bs - 3; yy++)
                for (int xx = 3; xx < bs - 3; xx++) {
                    uint16_t got = px[(dy + by * bs + yy) * stride + dx + bx * bs + xx];
                    if (!near(got, er, eg, eb, 2, 3)) {
                        if (shown++ < 3)
                            papp_log("  %s block %d,%d: got r%d g%d b%d, want r%d g%d b%d (yuv %d %d %d)\n", name, bx, by,
                                     (got >> 11) << 3, ((got >> 5) & 63) << 2, (got & 31) << 3, er, eg, eb, y, u, v);
                        bad++;
                    }
                }
        }
    if (px[(dy + 5) * stride + dx - 1] != before_l || px[(dy - 1) * stride + dx + 5] != before_t) {
        papp_log("  %s: drew outside the frame\n", name);
        bad++;
    }
    if (bad) { papp_log("FAIL %s: %d pixels off\n", name, bad); fails++; }
    else papp_log("ok   %s (%lld us)\n", name, dt);
}

/* ── JPEG ────────────────────────────────────────────────────────────── */

static uint16_t jblock(int bx, int by) { return rgb(30 + bx * 47 % 200, 40 + by * 59 % 200, 60 + (bx + by) * 31 % 180); }

static void make_picture(uint16_t *p, int w, int h, int st)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) p[y * st + x] = jblock(x / 16, y / 16);
}

/* Block interiors within tol (5-bit units; JPEG is lossy) */
static int compare_picture(const char *name, const uint16_t *p, int w, int h, int st)
{
    int bad = 0, shown = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (x % 16 < 3 || x % 16 > 12 || y % 16 < 3 || y % 16 > 12 || x / 16 * 16 + 16 > w || y / 16 * 16 + 16 > h) continue;
            uint16_t e = jblock(x / 16, y / 16), g = p[y * st + x];
            if (!near(g, (e >> 11) << 3, ((e >> 5) & 63) << 2, (e & 31) << 3, 2, 4)) {
                if (shown++ < 3)
                    papp_log("  %s at %d,%d: got %04x want %04x\n", name, x, y, g, e);
                bad++;
            }
        }
    return bad;
}

static void jpeg_round_trip(const papp_if_codc_t *c, const char *name, int w, int h, int quality)
{
    tests++;
    int st = w + 8;
    uint16_t *pic = papp_alloc((size_t)st * h * 2, PAPP_MEM_LARGE, 64);
    uint16_t *back = papp_alloc((size_t)(w + 3) * h * 2, PAPP_MEM_LARGE, 4);
    size_t cap = (size_t)w * h * 2;
    uint8_t *jpg = papp_alloc(cap, PAPP_MEM_LARGE, 4);
    if (!pic || !back || !jpg) { papp_log("FAIL %s: no memory\n", name); fails++; goto out; }
    make_picture(pic, w, h, st);
    int64_t t0 = papp_time_us();
    int n = c->jpeg_encode(pic, w, h, st, quality, jpg, cap);
    int64_t t1 = papp_time_us();
    if (n <= 0) { papp_log("FAIL %s: encode returned %d\n", name, n); fails++; goto out; }
    uint16_t iw = 0, ih = 0;
    int r = c->jpeg_info(jpg, (size_t)n, &iw, &ih);
    if (r != 0 || iw != w || ih != h) { papp_log("FAIL %s: info %d %ux%u\n", name, r, iw, ih); fails++; goto out; }
    memset(back, 0, (size_t)(w + 3) * h * 2);
    int64_t t2 = papp_time_us();
    r = c->jpeg_decode(jpg, (size_t)n, back, w + 3, (size_t)(w + 3) * h * 2, &iw, &ih);
    int64_t t3 = papp_time_us();
    if (r != 0 || iw != w || ih != h) { papp_log("FAIL %s: decode %d %ux%u\n", name, r, iw, ih); fails++; goto out; }
    int bad = compare_picture(name, back, w, h, w + 3);
    if (bad) { papp_log("FAIL %s: %d pixels off\n", name, bad); fails++; }
    else papp_log("ok   %s: %d bytes, encode %lld us, decode %lld us\n", name, n, t1 - t0, t3 - t2);
out:
    papp_free(pic);
    papp_free(back);
    papp_free(jpg);
}

/* Odd sizes: the encoder takes them, the decoder answers PAPP_E_UNSUPPORTED (an engine limit) */
static void jpeg_odd(const papp_if_codc_t *c)
{
    int w = 100, h = 75;
    uint16_t *pic = papp_alloc(w * h * 2, PAPP_MEM_LARGE, 64);
    uint8_t *jpg = papp_alloc(w * h * 2, PAPP_MEM_LARGE, 4);
    if (!pic || !jpg) return;
    make_picture(pic, w, h, w);
    tests++;
    int n = c->jpeg_encode(pic, w, h, 0, 90, jpg, w * h * 2);
    uint16_t iw = 0, ih = 0;
    int ri = n > 0 ? c->jpeg_info(jpg, (size_t)n, &iw, &ih) : -99;
    int rd = n > 0 ? c->jpeg_decode(jpg, (size_t)n, pic, 0, (size_t)w * h * 2, NULL, NULL) : -99;
    if (n <= 0 || ri != 0 || iw != w || ih != h || rd != PAPP_E_UNSUPPORTED) {
        papp_log("FAIL jpeg 100x75: encode %d, info %d %ux%u, decode %d\n", n, ri, iw, ih, rd);
        fails++;
    } else papp_log("ok   jpeg 100x75: encoded %d bytes, decode refused (unsupported)\n", n);
    papp_free(pic);
    papp_free(jpg);
}

static void jpeg_errors(const papp_if_codc_t *c)
{
    int w = 64, h = 48;
    uint16_t *pic = papp_alloc(w * h * 2, PAPP_MEM_LARGE, 64);
    uint8_t *jpg = papp_alloc(w * h * 2, PAPP_MEM_LARGE, 4);
    if (!pic || !jpg) return;
    make_picture(pic, w, h, w);
    int n = c->jpeg_encode(pic, w, h, 0, 0, jpg, w * h * 2);
    tests++;
    int r = c->jpeg_decode(jpg, (size_t)n, pic, 0, (size_t)w * h * 2 - 2, NULL, NULL);
    if (r != PAPP_E_LIMIT) { papp_log("FAIL decode into too small a buffer: %d\n", r); fails++; } else papp_log("ok   decode into too small a buffer\n");
    tests++;
    r = c->jpeg_encode(pic, w, h, 0, 80, jpg, 16);
    if (r != PAPP_E_LIMIT) { papp_log("FAIL encode into 16 bytes: %d\n", r); fails++; } else papp_log("ok   encode into 16 bytes (%d)\n", r);
    tests++;
    static const uint8_t junk[64] = { 1, 2, 3, 4 };
    r = c->jpeg_decode(junk, sizeof(junk), pic, 0, (size_t)w * h * 2, NULL, NULL);
    if (r >= 0) { papp_log("FAIL decode of junk: %d\n", r); fails++; } else papp_log("ok   decode of junk (%d)\n", r);
    tests++;
    n = c->jpeg_encode(pic, w, h, 0, 50, jpg, w * h * 2);
    if (n > 20) memset(jpg + n / 2, 0x55, 20);          /* damaged in the middle */
    r = n > 0 ? c->jpeg_decode(jpg, (size_t)n, pic, 0, (size_t)w * h * 2, NULL, NULL) : -99;
    papp_log("ok   decode of a damaged picture: %d (no crash)\n", r);
    papp_free(pic);
    papp_free(jpg);
}

static void jpeg_bench(const papp_if_codc_t *c, int w, int h)
{
    uint16_t *pic = papp_alloc((size_t)w * h * 2, PAPP_MEM_LARGE, 64);
    uint8_t *jpg = papp_alloc((size_t)w * h * 2, PAPP_MEM_LARGE, 4);
    if (!pic || !jpg) return;
    for (int y = 0; y < h; y++)                         /* something photo-like: gradients and noise */
        for (int x = 0; x < w; x++) pic[y * w + x] = rgb(x * 255 / w, y * 255 / h, ((x ^ y) * 7 + rand() % 24) & 255);
    int n = c->jpeg_encode(pic, w, h, 0, 80, jpg, (size_t)w * h * 2);
    if (n <= 0) { papp_log("bench %dx%d: encode %d\n", w, h, n); goto out; }
    int64_t t0 = papp_time_us();
    const int N = 20;
    for (int i = 0; i < N; i++) c->jpeg_decode(jpg, (size_t)n, pic, 0, (size_t)w * h * 2, NULL, NULL);
    int64_t dt = papp_time_us() - t0;
    papp_log("bench jpeg decode %dx%d (%d bytes): %lld us a frame (%.0f fps)\n", w, h, n, dt / N, N * 1e6 / dt);
out:
    papp_free(pic);
    papp_free(jpg);
}

static void yuv_bench(int w, int h, int scale256)
{
    uint8_t *y = papp_alloc((size_t)w * h * 3 / 2, PAPP_MEM_LARGE, 64);
    if (!y) return;
    memset(y, 128, (size_t)w * h * 3 / 2);
    papp_yuv_t f = { .y = y, .u = y + w * h, .v = y + w * h + w * h / 4, .w = (uint16_t)w, .h = (uint16_t)h,
                     .scale_x = (uint16_t)scale256, .scale_y = (uint16_t)scale256 };
    int k = scale256 * w / 256;
    f.dx = (int16_t)(((SW - k) / 2) & ~1);
    f.dy = 0;
    int64_t t0 = papp_time_us();
    const int N = 30;
    int r = 0;
    for (int i = 0; i < N && r == 0; i++) r = papp_disp_blit_yuv(s, &f);
    int64_t dt = papp_time_us() - t0;
    papp_log("bench blit_yuv %dx%d at %d/256: %lld us a frame (r %d)\n", w, h, scale256, dt / N, r);
    papp_free(y);
}

int main(int argc, char **argv)
{
    int secs = argc > 1 ? atoi(argv[1]) : 3;
    papp_surface_cfg_t cfg = { .width = SW, .height = SH, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
    s = papp_disp_surface_create(&cfg);
    fy = papp_alloc(SY * FH, PAPP_MEM_LARGE, 4);
    fu = papp_alloc(SUV * FH / 2, PAPP_MEM_LARGE, 4);
    fv = papp_alloc(SUV * FH / 2, PAPP_MEM_LARGE, 4);
    if (!s || !fy || !fu || !fv) return 100;
    px = papp_disp_lock(s, &stride);
    make_frame();

    papp_yuv_t probe = { .y = fy, .u = fu, .v = fv, .w = FW, .h = FH };
    if (papp_disp_blit_yuv(s, &probe) == PAPP_E_UNSUPPORTED) {
        papp_log("SKIP yuv: this launcher has no DISP v5\n");
    } else {
        check_yuv("yuv bt601 limited", 100, 60, 0, 0, false);
        check_yuv("yuv bt601 limited, strides", 100, 60, 0, 0, true);
        check_yuv("yuv bt601 limited, odd addresses", 100, 60, 0, 0, 2);
        check_yuv("yuv bt601 full", 100, 60, 0, PAPP_YUV_FULL_RANGE, true);
        check_yuv("yuv bt709 limited", 100, 60, 0, PAPP_YUV_BT709, true);
        check_yuv("yuv bt709 full", 100, 60, 0, PAPP_YUV_BT709 | PAPP_YUV_FULL_RANGE, true);
        check_yuv("yuv scale 2x", 50, 40, 512, 0, true);
        check_yuv("yuv scale 1.5x", 60, 30, 384, 0, true);
        tests++;
        papp_yuv_t f = probe;
        f.dx = SW - FW + 2;
        if (papp_disp_blit_yuv(s, &f) != PAPP_E_ARG) { papp_log("FAIL yuv outside the surface accepted\n"); fails++; }
        else papp_log("ok   yuv outside the surface refused\n");
        yuv_bench(640, 360, 320);
        yuv_bench(1280, 720, 160);
    }

    const papp_if_codc_t *c = papp_codc();
    if (!c) papp_log("SKIP jpeg: this launcher has no CODC\n");
    else {
        jpeg_round_trip(c, "jpeg 128x96 q90", 128, 96, 90);
        jpeg_odd(c);
        jpeg_round_trip(c, "jpeg 640x360 q85", 640, 360, 85);
        jpeg_errors(c);
        jpeg_bench(c, 640, 360);
        jpeg_bench(c, 1280, 720);
        if (c->hdr.size >= sizeof(papp_if_codc_t)) dec_tests(c, s, &fails, &tests);
    }

    /* a test card to look at: the frame scaled 3x */
    paint_background();
    papp_yuv_t f = { .y = fy, .u = fu, .v = fv, .w = FW, .h = FH, .stride_y = SY, .stride_uv = SUV,
                     .dx = (SW - 3 * FW) / 2, .dy = (SH - 3 * FH) / 2, .scale_x = 768, .scale_y = 768 };
    papp_disp_blit_yuv(s, &f);
    papp_disp_present(s);
    papp_log("%d of %d tests passed\n", tests - fails, tests);
    for (int i = 0; i < secs * 10 && !papp_exit_requested(); i++) papp_delay_ms(100);
    papp_free(fy);
    papp_free(fu);
    papp_free(fv);
    papp_disp_surface_destroy(s);
    return fails;
}
