/*
 * Sprite Demo (education): fly a spaceship sprite over a space background with the pad, and see
 * how a sprite gets onto the screen. The PPA (the P4's picture engine) does the work:
 *
 *   Key on   The sprite is a 64x96 RGB565 picture: the ship painted on magenta, the "key colour"
 *            (a colour the ship never uses). The PPA leaves the magenta pixels out.
 *   Key off  The PPA copies the whole rectangle, magenta and all.
 *   Alpha    The sprite is ARGB8888: every pixel has its own alpha (0 = see-through, 255 = solid),
 *            so the edges are soft and a blue glow and the flame mix with the space behind.
 *
 * The PPA also turns the ship to face where it flies (quarter turns; flying down it is mirrored
 * upside down instead, so the light stays on its left side), scales it (L1 / R1, in steps of
 * 1/16) and clears the screen (a PPA fill). The zoom box shows the pixels 3x bigger.
 * Scaling is smooth: it mixes neighbouring pixels, so with the colour key the ship's edge pixels
 * get some magenta in them, are no longer exactly the key colour, and show as a pink fringe.
 * The alpha sprite has no such problem: that's why scaled sprites use alpha.
 *
 * Pad / stick: move.  L1 / R1: size.  A or the buttons: sprite mode.  X or close: exit.
 * On a launcher without the PPA calls (0.29.0 and older) the CPU does the same work, slower.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "edu_ui.h"

PAPP_MANIFEST(.id = "edu.sprite", .name = "Sprite Demo", .version = PAPP_APP_VERSION, .main_stack = 16384);

/* Playfield and side panel (in the 800x480 surface) */
#define PF_X 20
#define PF_Y 112
#define PF_W 540
#define PF_H 352
#define SIDE_X 576
#define SIDE_W 204

#define SW 64                          /* the sprite: ship + flame */
#define SH 96
#define FLAME_Y 54                     /* the flame starts at this line of the sprite */
#define KEY PAPP_RGB565(255, 0, 255)   /* the key colour: magenta */

#define ZOOM_SRC 36                    /* zoom box: 36x36 pixels, 3x */
#define ZOOM 3
#define ZOOM_X (SIDE_X + (SIDE_W - ZOOM_SRC * ZOOM) / 2)
#define ZOOM_Y 346

enum { MODE_KEY_ON, MODE_KEY_OFF, MODE_ALPHA, MODES };
static const char *const mode_name[MODES] = { "Key on", "Key off", "Alpha" };
static const char *const mode_text[MODES] = { "Magenta is left out: only the ship",
                                              "The whole rectangle, magenta too",
                                              "Alpha per pixel: soft edges, glow" };
enum { ID_MODE = 1 };                  /* ID_MODE + mode */

static uint16_t s_ship16[SW * SH];     /* the ship on magenta (RGB565), drawn once */
static uint32_t s_ship32[SW * SH];     /* the ship with soft edges and a glow (ARGB8888) */
static uint16_t *s_spr16;              /* this frame's sprites (DMA memory for the PPA) */
static uint32_t *s_spr32;
static uint16_t *s_bg;                 /* PF_W x PF_H, drawn once */

static int s_mode = MODE_KEY_ON;
static int s_size = 16;                /* sixteenths: 16 = 1x */
static bool s_cpu;                     /* no PPA calls in this launcher: the CPU does the work */
static bool s_quit;
static uint32_t s_rng = 2463534242u;

typedef struct { int16_t x, y; uint8_t size; float phase; } star_t;
static star_t s_star[70];

static int rnd(int n)                  /* 0 .. n-1 (xorshift) */
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (int)(s_rng % (uint32_t)n);
}

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint16_t rgbf(float r, float g, float b)
{
    return PAPP_RGB565((int)(clampf(r, 0, 1) * 255), (int)(clampf(g, 0, 1) * 255), (int)(clampf(b, 0, 1) * 255));
}

static uint32_t argb(float a, float r, float g, float b)    /* 0xAARRGGBB */
{
    return (uint32_t)(clampf(a, 0, 1) * 255 + 0.5f) << 24 | (uint32_t)(clampf(r, 0, 1) * 255 + 0.5f) << 16 |
           (uint32_t)(clampf(g, 0, 1) * 255 + 0.5f) << 8 | (uint32_t)(clampf(b, 0, 1) * 255 + 0.5f);
}

static uint16_t argb_to_565(uint32_t c) { return PAPP_RGB565((c >> 16) & 255, (c >> 8) & 255, c & 255); }

/* ── The ship ──────────────────────────────────────────────────────────── */

static bool in_tri(float x, float y, float ax, float ay, float bx, float by, float cx, float cy)
{
    float d1 = (x - bx) * (ay - by) - (ax - bx) * (y - by);
    float d2 = (x - cx) * (by - cy) - (bx - cx) * (y - cy);
    float d3 = (x - ax) * (cy - ay) - (cx - ax) * (y - ay);
    bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

static uint32_t ship_color(float x, float y)        /* 0xRRGGBB, or 0xFF000000 = not the ship */
{
    float dx = (x - 32) / 4.5f, dy = (y - 23) / 8.5f;
    if (dx * dx + dy * dy <= 1) return (x < 31 && y < 21) ? 0xA8F0FF : 0x40C8F2;   /* cockpit */
    if (in_tri(x, y, 32, 3, 23, 50, 41, 50)) return x < 32 ? 0xE6ECF4 : 0xAEBACB;  /* hull, lit from the left */
    if (x >= 25 && x <= 39 && y >= 48 && y <= 54 && !(x > 31 && x < 33)) return 0x4A5468;   /* nozzles */
    if (in_tri(x, y, 32, 17, 5, 47, 59, 47) && y <= 47) {                          /* wings, red tips */
        if (x < 13 || x > 51) return 0xD9453B;
        return y < 36 ? 0x4A6FA5 : 0x34507E;
    }
    return 0xFF000000u;
}

static void make_ships(void)
{
    static float cov[SW * SH], tmp[SW * SH], pr[SW * SH], pg[SW * SH], pb[SW * SH];
    for (int y = 0; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            uint32_t c = ship_color(x + 0.5f, y + 0.5f);              /* hard edges: one sample */
            s_ship16[y * SW + x] = c >> 24 ? KEY : UI_RGB(c);
            float a = 0, r = 0, g = 0, b = 0;                         /* soft edges: 4x4 samples */
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) {
                    uint32_t s = ship_color(x + (i + 0.5f) / 4, y + (j + 0.5f) / 4);
                    if (s >> 24) continue;
                    a += 1; r += (s >> 16 & 255) / 255.0f; g += (s >> 8 & 255) / 255.0f; b += (s & 255) / 255.0f;
                }
            int k = y * SW + x;
            cov[k] = a / 16; pr[k] = r / 16; pg[k] = g / 16; pb[k] = b / 16;   /* premultiplied */
        }
    const int R = 5;                       /* glow: the shape blurred both ways */
    for (int pass = 0; pass < 2; pass++)
        for (int y = 0; y < SH; y++)
            for (int x = 0; x < SW; x++) {
                float s = 0;
                for (int k = -R; k <= R; k++) {
                    int xx = pass ? x : x + k, yy = pass ? y + k : y;
                    if (xx >= 0 && xx < SW && yy >= 0 && yy < SH) s += (pass ? tmp : cov)[yy * SW + xx];
                }
                (pass ? cov : tmp)[y * SW + x] = s / (2 * R + 1);
            }
    for (int y = 0; y < SH; y++)                     /* the ship over its glow */
        for (int x = 0; x < SW; x++) {
            int k = y * SW + x;
            /* coverage of the ship itself (cov now holds the glow, so sample again) */
            float a = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++) a += !(ship_color(x + (i + 0.5f) / 4, y + (j + 0.5f) / 4) >> 24);
            a /= 16;
            float ga = clampf(cov[k] * 1.6f, 0, 1) * 0.7f * (1 - a);   /* glow under the ship */
            float oa = a + ga;
            if (oa <= 0.004f) { s_ship32[k] = 0; continue; }
            s_ship32[k] = argb(oa, (pr[k] + 0.37f * ga) / oa, (pg[k] + 0.66f * ga) / oa, (pb[k] + 1.0f * ga) / oa);
        }
}

/* This frame's sprites: the ship, then the flame (len = how far it reaches) where it is empty */
static void make_sprites(float len)
{
    memcpy(s_spr16, s_ship16, sizeof(s_ship16));
    memcpy(s_spr32, s_ship32, sizeof(s_ship32));
    for (int y = FLAME_Y; y < SH; y++)
        for (int x = 0; x < SW; x++) {
            float t = (y - FLAME_Y + 0.5f) / len;             /* 0 at the nozzles .. 1 at the tip */
            if (t >= 1) continue;
            float half = 7.0f * powf(1 - t, 0.6f) + 0.01f;
            float d = fabsf(x + 0.5f - SW / 2.0f) / half;      /* 0 centre .. 1 edge */
            float heat = clampf(1 - (d * 0.8f + t * 0.9f), 0, 1);   /* white core -> yellow -> red */
            float r = 1, g = 0.35f + 0.65f * heat, b = 0.1f + 0.9f * heat * heat;
            int k = y * SW + x;
            if (d < 1 && s_spr16[k] == KEY) s_spr16[k] = rgbf(r, g, b);
            float fa = clampf((1 - d) * 1.6f, 0, 1) * (1 - t);     /* soft flame, under the ship */
            uint32_t s = s_spr32[k];
            float sa = (s >> 24) / 255.0f;
            float oa = sa + fa * (1 - sa);
            if (oa <= 0.004f) continue;
            float k2 = fa * (1 - sa);
            s_spr32[k] = argb(oa, ((s >> 16 & 255) / 255.0f * sa + r * k2) / oa,
                              ((s >> 8 & 255) / 255.0f * sa + g * k2) / oa, ((s & 255) / 255.0f * sa + b * k2) / oa);
        }
}

/* ── Background: space, two nebulae, a planet and a moon (drawn once) ─── */

static void make_background(void)
{
    s_bg = malloc(PF_W * PF_H * sizeof(uint16_t));
    if (!s_bg) return;
    const float pcx = 440, pcy = 300, pr = 120;          /* planet */
    const float mcx = 110, mcy = 270, mr = 26;           /* moon */
    for (int y = 0; y < PF_H; y++)
        for (int x = 0; x < PF_W; x++) {
            float fy = (float)y / PF_H;
            float r = 0.02f + 0.03f * fy, g = 0.03f + 0.04f * fy, b = 0.10f + 0.12f * fy;
            float n1 = expf(-((x - 150.0f) * (x - 150.0f) + (y - 90.0f) * (y - 90.0f)) / 9000.0f);
            float n2 = expf(-((x - 330.0f) * (x - 330.0f) + (y - 60.0f) * (y - 60.0f)) / 5000.0f);
            r += 0.35f * n1 + 0.10f * n2;
            g += 0.05f * n1 + 0.22f * n2;
            b += 0.30f * n1 + 0.30f * n2;

            float dx = x + 0.5f - pcx, dy = y + 0.5f - pcy, d = sqrtf(dx * dx + dy * dy);
            if (d < pr + 8) {                             /* atmosphere, then the planet */
                float glow = clampf(1 - fabsf(d - pr) / 8, 0, 1) * 0.5f;
                r += 0.2f * glow; g += 0.5f * glow; b += 0.9f * glow;
                float c = clampf(pr - d + 0.5f, 0, 1);
                if (c > 0) {
                    float nx = dx / pr, ny = dy / pr, nz = sqrtf(clampf(1 - nx * nx - ny * ny, 0, 1));
                    float lit = clampf(-0.6f * nx - 0.5f * ny + 0.62f * nz, 0, 1);
                    float band = 0.5f + 0.5f * sinf(ny * 14 + nx * 2);
                    float pr_ = 0.05f + 0.20f * lit, pg = 0.12f + (0.30f + 0.08f * band) * lit,
                          pb = 0.25f + 0.55f * lit;
                    r = r * (1 - c) + pr_ * c; g = g * (1 - c) + pg * c; b = b * (1 - c) + pb * c;
                }
            }
            dx = x + 0.5f - mcx; dy = y + 0.5f - mcy; d = sqrtf(dx * dx + dy * dy);
            float c = clampf(mr - d + 0.5f, 0, 1);
            if (c > 0) {
                float lit = clampf(0.4f - 0.6f * dx / mr - 0.4f * dy / mr, 0.08f, 1);
                r = r * (1 - c) + 0.75f * lit * c; g = g * (1 - c) + 0.72f * lit * c; b = b * (1 - c) + 0.70f * lit * c;
            }
            s_bg[y * PF_W + x] = rgbf(r, g, b);
        }

    for (int i = 0; i < (int)(sizeof(s_star) / sizeof(s_star[0])); i++) {
        int x, y;
        do {                                              /* not on the planet or the moon */
            x = rnd(PF_W - 1);
            y = rnd(PF_H - 1);
        } while ((x - pcx) * (x - pcx) + (y - pcy) * (y - pcy) < (pr + 10) * (pr + 10) ||
                 (x - mcx) * (x - mcx) + (y - mcy) * (y - mcy) < (mr + 4) * (mr + 4));
        s_star[i] = (star_t){ (int16_t)x, (int16_t)y, (uint8_t)(rnd(6) == 0 ? 2 : 1), rnd(628) / 100.0f };
    }
}

/* ── Drawing ───────────────────────────────────────────────────────────── */

typedef struct { int x, y, w, h; } rect_t;

/* Size of the drawn picture: scaled in sixteenths (as the PPA works it out), then turned */
static void drawn_size(int q, int rotate, int *w, int *h)
{
    int sw = (q >> 4) * SW + (q & 15) * SW / 16, sh = (q >> 4) * SH + (q & 15) * SH / 16;
    *w = (rotate & 1) ? sh : sw;
    *h = (rotate & 1) ? sw : sh;
}

/*
 * What the PPA does, done by the CPU (launchers without papp_disp_blit): for each pixel of the
 * drawn picture, find the sprite pixel it comes from (undo the mirror, the turn and the scale),
 * then leave it out (key), mix it in (alpha) or copy it. The CPU takes the nearest pixel; the
 * PPA scales smoothly, mixing neighbouring pixels.
 */
static void cpu_blit(uint16_t *px, int stride, const papp_blit_t *b)
{
    int q = b->scale_x / 16, sw, sh, tw, th;
    drawn_size(q, 0, &sw, &sh);
    drawn_size(q, b->rotate, &tw, &th);
    for (int j = 0; j < th; j++) {
        int y = b->y + j;
        if (y < 0 || y >= UI_H) continue;
        for (int i = 0; i < tw; i++) {
            int x = b->x + i;
            if (x < 0 || x >= UI_W) continue;
            int mi = (b->flags & PAPP_BLIT_MIRROR_X) ? tw - 1 - i : i;   /* undo the mirror (it flips */
            int mj = (b->flags & PAPP_BLIT_MIRROR_Y) ? th - 1 - j : j;   /* the turned picture)      */
            int u, v;                                       /* in the scaled, unturned picture */
            switch (b->rotate) {
            case 1: u = sw - 1 - mj; v = mi; break;         /* 90 degrees counter-clockwise */
            case 2: u = sw - 1 - mi; v = sh - 1 - mj; break;
            case 3: u = mj; v = sh - 1 - mi; break;
            default: u = mi; v = mj; break;
            }
            int k = (v * 16 / q) * SW + u * 16 / q;          /* the sprite pixel */
            uint16_t *d = &px[y * stride + x];
            if (b->format == PAPP_BLIT_ARGB8888) {
                uint32_t c = ((const uint32_t *)b->pixels)[k];
                int a = c >> 24;
                if (a) *d = ui_mix(*d, argb_to_565(c), a + (a >> 7));
            } else {
                uint16_t c = ((const uint16_t *)b->pixels)[k];
                if (!(b->flags & PAPP_BLIT_COLORKEY) || c != b->key) *d = c;
            }
        }
    }
}

/* Puts the sprite on the screen, turned and scaled, with its top left at (x, y) */
static void draw_sprite(papp_surface_t *s, uint16_t *px, int stride, int x, int y, int rotate, bool flip)
{
    papp_blit_t b = {
        .pixels = s_mode == MODE_ALPHA ? (const void *)s_spr32 : (const void *)s_spr16,
        .w = SW, .h = SH,
        .format = s_mode == MODE_ALPHA ? PAPP_BLIT_ARGB8888 : PAPP_BLIT_RGB565,
        .flags = (uint8_t)((s_mode == MODE_KEY_ON ? PAPP_BLIT_COLORKEY : 0) | (flip ? PAPP_BLIT_MIRROR_Y : 0)),
        .x = (int16_t)x, .y = (int16_t)y,
        .key = KEY,
        .rotate = (uint8_t)rotate,
        .scale_x = (uint16_t)(s_size * 16), .scale_y = (uint16_t)(s_size * 16),
    };
    if (!s_cpu && papp_disp_blit(s, &b) == PAPP_E_OK) return;
    s_cpu = true;                                         /* an older launcher: do it ourselves */
    cpu_blit(px, stride, &b);
}

/* Copies the background back over r: rubs out the ship drawn there before */
static void restore(uint16_t *px, int stride, rect_t r)
{
    for (int j = 0; j < r.h; j++)
        memcpy(&px[(r.y + j) * stride + r.x], &s_bg[(r.y - PF_Y + j) * PF_W + r.x - PF_X], r.w * 2);
}

static void draw_stars(uint16_t *px, int stride)          /* twinkling: drawn from the background */
{
    float t = papp_time_us() / 1e6f;
    for (int i = 0; i < (int)(sizeof(s_star) / sizeof(s_star[0])); i++) {
        const star_t *st = &s_star[i];
        int b = 120 + (int)(120 * sinf(t * 2.2f + st->phase));
        for (int k = 0; k < st->size * st->size; k++) {
            int x = st->x + k % 2, y = st->y + k / 2;
            px[(PF_Y + y) * stride + PF_X + x] = ui_mix(s_bg[y * PF_W + x], C_TEXT, b);
        }
    }
}

/* The zoom box: the pixels around (cx, cy), ZOOM times bigger */
static void draw_zoom(uint16_t *px, int stride, int cx, int cy)
{
    int x0 = cx - ZOOM_SRC / 2, y0 = cy - ZOOM_SRC / 2;
    x0 = x0 < PF_X ? PF_X : x0 > PF_X + PF_W - ZOOM_SRC ? PF_X + PF_W - ZOOM_SRC : x0;
    y0 = y0 < PF_Y ? PF_Y : y0 > PF_Y + PF_H - ZOOM_SRC ? PF_Y + PF_H - ZOOM_SRC : y0;
    for (int j = 0; j < ZOOM_SRC * ZOOM; j++) {
        const uint16_t *src = &px[(y0 + j / ZOOM) * stride + x0];
        uint16_t *d = &px[(ZOOM_Y + j) * stride + ZOOM_X];
        for (int i = 0; i < ZOOM_SRC * ZOOM; i++) d[i] = src[i / ZOOM];
    }
}

#define RADIO_Y(i) (168 + (i) * 38)

static void radio(int i)
{
    int cy = RADIO_Y(i), id = ID_MODE + i;
    bool on = s_mode == i;
    ui_hit(SIDE_X, cy - 19, SIDE_W, 38, id, UI_TAP);
    ui_disc(SIDE_X + 36, cy, 13, ui_pressed_color(id, on ? C_BLUE : C_BORDER), 255);
    ui_disc(SIDE_X + 36, cy, 10, C_PANEL, 255);
    if (on) ui_disc(SIDE_X + 36, cy, 6, C_BLUE, 255);
    ui_text_l(ui_med, SIDE_X + 62, cy, mode_name[i], on ? C_TEXT : C_DIM);
}

/* Everything that only changes with the mode, the size or a touched button */
static void draw_static(papp_surface_t *s, uint16_t *px, int stride)
{
    if (s_cpu || papp_disp_fill(s, 0, 0, UI_W, UI_H, C_BG) != PAPP_E_OK) ui_fill(0, 0, UI_W, UI_H, C_BG);
    char status[96];
    snprintf(status, sizeof(status), "Pad: move     L1 / R1: size %d.%02dx     A: mode     X: exit", s_size / 16,
             s_size % 16 * 100 / 16);
    ui_bar("Sprite Demo", NULL, 0, 0, status, s_mode == MODE_KEY_OFF ? C_ORANGE : C_GREEN, false);
    ui_text_r(ui_small, 712, 32, s_cpu ? "drawn by the CPU" : "drawn by the PPA", C_DIM);
    if (s_bg) restore(px, stride, (rect_t){ PF_X, PF_Y, PF_W, PF_H });
    else ui_fill(PF_X, PF_Y, PF_W, PF_H, UI_RGB(0x050A1A));

    ui_rrect(SIDE_X, PF_Y, SIDE_W, PF_H, 18, C_PANEL);
    ui_text_l(ui_small, SIDE_X + 20, 136, "SPRITE", C_FAINT);
    for (int i = 0; i < MODES; i++) radio(i);
    const char *text = mode_text[s_mode];
    if (s_mode == MODE_KEY_ON && s_size != 16 && !s_cpu) text = "Scaled smoothly: the edges turn pink";
    ui_text_wrap(ui_small, SIDE_X + 20, 264, SIDE_W - 32, text, C_DIM, 2);
    ui_text_l(ui_small, SIDE_X + 20, 326, "ZOOM 3x", C_FAINT);
    ui_rrect(ZOOM_X - 4, ZOOM_Y - 4, ZOOM_SRC * ZOOM + 8, ZOOM_SRC * ZOOM + 8, 6, C_BORDER);
}

/*
 * Drawing only what changed ("dirty rectangles"): redrawing the whole 800x480 screen every frame
 * is slow, and most of it stays the same. So each frame puts the background back where the ship
 * was, then draws the ship at its new place. The display has two buffers that take turns (one is
 * shown while the other is drawn), so each buffer remembers where its own ship was.
 */
typedef struct { uint16_t *px; int look; rect_t ship; } buffer_t;
static buffer_t s_buf[2];

static void draw(papp_surface_t *s, float cx, float cy, int rotate, bool flip, float flame)
{
    int look = s_mode | s_size << 2 | (s_cpu ? 1 << 10 : 0) | (ui_pressed_color(UI_ID_CLOSE, 0) ? 1 << 11 : 0);
    for (int i = 0; i < MODES; i++)
        if (ui_pressed_color(ID_MODE + i, 0)) look |= 1 << (12 + i);
    ui_begin_keep(s);
    int stride;
    uint16_t *px = ui_pixels(&stride);
    buffer_t *b = s_buf[0].px == px ? &s_buf[0] : s_buf[1].px == px ? &s_buf[1] : NULL;
    if (!b) {
        b = s_buf[0].px ? &s_buf[1] : &s_buf[0];
        b->px = px;
        b->look = -1;
    }

    if (b->look != look || !s_bg) {
        draw_static(s, px, stride);                             /* also sets the touch targets */
        b->look = look;
    } else {
        ui_hit(728, 0, 72, 64, UI_ID_CLOSE, UI_TAP);            /* the same touch targets */
        for (int i = 0; i < MODES; i++) ui_hit(SIDE_X, RADIO_Y(i) - 19, SIDE_W, 38, ID_MODE + i, UI_TAP);
        restore(px, stride, b->ship);                           /* rub out the old ship */
    }

    if (s_bg) draw_stars(px, stride);
    int w, h;
    drawn_size(s_size, rotate, &w, &h);
    int x = (int)lroundf(cx) - w / 2, y = (int)lroundf(cy) - h / 2;
    make_sprites(flame);
    draw_sprite(s, px, stride, x, y, rotate, flip);             /* after the CPU's drawing under it */
    b->ship = (rect_t){ x, y, w, h };                           /* what to rub out next time */

    draw_zoom(px, stride, x + w / 2, y + h / 2);
    ui_end(s);
}

static void act(int id, int x)
{
    (void)x;
    if (id >= ID_MODE && id < ID_MODE + MODES) s_mode = id - ID_MODE;
    else if (id == UI_ID_CLOSE) s_quit = true;
}

static float axis(int16_t v)                      /* stick -> -1..1 with a dead zone */
{
    const float dz = 8000;
    if (v > dz) return (v - dz) / (32767 - dz);
    if (v < -dz) return (v + dz) / (32768 - dz);
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    papp_surface_t *s = ui_open();
    if (!s) return 1;
    s_spr16 = papp_alloc(sizeof(s_ship16), PAPP_MEM_DMA, 64);
    s_spr32 = papp_alloc(sizeof(s_ship32), PAPP_MEM_DMA, 64);
    if (!s_spr16 || !s_spr32) return 1;
    s_rng ^= (uint32_t)papp_time_us();
    make_ships();
    make_background();

    const float speed = 360;                      /* pixels per second */
    float x = PF_X + 180, y = PF_Y + PF_H - 80, vx = 0, vy = 0;
    int rotate = 0;                               /* where the ship faces: 0 up, 1 left, 3 right */
    bool flip = false;                            /* down: mirrored upside down */
    uint32_t prev = 0xFFFFFFFFu;                  /* nothing counts as pressed until released once */
    int64_t last = papp_time_us(), size_at = 0;
    while (!papp_exit_requested() && !s_quit) {
        int64_t now = papp_time_us();
        float dt = clampf((now - last) / 1e6f, 0, 0.1f);   /* seconds since the last frame */
        last = now;

        papp_gamepad_t gp = { 0 };
        papp_input_gamepad(0, &gp);
        uint32_t press = gp.buttons & ~prev;
        prev = gp.buttons;
        if (press & PAPP_BTN_X) break;
        if (press & PAPP_BTN_A) s_mode = (s_mode + 1) % MODES;
        if ((gp.buttons & (PAPP_BTN_L1 | PAPP_BTN_R1)) && now >= size_at) {   /* size: 1/2x .. 2x */
            if (gp.buttons & PAPP_BTN_L1 && s_size > 8) s_size--;
            if (gp.buttons & PAPP_BTN_R1 && s_size < 32) s_size++;
            size_at = now + ((press & (PAPP_BTN_L1 | PAPP_BTN_R1)) ? 300000 : 60000);
        }

        float tx = axis(gp.lx), ty = axis(gp.ly);
        if (gp.buttons & PAPP_BTN_LEFT) tx = -1;
        if (gp.buttons & PAPP_BTN_RIGHT) tx = 1;
        if (gp.buttons & PAPP_BTN_UP) ty = -1;
        if (gp.buttons & PAPP_BTN_DOWN) ty = 1;
        float m = sqrtf(tx * tx + ty * ty);
        if (m > 1) { tx /= m; ty /= m; }
        if (m > 0.3f) {                           /* face the way the stick points */
            if (fabsf(tx) > fabsf(ty)) { rotate = tx < 0 ? 1 : 3; flip = false; }
            else { rotate = 0; flip = ty > 0; }
        }
        float k = 1 - expf(-dt * 10);             /* a little inertia */
        vx += (tx * speed - vx) * k;
        vy += (ty * speed - vy) * k;

        int w, h;                                 /* keep the whole ship in the playfield */
        drawn_size(s_size, rotate, &w, &h);
        x = clampf(x + vx * dt, PF_X + w / 2 + 1, PF_X + PF_W - w / 2 - 1);
        y = clampf(y + vy * dt, PF_Y + h / 2 + 1, PF_Y + PF_H - h / 2 - 1);

        float thrust = clampf(sqrtf(vx * vx + vy * vy) / speed, 0, 1);
        float flame = 10 + 28 * thrust + rnd(5);  /* at most 42: fits in the sprite */

        ui_touch(s, act);
        draw(s, x, y, rotate, flip, flame);
    }
    free(s_bg);
    papp_free(s_spr16);
    papp_free(s_spr32);
    papp_disp_surface_destroy(s);
    return 0;
}
