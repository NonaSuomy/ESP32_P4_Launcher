/*
 * Physics Playground (education): balls that fall, bounce and knock into each other.
 *
 * Each ball has a position and a velocity. Many times a second (240) the program:
 *   1. adds gravity to every velocity (falling speeds up),
 *   2. moves every ball by its velocity,
 *   3. finds balls that overlap a wall or each other and bounces them: the speed along the
 *      line between them flips, times the bounciness (100% = no energy lost).
 * Energy = movement (1/2 m v^2) + height (m g h). With 100% bounce it stays about the same: the
 * bar only swaps between the two. Less bounce turns some into heat at every hit, and the
 * balls come to rest. Big balls are heavier (mass grows with the area) and push small ones.
 *
 * Touch an empty spot to drop a ball there; drag a ball and let go to throw it (several
 * fingers: several balls). The balls are drawn by the PPA, the P4's picture engine: one
 * shaded ball picture per colour, scaled to each size.
 * Pad: A add a ball, B shake, LEFT / RIGHT (or the stick) tilt the box, L1 / R1 gravity, X exits.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "edu_ui.h"

PAPP_MANIFEST(.id = "edu.physics", .name = "Physics Playground", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define FX 20                           /* the box */
#define FY 112
#define FW 540
#define FH 352
#define PX 572                          /* the panel */
#define PW 208
#define MAX_BALLS 48
#define SPR 64                          /* ball pictures: 64 x 64, radius 32 */
#define NCOL 6
#define SUBSTEPS 4

enum { ID_ADD = 1, ID_SHAKE, ID_CLEAR, ID_PAUSE, ID_GRAVITY, ID_BOUNCE };

typedef struct {
    float x, y, vx, vy, r, m;
    int col;
    int grab;                           /* touch id holding it, -1 none */
} ball_t;

static ball_t s_ball[MAX_BALLS];
static int s_n;
static float s_gravity = 900;           /* pixels / s^2 */
static float s_bounce = 0.85f;
static float s_tilt;                    /* -1..1: which way "down" leans */
static bool s_pause, s_quit, s_cpu;
static uint32_t *s_spr[NCOL];
static const uint32_t ball_rgb[NCOL] = { 0xE53935, 0xFB8C00, 0xFDD835, 0x43A047, 0x1E88E5, 0x8E24AA };
static uint32_t s_rng = 0x9E3779B9u;
static float s_energy, s_energy_max = 1;
static float s_fps;

static float frand(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (s_rng >> 8) * (1.0f / 16777216.0f);
}

/* A shaded ball: light from the top left, soft edge (alpha) */
static void make_sprites(void)
{
    for (int c = 0; c < NCOL; c++) {
        s_spr[c] = papp_alloc(SPR * SPR * 4, PAPP_MEM_DMA, 64);
        if (!s_spr[c]) continue;
        float br = (ball_rgb[c] >> 16) / 255.0f, bg = ((ball_rgb[c] >> 8) & 255) / 255.0f, bb = (ball_rgb[c] & 255) / 255.0f;
        for (int y = 0; y < SPR; y++)
            for (int x = 0; x < SPR; x++) {
                float dx = (x + 0.5f - SPR / 2) / (SPR / 2), dy = (y + 0.5f - SPR / 2) / (SPR / 2);
                float d = sqrtf(dx * dx + dy * dy);
                float a = (1.0f - d) * (SPR / 2);       /* pixels inside the edge */
                a = a < 0 ? 0 : a > 1 ? 1 : a;
                float nz = d < 1 ? sqrtf(1 - d * d) : 0;
                float light = 0.35f + 0.65f * fmaxf(0, -0.45f * dx - 0.55f * dy + 0.7f * nz);
                float shine = powf(fmaxf(0, -0.4f * dx - 0.5f * dy + 0.77f * nz), 30) * 0.8f;
                int r = (int)(fminf(1, br * light + shine) * 255), g = (int)(fminf(1, bg * light + shine) * 255),
                    b = (int)(fminf(1, bb * light + shine) * 255);
                s_spr[c][y * SPR + x] = (uint32_t)(a * 255) << 24 | r << 16 | g << 8 | b;
            }
    }
}

/* Radius in steps the PPA can draw exactly: scale k/16 of 32 = 2k pixels */
static void add_ball(float x, float y, float vx, float vy)
{
    if (s_n >= MAX_BALLS) return;
    ball_t *b = &s_ball[s_n++];
    int k = 6 + (int)(frand() * 9);                 /* radius 12..28 */
    b->r = 2.0f * k;
    b->m = b->r * b->r;
    b->x = fminf(fmaxf(x, FX + b->r), FX + FW - b->r);
    b->y = fminf(fmaxf(y, FY + b->r), FY + FH - b->r);
    b->vx = vx;
    b->vy = vy;
    b->col = (int)(frand() * NCOL) % NCOL;
    b->grab = -1;
}

static void shake(void)
{
    for (int i = 0; i < s_n; i++) {
        s_ball[i].vx += (frand() - 0.5f) * 1400;
        s_ball[i].vy -= 500 + frand() * 900;
    }
}

/* ── Physics ───────────────────────────────────────────────────────────── */

static void step(float dt)
{
    float gx = s_gravity * s_tilt * 0.7f, gy = s_gravity * sqrtf(1 - 0.49f * s_tilt * s_tilt);
    for (int i = 0; i < s_n; i++) {                /* 1 + 2: gravity, then move */
        ball_t *b = &s_ball[i];
        if (b->grab >= 0) continue;                 /* a finger holds it */
        b->vx += gx * dt;
        b->vy += gy * dt;
        b->x += b->vx * dt;
        b->y += b->vy * dt;
    }
    for (int i = 0; i < s_n; i++) {                /* 3a: walls */
        ball_t *b = &s_ball[i];
        if (b->x < FX + b->r) { b->x = FX + b->r; if (b->vx < 0) b->vx = -b->vx * s_bounce; }
        if (b->x > FX + FW - b->r) { b->x = FX + FW - b->r; if (b->vx > 0) b->vx = -b->vx * s_bounce; }
        if (b->y < FY + b->r) { b->y = FY + b->r; if (b->vy < 0) b->vy = -b->vy * s_bounce; }
        if (b->y > FY + FH - b->r) { b->y = FY + FH - b->r; if (b->vy > 0) b->vy = -b->vy * s_bounce; }
    }
    for (int i = 0; i < s_n; i++)                  /* 3b: each pair of balls */
        for (int j = i + 1; j < s_n; j++) {
            ball_t *a = &s_ball[i], *b = &s_ball[j];
            float dx = b->x - a->x, dy = b->y - a->y, rr = a->r + b->r;
            float d2 = dx * dx + dy * dy;
            if (d2 >= rr * rr || d2 < 1e-6f) continue;
            float d = sqrtf(d2), nx = dx / d, ny = dy / d;
            /* a held ball acts as if it were very heavy */
            float ima = a->grab >= 0 ? 0 : 1 / a->m, imb = b->grab >= 0 ? 0 : 1 / b->m;
            if (ima + imb == 0) continue;
            float push = (rr - d) / (ima + imb);   /* move them apart, the light one more */
            a->x -= nx * push * ima;
            a->y -= ny * push * ima;
            b->x += nx * push * imb;
            b->y += ny * push * imb;
            float vn = (b->vx - a->vx) * nx + (b->vy - a->vy) * ny;  /* closing speed */
            if (vn >= 0) continue;
            float jimp = -(1 + s_bounce) * vn / (ima + imb);
            a->vx -= jimp * ima * nx;
            a->vy -= jimp * ima * ny;
            b->vx += jimp * imb * nx;
            b->vy += jimp * imb * ny;
        }
}

static void measure_energy(void)
{
    float gx = s_gravity * s_tilt * 0.7f, gy = s_gravity * sqrtf(1 - 0.49f * s_tilt * s_tilt);
    float e = 0;
    for (int i = 0; i < s_n; i++) {
        const ball_t *b = &s_ball[i];
        float h = -(gx * (b->x - FX - FW / 2) + gy * (b->y - (FY + FH))) / (s_gravity > 1 ? s_gravity : 1);
        e += 0.5f * b->m * (b->vx * b->vx + b->vy * b->vy) + b->m * s_gravity * (h > 0 ? h : 0);
    }
    s_energy = s_energy * 0.8f + e * 0.2f;
    if (s_energy > s_energy_max) s_energy_max = s_energy;
}

/* ── Touch: grab and throw ─────────────────────────────────────────────── */

typedef struct { bool down; int ball; float x, y, vx, vy; int64_t t; } finger_t;
static finger_t s_finger[256];

static void touch_box(papp_surface_t *s)
{
    papp_touch_point_t pts[5];
    int n = papp_disp_touch(s, pts, 5);
    bool seen[256] = { 0 };
    int64_t now = papp_time_us();
    for (int i = 0; i < n; i++) {
        int id = pts[i].id;
        finger_t *f = &s_finger[id];
        float x = pts[i].x, y = pts[i].y;
        seen[id] = true;
        if (!f->down) {                            /* a new finger: on a ball, or in the box */
            f->down = true;
            f->ball = -2;                          /* -2: not ours (started outside the box) */
            f->vx = f->vy = 0;
            if (x >= FX && x < FX + FW && y >= FY && y < FY + FH) {
                f->ball = -1;
                for (int k = 0; k < s_n; k++) {
                    float dx = s_ball[k].x - x, dy = s_ball[k].y - y;
                    if (s_ball[k].grab < 0 && dx * dx + dy * dy <= (s_ball[k].r + 8) * (s_ball[k].r + 8)) {
                        f->ball = k;
                        s_ball[k].grab = id;
                        break;
                    }
                }
            }
        } else if (f->ball >= 0) {                 /* move the ball with the finger, remember its speed */
            float dt = (now - f->t) / 1e6f;
            if (dt > 0.001f) {
                f->vx = f->vx * 0.5f + (x - f->x) / dt * 0.5f;
                f->vy = f->vy * 0.5f + (y - f->y) / dt * 0.5f;
            }
            ball_t *b = &s_ball[f->ball];
            b->x = fminf(fmaxf(x, FX + b->r), FX + FW - b->r);
            b->y = fminf(fmaxf(y, FY + b->r), FY + FH - b->r);
            b->vx = f->vx;
            b->vy = f->vy;
        }
        f->x = x;
        f->y = y;
        f->t = now;
    }
    for (int id = 0; id < 256; id++) {
        finger_t *f = &s_finger[id];
        if (!f->down || seen[id]) continue;
        f->down = false;                           /* let go: throw, or drop a new ball */
        if (f->ball >= 0) {
            s_ball[f->ball].grab = -1;
            s_ball[f->ball].vx = fminf(fmaxf(f->vx, -3000), 3000);
            s_ball[f->ball].vy = fminf(fmaxf(f->vy, -3000), 3000);
        } else if (f->ball == -1) {
            add_ball(f->x, f->y, 0, 0);
        }
    }
}

/* ── Screen ────────────────────────────────────────────────────────────── */

static void slider(int x, int y, int w, const char *label, const char *value, float t, int id)
{
    ui_text_l(ui_small, x, y, label, C_FAINT);
    ui_text_r(ui_small, x + w, y, value, C_TEXT);
    int kx = x + (int)(t * w);
    ui_rrect(x, y + 20, w, 8, 4, C_BORDER);
    ui_rrect(x, y + 20, kx - x > 8 ? kx - x : 8, 8, 4, C_BLUE);
    ui_disc(kx, y + 24, 11, ui_pressed_color(id, C_TEXT), 255);
    ui_hit(x - 12, y - 10, w + 24, 46, id, UI_DRAG);
}

static void small_button(int x, int y, int w, int h, const char *label, uint16_t bg, int id)
{
    ui_rrect(x, y, w, h, h / 2, ui_pressed_color(id, bg));
    ui_text_c(ui_small, x + w / 2, y + h / 2, label, C_TEXT);
    ui_hit(x, y, w, h, id, UI_TAP);
}

static void draw_ball(papp_surface_t *s, const ball_t *b)
{
    int k = (int)(b->r / 2 + 0.5f);                /* scale k/16 */
    int d = 4 * k;                                 /* drawn size: 64 * k / 16 */
    papp_blit_t bl = {
        .pixels = s_spr[b->col], .w = SPR, .h = SPR, .format = PAPP_BLIT_ARGB8888,
        .x = (int16_t)(b->x - d / 2.0f + 0.5f), .y = (int16_t)(b->y - d / 2.0f + 0.5f),
        .scale_x = (uint16_t)(k * 16), .scale_y = (uint16_t)(k * 16),
    };
    if (!s_cpu && s_spr[b->col] && papp_disp_blit(s, &bl) == PAPP_E_OK) return;
    s_cpu = true;                                  /* an older launcher: plain discs */
    ui_disc(b->x, b->y, b->r, UI_RGB(ball_rgb[b->col]), 255);
    ui_disc(b->x - b->r * 0.35f, b->y - b->r * 0.35f, b->r * 0.25f, UI_RGB(0xFFFFFF), 120);
}

static void draw(papp_surface_t *s)
{
    ui_begin_keep(s);                              /* the box is cleared by the PPA below */
    ui_fill(0, 0, UI_W, FY, C_BG);
    ui_fill(0, FY, FX, FH, C_BG);
    ui_fill(FX + FW, FY, PX - FX - FW, FH, C_BG);
    ui_fill(PX, FY, UI_W - PX, FH, C_BG);          /* the panel's round corners show it */
    ui_fill(0, FY + FH, UI_W, UI_H - FY - FH, C_BG);
    char status[96];
    snprintf(status, sizeof(status), "%d balls     %d fps     %s", s_n, (int)(s_fps + 0.5f),
             s_pause ? "Paused" : "Touch to add, drag to throw");
    ui_bar("Physics Playground", NULL, 0, 0, status, s_pause ? C_ORANGE : C_GREEN, false);

    ui_rrect(PX, FY, PW, FH, 18, C_PANEL);
    int x = PX + 14, w = PW - 28;
    char v[16];
    int g10 = (int)(s_gravity / 90 + 0.5f);       /* no %f: newlib keeps its buffers */
    snprintf(v, sizeof(v), "%d.%d g", g10 / 10, g10 % 10);
    slider(x, FY + 22, w, "Gravity", v, s_gravity / 1800, ID_GRAVITY);
    snprintf(v, sizeof(v), "%d%%", (int)(s_bounce * 100 + 0.5f));
    slider(x, FY + 82, w, "Bounce", v, s_bounce, ID_BOUNCE);

    ui_text_l(ui_small, x, FY + 146, "Energy", C_FAINT);  /* energy: movement + height */
    float t = s_energy / s_energy_max;
    ui_rrect(x, FY + 164, w, 14, 7, C_BG);
    ui_rrect(x, FY + 164, (int)(t * w) > 14 ? (int)(t * w) : 14, 14, 7, C_ORANGE);

    int bw = (w - 8) / 2;
    small_button(x, FY + 196, bw, 40, "Add", C_BLUE, ID_ADD);
    small_button(x + bw + 8, FY + 196, bw, 40, "Shake", C_BLUE, ID_SHAKE);
    small_button(x, FY + 244, bw, 40, s_pause ? "Run" : "Pause", s_pause ? C_GREEN : C_RAISED, ID_PAUSE);
    small_button(x + bw + 8, FY + 244, bw, 40, "Clear", C_RAISED, ID_CLEAR);
    ui_text_wrap(ui_small, x, FY + 294, w, s_tilt < -0.05f ? "Tilted left" : s_tilt > 0.05f ? "Tilted right"
                                                                 : "Tilt: LEFT / RIGHT", C_FAINT, 1);

    /* CPU drawing is done: the PPA clears the box and draws the balls on top */
    const uint16_t box = UI_RGB(0x0E1A30);
    if (s_cpu || papp_disp_fill(s, FX, FY, FW, FH, box) != PAPP_E_OK) {
        s_cpu = true;
        ui_fill(FX, FY, FW, FH, box);
    }
    for (int i = 0; i < s_n; i++) draw_ball(s, &s_ball[i]);
    ui_end(s);
}

static void act(int id, int x)
{
    float t = (x - (PX + 14)) / (float)(PW - 28);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    switch (id) {
    case ID_ADD: add_ball(FX + 40 + frand() * (FW - 80), FY + 40, (frand() - 0.5f) * 400, 0); break;
    case ID_SHAKE: shake(); break;
    case ID_CLEAR: s_n = 0; s_energy = 0; s_energy_max = 1; break;
    case ID_PAUSE: s_pause = !s_pause; break;
    case ID_GRAVITY: s_gravity = roundf(t * 18) * 100; break;     /* 0 .. 2 g in steps of 0.1 g */
    case ID_BOUNCE: s_bounce = roundf(t * 20) / 20; s_energy_max = s_energy > 1 ? s_energy : 1; break;
    case UI_ID_CLOSE: s_quit = true; break;
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    papp_surface_t *s = ui_open();
    if (!s) return 1;
    s_rng ^= (uint32_t)papp_time_us();
    make_sprites();
    for (int i = 0; i < 12; i++) add_ball(FX + 40 + (i % 6) * 85, FY + 60 + (i / 6) * 70, (frand() - 0.5f) * 600, 0);

    uint32_t prev = 0xFFFFFFFFu;
    int64_t last = papp_time_us(), fps_t = last;
    int frames = 0;
    while (!papp_exit_requested() && !s_quit) {
        papp_gamepad_t gp = { 0 };
        papp_input_gamepad(0, &gp);
        uint32_t press = gp.buttons & ~prev;
        prev = gp.buttons;
        if (press & PAPP_BTN_X) break;
        if (press & PAPP_BTN_A) act(ID_ADD, 0);
        if (press & PAPP_BTN_B) shake();
        if (press & PAPP_BTN_L1 && s_gravity > 0) s_gravity -= 100;
        if (press & PAPP_BTN_R1 && s_gravity < 1800) s_gravity += 100;
        float want = (gp.buttons & PAPP_BTN_LEFT) ? -1 : (gp.buttons & PAPP_BTN_RIGHT) ? 1 : gp.lx / 32767.0f;
        if (fabsf(want) < 0.15f) want = 0;
        s_tilt += (want - s_tilt) * 0.15f;         /* lean slowly, like a box in your hands */

        touch_box(s);
        ui_touch(s, act);
        int64_t now = papp_time_us();
        float dt = (now - last) / 1e6f;            /* real time since the last frame */
        last = now;
        if (dt > 0.05f) dt = 0.05f;                /* a slow frame: don't jump */
        if (!s_pause)
            for (int i = 0; i < SUBSTEPS; i++) step(dt / SUBSTEPS);
        measure_energy();
        draw(s);
        frames++;
        if (now - fps_t >= 1000000) {
            s_fps = frames * 1e6f / (now - fps_t);
            static int sec;
            if (++sec % 5 == 0) papp_log("%d fps, %d balls, %s\n", (int)(s_fps + 0.5f), s_n, s_cpu ? "CPU" : "PPA");
            frames = 0;
            fps_t = now;
        }
    }
    for (int c = 0; c < NCOL; c++) papp_free(s_spr[c]);
    papp_disp_surface_destroy(s);
    return 0;
}
