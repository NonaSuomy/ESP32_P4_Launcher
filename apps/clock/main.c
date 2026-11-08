/*
 * Clock — clock, alarms, stopwatch, timer and world clock. Time and time zone come from
 * the launcher (network or DevLink); the world clock needs its UTC time (launcher 0.27+).
 *
 * Touch: the round buttons at the top pick the function, X (top right) closes the app.
 * Pad:   L1 / R1 previous / next function, X or START the menu (functions, Exit).
 *   Alarm       UP/DOWN pick, A on/off (or add), Y edit; editing: LEFT/RIGHT field,
 *               UP/DOWN change, A done, Y delete
 *   Stopwatch   A start/stop, B lap/reset
 *   Timer       UP/DOWN minutes, A start/stop, B reset
 *   World       UP/DOWN pick, A remove (or add), Y add a city
 *   Ringing     A stop, B snooze (alarms)
 * Alarms ring while the Clock is open. Settings: /sd/data/tool.clock/clock.txt.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "papp/papp.h"
#include "clock_font.h"

PAPP_MANIFEST(.id = "tool.clock", .name = "Clock", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define W     800
#define H     480
#define TOP   124                 /* content starts below the function bar */

#define RGB(h) PAPP_RGB565(((h) >> 16) & 255, ((h) >> 8) & 255, (h) & 255)
#define C_BG      RGB(0x081226)
#define C_PANEL   RGB(0x0E1A30)
#define C_RAISED  RGB(0x16274A)
#define C_BORDER  RGB(0x24406E)
#define C_BLUE    RGB(0x1E6FFF)
#define C_FOCUS   RGB(0x5FA8FF)
#define C_TEXT    RGB(0xFFFFFF)
#define C_DIM     RGB(0x8FA3BF)
#define C_FAINT   RGB(0x4A5C7A)
#define C_GREEN   RGB(0x3DDC84)
#define C_ORANGE  RGB(0xF5A623)
#define C_RED     RGB(0xD9453B)

enum { M_CLOCK, M_ALARM, M_STOPWATCH, M_TIMER, M_WORLD, M_N };
static const char *const k_mode_name[M_N] = { "Clock", "Alarm", "Stopwatch", "Timer", "World" };

static const char *const k_wday[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const k_month[12] = { "January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December" };

/* Cities of the world clock, with their POSIX time zone rules */
static const struct { const char *name, *tz; } k_city[] = {
    { "Honolulu", "HST10" },
    { "Anchorage", "AKST9AKDT,M3.2.0,M11.1.0" },
    { "Los Angeles", "PST8PDT,M3.2.0,M11.1.0" },
    { "Denver", "MST7MDT,M3.2.0,M11.1.0" },
    { "Chicago", "CST6CDT,M3.2.0,M11.1.0" },
    { "Mexico City", "CST6" },
    { "New York", "EST5EDT,M3.2.0,M11.1.0" },
    { "Toronto", "EST5EDT,M3.2.0,M11.1.0" },
    { "Bogota", "COT5" },
    { "Santiago", "CLT4CLST,M9.1.6/24,M4.1.6/24" },
    { "Buenos Aires", "ART3" },
    { "Sao Paulo", "BRT3" },
    { "Reykjavik", "GMT0" },
    { "London", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Lisbon", "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Paris", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Berlin", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Rome", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Madrid", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Johannesburg", "SAST-2" },
    { "Cairo", "EET-2EEST,M4.5.5/0,M10.5.4/24" },
    { "Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Tel Aviv", "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "Istanbul", "TRT-3" },
    { "Moscow", "MSK-3" },
    { "Dubai", "GST-4" },
    { "Karachi", "PKT-5" },
    { "New Delhi", "IST-5:30" },
    { "Bangkok", "ICT-7" },
    { "Singapore", "SGT-8" },
    { "Hong Kong", "HKT-8" },
    { "Beijing", "CST-8" },
    { "Seoul", "KST-9" },
    { "Tokyo", "JST-9" },
    { "Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Auckland", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
};
#define CITIES ((int)(sizeof(k_city) / sizeof(k_city[0])))

/* ── Drawing ───────────────────────────────────────────────────────────── */

static uint16_t *s_px;
static int s_stride;

static inline uint16_t mix(uint16_t a, uint16_t b, int t)        /* t 0..256 = a..b */
{
    int r = ((a >> 11) * (256 - t) + (b >> 11) * t) >> 8;
    int g = (((a >> 5) & 63) * (256 - t) + ((b >> 5) & 63) * t) >> 8;
    int bl = ((a & 31) * (256 - t) + (b & 31) * t) >> 8;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

static inline void plot(int x, int y, uint16_t c, int a)          /* a 0..255 */
{
    if ((unsigned)x >= W || (unsigned)y >= H || a <= 0) return;
    uint16_t *p = &s_px[y * s_stride + x];
    *p = a >= 255 ? c : mix(*p, c, a + (a >> 7));
}

static inline int cover(float v)                                   /* 0..1 -> 0..255 */
{
    return v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f);
}

static void fill(int x, int y, int w, int h, uint16_t c)
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

static void darken(int percent)
{
    for (int j = 0; j < H; j++) {
        uint16_t *p = &s_px[j * s_stride];
        for (int i = 0; i < W; i++) p[i] = mix(p[i], 0, percent * 256 / 100);
    }
}

/* Filled rounded rectangle, anti-aliased corners */
static void rrect(int x, int y, int w, int h, int r, uint16_t c)
{
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int j = 0; j < h; j++) {
        float py = j + 0.5f;
        float dy = py < r ? r - py : py > h - r ? py - (h - r) : 0;
        if (dy == 0) { fill(x, y + j, w, 1, c); continue; }
        fill(x + r, y + j, w - 2 * r, 1, c);                /* only the corners need care */
        for (int i = 0; i < w; i++) {
            if (i == r && w - r > r) i = w - r;
            float px = i + 0.5f;
            float dx = px < r ? r - px : px > w - r ? px - (w - r) : 0;
            float d = sqrtf(dx * dx + dy * dy);
            plot(x + i, y + j, c, cover(r - d + 0.5f));
        }
    }
}

static void disc(float cx, float cy, float r, uint16_t c)
{
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            plot(x, y, c, cover(r - sqrtf(dx * dx + dy * dy) + 0.5f));
        }
}

/* Ring of radius r and thickness th; with frac < 1, only the arc clockwise from the top */
static void arc(float cx, float cy, float r, float th, float frac, uint16_t c)
{
    float lim = frac * 2.0f * 3.14159265f;
    for (int y = (int)(cy - r - th); y <= (int)(cy + r + th); y++)
        for (int x = (int)(cx - r - th); x <= (int)(cx + r + th); x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            int a = cover(th / 2 - fabsf(sqrtf(dx * dx + dy * dy) - r) + 0.5f);
            if (!a) continue;
            if (frac < 1.0f) {
                float ang = atan2f(dx, -dy);
                if (ang < 0) ang += 2.0f * 3.14159265f;
                if (ang > lim) continue;
            }
            plot(x, y, c, a);
        }
}

static void ring(float cx, float cy, float r, float th, uint16_t c)
{
    arc(cx, cy, r, th, 1.0f, c);
}

static void ellipse_ring(float cx, float cy, float a, float b, float th, uint16_t c)
{
    for (int y = (int)(cy - b - th); y <= (int)(cy + b + th); y++)
        for (int x = (int)(cx - a - th); x <= (int)(cx + a + th); x++) {
            float nx = (x + 0.5f - cx) / a, ny = (y + 0.5f - cy) / b;
            float f = nx * nx + ny * ny - 1.0f;
            float g = 2.0f * sqrtf(nx * nx / (a * a) + ny * ny / (b * b)) + 1e-6f;
            plot(x, y, c, cover(th / 2 - fabsf(f / g) + 0.5f));
        }
}

/* Line with round ends */
static void line(float x0, float y0, float x1, float y1, float th, uint16_t c)
{
    float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy + 1e-6f, h = th / 2 + 1;
    int xa = (int)(fminf(x0, x1) - h), xb = (int)(fmaxf(x0, x1) + h);
    int ya = (int)(fminf(y0, y1) - h), yb = (int)(fmaxf(y0, y1) + h);
    for (int y = ya; y <= yb; y++)
        for (int x = xa; x <= xb; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = (px * vx + py * vy) / len2;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float dx = px - t * vx, dy = py - t * vy;
            plot(x, y, c, cover(th / 2 - sqrtf(dx * dx + dy * dy) + 0.5f));
        }
}

/* ── Text (Montserrat, see make_font.py; digits are drawn at one width) ── */

static const cf_glyph_t *glyph(const cf_font_t *f, int ch)
{
    if (f->n == 95) return &f->g[ch >= 32 && ch < 127 ? ch - 32 : 0];
    for (int i = 0; i < f->n; i++)
        if (f->g[i].ch == ch) return &f->g[i];
    return NULL;
}

static inline bool is_digit(int c) { return c >= '0' && c <= '9'; }

static int text_w(const cf_font_t *f, const char *s)
{
    int da = glyph(f, '0')->adv, w = 0;
    for (; *s; s++) {
        const cf_glyph_t *g = glyph(f, (unsigned char)*s);
        if (g) w += is_digit(*s) ? da : g->adv;
    }
    return w;
}

/* y = top of the line; returns the x after the text */
static int text(const cf_font_t *f, int x, int y, const char *s, uint16_t c)
{
    int da = glyph(f, '0')->adv;
    for (; *s; s++) {
        const cf_glyph_t *g = glyph(f, (unsigned char)*s);
        if (!g) continue;
        int adv = is_digit(*s) ? da : g->adv;
        int gx = x + g->x + (is_digit(*s) ? (da - g->adv) / 2 : 0), gy = y + g->y, bw = (g->w + 1) / 2;
        const uint8_t *p = f->px + g->off;
        for (int j = 0; j < g->h; j++) {
            if ((unsigned)(gy + j) >= H) continue;
            for (int i = 0; i < g->w; i++) {
                int v = p[j * bw + i / 2];
                v = (i & 1) ? v & 15 : v >> 4;
                if (v) plot(gx + i, gy + j, c, v * 17);
            }
        }
        x += adv;
    }
    return x;
}

/* The line top that centres digits and capitals on cy */
static int line_top(const cf_font_t *f, int cy)
{
    const cf_glyph_t *g = glyph(f, '0');
    return cy - (g->y + g->h / 2);
}

static void text_c(const cf_font_t *f, int cx, int cy, const char *s, uint16_t c)
{
    text(f, cx - text_w(f, s) / 2, line_top(f, cy), s, c);
}

static void text_l(const cf_font_t *f, int x, int cy, const char *s, uint16_t c)
{
    text(f, x, line_top(f, cy), s, c);
}

static void text_r(const cf_font_t *f, int x, int cy, const char *s, uint16_t c)
{
    text(f, x - text_w(f, s), line_top(f, cy), s, c);
}

/* ── Icons ─────────────────────────────────────────────────────────────── */

enum { IC_CLOCK, IC_ALARM, IC_STOPWATCH, IC_TIMER, IC_WORLD, IC_CLOSE, IC_PLUS, IC_MINUS, IC_UP, IC_DOWN };

static void icon(int kind, float cx, float cy, float r, uint16_t c)
{
    float t = r * 0.09f + 0.8f;                 /* stroke */
    switch (kind) {
    case IC_CLOCK:
        ring(cx, cy, r * 0.6f, t, c);
        line(cx, cy, cx, cy - r * 0.38f, t, c);
        line(cx, cy, cx + r * 0.28f, cy + r * 0.1f, t, c);
        break;
    case IC_ALARM:
        disc(cx, cy - r * 0.12f, r * 0.4f, c);
        fill((int)(cx - r * 0.4f), (int)(cy - r * 0.12f), (int)(r * 0.8f + 0.5f), (int)(r * 0.44f), c);
        line(cx - r * 0.56f, cy + r * 0.34f, cx + r * 0.56f, cy + r * 0.34f, t * 1.2f, c);
        disc(cx, cy + r * 0.5f, r * 0.12f, c);
        disc(cx, cy - r * 0.56f, r * 0.08f, c);
        break;
    case IC_STOPWATCH:
        ring(cx, cy + r * 0.1f, r * 0.5f, t, c);
        line(cx, cy - r * 0.42f, cx, cy - r * 0.58f, t, c);
        line(cx - r * 0.14f, cy - r * 0.62f, cx + r * 0.14f, cy - r * 0.62f, t, c);
        line(cx, cy + r * 0.1f, cx + r * 0.2f, cy - r * 0.14f, t, c);
        break;
    case IC_TIMER:
        line(cx - r * 0.36f, cy - r * 0.56f, cx + r * 0.36f, cy - r * 0.56f, t, c);
        line(cx - r * 0.36f, cy + r * 0.56f, cx + r * 0.36f, cy + r * 0.56f, t, c);
        line(cx - r * 0.28f, cy - r * 0.56f, cx + r * 0.28f, cy + r * 0.56f, t, c);
        line(cx + r * 0.28f, cy - r * 0.56f, cx - r * 0.28f, cy + r * 0.56f, t, c);
        line(cx - r * 0.16f, cy + r * 0.44f, cx + r * 0.16f, cy + r * 0.44f, t * 1.3f, c);
        break;
    case IC_WORLD:
        ring(cx, cy, r * 0.6f, t, c);
        ellipse_ring(cx, cy, r * 0.25f, r * 0.6f, t * 0.85f, c);
        line(cx - r * 0.6f, cy, cx + r * 0.6f, cy, t * 0.85f, c);
        line(cx - r * 0.5f, cy - r * 0.3f, cx + r * 0.5f, cy - r * 0.3f, t * 0.7f, c);
        line(cx - r * 0.5f, cy + r * 0.3f, cx + r * 0.5f, cy + r * 0.3f, t * 0.7f, c);
        break;
    case IC_CLOSE:
        line(cx - r * 0.32f, cy - r * 0.32f, cx + r * 0.32f, cy + r * 0.32f, t * 1.2f, c);
        line(cx + r * 0.32f, cy - r * 0.32f, cx - r * 0.32f, cy + r * 0.32f, t * 1.2f, c);
        break;
    case IC_PLUS:
    case IC_MINUS:
        line(cx - r * 0.4f, cy, cx + r * 0.4f, cy, t * 1.2f, c);
        if (kind == IC_PLUS) line(cx, cy - r * 0.4f, cx, cy + r * 0.4f, t * 1.2f, c);
        break;
    case IC_UP:
    case IC_DOWN: {
        float d = kind == IC_UP ? -1 : 1;
        line(cx - r * 0.36f, cy - d * r * 0.16f, cx, cy + d * r * 0.2f, t * 1.2f, c);
        line(cx + r * 0.36f, cy - d * r * 0.16f, cx, cy + d * r * 0.2f, t * 1.2f, c);
        break;
    }
    }
}

/* Sun by day, moon by night */
static void sun_moon(float cx, float cy, int hour, uint16_t bg)
{
    if (hour >= 6 && hour < 19) {
        disc(cx, cy, 7, C_ORANGE);
        for (int k = 0; k < 8; k++) {
            float a = k * 0.785398f;
            line(cx + cosf(a) * 10, cy + sinf(a) * 10, cx + cosf(a) * 13, cy + sinf(a) * 13, 2, C_ORANGE);
        }
    } else {
        disc(cx, cy, 10, C_DIM);
        disc(cx + 5, cy - 4, 9, bg);
    }
}

/* ── Touch targets: recorded while drawing, read on the next touch ──────── */

enum {
    H_MODE = 0,                 /* + mode */
    H_CLOSE = 10,
    H_SW_GO, H_SW_LAP, H_TM_GO, H_TM_RESET, H_TM_MINUS, H_TM_PLUS,
    H_AL_ROW = 30,              /* + alarm */
    H_AL_TOGGLE = 40,           /* + alarm */
    H_AL_ADD = 50, H_ED_HUP, H_ED_HDN, H_ED_MUP, H_ED_MDN, H_ED_DONE, H_ED_DEL,
    H_ED_REP = 60,              /* + repeat choice */
    H_W_ROW = 70,               /* + city slot */
    H_W_ADD = 80, H_PICK_CANCEL,
    H_PICK = 100,               /* + city */
    H_RING_STOP = 150, H_RING_SNOOZE,
};

typedef struct { int16_t x, y, w, h; uint8_t id; bool rep; } hit_t;
static hit_t s_hit[64];
static int s_nhit, s_pressed = -1;

static void hit(int x, int y, int w, int h, int id, bool rep)
{
    if (s_nhit < (int)(sizeof(s_hit) / sizeof(s_hit[0])))
        s_hit[s_nhit++] = (hit_t){ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, (uint8_t)id, rep };
}

static const hit_t *hit_at(int x, int y)
{
    for (int i = s_nhit - 1; i >= 0; i--)
        if (x >= s_hit[i].x && x < s_hit[i].x + s_hit[i].w && y >= s_hit[i].y && y < s_hit[i].y + s_hit[i].h)
            return &s_hit[i];
    return NULL;
}

static uint16_t pressed_color(int id, uint16_t c)
{
    return id == s_pressed ? mix(c, C_TEXT, 60) : c;
}

static void round_button(float cx, float cy, float r, int ic, uint16_t bg, uint16_t fg, int id, bool rep)
{
    disc(cx, cy, r, pressed_color(id, bg));
    icon(ic, cx, cy, r, fg);
    hit((int)(cx - r - 6), (int)(cy - r - 6), (int)(2 * r + 12), (int)(2 * r + 12), id, rep);
}

static void round_label_button(float cx, float cy, float r, const char *label, uint16_t bg, int id)
{
    disc(cx, cy, r, pressed_color(id, bg));
    text_c(&F_MED, (int)cx, (int)cy, label, C_TEXT);
    hit((int)(cx - r), (int)(cy - r), (int)(2 * r), (int)(2 * r), id, false);
}

static void button(int x, int y, int w, int h, const char *label, uint16_t bg, int id)
{
    rrect(x, y, w, h, h / 2, pressed_color(id, bg));
    text_c(&F_MED, x + w / 2, y + h / 2, label, C_TEXT);
    hit(x, y, w, h, id, false);
}

static void toggle(int x, int y, bool on, int id)
{
    rrect(x, y, 64, 34, 17, pressed_color(id, on ? C_GREEN : C_BORDER));
    disc(on ? x + 47 : x + 17, y + 17, 13, C_TEXT);
    hit(x - 10, y - 12, 84, 58, id, false);
}

/* ── Time ──────────────────────────────────────────────────────────────── */

static int64_t days_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

/* POSIX TZ rules ("CET-1CEST,M3.5.0,M10.5.0/3"), read here instead of newlib's tzset,
 * which allocates. Offsets are seconds east of UTC. */
typedef struct { int std, dst; bool has_dst; int m[2], w[2], d[2], t[2]; } tzrule_t;

static const char *tz_name(const char *p)
{
    if (*p == '<') { while (*p && *p != '>') p++; return *p ? p + 1 : p; }
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) p++;
    return p;
}

static const char *tz_time(const char *p, int *out)        /* [+-]h[:mm[:ss]] in seconds */
{
    int sign = 1, v[3] = { 0, 0, 0 };
    if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
    for (int k = 0; k < 3; k++) {
        while (*p >= '0' && *p <= '9') v[k] = v[k] * 10 + *p++ - '0';
        if (*p != ':' || k == 2) break;
        p++;
    }
    *out = sign * (v[0] * 3600 + v[1] * 60 + v[2]);
    return p;
}

static void tz_parse(const char *p, tzrule_t *r)
{
    *r = (tzrule_t){ 0 };
    int off;
    p = tz_time(tz_name(p), &off);
    r->std = -off;                                  /* POSIX counts west of UTC */
    if (!*p || *p == ',') return;
    r->has_dst = true;
    p = tz_name(p);
    r->dst = r->std + 3600;
    if (*p && *p != ',') { p = tz_time(p, &off); r->dst = -off; }
    for (int k = 0; k < 2; k++) {
        r->t[k] = 7200;
        if (*p != ',' || p[1] != 'M') { r->has_dst = false; return; }   /* only Mm.w.d rules */
        p += 2;
        r->m[k] = (int)strtol(p, (char **)&p, 10);
        if (*p == '.') r->w[k] = (int)strtol(p + 1, (char **)&p, 10);
        if (*p == '.') r->d[k] = (int)strtol(p + 1, (char **)&p, 10);
        if (*p == '/') p = tz_time(p + 1, &r->t[k]);
    }
}

/* UTC second when rule k (0 = DST starts, 1 = ends) happens in year y */
static int64_t tz_switch(const tzrule_t *r, int k, int y)
{
    int m = r->m[k];
    int64_t first = days_civil(y, m, 1), next = m == 12 ? days_civil(y + 1, 1, 1) : days_civil(y, m + 1, 1);
    int wd_first = (int)(((first + 4) % 7 + 7) % 7);   /* 1970-01-01 was a Thursday */
    int day = 1 + (r->d[k] - wd_first + 7) % 7 + (r->w[k] - 1) * 7;
    while (day > next - first) day -= 7;
    return (first + day - 1) * 86400 + r->t[k] - (k == 0 ? r->std : r->dst);
}

/* A city's offset from UTC (seconds) at a UTC instant */
static int city_offset(int city, int64_t utc)
{
    tzrule_t r;
    tz_parse(k_city[city].tz, &r);
    if (!r.has_dst) return r.std;
    int y, m, d;
    civil((utc + r.std) / 86400 - ((utc + r.std) % 86400 < 0), &y, &m, &d);
    int64_t on = tz_switch(&r, 0, y), off = tz_switch(&r, 1, y);
    bool dst = on < off ? utc >= on && utc < off : !(utc >= off && utc < on);
    return dst ? r.dst : r.std;
}

/* ── Settings ──────────────────────────────────────────────────────────── */

#define DATA_DIR    "/sd/data/tool.clock"
#define SETTINGS    DATA_DIR "/clock.txt"
#define MAX_ALARMS  5
#define MAX_CITIES  6
#define SNOOZE_US   (5 * 60 * 1000000LL)
#define RING_US     (5 * 60 * 1000000LL)

typedef struct { bool on; uint8_t hour, minute, days; } alarm_t;   /* days: bit 0 = Sunday; 0 = once */

static const struct { const char *name; uint8_t days; } k_repeat[] = {
    { "Once", 0 }, { "Every day", 0x7F }, { "Weekdays", 0x3E }, { "Weekends", 0x41 },
};

static alarm_t s_al[MAX_ALARMS];
static int64_t s_snooze[MAX_ALARMS];     /* papp_time_us() to ring again, 0 = none */
static int s_nal;
static int s_city[MAX_CITIES], s_ncity;

static const char *repeat_name(uint8_t days)
{
    for (int i = 0; i < 4; i++)
        if (k_repeat[i].days == days) return k_repeat[i].name;
    return "Custom";
}

static void load(void)
{
    FILE *f = fopen(SETTINGS, "r");
    if (!f) {                                       /* first run */
        static const char *const def[] = { "London", "New York", "Tokyo", "Sydney" };
        for (int i = 0; i < 4; i++)
            for (int c = 0; c < CITIES; c++)
                if (strcmp(k_city[c].name, def[i]) == 0) s_city[s_ncity++] = c;
        return;
    }
    char ln[96];
    while (fgets(ln, sizeof(ln), f)) {
        ln[strcspn(ln, "\r\n")] = 0;
        int h, m, on, days;
        if (sscanf(ln, "alarm=%d:%d %d %d", &h, &m, &on, &days) == 4 && s_nal < MAX_ALARMS &&
            h >= 0 && h < 24 && m >= 0 && m < 60)
            s_al[s_nal++] = (alarm_t){ on != 0, (uint8_t)h, (uint8_t)m, (uint8_t)(days & 0x7F) };
        else if (strncmp(ln, "city=", 5) == 0 && s_ncity < MAX_CITIES)
            for (int c = 0; c < CITIES; c++)
                if (strcmp(k_city[c].name, ln + 5) == 0) s_city[s_ncity++] = c;
    }
    fclose(f);
}

static void save(void)
{
    mkdir("/sd/data", 0777);
    mkdir(DATA_DIR, 0777);
    FILE *f = fopen(SETTINGS, "w");
    if (!f) { papp_log("clock: can't write " SETTINGS "\n"); return; }
    fprintf(f, "# Clock: alarm=HH:MM on days (bit 0 = Sunday, 0 = once), city=name\n");
    for (int i = 0; i < s_nal; i++)
        fprintf(f, "alarm=%02d:%02d %d %d\n", s_al[i].hour, s_al[i].minute, s_al[i].on, s_al[i].days);
    for (int i = 0; i < s_ncity; i++) fprintf(f, "city=%s\n", k_city[s_city[i]].name);
    fclose(f);
}

/* ── Sound ─────────────────────────────────────────────────────────────── */

#define RATE 16000
static papp_audio_stream_t *s_audio;
static int16_t *s_chirp;                    /* two short tones and a pause */
static int s_chirp_n;

static void chirp_make(void)
{
    s_chirp_n = RATE * 6 / 10;
    s_chirp = papp_alloc(s_chirp_n * sizeof(int16_t), 0, 4);
    if (!s_chirp) return;
    int tone = RATE * 13 / 100, gap = RATE * 7 / 100;
    for (int i = 0; i < s_chirp_n; i++) {
        float v = 0;
        int k = i < tone ? i : i - tone - gap;
        if (i < tone || (k >= 0 && k < tone)) {
            float f = i < tone ? 988.0f : 1319.0f;
            float env = fminf(1.0f, fminf(k, tone - k) / 160.0f);
            v = sinf((float)k * 2.0f * 3.14159265f * f / RATE) * env;
        }
        s_chirp[i] = (int16_t)(v * 11000.0f);
    }
}

/* ── State ─────────────────────────────────────────────────────────────── */

enum { RING_NONE = -1, RING_TIMER = 99 };

static int s_mode = M_CLOCK;
static int s_ring = RING_NONE;              /* alarm index, RING_TIMER, or RING_NONE */
static int64_t s_ring_start, s_ring_next;
static int s_al_sel, s_edit = -1, s_field;  /* alarm list selection, alarm being edited, its field */
static int s_w_sel;
static bool s_picking;
static int s_pick_sel;

static bool s_sw_run;
static int64_t s_sw_base, s_sw_start;
static int64_t s_laps[99];
static int s_nlaps;

static int s_tm_min = 5;
static bool s_tm_run;
static int64_t s_tm_left = 5 * 60 * 1000000LL, s_tm_last;

static void ring_start(int which, int64_t now)
{
    if (s_ring != RING_NONE) return;
    s_ring = which;
    s_ring_start = now;
    s_ring_next = now;
}

static void ring_stop(bool snooze, int64_t now)
{
    if (s_ring >= 0 && s_ring < MAX_ALARMS) {
        if (snooze) s_snooze[s_ring] = now + SNOOZE_US;
        else if (s_al[s_ring].days == 0) { s_al[s_ring].on = false; save(); }
    }
    s_ring = RING_NONE;
    if (s_audio) papp_audio_clear(s_audio);
}

/* Minutes from now to an alarm's next ring, within a week; -1 = off */
static int alarm_in(const alarm_t *a, const papp_datetime_t *t)
{
    if (!a->on) return -1;
    int now = t->hour * 60 + t->minute, at = a->hour * 60 + a->minute;
    for (int d = 0; d <= 7; d++) {
        int wd = (t->weekday + d) % 7, m = d * 1440 + at - now;
        if (m <= 0) continue;
        if (a->days == 0 || (a->days >> wd & 1)) return m;
    }
    return -1;
}

static void alarm_delete(int i)
{
    for (int k = i; k + 1 < s_nal; k++) { s_al[k] = s_al[k + 1]; s_snooze[k] = s_snooze[k + 1]; }
    s_nal--;
    s_snooze[s_nal] = 0;
    if (s_ring == i) s_ring = RING_NONE;
    s_edit = -1;
    if (s_al_sel > s_nal) s_al_sel = s_nal;
    save();
}

static void alarm_add(void)
{
    if (s_nal >= MAX_ALARMS) return;
    s_al[s_nal] = (alarm_t){ true, 7, 0, 0 };
    s_snooze[s_nal] = 0;
    s_edit = s_al_sel = s_nal++;
    s_field = 0;
    save();
}

static void city_remove(int i)
{
    for (int k = i; k + 1 < s_ncity; k++) s_city[k] = s_city[k + 1];
    s_ncity--;
    if (s_w_sel > s_ncity) s_w_sel = s_ncity;
    save();
}

static bool city_added(int c)
{
    for (int i = 0; i < s_ncity; i++)
        if (s_city[i] == c) return true;
    return false;
}

static void city_add(int c)
{
    if (s_ncity < MAX_CITIES && !city_added(c)) { s_city[s_ncity++] = c; save(); }
    s_picking = false;
}

/* ── Screens ───────────────────────────────────────────────────────────── */

static void draw_bar(void)
{
    for (int m = 0; m < M_N; m++) {
        int cx = 400 + (m - 2) * 120;
        bool sel = m == s_mode;
        disc(cx, 48, 30, pressed_color(H_MODE + m, sel ? C_BLUE : C_RAISED));
        icon(m, cx, 48, 30, sel ? C_TEXT : C_DIM);
        text_c(&F_SMALL, cx, 96, k_mode_name[m], sel ? C_TEXT : C_DIM);
        hit(cx - 56, 8, 112, 104, H_MODE + m, false);
    }
    round_button(756, 40, 24, IC_CLOSE, C_RAISED, C_TEXT, H_CLOSE, false);
    fill(24, 114, W - 48, 1, C_BORDER);
}

static void draw_clock(const papp_datetime_t *t, bool have)
{
    if (!have) {
        text_c(&F_LARGE, 400, 250, "--:--", C_FAINT);
        text_c(&F_MED, 400, 340, "The clock isn't set yet", C_DIM);
        text_c(&F_SMALL, 400, 380, "Connect to Wi-Fi in the launcher's Settings", C_FAINT);
        return;
    }
    char hm[8], sec[4], buf[64];
    snprintf(hm, sizeof(hm), "%02d:%02d", t->hour, t->minute);
    snprintf(sec, sizeof(sec), "%02d", t->second);
    int w1 = text_w(&F_HUGE, hm), w2 = text_w(&F_LARGE, sec), x = 400 - (w1 + 14 + w2) / 2;
    int top = line_top(&F_HUGE, 236);
    text(&F_HUGE, x, top, hm, C_TEXT);
    /* seconds on the digits' baseline */
    const cf_glyph_t *g = glyph(&F_HUGE, '0'), *s = glyph(&F_LARGE, '0');
    text(&F_LARGE, x + w1 + 14, top + g->y + g->h - (s->y + s->h), sec, C_BLUE);

    snprintf(buf, sizeof(buf), "%s, %d %s %d", k_wday[t->weekday % 7], t->day, k_month[(t->month + 11) % 12], t->year);
    text_c(&F_MED, 400, 360, buf, C_DIM);

    int best = -1, bi = -1;
    for (int i = 0; i < s_nal; i++) {
        int m = alarm_in(&s_al[i], t);
        if (m >= 0 && (best < 0 || m < best)) { best = m; bi = i; }
    }
    if (bi >= 0) {
        int days = (t->hour * 60 + t->minute + best) / 1440;
        const char *when = days == 0 ? "today" : days == 1 ? "tomorrow" : k_wday[(t->weekday + days) % 7];
        snprintf(buf, sizeof(buf), "%02d:%02d %s", s_al[bi].hour, s_al[bi].minute, when);
        int w = text_w(&F_SMALL, buf) + 30, bx = 400 - w / 2;
        icon(IC_ALARM, bx + 10, 414, 20, C_ORANGE);
        text_l(&F_SMALL, bx + 30, 414, buf, C_ORANGE);
    }
}

static void draw_alarm_list(void)
{
    for (int i = 0; i < s_nal; i++) {
        int y = TOP + 8 + i * 62;
        alarm_t *a = &s_al[i];
        rrect(80, y, 640, 56, 14, pressed_color(H_AL_ROW + i, C_PANEL));
        if (i == s_al_sel) { rrect(78, y - 2, 644, 60, 16, C_FOCUS); rrect(81, y + 1, 638, 54, 13, C_PANEL); }
        char hm[8];
        snprintf(hm, sizeof(hm), "%02d:%02d", a->hour, a->minute);
        text_l(&F_LARGE, 104, y + 28, hm, a->on ? C_TEXT : C_FAINT);
        text_l(&F_SMALL, 280, y + 28, repeat_name(a->days), a->on ? C_DIM : C_FAINT);
        if (s_snooze[i]) text_l(&F_SMALL, 420, y + 28, "Snoozed", C_ORANGE);
        hit(80, y, 540, 56, H_AL_ROW + i, false);
        toggle(636, y + 11, a->on, H_AL_TOGGLE + i);
    }
    if (s_nal < MAX_ALARMS) {
        int y = TOP + 8 + s_nal * 62;
        if (s_al_sel == s_nal) rrect(78, y - 2, 644, 60, 16, C_FOCUS);
        rrect(80, y, 640, 56, 14, pressed_color(H_AL_ADD, C_RAISED));
        icon(IC_PLUS, 330, y + 28, 26, C_TEXT);
        text_l(&F_MED, 356, y + 28, "Add alarm", C_TEXT);
        hit(80, y, 640, 56, H_AL_ADD, false);
    }
    text_c(&F_SMALL, 400, 462, "Alarms ring while the Clock is open", C_FAINT);
}

static void draw_alarm_edit(void)
{
    alarm_t *a = &s_al[s_edit];
    char hh[4], mm[4];
    snprintf(hh, sizeof(hh), "%02d", a->hour);
    snprintf(mm, sizeof(mm), "%02d", a->minute);
    int cy = 236, wd = text_w(&F_BIG, "00"), wc = text_w(&F_BIG, ":");
    int x0 = 400 - (2 * wd + wc) / 2, hx = x0 + wd / 2, mx = x0 + wd + wc + wd / 2;
    int top = line_top(&F_BIG, cy);
    text(&F_BIG, x0, top, hh, s_field == 0 ? C_TEXT : C_DIM);
    text(&F_BIG, x0 + wd, top, ":", C_DIM);
    text(&F_BIG, x0 + wd + wc, top, mm, s_field == 1 ? C_TEXT : C_DIM);
    if (s_field < 2) fill((s_field ? mx : hx) - wd / 2 + 6, cy + 46, wd - 12, 4, C_FOCUS);
    round_button(hx, 156, 24, IC_UP, C_RAISED, C_TEXT, H_ED_HUP, true);
    round_button(hx, 318, 24, IC_DOWN, C_RAISED, C_TEXT, H_ED_HDN, true);
    round_button(mx, 156, 24, IC_UP, C_RAISED, C_TEXT, H_ED_MUP, true);
    round_button(mx, 318, 24, IC_DOWN, C_RAISED, C_TEXT, H_ED_MDN, true);
    for (int i = 0; i < 4; i++) {
        int x = 88 + i * 160, y = 362;
        bool sel = k_repeat[i].days == a->days;
        if (s_field == 2 && sel) rrect(x - 3, y - 3, 150, 46, 23, C_FOCUS);
        rrect(x, y, 144, 40, 20, pressed_color(H_ED_REP + i, sel ? C_BLUE : C_RAISED));
        text_c(&F_SMALL, x + 72, y + 20, k_repeat[i].name, sel ? C_TEXT : C_DIM);
        hit(x, y - 4, 144, 48, H_ED_REP + i, false);
    }
    button(88, 420, 180, 46, "Delete", C_RED, H_ED_DEL);
    button(532, 420, 180, 46, "Done", C_BLUE, H_ED_DONE);
}

static void fmt_sw(char *buf, size_t n, int64_t us)
{
    int cs = (int)(us / 10000 % 100), sec = (int)(us / 1000000);
    if (sec >= 3600) snprintf(buf, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
    else snprintf(buf, n, "%02d:%02d.%02d", sec / 60, sec % 60, cs);
}

static void draw_stopwatch(int64_t now)
{
    int64_t us = s_sw_base + (s_sw_run ? now - s_sw_start : 0);
    char buf[24];
    fmt_sw(buf, sizeof(buf), us);
    text_c(&F_BIG, 400, 210, buf, s_sw_run ? C_GREEN : C_TEXT);
    for (int k = 0; k < 3 && k < s_nlaps; k++) {
        int i = s_nlaps - 1 - k;
        char lap[16], t[24];
        snprintf(lap, sizeof(lap), "Lap %d", i + 1);
        fmt_sw(t, sizeof(t), s_laps[i] - (i ? s_laps[i - 1] : 0));
        text_l(&F_MED, 280, 300 + k * 40, lap, k ? C_FAINT : C_DIM);
        text_r(&F_MED, 520, 300 + k * 40, t, k ? C_FAINT : C_DIM);
    }
    round_label_button(110, 400, 52, s_sw_run ? "Lap" : "Reset", C_RAISED, H_SW_LAP);
    round_label_button(690, 400, 52, s_sw_run ? "Stop" : "Start", s_sw_run ? C_ORANGE : C_GREEN, H_SW_GO);
}

static void draw_timer(void)
{
    int sec = (int)((s_tm_left + 999999) / 1000000);
    float full = s_tm_min * 60.0f, frac = full > 0 ? sec / full : 0;
    arc(400, 300, 160, 12, 1.0f, C_RAISED);
    if (frac > 0) arc(400, 300, 160, 12, frac > 1 ? 1 : frac, s_tm_run ? C_GREEN : C_BLUE);
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", sec / 60, sec % 60);
    text_c(&F_BIG, 400, 290, buf, s_tm_run ? C_GREEN : s_tm_left == 0 ? C_RED : C_TEXT);
    if (!s_tm_run) {
        text_c(&F_SMALL, 400, 362, "minutes", C_FAINT);
        round_button(140, 300, 34, IC_MINUS, C_RAISED, C_TEXT, H_TM_MINUS, true);
        round_button(660, 300, 34, IC_PLUS, C_RAISED, C_TEXT, H_TM_PLUS, true);
    }
    round_label_button(100, 418, 50, "Reset", C_RAISED, H_TM_RESET);
    round_label_button(700, 418, 50, s_tm_run ? "Pause" : "Start", s_tm_run ? C_ORANGE : C_GREEN, H_TM_GO);
}

static void draw_world(const papp_datetime_t *t, bool have)
{
    int64_t utc;
    if (!papp_utc_time(&utc) || !have) {
        bool old = papp_launcher_abi() && !papp_utc_time(&utc) && have;
        text_c(&F_MED, 400, 270, old ? "The world clock needs launcher 0.27 or newer" : "The clock isn't set yet", C_DIM);
        text_c(&F_SMALL, 400, 310, old ? "Settings, Update" : "Connect to Wi-Fi in the launcher's Settings", C_FAINT);
        return;
    }
    int64_t local = days_civil(t->year, t->month, t->day) * 86400 + t->hour * 3600 + t->minute * 60 + t->second;
    int64_t local_off = (local - utc + 450) / 900 * 900;   /* a tick apart: round to 15 min */
    int64_t today = days_civil(t->year, t->month, t->day);
    for (int i = 0; i < s_ncity; i++) {
        int y = TOP + 6 + i * 56;
        int64_t coff = city_offset(s_city[i], utc), cl = utc + coff;
        int64_t cday = cl / 86400 - (cl % 86400 < 0);
        int cy, cm, cd, csec = (int)(cl - cday * 86400), chour = csec / 3600;
        civil(cday, &cy, &cm, &cd);
        int diff = (int)(coff - local_off) / 60;     /* minutes */
        char sub[48], d[24], hm[12];
        const char *day = cday == today ? "Today" : cday > today ? "Tomorrow" : "Yesterday";
        if (diff == 0) snprintf(d, sizeof(d), "same time");
        else if (diff % 60) snprintf(d, sizeof(d), "%c%d:%02d h", diff > 0 ? '+' : '-', abs(diff) / 60, abs(diff) % 60);
        else snprintf(d, sizeof(d), "%c%d h", diff > 0 ? '+' : '-', abs(diff) / 60);
        snprintf(sub, sizeof(sub), "%s, %s", day, d);
        snprintf(hm, sizeof(hm), "%02d:%02d", chour, csec / 60 % 60);
        uint16_t bg = pressed_color(H_W_ROW + i, C_PANEL);
        if (i == s_w_sel) rrect(78, y - 2, 644, 54, 15, C_FOCUS);
        rrect(80, y, 640, 50, 13, bg);
        sun_moon(112, y + 25, chour, bg);
        text_l(&F_MED, 140, y + 16, k_city[s_city[i]].name, C_TEXT);
        text_l(&F_SMALL, 140, y + 37, sub, C_DIM);
        text_r(&F_LARGE, 700, y + 25, hm, C_TEXT);
        hit(80, y, 640, 50, H_W_ROW + i, false);
    }
    if (s_ncity < MAX_CITIES) {
        int y = TOP + 6 + s_ncity * 56;
        if (s_w_sel == s_ncity) rrect(78, y - 2, 644, 54, 15, C_FOCUS);
        rrect(80, y, 640, 50, 13, pressed_color(H_W_ADD, C_RAISED));
        icon(IC_PLUS, 340, y + 25, 24, C_TEXT);
        text_l(&F_MED, 364, y + 25, "Add city", C_TEXT);
        hit(80, y, 640, 50, H_W_ADD, false);
    }
}

static void draw_picker(void)
{
    text_l(&F_MED, 30, TOP + 22, "Add a city", C_TEXT);
    button(600, TOP + 4, 176, 36, "Cancel", C_RAISED, H_PICK_CANCEL);
    for (int c = 0; c < CITIES; c++) {
        int x = 24 + (c % 4) * 190, y = TOP + 48 + (c / 4) * 34;
        bool have = city_added(c);
        if (c == s_pick_sel) rrect(x - 2, y - 2, 182, 34, 10, C_FOCUS);
        rrect(x, y, 178, 30, 8, pressed_color(H_PICK + c, have ? C_PANEL : C_RAISED));
        text_l(&F_SMALL, x + 12, y + 15, k_city[c].name, have ? C_FAINT : C_TEXT);
        if (!have) hit(x, y, 178, 30, H_PICK + c, false);
    }
}

static void draw_ring(int64_t now, const papp_datetime_t *t, bool have)
{
    darken(80);
    s_nhit = 0;                                 /* only the box's buttons now */
    rrect(150, 70, 500, 340, 24, C_PANEL);
    float pulse = 0.5f + 0.5f * sinf((float)((now - s_ring_start) / 1000) * 0.008f);
    disc(400, 150, 44 + pulse * 6, mix(C_RAISED, C_ORANGE, 70 + (int)(pulse * 150)));
    icon(s_ring == RING_TIMER ? IC_TIMER : IC_ALARM, 400, 150, 44, C_TEXT);
    bool alarm = s_ring != RING_TIMER;
    text_c(&F_LARGE, 400, 236, alarm ? "Alarm" : "Time's up", C_TEXT);
    char buf[32];
    if (alarm) snprintf(buf, sizeof(buf), "%02d:%02d", s_al[s_ring].hour, s_al[s_ring].minute);
    else snprintf(buf, sizeof(buf), "%d minute%s", s_tm_min, s_tm_min == 1 ? "" : "s");
    text_c(&F_MED, 400, 284, buf, C_DIM);
    (void)t; (void)have;
    if (alarm) {
        button(186, 330, 200, 54, "Snooze", C_ORANGE, H_RING_SNOOZE);
        button(414, 330, 200, 54, "Stop", C_BLUE, H_RING_STOP);
    } else {
        button(300, 330, 200, 54, "Stop", C_BLUE, H_RING_STOP);
    }
}

/* ── Actions (touch targets and pad) ───────────────────────────────────── */

static bool s_quit;

static void tm_set(int min)
{
    s_tm_min = min < 1 ? 1 : min > 99 ? 99 : min;
    s_tm_left = s_tm_min * 60 * 1000000LL;
}

static void act(int id, int64_t now)
{
    if (id < M_N) { s_mode = id; s_edit = -1; s_picking = false; return; }
    if (id == H_CLOSE) { s_quit = true; return; }
    if (id == H_RING_STOP) { ring_stop(false, now); return; }
    if (id == H_RING_SNOOZE) { ring_stop(true, now); return; }
    if (id == H_SW_GO) {
        if (s_sw_run) s_sw_base += now - s_sw_start;
        else s_sw_start = now;
        s_sw_run = !s_sw_run;
    } else if (id == H_SW_LAP) {
        if (s_sw_run) { if (s_nlaps < 99) s_laps[s_nlaps++] = s_sw_base + now - s_sw_start; }
        else { s_sw_base = 0; s_nlaps = 0; }
    } else if (id == H_TM_GO) {
        if (s_tm_left <= 0) tm_set(s_tm_min);
        s_tm_run = !s_tm_run;
        s_tm_last = now;
    } else if (id == H_TM_RESET) {
        s_tm_run = false;
        tm_set(s_tm_min);
    } else if (id == H_TM_MINUS) {
        if (!s_tm_run) tm_set(s_tm_min - 1);
    } else if (id == H_TM_PLUS) {
        if (!s_tm_run) tm_set(s_tm_min + 1);
    } else if (id >= H_AL_ROW && id < H_AL_ROW + MAX_ALARMS) {
        s_edit = s_al_sel = id - H_AL_ROW;
        s_field = 0;
    } else if (id >= H_AL_TOGGLE && id < H_AL_TOGGLE + MAX_ALARMS) {
        alarm_t *a = &s_al[id - H_AL_TOGGLE];
        a->on = !a->on;
        s_snooze[id - H_AL_TOGGLE] = 0;
        s_al_sel = id - H_AL_TOGGLE;
        save();
    } else if (id == H_AL_ADD) {
        alarm_add();
    } else if (s_edit >= 0 && id >= H_ED_HUP && id <= H_ED_DEL) {
        alarm_t *a = &s_al[s_edit];
        if (id == H_ED_HUP) { a->hour = (a->hour + 1) % 24; s_field = 0; }
        if (id == H_ED_HDN) { a->hour = (a->hour + 23) % 24; s_field = 0; }
        if (id == H_ED_MUP) { a->minute = (a->minute + 1) % 60; s_field = 1; }
        if (id == H_ED_MDN) { a->minute = (a->minute + 59) % 60; s_field = 1; }
        if (id == H_ED_DONE) { a->on = true; s_snooze[s_edit] = 0; s_edit = -1; save(); }
        if (id == H_ED_DEL) alarm_delete(s_edit);
    } else if (s_edit >= 0 && id >= H_ED_REP && id < H_ED_REP + 4) {
        s_al[s_edit].days = k_repeat[id - H_ED_REP].days;
        s_field = 2;
    } else if (id >= H_W_ROW && id < H_W_ROW + MAX_CITIES) {
        int i = id - H_W_ROW;
        s_w_sel = i;
        const char *items[] = { "Remove", "Keep" };
        if (papp_ui_menu(k_city[s_city[i]].name, items, 2, 1) == 0) city_remove(i);
    } else if (id == H_W_ADD) {
        s_picking = true;
        s_pick_sel = 0;
        while (s_pick_sel < CITIES - 1 && city_added(s_pick_sel)) s_pick_sel++;
    } else if (id == H_PICK_CANCEL) {
        s_picking = false;
    } else if (id >= H_PICK && id < H_PICK + CITIES) {
        city_add(id - H_PICK);
    }
}

#define DIRS (PAPP_BTN_UP | PAPP_BTN_DOWN | PAPP_BTN_LEFT | PAPP_BTN_RIGHT)

/* Held arrows repeat */
static uint32_t pad_repeat(uint32_t buttons, uint32_t press, int64_t now)
{
    static int64_t next;
    static uint32_t held;
    if (press & DIRS) { held = press & DIRS; next = now + 400000; return 0; }
    if (held && (buttons & held) == held && now >= next) { next = now + 90000; return held; }
    if ((buttons & held) != held) held = 0;
    return 0;
}

static void pad(uint32_t p, int64_t now)
{
    if (s_ring != RING_NONE) {
        if (p & PAPP_BTN_A) ring_stop(false, now);
        else if ((p & PAPP_BTN_B) && s_ring != RING_TIMER) ring_stop(true, now);
        return;
    }
    if (p & PAPP_BTN_L1) { act((s_mode + M_N - 1) % M_N, now); return; }
    if (p & PAPP_BTN_R1) { act((s_mode + 1) % M_N, now); return; }
    switch (s_mode) {
    case M_ALARM:
        if (s_edit >= 0) {
            alarm_t *a = &s_al[s_edit];
            if (p & PAPP_BTN_LEFT) s_field = (s_field + 2) % 3;
            if (p & PAPP_BTN_RIGHT) s_field = (s_field + 1) % 3;
            int d = (p & PAPP_BTN_UP) ? 1 : (p & PAPP_BTN_DOWN) ? -1 : 0;
            if (d && s_field == 0) a->hour = (uint8_t)((a->hour + 24 + d) % 24);
            if (d && s_field == 1) a->minute = (uint8_t)((a->minute + 60 + d) % 60);
            if (d && s_field == 2) {
                int r = 0;
                while (r < 3 && k_repeat[r].days != a->days) r++;
                a->days = k_repeat[(r + 4 + d) % 4].days;
            }
            if (p & (PAPP_BTN_A | PAPP_BTN_B)) act(H_ED_DONE, now);
            else if (p & PAPP_BTN_Y) act(H_ED_DEL, now);
            break;
        }
        {
            int last = s_nal < MAX_ALARMS ? s_nal : s_nal - 1;
            if ((p & PAPP_BTN_UP) && s_al_sel > 0) s_al_sel--;
            if ((p & PAPP_BTN_DOWN) && s_al_sel < last) s_al_sel++;
            if (p & PAPP_BTN_A) act(s_al_sel == s_nal ? H_AL_ADD : H_AL_TOGGLE + s_al_sel, now);
            else if ((p & PAPP_BTN_Y) && s_al_sel < s_nal) act(H_AL_ROW + s_al_sel, now);
        }
        break;
    case M_STOPWATCH:
        if (p & PAPP_BTN_A) act(H_SW_GO, now);
        if (p & PAPP_BTN_B) act(H_SW_LAP, now);
        break;
    case M_TIMER:
        if (p & PAPP_BTN_UP) act(H_TM_PLUS, now);
        if (p & PAPP_BTN_DOWN) act(H_TM_MINUS, now);
        if (p & PAPP_BTN_A) act(H_TM_GO, now);
        if (p & PAPP_BTN_B) act(H_TM_RESET, now);
        break;
    case M_WORLD:
        if (s_picking) {
            int c = s_pick_sel;
            if (p & PAPP_BTN_LEFT) c--;
            if (p & PAPP_BTN_RIGHT) c++;
            if (p & PAPP_BTN_UP) c -= 4;
            if (p & PAPP_BTN_DOWN) c += 4;
            if (c >= 0 && c < CITIES) s_pick_sel = c;
            if ((p & PAPP_BTN_A) && !city_added(s_pick_sel)) act(H_PICK + s_pick_sel, now);
            if (p & PAPP_BTN_B) s_picking = false;
            break;
        }
        {
            int last = s_ncity < MAX_CITIES ? s_ncity : s_ncity - 1;
            if ((p & PAPP_BTN_UP) && s_w_sel > 0) s_w_sel--;
            if ((p & PAPP_BTN_DOWN) && s_w_sel < last) s_w_sel++;
            if (p & PAPP_BTN_A) act(s_w_sel == s_ncity ? H_W_ADD : H_W_ROW + s_w_sel, now);
            else if ((p & PAPP_BTN_Y) && s_ncity < MAX_CITIES) act(H_W_ADD, now);
        }
        break;
    }
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *s = papp_disp_surface_create(&cfg);
    if (!s) return 1;
    papp_audio_cfg_t ac = { .rate = RATE, .channels = 1, .format = PAPP_AUDIO_S16 };
    s_audio = papp_audio_open(&ac);
    chirp_make();
    load();

    uint32_t prev = 0;
    bool down = false, repeated = false, wait_release = false;
    int down_id = -1, last_key = -1;
    int64_t rep_next = 0;
    papp_touch_point_t tp = { 0 };

    while (!papp_exit_requested() && !s_quit) {
        int64_t now = papp_time_us();
        papp_gamepad_t gp = { 0 };
        papp_input_gamepad(0, &gp);
        uint32_t press = gp.buttons & ~prev;
        prev = gp.buttons;
        press |= pad_repeat(gp.buttons, press, now);

        papp_datetime_t t;
        bool have = papp_local_time(&t);

        /* Alarms: on the minute they're set for; snoozed ones when their time comes */
        if (have) {
            int key = t.day * 1440 + t.hour * 60 + t.minute;
            if (key != last_key) {
                if (last_key >= 0)
                    for (int i = 0; i < s_nal; i++) {
                        alarm_t *a = &s_al[i];
                        if (a->on && a->hour == t.hour && a->minute == t.minute &&
                            (a->days == 0 || (a->days >> t.weekday & 1)))
                            ring_start(i, now);
                    }
                last_key = key;
            }
        }
        for (int i = 0; i < s_nal; i++)
            if (s_snooze[i] && now >= s_snooze[i] && s_ring == RING_NONE) { s_snooze[i] = 0; ring_start(i, now); }
        if (s_tm_run) {
            s_tm_left -= now - s_tm_last;
            s_tm_last = now;
            if (s_tm_left <= 0) { s_tm_left = 0; s_tm_run = false; ring_start(RING_TIMER, now); }
        }
        if (s_ring != RING_NONE) {
            if (now - s_ring_start > RING_US) ring_stop(false, now);
            else if (now >= s_ring_next && s_audio && s_chirp) {
                papp_audio_write(s_audio, s_chirp, s_chirp_n, 0);
                s_ring_next = now + 1000000;
            }
        }

        /* Touch: buttons act when released on them; arrows and +/- act at once and repeat */
        papp_touch_point_t cur;
        bool touch = papp_disp_touch(s, &cur, 1) > 0;
        if (wait_release) {
            wait_release = touch;
        } else if (touch) {
            tp = cur;
            if (!down) {
                down = true;
                repeated = false;
                const hit_t *h = hit_at(cur.x, cur.y);
                down_id = h ? h->id : -1;
                if (h && h->rep) { act(down_id, now); repeated = true; rep_next = now + 400000; }
            } else if (repeated && down_id >= 0 && now >= rep_next) {
                act(down_id, now);
                rep_next = now + 80000;
            }
        } else if (down) {
            down = false;
            const hit_t *h = hit_at(tp.x, tp.y);
            if (!repeated && h && h->id == down_id) act(down_id, now);
            down_id = -1;
        }
        s_pressed = down ? down_id : -1;

        if (s_ring == RING_NONE && (press & (PAPP_BTN_X | PAPP_BTN_START))) {
            static const char *const items[] = { "Clock", "Alarm", "Stopwatch", "Timer", "World clock", "Exit" };
            int r = papp_ui_menu("Clock", items, 6, s_mode);
            if (r == 5) break;
            if (r >= 0) act(r, now);
            prev = 0xFFFFFFFFu;                 /* ignore what closed the menu until released */
            down = false;
            wait_release = true;
            continue;
        }
        pad(press, now);

        s_px = papp_disp_lock(s, &s_stride);
        s_nhit = 0;
        fill(0, 0, W, H, C_BG);
        draw_bar();
        switch (s_mode) {
        case M_CLOCK: draw_clock(&t, have); break;
        case M_ALARM: if (s_edit >= 0) draw_alarm_edit(); else draw_alarm_list(); break;
        case M_STOPWATCH: draw_stopwatch(now); break;
        case M_TIMER: draw_timer(); break;
        case M_WORLD: if (s_picking) draw_picker(); else draw_world(&t, have); break;
        }
        if (s_ring != RING_NONE) draw_ring(now, &t, have);
        papp_disp_present(s);
    }
    if (s_audio) papp_audio_close(s_audio);
    if (s_chirp) papp_free(s_chirp);
    papp_disp_surface_destroy(s);
    return 0;
}
