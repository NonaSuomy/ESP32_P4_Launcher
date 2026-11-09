/* edu_ui — see edu_ui.h */
#include "edu_ui.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#include "../../clock/clock_font.h"
#pragma GCC diagnostic pop

#define W UI_W
#define H UI_H

const ui_font_t *const ui_small = (const ui_font_t *)&F_SMALL;
const ui_font_t *const ui_med = (const ui_font_t *)&F_MED;
const ui_font_t *const ui_large = (const ui_font_t *)&F_LARGE;
#define CF(f) ((const cf_font_t *)(f))

static uint16_t *s_px;
static int s_stride;

/* ── Frame ─────────────────────────────────────────────────────────────── */

static int s_nhit;

papp_surface_t *ui_open(void)
{
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    return papp_disp_surface_create(&cfg);
}

void ui_begin(papp_surface_t *s)
{
    s_px = papp_disp_lock(s, &s_stride);
    s_nhit = 0;
    ui_fill(0, 0, W, H, C_BG);
}

void ui_end(papp_surface_t *s) { papp_disp_present(s); }

/* ── Shapes ────────────────────────────────────────────────────────────── */

uint16_t ui_mix(uint16_t a, uint16_t b, int t)
{
    int r = ((a >> 11) * (256 - t) + (b >> 11) * t) >> 8;
    int g = (((a >> 5) & 63) * (256 - t) + ((b >> 5) & 63) * t) >> 8;
    int bl = ((a & 31) * (256 - t) + (b & 31) * t) >> 8;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

static inline void plot(int x, int y, uint16_t c, int a)
{
    if ((unsigned)x >= W || (unsigned)y >= H || a <= 0) return;
    uint16_t *p = &s_px[y * s_stride + x];
    *p = a >= 255 ? c : ui_mix(*p, c, a + (a >> 7));
}

static inline int cover(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f); }

void ui_fill(int x, int y, int w, int h, uint16_t c)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = y; j < y + h; j++) {
        uint16_t *p = &s_px[j * s_stride + x];
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

void ui_rrect(int x, int y, int w, int h, int r, uint16_t c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        float py = j + 0.5f;
        float dy = py < r ? r - py : py > h - r ? py - (h - r) : 0;
        if (dy == 0) { ui_fill(x, y + j, w, 1, c); continue; }
        ui_fill(x + r, y + j, w - 2 * r, 1, c);
        for (int i = 0; i < w; i++) {
            if (i == r && w - r > r) i = w - r;
            float px = i + 0.5f;
            float dx = px < r ? r - px : px > w - r ? px - (w - r) : 0;
            plot(x + i, y + j, c, cover(r - sqrtf(dx * dx + dy * dy) + 0.5f));
        }
    }
}

void ui_disc(float cx, float cy, float r, uint16_t c, int alpha)
{
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            plot(x, y, c, cover(r - sqrtf(dx * dx + dy * dy) + 0.5f) * alpha / 255);
        }
}

void ui_line(float x0, float y0, float x1, float y1, float th, uint16_t c)
{
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy + 1e-6f, h = th / 2 + 1;
    for (int y = (int)(fminf(y0, y1) - h); y <= (int)(fmaxf(y0, y1) + h); y++)
        for (int x = (int)(fminf(x0, x1) - h); x <= (int)(fmaxf(x0, x1) + h); x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = (px * vx + py * vy) / len2;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float dx = px - t * vx, dy = py - t * vy;
            plot(x, y, c, cover(th / 2 - sqrtf(dx * dx + dy * dy) + 0.5f));
        }
}

/* ── Text ──────────────────────────────────────────────────────────────── */

static const cf_glyph_t *glyph(const ui_font_t *f, int ch)
{
    return &CF(f)->g[ch >= 32 && ch < 127 ? ch - 32 : 0];
}

static int text_wn(const ui_font_t *f, const char *s, int n)
{
    int w = 0;
    for (int i = 0; i < n && s[i]; i++) w += glyph(f, (unsigned char)s[i])->adv;
    return w;
}

int ui_text_w(const ui_font_t *f, const char *s) { return text_wn(f, s, 1 << 30); }
int ui_line_h(const ui_font_t *f) { return CF(f)->height; }

int ui_line_top(const ui_font_t *f, int cy)
{
    const cf_glyph_t *g = glyph(f, 'H');
    return cy - (g->y + g->h / 2);
}

int ui_text_n(const ui_font_t *f, int x, int y, const char *s, int n, uint16_t c, int x_max)
{
    for (int k = 0; k < n && s[k]; k++) {
        const cf_glyph_t *g = glyph(f, (unsigned char)s[k]);
        if (x + g->adv > x_max) break;
        int gx = x + g->x, gy = y + g->y, bw = (g->w + 1) / 2;
        const uint8_t *p = CF(f)->px + g->off;
        for (int j = 0; j < g->h; j++)
            for (int i = 0; i < g->w; i++) {
                int v = p[j * bw + i / 2];
                v = (i & 1) ? v & 15 : v >> 4;
                if (v) plot(gx + i, gy + j, c, v * 17);
            }
        x += g->adv;
    }
    return x;
}

void ui_text_l(const ui_font_t *f, int x, int cy, const char *s, uint16_t c)
{
    ui_text_n(f, x, ui_line_top(f, cy), s, 1 << 30, c, W);
}

void ui_text_c(const ui_font_t *f, int cx, int cy, const char *s, uint16_t c)
{
    ui_text_n(f, cx - ui_text_w(f, s) / 2, ui_line_top(f, cy), s, 1 << 30, c, W);
}

void ui_text_r(const ui_font_t *f, int x, int cy, const char *s, uint16_t c)
{
    ui_text_n(f, x - ui_text_w(f, s), ui_line_top(f, cy), s, 1 << 30, c, W);
}

int ui_text_wrap(const ui_font_t *f, int x, int y, int w, const char *s, uint16_t c, int max_lines)
{
    int lines = 0;
    while (*s && lines < max_lines) {
        int n = 0, fit = 0;                   /* n: characters that fit, fit: up to the last space */
        while (s[n] && text_wn(f, s, n + 1) <= w) {
            n++;
            if (s[n] == ' ' || !s[n]) fit = n;
        }
        if (!s[n]) fit = n;
        if (!fit) fit = n ? n : 1;            /* one long word: break it */
        ui_text_n(f, x, y, s, fit, c, x + w);
        s += fit;
        while (*s == ' ') s++;
        y += CF(f)->height;
        lines++;
    }
    return lines;
}

/* ── Touch ─────────────────────────────────────────────────────────────── */

typedef struct { int16_t x, y, w, h; uint8_t id, kind; } hit_t;
static hit_t s_hit[48];
static int s_pressed = -1;

void ui_hit(int x, int y, int w, int h, int id, int kind)
{
    if (s_nhit < (int)(sizeof(s_hit) / sizeof(s_hit[0])))
        s_hit[s_nhit++] = (hit_t){ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (uint8_t)id, (uint8_t)kind };
}

static const hit_t *hit_at(int x, int y)
{
    for (int i = s_nhit - 1; i >= 0; i--)
        if (x >= s_hit[i].x && x < s_hit[i].x + s_hit[i].w && y >= s_hit[i].y && y < s_hit[i].y + s_hit[i].h)
            return &s_hit[i];
    return NULL;
}

uint16_t ui_pressed_color(int id, uint16_t c) { return id == s_pressed ? ui_mix(c, C_TEXT, 60) : c; }

void ui_button(int x, int y, int w, int h, const char *label, uint16_t bg, int id)
{
    ui_rrect(x, y, w, h, h / 2, ui_pressed_color(id, bg));
    ui_text_c(ui_med, x + w / 2, y + h / 2, label, C_TEXT);
    ui_hit(x, y, w, h, id, UI_TAP);
}

void ui_toggle(int x, int y, bool on, int id)
{
    ui_rrect(x, y, 72, 36, 18, ui_pressed_color(id, on ? C_GREEN : C_BORDER));
    ui_disc(on ? x + 54 : x + 18, y + 18, 14, C_TEXT, 255);
    ui_hit(x - 30, y - 16, 112, 68, id, UI_TAP);
}

static bool s_down, s_wait_release;
static int s_down_id = -1, s_down_kind;
static int64_t s_rep_next;
static papp_touch_point_t s_last;

void ui_touch_reset(void)
{
    s_down = false;
    s_down_id = -1;
    s_wait_release = true;
}

void ui_touch(papp_surface_t *s, void (*act)(int id, int x))
{
    int64_t now = papp_time_us();
    papp_touch_point_t cur;
    bool touch = papp_disp_touch(s, &cur, 1) > 0;
    if (s_wait_release) {
        s_wait_release = touch;
    } else if (touch) {
        s_last = cur;
        if (!s_down) {
            s_down = true;
            const hit_t *h = hit_at(cur.x, cur.y);
            s_down_id = h ? h->id : -1;
            s_down_kind = h ? h->kind : UI_TAP;
            if (s_down_id >= 0 && s_down_kind == UI_REPEAT) {
                act(s_down_id, cur.x);
                s_rep_next = now + 400000;
            }
        }
        if (s_down_id >= 0 && s_down_kind == UI_DRAG) act(s_down_id, cur.x);
        else if (s_down_id >= 0 && s_down_kind == UI_REPEAT && now >= s_rep_next) {
            act(s_down_id, cur.x);
            s_rep_next = now + 60000;
        }
    } else if (s_down) {
        s_down = false;
        const hit_t *h = hit_at(s_last.x, s_last.y);
        if (h && h->id == s_down_id && s_down_kind == UI_TAP) act(s_down_id, s_last.x);
        s_down_id = -1;
    }
    s_pressed = s_down ? s_down_id : -1;
}

/* ── Title bar ─────────────────────────────────────────────────────────── */

void ui_bar(const char *title, const char *const *tabs, int ntabs, int tab, const char *status, uint16_t dot,
            bool blink)
{
    ui_fill(0, 0, W, 64, C_PANEL);
    ui_text_l(ui_med, 24, 32, title, C_TEXT);
    for (int i = 0; i < ntabs; i++) {
        int x = 270 + i * 150;
        bool on = tab == i;
        ui_rrect(x, 12, 136, 40, 20, ui_pressed_color(UI_ID_TAB + i, on ? C_BLUE : C_RAISED));
        ui_text_c(ui_med, x + 68, 32, tabs[i], on ? C_TEXT : C_DIM);
        ui_hit(x, 4, 136, 56, UI_ID_TAB + i, UI_TAP);
    }
    ui_disc(760, 32, 22, ui_pressed_color(UI_ID_CLOSE, C_RAISED), 255);
    ui_line(752, 24, 768, 40, 3, C_TEXT);
    ui_line(768, 24, 752, 40, 3, C_TEXT);
    ui_hit(728, 0, 72, 64, UI_ID_CLOSE, UI_TAP);

    if (blink && (papp_time_us() / 500000) & 1) dot = ui_mix(dot, C_BG, 160);
    ui_disc(32, 88, 7, dot, 255);
    ui_text_l(ui_small, 50, 88, status, dot == C_RED ? C_RED : C_DIM);
}

/* ── Log ───────────────────────────────────────────────────────────────── */

#define LOG_N 64
#define LOG_W 72
typedef struct { char t[LOG_W + 1]; uint8_t dir; bool open; } logline_t;
static logline_t s_log[LOG_N];
static int s_logn;

static logline_t *log_new(int dir)
{
    if (s_logn == LOG_N) {
        memmove(s_log, s_log + 1, sizeof(s_log[0]) * (LOG_N - 1));
        s_logn--;
    }
    logline_t *l = &s_log[s_logn++];
    l->t[0] = 0;
    l->dir = (uint8_t)dir;
    l->open = true;
    return l;
}

void ui_log_bytes(int dir, const char *p, int n)
{
    logline_t *l = s_logn && s_log[s_logn - 1].open && s_log[s_logn - 1].dir == dir ? &s_log[s_logn - 1] : NULL;
    for (int i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (!l) l = log_new(dir);
            l->open = false;
            l = NULL;
            continue;
        }
        if (!l) l = log_new(dir);
        int k = (int)strlen(l->t);
        if (k == LOG_W) {
            l->open = false;
            l = log_new(dir);
            k = 0;
        }
        l->t[k] = (c >= 32 && c < 127) ? c : '.';
        l->t[k + 1] = 0;
    }
}

void ui_log(int dir, const char *fmt, ...)
{
    char b[LOG_W + 1];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    logline_t *l = log_new(dir);
    strcpy(l->t, b);
    l->open = false;
    papp_log("%s\n", b);
}

void ui_log_clear(void) { s_logn = 0; }

void ui_log_draw(int x, int y, int w, int h, const char *empty)
{
    ui_rrect(x, y, w, h, 18, C_PANEL);
    int lh = ui_line_h(ui_small), rows = (h - 24) / lh;
    int first = s_logn > rows ? s_logn - rows : 0;
    if (!s_logn) ui_text_c(ui_small, x + w / 2, y + h / 2, empty, C_FAINT);
    for (int i = first, ty = y + 12; i < s_logn; i++, ty += lh) {
        const logline_t *l = &s_log[i];
        uint16_t c = l->dir == 0 ? C_IN : l->dir == 1 ? C_OUT : C_DIM;
        ui_text_n(ui_small, x + 16, ty, l->dir == 0 ? "<" : l->dir == 1 ? ">" : "-", 1, c, x + 40);
        ui_text_n(ui_small, x + 36, ty, l->t, LOG_W, c, x + w - 12);
    }
}

/* ── The remote ────────────────────────────────────────────────────────── */

remote_t remote = { .mode = 0, .level = 50, .led = 0, .msg = "Hello from the P4" };
const char *const remote_mode_name[3] = { "Idle", "Run", "Test" };

enum { R_MODE = 220, R_SLIDER = 223, R_MINUS, R_PLUS, R_LED };
#define SL_X 64
#define SL_W 252
#define SL_Y 268

void remote_changed(int what, const char *who)
{
    if (!what) return;
    remote.rev++;
    if (who) snprintf(remote.source, sizeof(remote.source), "Last change: %s", who);
}

void remote_draw(const char *hint)
{
    char b[24];
    ui_rrect(20, 112, 372, 352, 18, C_PANEL);
    ui_text_l(ui_small, 44, 140, "MODE", C_FAINT);
    for (int i = 0; i < 3; i++) {
        int x = 44 + i * 112;
        bool on = remote.mode == i;
        ui_rrect(x, 158, 104, 48, 24, ui_pressed_color(R_MODE + i, on ? C_BLUE : C_RAISED));
        ui_text_c(ui_med, x + 52, 182, remote_mode_name[i], on ? C_TEXT : C_DIM);
        ui_hit(x, 152, 104, 60, R_MODE + i, UI_TAP);
    }

    ui_text_l(ui_small, 44, 236, "LEVEL", C_FAINT);
    snprintf(b, sizeof(b), "%d", remote.level);
    ui_text_r(ui_med, 368, 236, b, C_TEXT);
    ui_disc(40, SL_Y, 18, ui_pressed_color(R_MINUS, C_RAISED), 255);
    ui_line(32, SL_Y, 48, SL_Y, 3, C_TEXT);
    ui_hit(16, SL_Y - 26, 44, 52, R_MINUS, UI_REPEAT);
    ui_disc(340, SL_Y, 18, ui_pressed_color(R_PLUS, C_RAISED), 255);
    ui_line(332, SL_Y, 348, SL_Y, 3, C_TEXT);
    ui_line(340, SL_Y - 8, 340, SL_Y + 8, 3, C_TEXT);
    ui_hit(320, SL_Y - 26, 44, 52, R_PLUS, UI_REPEAT);
    int kx = SL_X + SL_W * remote.level / 100;
    ui_rrect(SL_X, SL_Y - 5, SL_W, 10, 5, C_BORDER);
    if (kx > SL_X) ui_rrect(SL_X, SL_Y - 5, kx - SL_X + 5, 10, 5, C_BLUE);
    ui_disc(kx, SL_Y, 13, C_TEXT, 255);
    ui_hit(SL_X - 8, SL_Y - 26, SL_W + 16, 52, R_SLIDER, UI_DRAG);

    ui_text_l(ui_small, 44, 330, "LED", C_FAINT);
    ui_text_l(ui_med, 44, 366, remote.led ? "On" : "Off", C_TEXT);
    ui_toggle(290, 349, remote.led, R_LED);

    ui_text_l(ui_small, 44, 428, remote.source[0] ? remote.source : hint, C_FAINT);

    /* the LED and the message */
    ui_rrect(408, 112, 372, 352, 18, C_PANEL);
    float cx = 594, cy = 214;
    uint16_t lc = remote.mode == 2 ? C_ORANGE : C_GREEN;
    if (remote.led) {
        int glow = 60 + remote.level * 150 / 100;
        for (int r = 70; r > 44; r -= 4) ui_disc(cx, cy, r, lc, glow * (70 - r) / 40);
        ui_disc(cx, cy, 44, ui_mix(C_RAISED, lc, 80 + remote.level * 176 / 100), 255);
    } else {
        ui_disc(cx, cy, 44, C_RAISED, 255);
    }
    ui_disc(cx - 14, cy - 14, 10, C_TEXT, remote.led ? 120 : 30);
    snprintf(b, sizeof(b), "%s  %d%%", remote_mode_name[remote.mode], remote.level);
    ui_text_c(ui_small, (int)cx, 300, b, C_DIM);

    ui_text_l(ui_small, 432, 340, "MESSAGE", C_FAINT);
    ui_rrect(428, 356, 332, 92, 12, C_RAISED);
    ui_text_wrap(ui_med, 444, 364, 300, remote.msg[0] ? remote.msg : " ", C_TEXT, 2);
}

static int set_level(int v)
{
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (v == remote.level) return 0;
    remote.level = (uint8_t)v;
    return REMOTE_LEVEL;
}

int remote_act(int id, int x)
{
    int what = 0;
    if (id >= R_MODE && id < R_MODE + 3) {
        remote.mode = (uint8_t)(id - R_MODE);
        what = REMOTE_MODE;
    } else if (id == R_MINUS) what = set_level(remote.level - 1);
    else if (id == R_PLUS) what = set_level(remote.level + 1);
    else if (id == R_SLIDER) what = set_level((x - SL_X) * 100 / SL_W);
    else if (id == R_LED) {
        remote.led = !remote.led;
        what = REMOTE_LED;
    }
    remote_changed(what, "screen");
    return what;
}

int remote_pad(uint32_t press)
{
    int what = 0;
    if (press & PAPP_BTN_UP) { remote.mode = (uint8_t)((remote.mode + 2) % 3); what |= REMOTE_MODE; }
    if (press & PAPP_BTN_DOWN) { remote.mode = (uint8_t)((remote.mode + 1) % 3); what |= REMOTE_MODE; }
    if (press & PAPP_BTN_LEFT) what |= set_level(remote.level - 5);
    if (press & PAPP_BTN_RIGHT) what |= set_level(remote.level + 5);
    if (press & PAPP_BTN_A) { remote.led = !remote.led; what |= REMOTE_LED; }
    remote_changed(what, "pad");
    return what;
}
