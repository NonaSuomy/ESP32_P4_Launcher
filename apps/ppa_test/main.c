/*
 * ppa_test — checks DISP v4 (papp_disp_blit / papp_disp_fill) against a CPU model.
 *
 * Each test paints a known background into the surface, lets the PPA draw, works out on the CPU
 * what every pixel of the test area should be, and compares. Blends allow a small rounding
 * difference. The PPA scales smoothly (bilinear), so scaled tests only check pixels whose source
 * neighbourhood is one colour, and skip the last line and column (the PPA may round them away).
 * Mirrors apply after the turn (to the drawn picture). Exit code = number of failed tests.
 *
 *   python papp-sdk/tools/papp_build.py --name ppa_test --src apps/ppa_test/main.c --libc
 *   python tools/papp_dev.py --port COM10 deploy build/apps/ppa_test/ppa_test.papp --timeout 30
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "papp/papp.h"

PAPP_MANIFEST(.id = "test.ppa", .name = "PPA test", .version = PAPP_APP_VERSION, .main_stack = 16384,
              .flags = PAPP_MF_HIDDEN);

#define SW 800
#define SH 480
#define AX 100                         /* the test area */
#define AY 60
#define AW 300
#define AH 300

static papp_surface_t *s;
static uint16_t *px;
static int stride;
static uint16_t bgcopy[AW * AH];       /* the background before the PPA drew */
static int fails, tests;

static uint16_t *src16;                /* source pictures (DMA memory) */
static uint32_t *src32;
static uint8_t *src8;
#define SRC_W 48
#define SRC_H 40

static uint16_t rgb(int r, int g, int b) { return PAPP_RGB565(r, g, b); }

static void paint_background(void)
{
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) px[y * stride + x] = rgb((x * 3) & 255, (y * 5) & 255, ((x + y) * 2) & 255);
    for (int y = 0; y < AH; y++) memcpy(&bgcopy[y * AW], &px[(AY + y) * stride + AX], AW * 2);
}

static uint16_t bg_at(int x, int y) { return bgcopy[(y - AY) * AW + (x - AX)]; }

/* 8x8 blocks of distinct colours, so a scaled block keeps its colour inside */
static uint16_t block_color(int bx, int by) { return rgb(40 + bx * 37 % 200, 30 + by * 53 % 220, 90 + (bx + by) * 29 % 160); }

static void make_sources(void)
{
    for (int y = 0; y < SRC_H; y++)
        for (int x = 0; x < SRC_W; x++) {
            uint16_t c = block_color(x / 8, y / 8);
            if (x == 3 && y == 5) c = rgb(250, 10, 10);        /* a marker pixel for the turns */
            src16[y * SRC_W + x] = c;
            int a = (x * 255) / (SRC_W - 1);                   /* alpha grows left to right */
            src32[y * SRC_W + x] = (uint32_t)a << 24 | (uint32_t)((c >> 11) << 3) << 16 |
                                   (uint32_t)(((c >> 5) & 63) << 2) << 8 | (uint32_t)((c & 31) << 3);
            src8[y * SRC_W + x] = (uint8_t)((y * 255) / (SRC_H - 1));
        }
    /* key-coloured holes in the RGB565 picture */
    for (int y = 10; y < 20; y++)
        for (int x = 20; x < 30; x++) src16[y * SRC_W + x] = rgb(255, 0, 255);
}

static uint16_t mix565(uint16_t bg, uint16_t fg, float a)
{
    int r = (int)(((bg >> 11) * (1 - a) + (fg >> 11) * a) + 0.5f);
    int g = (int)((((bg >> 5) & 63) * (1 - a) + ((fg >> 5) & 63) * a) + 0.5f);
    int b = (int)(((bg & 31) * (1 - a) + (fg & 31) * a) + 0.5f);
    return (uint16_t)(r << 11 | g << 5 | b);
}

static bool close565(uint16_t a, uint16_t b, int tol)
{
    int dr = (a >> 11) - (b >> 11), dg = ((a >> 5) & 63) - ((b >> 5) & 63), db = (a & 31) - (b & 31);
    return abs(dr) <= tol && abs(dg) <= tol * 2 && abs(db) <= tol;
}

/* ── The CPU model of a blit ── */

static int scaled(int n, int q) { return (q >> 4) * n + (q & 15) * n / 16; }

typedef struct { int x, y; bool skip; } srcpos_t;

static int src_stride(const papp_blit_t *b) { return b->stride ? b->stride : b->w; }

static uint16_t src_rgb(const papp_blit_t *b, int x, int y)     /* the source's colour, as RGB565 */
{
    int k = y * src_stride(b) + x;
    if (b->format == PAPP_BLIT_ARGB8888) {
        uint32_t c = src32[k];
        return rgb((c >> 16) & 255, (c >> 8) & 255, c & 255);
    }
    if (b->format == PAPP_BLIT_A8) return src8[k];
    return src16[k];
}

/* Where drawn pixel (i, j) comes from; skip = too close to a colour change of a scaled source,
 * or the rounded last line. Mirrors first (they flip the drawn picture), then the turn is undone. */
static srcpos_t source_of(const papp_blit_t *b, int i, int j)
{
    int qx = b->scale_x ? b->scale_x / 16 : 16, qy = b->scale_y ? b->scale_y / 16 : 16;
    int sw = scaled(b->w, qx), sh = scaled(b->h, qy);
    int dw = (b->rotate & 1) ? sh : sw, dh = (b->rotate & 1) ? sw : sh;
    if (b->flags & PAPP_BLIT_MIRROR_X) i = dw - 1 - i;
    if (b->flags & PAPP_BLIT_MIRROR_Y) j = dh - 1 - j;
    int u, v;
    switch (b->rotate & 3) {
    case 1: u = sw - 1 - j; v = i; break;
    case 2: u = sw - 1 - i; v = sh - 1 - j; break;
    case 3: u = j; v = sh - 1 - i; break;
    default: u = i; v = j; break;
    }
    srcpos_t p = { u * 16 / qx, v * 16 / qy, false };
    if (p.x >= b->w || p.y >= b->h) { p.skip = true; return p; }
    if (qx != 16 || qy != 16) {
        if (u >= sw - 2 || v >= sh - 2) p.skip = true;
        int rx = qx < 16 ? (16 + qx - 1) / qx + 1 : 1, ry = qy < 16 ? (16 + qy - 1) / qy + 1 : 1;
        uint16_t c = src_rgb(b, p.x, p.y);
        for (int y = p.y - ry; y <= p.y + ry && !p.skip; y++)
            for (int x = p.x - rx; x <= p.x + rx; x++) {
                int cx = x < 0 ? 0 : x >= b->w ? b->w - 1 : x, cy = y < 0 ? 0 : y >= b->h ? b->h - 1 : y;
                if (src_rgb(b, cx, cy) != c) { p.skip = true; break; }
            }
    }
    return p;
}

static void drawn_size(const papp_blit_t *b, int *w, int *h)
{
    int qx = b->scale_x ? b->scale_x / 16 : 16, qy = b->scale_y ? b->scale_y / 16 : 16;
    int sw = scaled(b->w, qx), sh = scaled(b->h, qy);
    *w = (b->rotate & 1) ? sh : sw;
    *h = (b->rotate & 1) ? sw : sh;
}

static bool expect(const papp_blit_t *b, int x, int y, uint16_t *out)    /* false: don't check */
{
    int w, h;
    drawn_size(b, &w, &h);
    uint16_t bg = bg_at(x, y);
    int i = x - b->x, j = y - b->y;
    if (i < 0 || j < 0 || i >= w || j >= h) { *out = bg; return true; }
    srcpos_t p = source_of(b, i, j);
    if (p.skip) return false;
    int k = p.y * src_stride(b) + p.x;
    float ga = (b->flags & PAPP_BLIT_ALPHA) ? b->alpha / 255.0f : 1;
    if (b->format == PAPP_BLIT_RGB565) {
        uint16_t c = src16[k];
        if ((b->flags & PAPP_BLIT_COLORKEY) && c == b->key) { *out = bg; return true; }
        *out = ga >= 1 ? c : mix565(bg, c, ga);
    } else if (b->format == PAPP_BLIT_ARGB8888) {
        uint32_t c = src32[k];
        uint16_t c565 = rgb((c >> 16) & 255, (c >> 8) & 255, c & 255);
        if ((b->flags & PAPP_BLIT_COLORKEY) && c565 == b->key) { *out = bg; return true; }
        *out = mix565(bg, c565, (c >> 24) / 255.0f * ga);
    } else {
        *out = mix565(bg, b->color, src8[k] / 255.0f * ga);
    }
    return true;
}

static void check(const char *name, const papp_blit_t *b, int want_rc, int tol)
{
    tests++;
    paint_background();
    int rc = papp_disp_blit(s, b);
    if (rc != want_rc) {
        papp_log("FAIL %-28s returned %d, want %d\n", name, rc, want_rc);
        fails++;
        return;
    }
    if (rc != PAPP_E_OK) {
        papp_log("PASS %-28s (returned %d)\n", name, rc);
        return;
    }
    int bad = 0, checked = 0, fx = 0, fy = 0;
    uint16_t got0 = 0, want0 = 0;
    for (int y = AY; y < AY + AH; y++)
        for (int x = AX; x < AX + AW; x++) {
            uint16_t want, got = px[y * stride + x];
            if (!expect(b, x, y, &want)) continue;
            checked++;
            if (!close565(got, want, tol)) {
                if (!bad++) { fx = x; fy = y; got0 = got; want0 = want; }
            }
        }
    if (bad) {
        papp_log("FAIL %-28s %d of %d pixels, first (%d,%d) got %04X want %04X\n", name, bad, checked,
                 fx - b->x, fy - b->y, got0, want0);
        fails++;
    } else {
        papp_log("PASS %-28s %d pixels\n", name, checked);
    }
}

static void check_fill(const char *name, int x, int y, int w, int h, uint16_t c)
{
    tests++;
    paint_background();
    int rc = papp_disp_fill(s, x, y, w, h, c);
    int bad = 0, checked = 0;
    for (int yy = AY; yy < AY + AH; yy++)
        for (int xx = AX; xx < AX + AW; xx++) {
            bool in = xx >= x && xx < x + w && yy >= y && yy < y + h;
            uint16_t want = in ? c : bg_at(xx, yy);
            checked++;
            if (px[yy * stride + xx] != want) bad++;
        }
    if (rc || bad) {
        papp_log("FAIL %-28s rc %d, %d of %d pixels\n", name, rc, bad, checked);
        fails++;
    } else {
        papp_log("PASS %-28s %d pixels\n", name, checked);
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    papp_surface_cfg_t cfg = { .width = SW, .height = SH, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
    s = papp_disp_surface_create(&cfg);
    src16 = papp_alloc(SRC_W * SRC_H * 2, PAPP_MEM_DMA, 64);
    src32 = papp_alloc(SRC_W * SRC_H * 4, PAPP_MEM_DMA, 64);
    src8 = papp_alloc(SRC_W * SRC_H, PAPP_MEM_DMA, 64);
    if (!s || !src16 || !src32 || !src8) return 100;
    px = papp_disp_lock(s, &stride);
    make_sources();

    papp_blit_t base = { .pixels = src16, .w = SRC_W, .h = SRC_H, .format = PAPP_BLIT_RGB565, .x = AX + 30, .y = AY + 20 };
    if (papp_disp_blit(s, &base) == PAPP_E_UNSUPPORTED) {
        papp_log("SKIP: this launcher has no DISP v4\n");
        return 0;
    }
    papp_blit_t b;
    const uint16_t KEY = rgb(255, 0, 255);

    /* fill */
    check_fill("fill partial", AX + 13, AY + 7, 50, 21, rgb(10, 200, 30));
    check_fill("fill odd size/offset", AX + 1, AY + 1, 1, 3, rgb(200, 10, 30));
    check_fill("fill clipped left/top", -20, -10, AX + 40, AY + 30, rgb(0, 0, 255));

    /* RGB565 */
    b = base; check("rgb565 copy", &b, 0, 0);
    b = base; b.flags = PAPP_BLIT_COLORKEY; b.key = KEY; check("rgb565 colour key", &b, 0, 0);
    b = base; b.flags = PAPP_BLIT_ALPHA; b.alpha = 128; check("rgb565 alpha 128", &b, 0, 1);
    b = base; b.flags = PAPP_BLIT_ALPHA; b.alpha = 40; check("rgb565 alpha 40", &b, 0, 1);
    b = base; b.flags = PAPP_BLIT_ALPHA | PAPP_BLIT_COLORKEY; b.key = KEY; b.alpha = 200;
    check("rgb565 key + alpha 200", &b, 0, 1);
    b = base; b.x = AX - 10; b.y = AY - 6; b.pixels = src16; check("rgb565 clipped", &b, 0, 0);
    b = base; b.w = 20; b.h = 15; b.stride = SRC_W; check("rgb565 stride", &b, 0, 0);

    /* ARGB8888 */
    b = base; b.pixels = src32; b.format = PAPP_BLIT_ARGB8888; check("argb8888", &b, 0, 1);
    b = base; b.pixels = src32; b.format = PAPP_BLIT_ARGB8888; b.flags = PAPP_BLIT_ALPHA; b.alpha = 100;
    check("argb8888 + alpha 100", &b, 0, 1);

    /* A8 */
    b = base; b.pixels = src8; b.format = PAPP_BLIT_A8; b.color = rgb(255, 220, 0); check("a8 in yellow", &b, 0, 1);
    b = base; b.pixels = src8; b.format = PAPP_BLIT_A8; b.color = rgb(0, 255, 255); b.flags = PAPP_BLIT_ALPHA;
    b.alpha = 128; check("a8 + alpha 128", &b, 0, 1);
    b = base; b.pixels = src8; b.format = PAPP_BLIT_A8; b.rotate = 1; check("a8 turned (refused)", &b, PAPP_E_ARG, 0);

    /* mirrors and turns */
    b = base; b.flags = PAPP_BLIT_MIRROR_X; check("mirror x", &b, 0, 0);
    b = base; b.flags = PAPP_BLIT_MIRROR_Y; check("mirror y", &b, 0, 0);
    b = base; b.flags = PAPP_BLIT_MIRROR_X | PAPP_BLIT_MIRROR_Y; check("mirror x+y", &b, 0, 0);
    for (int r = 1; r < 4; r++) {
        char n[32];
        snprintf(n, sizeof(n), "turn %d", r * 90);
        b = base; b.rotate = (uint8_t)r; check(n, &b, 0, 0);
    }
    b = base; b.rotate = 1; b.flags = PAPP_BLIT_MIRROR_X; check("turn 90 + mirror x", &b, 0, 0);
    b = base; b.rotate = 1; b.flags = PAPP_BLIT_COLORKEY; b.key = KEY; check("turn 90 + key", &b, 0, 0);
    b = base; b.pixels = src32; b.format = PAPP_BLIT_ARGB8888; b.rotate = 3; check("argb8888 turn 270", &b, 0, 1);

    /* scaling */
    b = base; b.scale_x = b.scale_y = 512; check("scale 2x", &b, 0, 0);
    b = base; b.scale_x = b.scale_y = 128; check("scale 0.5x", &b, 0, 0);
    b = base; b.scale_x = 512; b.scale_y = 256; check("scale 2x by 1x", &b, 0, 0);
    b = base; b.scale_x = 384; b.scale_y = 768; b.rotate = 1; check("scale 1.5x3 + turn 90", &b, 0, 0);
    b = base; b.scale_x = b.scale_y = 400; b.flags = PAPP_BLIT_COLORKEY; b.key = KEY; check("scale 1.5625x + key", &b, 0, 0);
    b = base; b.pixels = src32; b.format = PAPP_BLIT_ARGB8888; b.scale_x = b.scale_y = 512; check("argb8888 scale 2x", &b, 0, 1);
    b = base; b.scale_x = b.scale_y = 512; b.x = AX - 40; b.y = AY - 30; check("scale 2x clipped", &b, 0, 0);

    papp_disp_present(s);
    papp_log("%d of %d tests passed\n", tests - fails, tests);
    papp_free(src16);
    papp_free(src32);
    papp_free(src8);
    papp_disp_surface_destroy(s);
    return fails;
}
