/*
 * Calculator — scientific calculator, function graphs and currency conversion.
 *
 * Calc      expressions with precedence, implicit multiplication (2pi, 3(4+1)), sin cos tan and their
 *           inverses / hyperbolic forms (2nd, hyp), ln log e^x 10^x, powers and roots, n!, %, Ans,
 *           degrees or radians. A live preview shows the result while typing.
 * Graph     up to three functions of x; drag the plot to pan, + / - to zoom, tap to trace.
 * Currency  rates from exchangerate-api.com (open access, no key), cached on the card so the
 *           converter works offline; the amount can be an expression (12*3.5).
 *
 * Touch: the tabs at the top pick the mode, X closes. USB keyboard: type the expression, Enter =,
 * Backspace, Esc clears. Pad: D-pad moves between keys, A presses, B deletes, Y clears,
 * L1 / R1 mode, START or X menu; on the graph the left stick pans and L2 / R2 zoom.
 * Settings and rates: /sd/data/tool.calc/. Argument "selftest": checks the parser, exit code =
 * failures.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "papp/papp.h"
#include "edu_ui.h"

PAPP_MANIFEST(.id = "tool.calc", .name = "Calculator", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define DATA_DIR  "/sd/data/tool.calc"
#define SETTINGS  DATA_DIR "/calc.txt"
#define RATES     DATA_DIR "/rates.txt"
#define RATES_URL "https://open.er-api.com/v6/latest/USD"
#define RATES_AGE (6 * 3600)                      /* refresh older rates when the tab opens */

#define C_FOCUS   UI_RGB(0x5FA8FF)
#define C_KEY     UI_RGB(0x16274A)
#define C_OP      UI_RGB(0x223A66)
#define C_FN      UI_RGB(0x12203C)

enum { T_CALC, T_GRAPH, T_CUR, T_N };
static const char *const k_tab[T_N] = { "Calc", "Graph", "Currency" };
static int s_tab;
static bool s_quit;

/* ── Parser ─────────────────────────────────────────────────────────────
 * expr  := term (('+' | '-') term)*
 * term  := unary (('*' | '/') unary | unary)*          the second form: implicit multiplication
 * unary := ('-' | '+') unary | power
 * power := post ('^' unary)?                           right associative, -2^2 = -4
 * post  := prim ('!' | '%')*
 * prim  := number [E [+-] digits] | '(' expr [')'] | name '(' expr [')'] | pi | e | x | Ans
 * A missing ')' at the end is closed automatically. */

static bool s_deg = true;
static bool s_gdeg;                                   /* the graph's own angle mode: radians by default */
static double s_ans;

typedef struct { const char *p; double x; bool has_x; bool err; } pz_t;

static double deg_in(double v) { return s_deg ? v * (M_PI / 180.0) : v; }
static double deg_out(double v) { return s_deg ? v * (180.0 / M_PI) : v; }
static double snap(double v) { return fabs(v) < 1e-15 ? 0.0 : v; }

static double f_sin(double v)
{
    if (s_deg) {
        double r = fmod(v, 360.0);
        if (r < 0) r += 360.0;
        if (r == 0 || r == 180) return 0;
        if (r == 90) return 1;
        if (r == 270) return -1;
    }
    return snap(sin(deg_in(v)));
}

static double f_cos(double v)
{
    if (s_deg) {
        double r = fmod(v, 360.0);
        if (r < 0) r += 360.0;
        if (r == 90 || r == 270) return 0;
        if (r == 0) return 1;
        if (r == 180) return -1;
    }
    return snap(cos(deg_in(v)));
}

static double f_tan(double v)
{
    if (s_deg) {
        double r = fmod(v, 180.0);
        if (r < 0) r += 180.0;
        if (r == 0) return 0;
        if (r == 90) return NAN;
    }
    return snap(tan(deg_in(v)));
}

static double f_asin(double v) { return deg_out(asin(v)); }
static double f_acos(double v) { return deg_out(acos(v)); }
static double f_atan(double v) { return deg_out(atan(v)); }
static double f_log(double v) { return log10(v); }
static double f_abs(double v) { return fabs(v); }

static double fact(double v)
{
    if (v < 0 && v == floor(v)) return NAN;
    if (v == floor(v) && v <= 170) {
        double r = 1;
        for (int i = 2; i <= (int)v; i++) r *= i;
        return r;
    }
    return tgamma(v + 1);
}

typedef double (*fn1_t)(double);
/* Longest names first: an identifier run is matched by its longest known prefix, so "pix" is pi*x */
static const struct { const char *name; fn1_t fn; } k_name[] = {
    { "asinh", asinh }, { "acosh", acosh }, { "atanh", atanh },
    { "asin", f_asin }, { "acos", f_acos }, { "atan", f_atan },
    { "sinh", sinh }, { "cosh", cosh }, { "tanh", tanh }, { "sqrt", sqrt }, { "cbrt", cbrt },
    { "sin", f_sin }, { "cos", f_cos }, { "tan", f_tan }, { "abs", f_abs }, { "exp", exp }, { "log", f_log },
    { "Ans", NULL }, { "ans", NULL }, { "ln", log }, { "pi", NULL }, { "e", NULL }, { "x", NULL },
};
#define NAMES ((int)(sizeof(k_name) / sizeof(k_name[0])))

static double p_expr(pz_t *z);
static double p_unary(pz_t *z);

static void skip(pz_t *z) { while (*z->p == ' ') z->p++; }
static bool is_alpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static bool is_dig(int c) { return c >= '0' && c <= '9'; }
static bool starts_prim(int c) { return is_dig(c) || c == '.' || c == '(' || is_alpha(c); }

static double p_number(pz_t *z)
{
    const char *p = z->p;
    double v = 0;
    bool any = false;
    while (is_dig(*p)) { v = v * 10 + (*p++ - '0'); any = true; }
    if (*p == '.') {
        p++;
        double f = 0.1;
        while (is_dig(*p)) { v += (*p++ - '0') * f; f *= 0.1; any = true; }
    }
    if (!any || *p == '.') { z->err = true; return 0; }       /* 1.2.3 */
    if (*p == 'E') {
        const char *q = p + 1;
        int sign = 1;
        if (*q == '+' || *q == '-') sign = *q++ == '-' ? -1 : 1;
        if (!is_dig(*q)) { z->err = true; return 0; }
        int e = 0;
        while (is_dig(*q)) e = e * 10 + (*q++ - '0');
        v *= pow(10.0, sign * e);
        p = q;
    }
    z->p = p;
    return v;
}

static double p_paren(pz_t *z)                     /* after '(' */
{
    double v = p_expr(z);
    skip(z);
    if (*z->p == ')') z->p++;
    else if (*z->p) z->err = true;
    return v;
}

static double p_prim(pz_t *z)
{
    skip(z);
    char c = *z->p;
    if (is_dig(c) || c == '.') return p_number(z);
    if (c == '(') { z->p++; return p_paren(z); }
    if (is_alpha(c)) {
        for (int i = 0; i < NAMES; i++) {
            size_t n = strlen(k_name[i].name);
            if (strncmp(z->p, k_name[i].name, n)) continue;
            z->p += n;
            const char *nm = k_name[i].name;
            if (k_name[i].fn) {
                skip(z);
                if (*z->p != '(') { z->err = true; return 0; }
                z->p++;
                return k_name[i].fn(p_paren(z));
            }
            if (!strcmp(nm, "pi")) return M_PI;
            if (!strcmp(nm, "e")) return M_E;
            if (!strcmp(nm, "x")) { if (!z->has_x) z->err = true; return z->x; }
            return s_ans;                              /* Ans */
        }
    }
    z->err = true;
    return 0;
}

static double p_post(pz_t *z)
{
    double v = p_prim(z);
    for (;;) {
        skip(z);
        if (*z->p == '!') { z->p++; v = fact(v); }
        else if (*z->p == '%') { z->p++; v /= 100.0; }
        else return v;
    }
}

static double p_power(pz_t *z)
{
    double b = p_post(z);
    skip(z);
    if (*z->p == '^') {
        z->p++;
        return pow(b, p_unary(z));
    }
    return b;
}

static double p_unary(pz_t *z)
{
    skip(z);
    if (*z->p == '-') { z->p++; return -p_unary(z); }
    if (*z->p == '+') { z->p++; return p_unary(z); }
    return p_power(z);
}

static double p_term(pz_t *z)
{
    double v = p_unary(z);
    while (!z->err) {
        skip(z);
        char c = *z->p;
        if (c == '*') { z->p++; v *= p_unary(z); }
        else if (c == '/') { z->p++; v /= p_unary(z); }
        else if (starts_prim(c)) v *= p_unary(z);
        else break;
    }
    return v;
}

static double p_expr(pz_t *z)
{
    double v = p_term(z);
    while (!z->err) {
        skip(z);
        if (*z->p == '+') { z->p++; v += p_term(z); }
        else if (*z->p == '-') { z->p++; v -= p_term(z); }
        else break;
    }
    return v;
}

/* NAN on a syntax error or an undefined result */
static double eval_x(const char *s, bool has_x, double x)
{
    pz_t z = { .p = s, .x = x, .has_x = has_x };
    skip(&z);
    if (!*z.p) return NAN;
    double v = p_expr(&z);
    skip(&z);
    if (z.err || *z.p) return NAN;
    return isfinite(v) ? v : NAN;
}

static double eval(const char *s) { return eval_x(s, false, 0); }

/* Well formed (whatever its value): for marking a graph function red */
static bool syntax_ok(const char *s, bool has_x)
{
    pz_t z = { .p = s, .x = 1.0, .has_x = has_x };
    p_expr(&z);
    skip(&z);
    return !z.err && !*z.p;
}

/* ── Number formatting ─────────────────────────────────────────────────── */

static void trim_zeros(char *b)
{
    char *e = strchr(b, 'e');
    char tail[16] = "";
    if (e) { snprintf(tail, sizeof(tail), "%s", e); *e = 0; }
    if (strchr(b, '.')) {
        size_t n = strlen(b);
        while (n && b[n - 1] == '0') b[--n] = 0;
        if (n && b[n - 1] == '.') b[--n] = 0;
    }
    if (e) {                                         /* e+12 -> E12, e-05 -> E-5 */
        int ex = atoi(tail + 1);
        size_t n = strlen(b);
        snprintf(b + n, 16, "E%d", ex);
    }
}

/* sig significant digits; plain notation from 1e-9 to 1e12, else 1.5E-12 */
static void fmt_num(double v, char *b, int cap, int sig)
{
    if (!isfinite(v)) { snprintf(b, cap, "Error"); return; }
    double a = fabs(v);
    if (a == 0 || a < 1e-300) { snprintf(b, cap, "0"); return; }
    if (a >= 1e12 || a < 1e-9) {
        snprintf(b, cap, "%.*e", sig - 1, v);
    } else {
        int mag = (int)floor(log10(a));
        int dec = sig - 1 - mag;
        if (dec < 0) dec = 0;
        if (dec > 15) dec = 15;
        snprintf(b, cap, "%.*f", dec, v);
    }
    trim_zeros(b);
    if (!strcmp(b, "-0")) snprintf(b, cap, "0");
}

/* Money: two decimals with thousands separators */
static void fmt_money(double v, char *b, int cap)
{
    if (!isfinite(v)) { snprintf(b, cap, "-"); return; }
    double a = fabs(v);
    if (a >= 1e15 || (a > 0 && a < 0.01)) { fmt_num(v, b, cap, 6); return; }
    char t[40];
    snprintf(t, sizeof(t), "%.2f", a);
    char *dot = strchr(t, '.');
    int ni = (int)(dot - t), o = 0;
    if (v < 0 && a >= 0.005) b[o++] = '-';
    for (int i = 0; i < ni && o < cap - 4; i++) {
        b[o++] = t[i];
        if ((ni - 1 - i) % 3 == 0 && i != ni - 1) b[o++] = ',';
    }
    snprintf(b + o, cap - o, "%s", dot);
}

/* ── Rich text: labels and expressions with symbols the font lacks ─────────
 * Bytes 1..8 are drawn as shapes; in labels byte 16 starts a superscript to the end. */

enum { G_PI = 1, G_SQRT, G_MUL, G_DIV, G_CBRT, G_MINUS, G_BS, G_SWAP };
#define SUP "\x10"

static float cap_of(const ui_font_t *f) { return ui_line_h(f) * 0.6f; }

static float glyph_adv(int g, float c)
{
    switch (g) {
    case G_PI:    return c * 0.95f;
    case G_SQRT:  return c * 0.85f;
    case G_CBRT:  return c * 0.85f;
    case G_BS:    return c * 1.5f;
    case G_SWAP:  return c * 1.2f;
    default:      return c * 0.95f;              /* x / - */
    }
}

static void glyph_draw(int g, float x, float cy, float c, uint16_t col)
{
    float t = c * 0.11f;
    if (t < 1.6f) t = 1.6f;
    float m = c * 0.15f;                         /* side bearing */
    switch (g) {
    case G_PI: {
        float y0 = cy - c * 0.36f, x0 = x + m, x1 = x + c * 0.95f - m;
        ui_line(x0, y0, x1, y0, t, col);
        ui_line(x0 + (x1 - x0) * 0.3f, y0, x0 + (x1 - x0) * 0.22f, cy + c * 0.5f, t, col);
        ui_line(x0 + (x1 - x0) * 0.72f, y0, x0 + (x1 - x0) * 0.74f, cy + c * 0.42f, t, col);
        ui_line(x0 + (x1 - x0) * 0.74f, cy + c * 0.42f, x0 + (x1 - x0) * 0.92f, cy + c * 0.5f, t, col);
        break;
    }
    case G_SQRT:
    case G_CBRT: {
        float x0 = x + m * 0.5f;
        ui_line(x0, cy + c * 0.05f, x0 + c * 0.16f, cy - c * 0.02f, t * 0.9f, col);
        ui_line(x0 + c * 0.16f, cy - c * 0.02f, x0 + c * 0.34f, cy + c * 0.52f, t, col);
        ui_line(x0 + c * 0.34f, cy + c * 0.52f, x0 + c * 0.6f, cy - c * 0.62f, t, col);
        ui_line(x0 + c * 0.6f, cy - c * 0.62f, x + c * 0.85f + c * 0.25f, cy - c * 0.62f, t, col);
        if (g == G_CBRT) ui_text_c(ui_small, (int)(x0 + c * 0.1f), (int)(cy - c * 0.5f), "3", col);
        break;
    }
    case G_MUL: {
        float a = c * 0.27f, xc = x + c * 0.475f;
        ui_line(xc - a, cy - a, xc + a, cy + a, t, col);
        ui_line(xc - a, cy + a, xc + a, cy - a, t, col);
        break;
    }
    case G_DIV: {
        float a = c * 0.34f, xc = x + c * 0.475f;
        ui_line(xc - a, cy, xc + a, cy, t, col);
        ui_disc(xc, cy - c * 0.3f, t * 0.8f, col, 255);
        ui_disc(xc, cy + c * 0.3f, t * 0.8f, col, 255);
        break;
    }
    case G_MINUS: {
        float a = c * 0.3f, xc = x + c * 0.475f;
        ui_line(xc - a, cy, xc + a, cy, t, col);
        break;
    }
    case G_BS: {
        float x0 = x + m, x1 = x + c * 1.5f - m, h = c * 0.45f, tip = x0 + c * 0.35f;
        ui_line(x0, cy, tip, cy - h, t, col);
        ui_line(x0, cy, tip, cy + h, t, col);
        ui_line(tip, cy - h, x1, cy - h, t, col);
        ui_line(tip, cy + h, x1, cy + h, t, col);
        ui_line(x1, cy - h, x1, cy + h, t, col);
        float xc = (tip + x1) / 2 + c * 0.04f, a = c * 0.17f;
        ui_line(xc - a, cy - a, xc + a, cy + a, t, col);
        ui_line(xc - a, cy + a, xc + a, cy - a, t, col);
        break;
    }
    case G_SWAP: {
        float xa = x + c * 0.38f, xb = x + c * 0.82f, h = c * 0.5f, a = c * 0.2f;
        ui_line(xa, cy + h, xa, cy - h, t, col);
        ui_line(xa, cy - h, xa - a, cy - h + a, t, col);
        ui_line(xa, cy - h, xa + a, cy - h + a, t, col);
        ui_line(xb, cy - h, xb, cy + h, t, col);
        ui_line(xb, cy + h, xb - a, cy + h - a, t, col);
        ui_line(xb, cy + h, xb + a, cy + h - a, t, col);
        break;
    }
    }
}

/* Walks a rich string: draws when draw is true, returns the width */
static float rich_walk(const ui_font_t *f, float x, int cy, const char *s, uint16_t col, bool draw)
{
    float x0 = x, c = cap_of(f);
    char run[96];
    int n = 0;
    const ui_font_t *cf = f;
    int ccy = cy;
    for (const char *p = s;; p++) {
        unsigned char ch = (unsigned char)*p;
        bool flush = ch == 0 || ch < 32;
        if (flush && n) {
            run[n] = 0;
            if (draw) ui_text_l(cf, (int)x, ccy, run, col);
            x += ui_text_w(cf, run);
            n = 0;
        }
        if (!ch) break;
        if (ch == 16) {                                  /* superscript */
            cf = ui_small;
            ccy = cy - (int)(c * 0.55f);
            c = cap_of(ui_small);
        } else if (ch < 16) {
            if (draw) glyph_draw(ch, x, ccy, c, col);
            x += glyph_adv(ch, c);
        } else if (n < (int)sizeof(run) - 1) {
            run[n++] = (char)ch;
        }
    }
    return x - x0;
}

static int rich_w(const ui_font_t *f, const char *s) { return (int)(rich_walk(f, 0, 0, s, 0, false) + 0.5f); }
static void rich_l(const ui_font_t *f, int x, int cy, const char *s, uint16_t c) { rich_walk(f, x, cy, s, c, true); }
static void rich_c(const ui_font_t *f, int cx, int cy, const char *s, uint16_t c) { rich_l(f, cx - rich_w(f, s) / 2, cy, s, c); }

/* Right-aligned at x_r, at most w wide: the start is cut ("..") when it doesn't fit */
static void rich_r_fit(const ui_font_t *f, int x_r, int cy, const char *s, int w, uint16_t c)
{
    int i = 0, n = (int)strlen(s), dots = ui_text_w(f, "..");
    if (rich_w(f, s) > w) {
        while (i < n && rich_w(f, s + i) > w - dots) i++;
        ui_text_l(f, x_r - rich_w(f, s + i) - dots, cy, "..", C_DIM);
    }
    rich_l(f, x_r - rich_w(f, s + i), cy, s + i, c);
}

/* Internal expression -> display: pi, sqrt, cbrt, * / - as symbols */
static void to_display(const char *e, char *out, int cap)
{
    int o = 0;
    while (*e && o < cap - 1) {
        if (!strncmp(e, "pi", 2)) { out[o++] = G_PI; e += 2; }
        else if (!strncmp(e, "sqrt", 4)) { out[o++] = G_SQRT; e += 4; }
        else if (!strncmp(e, "cbrt", 4)) { out[o++] = G_CBRT; e += 4; }
        else if (*e == '*') { out[o++] = G_MUL; e++; }
        else if (*e == '/') { out[o++] = G_DIV; e++; }
        else if (*e == '-') { out[o++] = G_MINUS; e++; }
        else out[o++] = *e++;
    }
    out[o] = 0;
}

/* ── Keys: touch targets that the pad can also walk ───────────────────── */

enum {
    K_D0 = 1, K_D9 = 10, K_DOT, K_ADD, K_SUB, K_MUL, K_DIV, K_POW, K_SQ, K_SQRT, K_LP, K_RP,
    K_SIN, K_COS, K_TAN, K_LN, K_LOG, K_PI, K_E, K_FACT, K_INV, K_EE, K_ANS, K_ABS, K_DEG,
    K_2ND, K_HYP, K_BS, K_AC, K_EQ, K_X, K_PCT,
    G_FN0 = 60, G_ON0 = 63, G_ZIN = 66, G_ZOUT, G_HOME, G_DEG,
    CU_FROM = 80, CU_TO, CU_SWAP, CU_UPDATE, CU_WIFI,
    P_PREV = 90, P_NEXT, P_CANCEL, P_CELL = 100,     /* 100..129 */
};

typedef struct { int16_t x, y, w, h; int id; } navkey_t;
static navkey_t s_nav[96];
static int s_nnav, s_focus = K_D0;
static bool s_pad_mode;                              /* a pad button was used: show the focus */
static bool s_2nd, s_hyp;

static void key(int x, int y, int w, int h, int id, const ui_font_t *f, const char *label, uint16_t bg,
                uint16_t fg)
{
    if (s_nnav < (int)(sizeof(s_nav) / sizeof(s_nav[0])))
        s_nav[s_nnav++] = (navkey_t){ (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h, id };
    if (s_pad_mode && s_focus == id) ui_rrect(x - 4, y - 4, w + 8, h + 8, 16, C_FOCUS);
    ui_rrect(x, y, w, h, 12, ui_pressed_color(id, bg));
    rich_c(f, x + w / 2, y + h / 2, label, fg);
    ui_hit(x, y, w, h, id, id == K_BS ? UI_REPEAT : UI_TAP);
}

/* D-pad: the nearest key whose centre lies in that direction */
static void nav_move(int dx, int dy)
{
    const navkey_t *cur = NULL;
    for (int i = 0; i < s_nnav; i++) if (s_nav[i].id == s_focus) cur = &s_nav[i];
    if (!cur) { if (s_nnav) s_focus = s_nav[0].id; return; }
    float cx = cur->x + cur->w / 2.0f, cy = cur->y + cur->h / 2.0f;
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; pass++) {      /* first only keys within ~60 degrees */
        float bestd = 1e9f;
        for (int i = 0; i < s_nnav; i++) {
            const navkey_t *k = &s_nav[i];
            if (k == cur) continue;
            float kx = k->x + k->w / 2.0f, ky = k->y + k->h / 2.0f;
            float along = dx ? (kx - cx) * dx : (ky - cy) * dy;
            float across = dx ? fabsf(ky - cy) : fabsf(kx - cx);
            if (along < 8 || (pass == 0 && across > along * 2)) continue;
            float d = along + across * 2.5f;
            if (d < bestd) { bestd = d; best = i; }
        }
    }
    if (best >= 0) s_focus = s_nav[best].id;
}

/* ── Editing an expression (shared by the three tabs) ─────────────────── */

static void ins(char *b, int cap, const char *t)
{
    size_t n = strlen(b), m = strlen(t);
    if (n + m < (size_t)cap) memcpy(b + n, t, m + 1);
}

static void backspace(char *b)
{
    int n = (int)strlen(b);
    if (!n) return;
    if (b[n - 1] == '(' && n >= 2 && is_alpha(b[n - 2])) {
        n--;
        while (n && is_alpha(b[n - 1])) n--;
    } else if (is_alpha(b[n - 1]) && b[n - 1] != 'E') {
        while (n && is_alpha(b[n - 1]) && b[n - 1] != 'E') n--;
    } else {
        n--;
    }
    b[n] = 0;
}

/* The text a key adds, or NULL for keys that don't insert */
static const char *key_text(int id, char *tmp)
{
    if (id >= K_D0 && id <= K_D9) { tmp[0] = (char)('0' + id - K_D0); tmp[1] = 0; return tmp; }
    switch (id) {
    case K_DOT:  return ".";
    case K_ADD:  return "+";
    case K_SUB:  return "-";
    case K_MUL:  return "*";
    case K_DIV:  return "/";
    case K_POW:  return "^";
    case K_SQ:   return s_2nd ? "^3" : "^2";
    case K_SQRT: return s_2nd ? "cbrt(" : "sqrt(";
    case K_LP:   return "(";
    case K_RP:   return ")";
    case K_SIN: case K_COS: case K_TAN:
        snprintf(tmp, 12, "%s%s%s(", s_2nd ? "a" : "", id == K_SIN ? "sin" : id == K_COS ? "cos" : "tan",
                 s_hyp ? "h" : "");
        return tmp;
    case K_LN:   return s_2nd ? "exp(" : "ln(";
    case K_LOG:  return s_2nd ? "10^(" : "log(";
    case K_PI:   return "pi";
    case K_E:    return "e";
    case K_FACT: return "!";
    case K_INV:  return "^(-1)";
    case K_EE:   return "E";
    case K_ANS:  return "Ans";
    case K_ABS:  return "abs(";
    case K_X:    return "x";
    case K_PCT:  return "%";
    }
    return NULL;
}

/* Keys that continue a finished result ("Ans +") rather than start a new expression */
static bool continues(int id)
{
    return id == K_ADD || id == K_SUB || id == K_MUL || id == K_DIV || id == K_POW || id == K_SQ ||
           id == K_FACT || id == K_INV || id == K_PCT;
}

/* Common edits; returns true if the key was handled */
static bool edit(char *b, int cap, int id)
{
    char tmp[16];
    const char *t = key_text(id, tmp);
    if (t) { ins(b, cap, t); s_2nd = s_hyp = false; return true; }
    if (id == K_BS) { backspace(b); return true; }
    if (id == K_AC) { b[0] = 0; s_2nd = s_hyp = false; return true; }
    if (id == K_2ND) { s_2nd = !s_2nd; return true; }
    if (id == K_HYP) { s_hyp = !s_hyp; return true; }
    if (id == K_DEG) { s_deg = !s_deg; return true; }
    if (id == G_DEG) { s_gdeg = !s_gdeg; return true; }
    return false;
}

/* Labels: 2nd and hyp change some of them */
static const char *label_of(int id, char *tmp)
{
    if (id >= K_D0 && id <= K_D9) { tmp[0] = (char)('0' + id - K_D0); tmp[1] = 0; return tmp; }
    switch (id) {
    case K_DOT:  return ".";
    case K_ADD:  return "+";
    case K_SUB:  return "\x06";
    case K_MUL:  return "\x03";
    case K_DIV:  return "\x04";
    case K_POW:  return "x" SUP "y";
    case K_SQ:   return s_2nd ? "x" SUP "3" : "x" SUP "2";
    case K_SQRT: return s_2nd ? "\x05" : "\x02";
    case K_LP:   return "(";
    case K_RP:   return ")";
    case K_SIN: case K_COS: case K_TAN:
        snprintf(tmp, 16, "%s%s%s", id == K_SIN ? "sin" : id == K_COS ? "cos" : "tan", s_hyp ? "h" : "",
                 s_2nd ? SUP "-1" : "");
        return tmp;
    case K_LN:   return s_2nd ? "e" SUP "x" : "ln";
    case K_LOG:  return s_2nd ? "10" SUP "x" : "log";
    case K_PI:   return "\x01";
    case K_E:    return "e";
    case K_FACT: return "n!";
    case K_INV:  return "x" SUP "-1";
    case K_EE:   return "EE";
    case K_ANS:  return "Ans";
    case K_ABS:  return "|x|";
    case K_DEG:  return s_deg ? "DEG" : "RAD";
    case K_2ND:  return "2nd";
    case K_HYP:  return "hyp";
    case K_BS:   return "\x07";
    case K_AC:   return "AC";
    case K_EQ:   return "=";
    case K_X:    return "x";
    case K_PCT:  return "%";
    }
    return "?";
}

static uint16_t key_bg(int id)
{
    if (id == K_EQ) return C_BLUE;
    if (id == K_AC) return UI_RGB(0x8A2A24);
    if (id == K_2ND) return s_2nd ? C_ORANGE : C_FN;
    if (id == K_HYP) return s_hyp ? C_ORANGE : C_FN;
    if ((id >= K_D0 && id <= K_D9) || id == K_DOT) return C_KEY;
    if (id == K_ADD || id == K_SUB || id == K_MUL || id == K_DIV || id == K_BS) return C_OP;
    return C_FN;
}

/* A grid of keys; ids 0 = gap, negative = previous key is that many columns wider */
static void key_grid(int x, int y, int cols, int rows, int kw, int kh, int gap, const int *ids,
                     const ui_font_t *f)
{
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int id = ids[r * cols + c];
            if (id <= 0) continue;
            int span = 1;
            while (c + span < cols && ids[r * cols + c + span] < 0) span++;
            char tmp[16];
            const char *lab = label_of(id, tmp);
            bool lone = (unsigned char)lab[0] < 16 && !lab[1];          /* one symbol: pi, root, x, ... */
            const ui_font_t *ff = (id >= K_D0 && id <= K_D9) || id == K_DOT || id == K_EQ || lone ? ui_med : f;
            if (lone && kh >= 60) ff = ui_large;
            key(x + c * (kw + gap), y + r * (kh + gap), kw * span + gap * (span - 1), kh, id, ff, lab,
                key_bg(id), C_TEXT);
        }
    }
}

/* ── Calc tab ───────────────────────────────────────────────────────────── */

static char s_expr[160];
static char s_last[160];                             /* the expression of the shown result */
static bool s_done;                                  /* s_expr is a result: a digit starts anew */
static bool s_error;

static void calc_key(int id)
{
    if (id == K_EQ) {
        if (!s_expr[0]) return;
        double v = eval(s_expr);
        if (isnan(v)) { s_error = true; return; }
        s_ans = v;
        snprintf(s_last, sizeof(s_last), "%s", s_expr);
        fmt_num(v, s_expr, sizeof(s_expr), 10);
        s_done = true;
        s_error = false;
        return;
    }
    s_error = false;
    if (s_done && id != K_2ND && id != K_HYP && id != K_DEG) {
        s_done = false;
        if (id == K_BS) { s_expr[0] = 0; s_last[0] = 0; return; }
        if (continues(id)) snprintf(s_expr, sizeof(s_expr), "Ans");
        else if (key_text(id, (char[16]){ 0 })) s_expr[0] = 0;
        s_last[0] = 0;
    }
    if (id == K_AC) s_last[0] = 0;
    edit(s_expr, sizeof(s_expr), id);
}

static void draw_calc(void)
{
    /* Display */
    ui_rrect(16, 112, 768, 92, 14, C_PANEL);
    char d[200], r[48];
    if (s_done) {
        to_display(s_last, d, sizeof(d));
        ins(d, sizeof(d), " =");
        rich_r_fit(ui_small, 764, 132, d, 720, C_DIM);
        to_display(s_expr, d, sizeof(d));
        rich_r_fit(ui_large, 764, 176, d, 720, C_TEXT);
    } else {
        to_display(s_expr, d, sizeof(d));
        if (!d[0]) { d[0] = '0'; d[1] = 0; }
        rich_r_fit(ui_med, 764, 140, d, 720, C_TEXT);
        double v = s_expr[0] ? eval(s_expr) : NAN;
        if (s_error) ui_text_r(ui_med, 764, 180, "Error", C_RED);
        else if (!isnan(v)) {
            fmt_num(v, r, sizeof(r), 10);
            if (strcmp(r, s_expr)) {
                char t[56];
                snprintf(t, sizeof(t), "= %s", r);
                ui_text_r(ui_med, 764, 180, t, C_DIM);
            }
        }
    }
    ui_text_l(ui_small, 32, 132, s_deg ? "DEG" : "RAD", C_FOCUS);
    if (s_2nd) ui_text_l(ui_small, 82, 132, "2nd", C_ORANGE);
    if (s_hyp) ui_text_l(ui_small, 128, 132, "hyp", C_ORANGE);

    static const int sci[] = {
        K_2ND, K_SIN, K_COS, K_TAN, K_PI,
        K_LN,  K_LOG, K_SQ,  K_POW, K_SQRT,
        K_LP,  K_RP,  K_FACT, K_INV, K_E,
        K_DEG, K_EE,  K_ANS, K_HYP, K_ABS,
    };
    static const int num[] = {
        K_D0 + 7, K_D0 + 8, K_D0 + 9, K_BS,  K_AC,
        K_D0 + 4, K_D0 + 5, K_D0 + 6, K_MUL, K_DIV,
        K_D0 + 1, K_D0 + 2, K_D0 + 3, K_ADD, K_SUB,
        K_D0,     -1,       K_DOT,    K_EQ,  -1,
    };
    key_grid(20, 216, 5, 4, 68, 58, 8, sci, ui_small);
    key_grid(412, 216, 5, 4, 68, 58, 8, num, ui_med);
}

/* ── Graph tab ──────────────────────────────────────────────────────────── */

#define PX 344
#define PY 112
#define PW 440
#define PH 360
#define NFN 3
static char s_fn[NFN][96] = { "sin(x)", "x^2/4-2", "" };
static bool s_fn_on[NFN] = { true, true, true };
static int s_sel;
static double s_x0 = -10, s_x1 = 10, s_y0 = -8.1818, s_y1 = 8.1818;
static const uint16_t k_fn_col[NFN] = { UI_RGB(0x5FA8FF), UI_RGB(0xF5A623), UI_RGB(0x3DDC84) };
static bool s_trace;
static double s_trace_x;

static double fn_eval(int i, double x)
{
    bool d = s_deg;
    s_deg = s_gdeg;
    double v = eval_x(s_fn[i], true, x);
    s_deg = d;
    return v;
}

static void view_home(void)
{
    s_x0 = -10; s_x1 = 10;
    double h = (s_x1 - s_x0) * PH / PW / 2;
    s_y0 = -h; s_y1 = h;
}

static void view_zoom(double k)                     /* k < 1 zooms in, about the centre */
{
    double cx = (s_x0 + s_x1) / 2, cy = (s_y0 + s_y1) / 2;
    double hw = (s_x1 - s_x0) / 2 * k, hh = (s_y1 - s_y0) / 2 * k;
    if (hw < 1e-6 || hw > 1e7) return;
    s_x0 = cx - hw; s_x1 = cx + hw; s_y0 = cy - hh; s_y1 = cy + hh;
}

static void view_pan(double dx_px, double dy_px)
{
    double sx = (s_x1 - s_x0) / PW, sy = (s_y1 - s_y0) / PH;
    s_x0 -= dx_px * sx; s_x1 -= dx_px * sx;
    s_y0 += dy_px * sy; s_y1 += dy_px * sy;
}

static float to_px(double x) { return (float)(PX + (x - s_x0) / (s_x1 - s_x0) * PW); }
static float to_py(double y) { return (float)(PY + (s_y1 - y) / (s_y1 - s_y0) * PH); }

static double nice_step(double range)
{
    double raw = range / 8, mag = pow(10, floor(log10(raw))), n = raw / mag;
    return (n < 1.5 ? 1 : n < 3.5 ? 2 : n < 7.5 ? 5 : 10) * mag;
}

/* Liang-Barsky clip to the plot */
static bool clip(float *x0, float *y0, float *x1, float *y1)
{
    float dx = *x1 - *x0, dy = *y1 - *y0, t0 = 0, t1 = 1;
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { *x0 - PX, PX + PW - *x0, *y0 - PY, PY + PH - *y0 };
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0) { if (q[i] < 0) return false; continue; }
        float t = q[i] / p[i];
        if (p[i] < 0) { if (t > t1) return false; if (t > t0) t0 = t; }
        else { if (t < t0) return false; if (t < t1) t1 = t; }
    }
    float ox = *x0, oy = *y0;
    *x0 = ox + t0 * dx; *y0 = oy + t0 * dy;
    *x1 = ox + t1 * dx; *y1 = oy + t1 * dy;
    return true;
}

static void draw_plot(void)
{
    ui_fill(PX, PY, PW, PH, C_PANEL);
    double sx = nice_step(s_x1 - s_x0), sy = nice_step(s_y1 - s_y0);
    uint16_t grid = UI_RGB(0x17284A), axis = UI_RGB(0x6C82A6);
    for (double gx = ceil(s_x0 / sx) * sx; gx <= s_x1; gx += sx) ui_fill((int)to_px(gx), PY, 1, PH, grid);
    for (double gy = ceil(s_y0 / sy) * sy; gy <= s_y1; gy += sy) ui_fill(PX, (int)to_py(gy), PW, 1, grid);

    /* Axes, or the edge they are off */
    int ax = (int)to_px(0), ay = (int)to_py(0);
    bool xin = ax >= PX && ax < PX + PW, yin = ay >= PY && ay < PY + PH;
    if (xin) ui_fill(ax, PY, 2, PH, axis);
    if (yin) ui_fill(PX, ay, PW, 2, axis);
    int ly = yin ? ay + 14 : PY + PH - 12, lx = xin ? ax - 6 : PX + 6;
    char b[24];
    for (double gx = ceil(s_x0 / sx) * sx; gx <= s_x1; gx += sx) {
        if (fabs(gx) < sx / 2) continue;
        int px = (int)to_px(gx);
        if (px < PX + 14 || px > PX + PW - 14 || (xin && abs(px - ax) < 22)) continue;
        fmt_num(gx, b, sizeof(b), 4);
        ui_text_c(ui_small, px, ly > PY + PH - 12 ? PY + PH - 12 : ly, b, C_DIM);
    }
    for (double gy = ceil(s_y0 / sy) * sy; gy <= s_y1; gy += sy) {
        if (fabs(gy) < sy / 2) continue;
        int py = (int)to_py(gy);
        if (py < PY + 12 || py > PY + PH - 24 || (yin && abs(py - ay) < 16)) continue;
        fmt_num(gy, b, sizeof(b), 4);
        if (xin) ui_text_r(ui_small, lx, py, b, C_DIM);
        else ui_text_l(ui_small, lx, py, b, C_DIM);
    }

    /* Curves: one sample a pixel; a jump across the whole view (tan, 1/x) is not joined */
    for (int f = 0; f < NFN; f++) {
        if (!s_fn_on[f] || !s_fn[f][0]) continue;
        float px0 = 0, py0 = 0;
        bool have = false;
        double prev = NAN;
        for (int i = 0; i <= PW; i++) {
            double x = s_x0 + (s_x1 - s_x0) * i / PW;
            double y = fn_eval(f, x);
            float px = PX + i, py = isnan(y) ? 0 : (float)(PY + (s_y1 - y) / (s_y1 - s_y0) * PH);
            if (!isnan(y) && py > PY - 4 * PH) {
                if (py > PY + 5 * PH) py = PY + 5 * PH;
                if (py < PY - 4 * PH) py = PY - 4 * PH;
            }
            if (!isnan(y) && have && !isnan(prev)) {
                bool jump = (prev > s_y1 && y < s_y0) || (prev < s_y0 && y > s_y1);
                float a = px0, b0 = py0, c = px, d = py;
                if (!jump && clip(&a, &b0, &c, &d)) ui_line(a, b0, c, d, f == s_sel ? 3.0f : 2.2f, k_fn_col[f]);
            }
            have = !isnan(y);
            prev = y;
            px0 = px; py0 = py;
        }
    }

    /* Trace */
    if (s_trace && s_fn[s_sel][0]) {
        float tx = to_px(s_trace_x);
        if (tx >= PX && tx < PX + PW) {
            ui_fill((int)tx, PY, 1, PH, C_DIM);
            double y = fn_eval(s_sel, s_trace_x);
            char xs[20], ys[20], t[64];
            fmt_num(s_trace_x, xs, sizeof(xs), 6);
            if (isnan(y)) snprintf(ys, sizeof(ys), "undefined");
            else fmt_num(y, ys, sizeof(ys), 6);
            snprintf(t, sizeof(t), "x = %s   y = %s", xs, ys);
            if (!isnan(y)) {
                float ty = to_py(y);
                if (ty >= PY && ty < PY + PH) ui_disc(tx, ty, 6, k_fn_col[s_sel], 255);
            }
            int w = ui_text_w(ui_small, t) + 20;
            ui_rrect(PX + 8, PY + PH - 40, w, 32, 10, C_RAISED);
            ui_text_l(ui_small, PX + 18, PY + PH - 24, t, C_TEXT);
        }
    }
}

static void draw_graph(void)
{
    /* Functions */
    for (int i = 0; i < NFN; i++) {
        int y = 112 + i * 38;
        bool sel = i == s_sel;
        ui_rrect(16, y, 318, 34, 10, ui_pressed_color(G_FN0 + i, sel ? C_RAISED : C_PANEL));
        ui_hit(60, y, 274, 34, G_FN0 + i, UI_TAP);
        if (s_nnav < (int)(sizeof(s_nav) / sizeof(s_nav[0])))
            s_nav[s_nnav++] = (navkey_t){ 60, (int16_t)y, 274, 34, G_FN0 + i };
        if (s_pad_mode && s_focus == G_FN0 + i) ui_rrect(14, y - 2, 4, 38, 2, C_FOCUS);
        /* the dot shows the colour and switches the curve on/off */
        ui_disc(36, y + 17, 9, s_fn_on[i] ? k_fn_col[i] : C_BG, 255);
        if (!s_fn_on[i]) ui_disc(36, y + 17, 6, C_PANEL, 255);
        ui_hit(16, y, 44, 34, G_ON0 + i, UI_TAP);
        char lab[8], d[160];
        snprintf(lab, sizeof(lab), "y%d=", i + 1);
        ui_text_l(ui_small, 54, y + 17, lab, sel ? C_TEXT : C_DIM);
        to_display(s_fn[i], d, sizeof(d));
        if (sel && (papp_time_us() / 500000) & 1) ins(d, sizeof(d), "|");
        bool bad = s_fn[i][0] && !syntax_ok(s_fn[i], true);
        int x0 = 54 + ui_text_w(ui_small, lab) + 6;
        if (rich_w(ui_small, d) <= 326 - x0) rich_l(ui_small, x0, y + 17, d, bad ? C_RED : C_TEXT);
        else rich_r_fit(ui_small, 326, y + 17, d, 326 - x0, bad ? C_RED : C_TEXT);
    }

    static const int pad[] = {
        K_X,      K_SIN,    K_COS,    K_TAN,  K_LP,  K_RP,
        K_D0 + 7, K_D0 + 8, K_D0 + 9, K_DIV,  K_POW, K_SQRT,
        K_D0 + 4, K_D0 + 5, K_D0 + 6, K_MUL,  K_LN,  K_LOG,
        K_D0 + 1, K_D0 + 2, K_D0 + 3, K_SUB,  K_PI,  K_E,
        K_D0,     K_DOT,    K_BS,     K_ADD,  K_AC,  K_ABS,
    };
    key_grid(16, 230, 6, 5, 48, 42, 6, pad, ui_small);

    draw_plot();
    /* Zoom buttons over the plot */
    int bx = PX + PW - 52;
    key(bx, PY + 8, 44, 36, G_ZIN, ui_med, "+", C_RAISED, C_TEXT);
    key(bx, PY + 50, 44, 36, G_ZOUT, ui_med, "\x06", C_RAISED, C_TEXT);
    key(bx - 72, PY + 8, 64, 36, G_HOME, ui_small, "Reset", C_RAISED, C_TEXT);
    key(bx - 72, PY + 50, 64, 36, G_DEG, ui_small, s_gdeg ? "DEG" : "RAD", C_RAISED, C_FOCUS);
}

static void graph_key(int id)
{
    if (id >= G_FN0 && id < G_FN0 + NFN) { s_sel = id - G_FN0; return; }
    if (id >= G_ON0 && id < G_ON0 + NFN) { s_fn_on[id - G_ON0] = !s_fn_on[id - G_ON0]; return; }
    if (id == G_ZIN) { view_zoom(1 / 1.5); return; }
    if (id == G_ZOUT) { view_zoom(1.5); return; }
    if (id == G_HOME) { view_home(); s_trace = false; return; }
    if (id == K_EQ) return;
    edit(s_fn[s_sel], sizeof(s_fn[0]), id);
}

/* The plot reads the touch itself: drag pans, a tap traces */
static struct { bool down, drag; int x0, y0, lx, ly; } s_pt;

static void plot_touch(papp_surface_t *s)
{
    papp_touch_point_t t;
    bool down = papp_disp_touch(s, &t, 1) > 0;
    if (down && !s_pt.down) {
        bool in = t.x >= PX && t.x < PX + PW && t.y >= PY && t.y < PY + PH;
        bool on_btn = t.x >= PX + PW - 132 && t.y < PY + 92;
        s_pt.down = in && !on_btn;
        s_pt.drag = false;
        s_pt.x0 = s_pt.lx = t.x;
        s_pt.y0 = s_pt.ly = t.y;
        return;
    }
    if (!s_pt.down) return;
    if (down) {
        if (abs(t.x - s_pt.x0) + abs(t.y - s_pt.y0) > 10) s_pt.drag = true;
        if (s_pt.drag) view_pan(t.x - s_pt.lx, t.y - s_pt.ly);
        s_pt.lx = t.x;
        s_pt.ly = t.y;
    } else {
        if (!s_pt.drag) {
            s_trace = true;
            s_trace_x = s_x0 + (s_pt.lx - PX + 0.5) * (s_x1 - s_x0) / PW;
        }
        s_pt.down = false;
    }
}

/* ── Currency tab ───────────────────────────────────────────────────────── */

#define MAX_RATES 200
typedef struct { char code[4]; double r; } rate_t;   /* units of the currency per US dollar */
static rate_t s_rate[MAX_RATES];
static int s_nrate;
static long long s_rates_time;                        /* the provider's update time */
static long long s_fetched;                           /* when this board fetched them (0: clock not set) */
static char s_rates_date[24];
static char s_from[4] = "USD", s_to[4] = "EUR";
static char s_amt[64] = "1";
static int s_pick = -1, s_page;                       /* picker: 0 from, 1 to */

/* Fetch thread */
static volatile int s_fetch;                          /* 0 idle, 1 busy, 2 new rates ready, 3 failed */
static char s_fetch_msg[64];
static rate_t *s_new_rate;
static int s_new_n;
static long long s_new_time;
static char s_new_date[24];
static papp_thread_t *s_fetch_thr;
static bool s_tried;                                  /* fetched (or tried) during this run */

static const struct { const char *code, *name; } k_cur_name[] = {
    { "USD", "US dollar" }, { "EUR", "Euro" }, { "ILS", "Israeli shekel" }, { "GBP", "British pound" },
    { "JPY", "Japanese yen" }, { "CHF", "Swiss franc" }, { "CAD", "Canadian dollar" },
    { "AUD", "Australian dollar" }, { "CNY", "Chinese yuan" }, { "INR", "Indian rupee" },
    { "NZD", "New Zealand dollar" }, { "SEK", "Swedish krona" }, { "NOK", "Norwegian krone" },
    { "DKK", "Danish krone" }, { "PLN", "Polish zloty" }, { "CZK", "Czech koruna" },
    { "HUF", "Hungarian forint" }, { "RON", "Romanian leu" }, { "BGN", "Bulgarian lev" },
    { "TRY", "Turkish lira" }, { "RUB", "Russian ruble" }, { "UAH", "Ukrainian hryvnia" },
    { "BRL", "Brazilian real" }, { "MXN", "Mexican peso" }, { "ARS", "Argentine peso" },
    { "CLP", "Chilean peso" }, { "COP", "Colombian peso" }, { "PEN", "Peruvian sol" },
    { "ZAR", "South African rand" }, { "EGP", "Egyptian pound" }, { "JOD", "Jordanian dinar" },
    { "AED", "UAE dirham" }, { "SAR", "Saudi riyal" }, { "KRW", "South Korean won" },
    { "SGD", "Singapore dollar" }, { "HKD", "Hong Kong dollar" }, { "TWD", "Taiwan dollar" },
    { "THB", "Thai baht" }, { "PHP", "Philippine peso" }, { "IDR", "Indonesian rupiah" },
    { "MYR", "Malaysian ringgit" }, { "VND", "Vietnamese dong" }, { "ISK", "Icelandic krona" },
    { "NGN", "Nigerian naira" }, { "KES", "Kenyan shilling" }, { "MAD", "Moroccan dirham" },
};
#define CUR_NAMES ((int)(sizeof(k_cur_name) / sizeof(k_cur_name[0])))
#define FAVOURITES 10                                 /* the first ten names come first in the picker */

static const char *cur_name(const char *code)
{
    for (int i = 0; i < CUR_NAMES; i++) if (!strcmp(k_cur_name[i].code, code)) return k_cur_name[i].name;
    return "";
}

static double rate_of(const char *code)
{
    for (int i = 0; i < s_nrate; i++) if (!strcmp(s_rate[i].code, code)) return s_rate[i].r;
    return NAN;
}

/* "rates":{"USD":1,"AED":3.6725,...} plus the update time and date */
static int parse_rates(const char *j, rate_t *out, int max, long long *t, char *date, int dcap)
{
    const char *p = strstr(j, "\"time_last_update_unix\":");
    *t = p ? atoll(p + 24) : 0;
    p = strstr(j, "\"time_last_update_utc\":\"");
    date[0] = 0;
    if (p) {                                          /* "Mon, 16 Nov 2026 00:02:31 +0000" -> "16 Nov 2026" */
        p += 24;
        const char *c = strchr(p, ',');
        if (c && c - p < 6) p = c + 2;
        int n = 0;
        while (p[n] && p[n] != '"' && n < 11 && n < dcap - 1) n++;
        memcpy(date, p, n);
        date[n] = 0;
    }
    p = strstr(j, "\"rates\":{");
    if (!p) return 0;
    p += 9;
    int n = 0;
    while (*p && *p != '}' && n < max) {
        if (*p != '"') { p++; continue; }
        const char *q = strchr(p + 1, '"');
        if (!q || q - p - 1 != 3) break;
        memcpy(out[n].code, p + 1, 3);
        out[n].code[3] = 0;
        p = q + 1;
        if (*p != ':') break;
        char *end;
        out[n].r = strtod(p + 1, &end);
        if (end == p + 1) break;
        if (out[n].r > 0) n++;
        p = end;
        if (*p == ',') p++;
    }
    return n;
}

static int fetch_thread(void *arg)
{
    (void)arg;
    const papp_if_net_t *net = papp_net();
    if (!net) { snprintf(s_fetch_msg, sizeof(s_fetch_msg), "This launcher has no network for apps"); s_fetch = 3; return 0; }
    if (net->up(15000) != PAPP_E_OK) {
        snprintf(s_fetch_msg, sizeof(s_fetch_msg), "No Wi-Fi: tap Wi-Fi to choose a network");
        s_fetch = 3;
        return 0;
    }
    int status = 0;
    papp_http_req_t req = { .url = RATES_URL, .timeout_ms = 15000 };
    papp_http_t *h = net->http_open(&req, &status);
    if (!h || status != 200) {
        if (h) net->http_close(h);
        snprintf(s_fetch_msg, sizeof(s_fetch_msg), "Rates server didn't answer (%d)", status);
        s_fetch = 3;
        return 0;
    }
    int cap = 32768, len = 0;
    char *buf = malloc(cap);
    if (buf) {
        int n;
        while (len < cap - 1 && (n = net->http_read(h, buf + len, cap - 1 - len)) > 0) len += n;
        buf[len] = 0;
    }
    net->http_close(h);
    if (!s_new_rate) s_new_rate = malloc(sizeof(rate_t) * MAX_RATES);
    s_new_n = buf && s_new_rate ? parse_rates(buf, s_new_rate, MAX_RATES, &s_new_time, s_new_date,
                                              sizeof(s_new_date)) : 0;
    free(buf);
    if (s_new_n < 10) {
        snprintf(s_fetch_msg, sizeof(s_fetch_msg), "Couldn't read the rates");
        s_fetch = 3;
        return 0;
    }
    s_fetch = 2;
    return 0;
}

static void fetch_start(void)
{
    if (s_fetch == 1) return;
    if (s_fetch_thr) { papp_thread_join(s_fetch_thr, PAPP_WAIT_FOREVER, NULL); s_fetch_thr = NULL; }
    s_tried = true;
    s_fetch = 1;
    papp_thread_cfg_t tc = { .name = "rates", .stack_size = 16384, .prio = PAPP_PRIO_NORMAL, .core = -1 };
    s_fetch_thr = papp_thread_create(fetch_thread, NULL, &tc);
    if (!s_fetch_thr) { snprintf(s_fetch_msg, sizeof(s_fetch_msg), "Couldn't start the download"); s_fetch = 3; }
}

static void rates_save(void)
{
    mkdir("/sd/data", 0777);
    mkdir(DATA_DIR, 0777);
    FILE *f = fopen(RATES, "w");
    if (!f) return;
    fprintf(f, "fetched %lld\n", s_fetched);
    fprintf(f, "time %lld %s\n", s_rates_time, s_rates_date);
    for (int i = 0; i < s_nrate; i++) fprintf(f, "%s %.10g\n", s_rate[i].code, s_rate[i].r);
    fclose(f);
}

static void rates_load(void)
{
    FILE *f = fopen(RATES, "r");
    if (!f) return;
    char line[96];
    s_nrate = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "fetched ", 8)) {
            s_fetched = strtoll(line + 8, NULL, 10);
        } else if (!strncmp(line, "time ", 5)) {
            char *end;
            s_rates_time = strtoll(line + 5, &end, 10);
            snprintf(s_rates_date, sizeof(s_rates_date), "%s", *end ? end + 1 : "");
        } else if (strlen(line) > 4 && line[3] == ' ' && s_nrate < MAX_RATES) {
            memcpy(s_rate[s_nrate].code, line, 3);
            s_rate[s_nrate].code[3] = 0;
            s_rate[s_nrate].r = strtod(line + 4, NULL);
            if (s_rate[s_nrate].r > 0) s_nrate++;
        }
    }
    fclose(f);
}

static void fetch_poll(void)
{
    if (s_fetch == 2) {
        memcpy(s_rate, s_new_rate, sizeof(rate_t) * s_new_n);
        s_nrate = s_new_n;
        s_rates_time = s_new_time;
        snprintf(s_rates_date, sizeof(s_rates_date), "%s", s_new_date);
        int64_t now = 0;
        s_fetched = papp_utc_time(&now) ? now : 0;
        rates_save();
        s_fetch = 0;
    }
}

static void cur_enter(void)
{
    int64_t now = 0;
    if (!papp_utc_time(&now)) now = 0;
    long long since = s_fetched ? s_fetched : s_rates_time;
    bool old = !s_nrate || (now > 1600000000LL && now - since > RATES_AGE);
    if (old && !s_tried) fetch_start();
}

/* Picker order: the favourites, then every other currency alphabetically */
static int s_order[MAX_RATES], s_norder;

static void build_order(void)
{
    s_norder = 0;
    for (int i = 0; i < FAVOURITES; i++)
        for (int j = 0; j < s_nrate; j++)
            if (!strcmp(s_rate[j].code, k_cur_name[i].code)) s_order[s_norder++] = j;
    int rest0 = s_norder;
    for (int j = 0; j < s_nrate; j++) {
        bool fav = false;
        for (int i = 0; i < FAVOURITES; i++) if (!strcmp(s_rate[j].code, k_cur_name[i].code)) fav = true;
        if (!fav) s_order[s_norder++] = j;
    }
    for (int a = rest0; a < s_norder; a++)                /* insertion sort, ~160 entries */
        for (int b = a; b > rest0 && strcmp(s_rate[s_order[b]].code, s_rate[s_order[b - 1]].code) < 0; b--) {
            int t = s_order[b]; s_order[b] = s_order[b - 1]; s_order[b - 1] = t;
        }
}

#define PICK_COLS 5
#define PICK_ROWS 5
#define PICK_PAGE (PICK_COLS * PICK_ROWS)

static void draw_picker(void)
{
    ui_fill(0, 104, UI_W, UI_H - 104, C_BG);
    ui_text_l(ui_med, 24, 124, s_pick == 0 ? "Convert from" : "Convert to", C_TEXT);
    int pages = (s_norder + PICK_PAGE - 1) / PICK_PAGE;
    if (s_page >= pages) s_page = pages ? pages - 1 : 0;
    for (int k = 0; k < PICK_PAGE; k++) {
        int i = s_page * PICK_PAGE + k;
        if (i >= s_norder) break;
        const rate_t *r = &s_rate[s_order[i]];
        int x = 20 + (k % PICK_COLS) * 152, y = 146 + (k / PICK_COLS) * 56;
        bool cur = !strcmp(r->code, s_pick == 0 ? s_from : s_to);
        if (s_nnav < (int)(sizeof(s_nav) / sizeof(s_nav[0])))
            s_nav[s_nnav++] = (navkey_t){ (int16_t)x, (int16_t)y, 144, 50, P_CELL + k };
        if (s_pad_mode && s_focus == P_CELL + k) ui_rrect(x - 4, y - 4, 152, 58, 14, C_FOCUS);
        ui_rrect(x, y, 144, 50, 10, ui_pressed_color(P_CELL + k, cur ? C_BLUE : C_RAISED));
        const char *nm = cur_name(r->code);
        if (nm[0]) {
            ui_text_c(ui_med, x + 72, y + 17, r->code, C_TEXT);
            char shortn[24];
            snprintf(shortn, sizeof(shortn), "%s", nm);
            size_t n = strlen(shortn);
            while (n > 3 && ui_text_w(ui_small, shortn) > 136) {         /* "Indonesian rup." */
                shortn[--n] = 0;
                shortn[n - 1] = '.';
            }
            ui_text_c(ui_small, x + 72, y + 38, shortn, C_DIM);
        } else {
            ui_text_c(ui_med, x + 72, y + 25, r->code, C_TEXT);
        }
        ui_hit(x, y, 144, 50, P_CELL + k, UI_TAP);
    }
    char pg[40];
    snprintf(pg, sizeof(pg), "Page %d of %d", s_page + 1, pages ? pages : 1);
    key(20, 444 - 12, 150, 44, P_PREV, ui_med, "Back", C_RAISED, s_page ? C_TEXT : C_FAINT);
    ui_text_c(ui_small, 400, 454, pg, C_DIM);
    key(476, 444 - 12, 150, 44, P_NEXT, ui_med, "More", C_RAISED, s_page + 1 < pages ? C_TEXT : C_FAINT);
    key(634, 444 - 12, 146, 44, P_CANCEL, ui_med, "Cancel", C_OP, C_TEXT);
}

static void picker_key(int id)
{
    int pages = (s_norder + PICK_PAGE - 1) / PICK_PAGE;
    if (id == P_PREV) { if (s_page) s_page--; return; }
    if (id == P_NEXT) { if (s_page + 1 < pages) s_page++; return; }
    if (id == P_CANCEL || id == K_BS) { s_pick = -1; return; }
    if (id >= P_CELL && id < P_CELL + PICK_PAGE) {
        int i = s_page * PICK_PAGE + id - P_CELL;
        if (i < s_norder) {
            snprintf(s_pick == 0 ? s_from : s_to, 4, "%s", s_rate[s_order[i]].code);
            s_pick = -1;
            s_focus = K_D0 + 1;
        }
    }
}

static void cur_card(int y, int id, const char *code, const char *value, const char *sub)
{
    ui_rrect(16, y, 404, 120, 14, C_PANEL);
    if (s_nnav < (int)(sizeof(s_nav) / sizeof(s_nav[0])))
        s_nav[s_nnav++] = (navkey_t){ 16, (int16_t)y, 404, 120, id };          /* the whole card */
    if (s_pad_mode && s_focus == id) ui_rrect(24, y + 8, 158, 52, 14, C_FOCUS);
    ui_rrect(28, y + 12, 150, 44, 12, ui_pressed_color(id, C_RAISED));
    ui_text_l(ui_med, 42, y + 34, code, C_TEXT);
    ui_line(150, y + 30, 158, y + 38, 2.5f, C_DIM);
    ui_line(158, y + 38, 166, y + 30, 2.5f, C_DIM);
    ui_hit(28, y + 12, 150, 44, id, UI_TAP);
    ui_text_r(ui_small, 404, y + 34, cur_name(code), C_DIM);
    char d[96];
    to_display(value, d, sizeof(d));
    rich_r_fit(ui_large, 404, y + 86, d, 380, C_TEXT);
    if (sub && sub[0]) ui_text_l(ui_small, 32, y + 100, sub, C_DIM);
}

static void draw_cur(void)
{
    if (s_pick >= 0) { draw_picker(); return; }
    double rf = rate_of(s_from), rt = rate_of(s_to);
    double a = eval(s_amt);
    char out[48], sub[48] = "";
    if (s_amt[0] && !isnan(a) && !isnan(rf) && !isnan(rt)) fmt_money(a / rf * rt, out, sizeof(out));
    else snprintf(out, sizeof(out), "-");
    if (s_amt[0] && !isnan(a) && strpbrk(s_amt, "+-*/^(%")) {
        char v[32];
        fmt_money(a, v, sizeof(v));
        snprintf(sub, sizeof(sub), "= %s", v);
    }
    cur_card(112, CU_FROM, s_from, s_amt[0] ? s_amt : "0", sub);
    cur_card(292, CU_TO, s_to, out, NULL);

    /* Swap, between the cards */
    if (s_nnav < (int)(sizeof(s_nav) / sizeof(s_nav[0])))
        s_nav[s_nnav++] = (navkey_t){ 32, 238, 56, 48, CU_SWAP };
    if (s_pad_mode && s_focus == CU_SWAP) ui_disc(60, 262, 30, C_FOCUS, 255);
    ui_disc(60, 262, 26, ui_pressed_color(CU_SWAP, C_BLUE), 255);
    glyph_draw(G_SWAP, 60 - cap_of(ui_med) * 0.6f, 262, cap_of(ui_med), C_TEXT);
    ui_hit(28, 236, 64, 52, CU_SWAP, UI_TAP);

    if (!isnan(rf) && !isnan(rt)) {
        char r1[24], line[64];
        fmt_num(rt / rf, r1, sizeof(r1), 6);
        snprintf(line, sizeof(line), "1 %s = %s %s", s_from, r1, s_to);
        ui_text_r(ui_small, 404, 262, line, C_DIM);
    }

    /* Rates line */
    char st[96];
    if (s_fetch == 1) snprintf(st, sizeof(st), "Updating rates...");
    else if (!s_nrate) snprintf(st, sizeof(st), "%s", s_fetch == 3 ? s_fetch_msg : "No rates yet");
    else snprintf(st, sizeof(st), "Rates of %s", s_rates_date[0] ? s_rates_date : "?");
    ui_text_l(ui_small, 20, 434, st, s_fetch == 3 && !s_nrate ? C_ORANGE : C_DIM);
    ui_text_l(ui_small, 20, 458, "by exchangerate-api.com", C_FAINT);
    key(260, 424, 76, 44, CU_UPDATE, ui_small, "Update", C_RAISED, C_TEXT);
    key(344, 424, 76, 44, CU_WIFI, ui_small, "Wi-Fi", C_RAISED, C_TEXT);

    static const int pad[] = {
        K_D0 + 7, K_D0 + 8, K_D0 + 9, K_BS,
        K_D0 + 4, K_D0 + 5, K_D0 + 6, K_DIV,
        K_D0 + 1, K_D0 + 2, K_D0 + 3, K_MUL,
        K_D0,     K_DOT,    K_AC,     K_SUB,
        K_LP,     K_RP,     K_PCT,    K_ADD,
    };
    key_grid(440, 112, 4, 5, 80, 64, 10, pad, ui_med);
}

static void cur_key(int id)
{
    if (s_pick >= 0) { picker_key(id); return; }
    if (id == CU_FROM || id == CU_TO) {
        if (!s_nrate) return;
        build_order();
        s_pick = id == CU_FROM ? 0 : 1;
        s_page = 0;
        const char *cur = s_pick == 0 ? s_from : s_to;
        for (int i = 0; i < s_norder; i++) if (!strcmp(s_rate[s_order[i]].code, cur)) s_page = i / PICK_PAGE;
        s_focus = P_CELL;
        return;
    }
    if (id == CU_SWAP) {
        char t[4];
        memcpy(t, s_from, 4); memcpy(s_from, s_to, 4); memcpy(s_to, t, 4);
        return;
    }
    if (id == CU_UPDATE) { fetch_start(); return; }
    if (id == CU_WIFI) {
        if (papp_ui_wifi() == 1) fetch_start();
        ui_touch_reset();
        return;
    }
    if (id == K_EQ) return;
    if (!strcmp(s_amt, "1") && id >= K_D0 && id <= K_DOT) s_amt[0] = 0;   /* the default 1 is replaced */
    edit(s_amt, sizeof(s_amt), id);
}

/* ── Settings ───────────────────────────────────────────────────────────── */

static void settings_load(void)
{
    FILE *f = fopen(SETTINGS, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v = strchr(line, ' ');
        if (!v) continue;
        *v++ = 0;
        if (!strcmp(line, "deg")) s_deg = atoi(v) != 0;
        else if (!strcmp(line, "gdeg")) s_gdeg = atoi(v) != 0;
        else if (!strcmp(line, "tab")) { int t = atoi(v); if (t >= 0 && t < T_N) s_tab = t; }
        else if (!strcmp(line, "ans")) s_ans = strtod(v, NULL);
        else if (!strcmp(line, "from") && strlen(v) == 3) memcpy(s_from, v, 4);
        else if (!strcmp(line, "to") && strlen(v) == 3) memcpy(s_to, v, 4);
        else if (!strcmp(line, "amount")) snprintf(s_amt, sizeof(s_amt), "%s", v);
        else if (!strcmp(line, "view")) {
            double a, b, c, d;
            if (sscanf(v, "%lf %lf %lf %lf", &a, &b, &c, &d) == 4 && a < b && c < d) {
                s_x0 = a; s_x1 = b; s_y0 = c; s_y1 = d;
            }
        } else if (line[0] == 'f' && line[1] >= '1' && line[1] <= '0' + NFN && !line[2]) {
            int i = line[1] - '1';
            s_fn_on[i] = v[0] == '1';
            snprintf(s_fn[i], sizeof(s_fn[i]), "%s", v[0] && v[1] == ' ' ? v + 2 : "");
        }
    }
    fclose(f);
}

static void settings_save(void)
{
    mkdir("/sd/data", 0777);
    mkdir(DATA_DIR, 0777);
    FILE *f = fopen(SETTINGS, "w");
    if (!f) { papp_log("calc: can't write " SETTINGS "\n"); return; }
    fprintf(f, "gdeg %d\n", s_gdeg);
    fprintf(f, "deg %d\ntab %d\nans %.17g\nfrom %s\nto %s\namount %s\nview %.17g %.17g %.17g %.17g\n",
            s_deg, s_tab, s_ans, s_from, s_to, s_amt, s_x0, s_x1, s_y0, s_y1);
    for (int i = 0; i < NFN; i++) fprintf(f, "f%d %d %s\n", i + 1, s_fn_on[i], s_fn[i]);
    fclose(f);
}

/* ── Input ──────────────────────────────────────────────────────────────── */

static void act(int id, int x)
{
    (void)x;
    s_pad_mode = false;
    if (id >= UI_ID_TAB && id < UI_ID_TAB + T_N) {
        s_tab = id - UI_ID_TAB;
        s_pick = -1;
        if (s_tab == T_CUR) cur_enter();
        return;
    }
    if (id == UI_ID_CLOSE) { s_quit = true; return; }
    if (s_tab == T_CALC) calc_key(id);
    else if (s_tab == T_GRAPH) graph_key(id);
    else cur_key(id);
}

static void press(int id)                              /* a key from the pad or the keyboard */
{
    bool pm = s_pad_mode;
    act(id, 0);
    s_pad_mode = pm;
}

static void keyboard(void)
{
    papp_input_event_t ev;
    while (papp_input_poll_event(&ev)) {
        if (ev.type != PAPP_EV_KEY_DOWN) continue;
        char *buf = s_tab == T_CALC ? s_expr : s_tab == T_GRAPH ? s_fn[s_sel] : s_amt;
        int cap = s_tab == T_CALC ? (int)sizeof(s_expr) : s_tab == T_GRAPH ? (int)sizeof(s_fn[0]) : (int)sizeof(s_amt);
        if (ev.code == 0x28 || ev.code == 0x58) press(K_EQ);              /* Enter */
        else if (ev.code == 0x2A) press(K_BS);                            /* Backspace */
        else if (ev.code == 0x29) press(K_AC);                            /* Esc */
        else if (ev.code == 0x2B) { s_tab = (s_tab + 1) % T_N; s_pick = -1; if (s_tab == T_CUR) cur_enter(); }
        else if (ev.text >= 32 && ev.text < 127 && s_pick < 0) {
            char t[2] = { (char)ev.text, 0 };
            if (t[0] == '=') { press(K_EQ); continue; }
            if (s_tab == T_CALC && s_done) {                              /* like a key press */
                s_done = false;
                s_last[0] = 0;
                if (strchr("+-*/^!%", t[0])) snprintf(s_expr, sizeof(s_expr), "Ans");
                else s_expr[0] = 0;
            }
            if (s_tab == T_CUR && !strcmp(s_amt, "1") && (is_dig(t[0]) || t[0] == '.')) s_amt[0] = 0;
            s_error = false;
            ins(buf, cap, t);
        }
    }
}

/* ── Self-test ──────────────────────────────────────────────────────────── */

static int selftest(void)
{
    static const struct { const char *e; double v; bool deg; } t[] = {
        { "1+2*3", 7, 1 }, { "(1+2)*3", 9, 1 }, { "2^3^2", 512, 1 }, { "-2^2", -4, 1 }, { "2^-1", 0.5, 1 },
        { "2pi", 2 * M_PI, 1 }, { "3(4+1)", 15, 1 }, { "(2)(3)", 6, 1 }, { "sin(30)", 0.5, 1 },
        { "sin(180)", 0, 1 }, { "cos(90)", 0, 1 }, { "tan(45)", 1, 1 }, { "asin(1)", 90, 1 },
        { "sin(pi/2)", 1, 0 }, { "acos(-1)", M_PI, 0 }, { "ln(e)", 1, 1 }, { "log(1000)", 3, 1 },
        { "10^(2)", 100, 1 }, { "exp(0)", 1, 1 }, { "sqrt(16)", 4, 1 }, { "cbrt(27)", 3, 1 },
        { "5!", 120, 1 }, { "0!", 1, 1 }, { "50%", 0.5, 1 }, { "1.5E3", 1500, 1 }, { "2E-2", 0.02, 1 },
        { "abs(-3)", 3, 1 }, { "2^(-1)", 0.5, 1 }, { "sinh(0)", 0, 1 }, { "(1+2", 3, 1 }, { "sqrt(9", 3, 1 },
        { "4/2/2", 1, 1 }, { "2*-3", -6, 1 }, { "2e", 2 * M_E, 1 }, { "1-2-3", -4, 1 },
    };
    static const char *const bad[] = { "1/0", "2+", "sin30", "(", ")", "x+1", "2**3", "sqrt(-1)", "tan(90)",
                                       "1.2.3", "E5", "foo(1)" };
    int fails = 0;
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        s_deg = t[i].deg;
        double v = eval(t[i].e);
        if (isnan(v) || fabs(v - t[i].v) > 1e-9 * (1 + fabs(t[i].v))) {
            papp_log("calc selftest: %s = %.12g, expected %.12g\n", t[i].e, v, t[i].v);
            fails++;
        }
    }
    s_deg = true;
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        double v = eval(bad[i]);
        if (!isnan(v)) { papp_log("calc selftest: %s = %.12g, expected an error\n", bad[i], v); fails++; }
    }
    double v = eval_x("pix", true, 2);
    if (fabs(v - 2 * M_PI) > 1e-12) { papp_log("calc selftest: pix at 2 = %g\n", v); fails++; }
    char b[48];
    static const struct { double v; const char *s; } f[] = {
        { 0.1 + 0.2, "0.3" }, { 1.0 / 3, "0.3333333333" }, { 1e12, "1E12" }, { 123456789, "123456789" },
        { -2.5e-10, "-2.5E-10" }, { 1e100, "1E100" }, { 120, "120" },
    };
    for (size_t i = 0; i < sizeof(f) / sizeof(f[0]); i++) {
        fmt_num(f[i].v, b, sizeof(b), 10);
        if (strcmp(b, f[i].s)) { papp_log("calc selftest: format %.17g -> %s, expected %s\n", f[i].v, b, f[i].s); fails++; }
    }
    fmt_money(1234567.891, b, sizeof(b));
    if (strcmp(b, "1,234,567.89")) { papp_log("calc selftest: money -> %s\n", b); fails++; }
    rate_t r[4];
    long long tm;
    char date[24];
    int n = parse_rates("{\"time_last_update_unix\":1794787351,\"time_last_update_utc\":\"Mon, 16 Nov 2026 00:02:31 "
                        "+0000\",\"rates\":{\"USD\":1,\"AED\":3.6725,\"ILS\":3.7}}", r, 4, &tm, date, sizeof(date));
    if (n != 3 || tm != 1794787351LL || strcmp(date, "16 Nov 2026") || fabs(r[2].r - 3.7) > 1e-12) {
        papp_log("calc selftest: rates n=%d time=%lld date=%s\n", n, tm, date);
        fails++;
    }
    papp_log("calc selftest: %d failure(s)\n", fails);
    return fails;
}

/* ── Main ───────────────────────────────────────────────────────────────── */

static void draw(papp_surface_t *s)
{
    s_nnav = 0;
    ui_begin(s);
    char st[96];
    uint16_t dot = C_GREEN;
    if (s_tab == T_CALC) {
        char a[32];
        fmt_num(s_ans, a, sizeof(a), 10);
        snprintf(st, sizeof(st), "%s, Ans = %s", s_deg ? "Degrees" : "Radians", a);
    } else if (s_tab == T_GRAPH) {
        snprintf(st, sizeof(st), "Editing y%d. Drag the graph to move it, tap it to trace", s_sel + 1);
        dot = k_fn_col[s_sel];
    } else {
        if (s_fetch == 1) { snprintf(st, sizeof(st), "Updating rates..."); dot = C_ORANGE; }
        else if (s_nrate) snprintf(st, sizeof(st), "%d currencies, rates of %s", s_nrate, s_rates_date);
        else { snprintf(st, sizeof(st), "No rates yet"); dot = C_RED; }
    }
    ui_bar("Calculator", k_tab, T_N, s_tab, st, dot, s_tab == T_CUR && s_fetch == 1);
    if (s_tab == T_CALC) draw_calc();
    else if (s_tab == T_GRAPH) draw_graph();
    else draw_cur();
    ui_end(s);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "selftest")) return selftest();

    papp_surface_t *s = ui_open();
    if (!s) return 1;
    settings_load();
    rates_load();
    if (s_tab == T_CUR) cur_enter();

    uint32_t prev = 0;
    while (!papp_exit_requested() && !s_quit) {
        fetch_poll();
        keyboard();

        papp_gamepad_t gp = { 0 };
        papp_input_gamepad(0, &gp);
        uint32_t pr = gp.buttons & ~prev;
        prev = gp.buttons;
        if (pr) s_pad_mode = true;
        if (pr & PAPP_BTN_L1) { s_tab = (s_tab + T_N - 1) % T_N; s_pick = -1; if (s_tab == T_CUR) cur_enter(); }
        if (pr & PAPP_BTN_R1) { s_tab = (s_tab + 1) % T_N; s_pick = -1; if (s_tab == T_CUR) cur_enter(); }
        if (pr & PAPP_BTN_UP) nav_move(0, -1);
        if (pr & PAPP_BTN_DOWN) nav_move(0, 1);
        if (pr & PAPP_BTN_LEFT) nav_move(-1, 0);
        if (pr & PAPP_BTN_RIGHT) nav_move(1, 0);
        if (pr & PAPP_BTN_A) press(s_focus);
        if (pr & PAPP_BTN_B) press(K_BS);
        if (pr & PAPP_BTN_Y) press(K_AC);
        if (s_tab == T_GRAPH) {
            if (pr & PAPP_BTN_L2) view_zoom(1.5);
            if (pr & PAPP_BTN_R2) view_zoom(1 / 1.5);
            int lx = gp.lx, ly = gp.ly;
            if (abs(lx) > 8000 || abs(ly) > 8000)
                view_pan(abs(lx) > 8000 ? -lx / 4096.0 : 0, abs(ly) > 8000 ? -ly / 4096.0 : 0);
        }
        if (pr & (PAPP_BTN_START | PAPP_BTN_X)) {
            static const char *const items[] = { "Calc", "Graph", "Currency", "Exit" };
            int r = papp_ui_menu("Calculator", items, 4, s_tab);
            if (r == 3) break;
            if (r >= 0) { s_tab = r; s_pick = -1; if (s_tab == T_CUR) cur_enter(); }
            prev = 0xFFFFFFFFu;
            ui_touch_reset();
            continue;
        }
        if (s_tab == T_GRAPH && s_pick < 0) plot_touch(s);
        ui_touch(s, act);
        draw(s);
    }
    settings_save();
    if (s_fetch_thr) papp_thread_join(s_fetch_thr, 20000, NULL);
    papp_disp_surface_destroy(s);
    return 0;
}
