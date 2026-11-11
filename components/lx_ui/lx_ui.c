/*
 * Launcher UI screens: content browser + message box (see lx_ui.h).
 *
 * Everything is redrawn in full on each change (a 800x480 software frame is a
 * few ms) through lx_comp_takeover_begin/end, so the browser works the same
 * over a running app and on the bare launcher.
 */
#include "lx_ui.h"
#include "lx_gfx.h"
#include "lx_comp.h"
#include "lx_input.h"
#include "lx_audio.h"
#include "lx_net.h"
#include "nvs.h"
#include "bsp.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lx_ui";

/* Paths are capped at PATH_LEN on purpose; a longer one is cut, not overflowed */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define DATA_DIR      "/sd/launcher"
#define FAV_FILE      DATA_DIR "/favorites.txt"
#define RECENT_FILE   DATA_DIR "/recent.txt"
#define MAX_RECENT    50
#define MAX_ENTRIES   8192
#define PATH_LEN      256
#define SEARCH_LEN    32

/* Layout (landscape logical pixels; text is the 8x16 font at scale 2) */
#define HDR_H     56
#define BAR_H     40          /* search bar, when there is a search */
#define ROW_H     40
#define FTR_H     48
#define CW        16          /* character width at scale 2 */
#define MARGIN    16

#define C_BG      LX_RGB(16, 20, 32)
#define C_HDR     LX_RGB(32, 64, 128)
#define C_SEL     LX_RGB(48, 96, 176)
#define C_TEXT    LX_RGB(230, 230, 230)
#define C_DIM     LX_RGB(140, 150, 170)
#define C_DIR     LX_RGB(130, 190, 255)
#define C_FAV     LX_RGB(250, 210, 60)
#define C_SAVE    LX_RGB(80, 220, 120)
#define C_BOX     LX_RGB(36, 44, 64)
#define C_BTN     LX_RGB(56, 68, 96)
#define C_WARN    LX_RGB(240, 110, 90)

/* ── Guard (SVC_ENTER/LEAVE of the caller) ─────────────────────────────── */

static void (*s_enter)(void), (*s_leave)(void);

void lx_ui_set_guard(void (*enter)(void), void (*leave)(void))
{
    s_enter = enter;
    s_leave = leave;
}

/* What modal boxes are drawn on: the browser list, or a frozen copy of the
 * app's picture (menus and messages an app calls up over its own screen). */
static uint16_t *s_backdrop;

static void guard_in(void)  { if (s_enter) s_enter(); }
static void guard_out(void) { if (s_leave) s_leave(); }

/* ── Small helpers ─────────────────────────────────────────────────────── */

static void *ps_alloc(size_t n)            { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
static void *ps_realloc(void *p, size_t n) { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static bool ci_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return true;
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, n) == 0) return true;
    return false;
}

/* "Super_Mario Bros (U).nes" -> "Super Mario Bros (U)" */
static bool s_real_names;               /* file manager: show names as they are */

static void pretty(const char *name, bool is_dir, char *out, size_t len)
{
    snprintf(out, len, "%s", name);
    if (s_real_names) return;
    if (!is_dir) {
        char *dot = strrchr(out, '.');
        if (dot && dot != out) *dot = 0;
    }
    for (char *p = out; *p; p++)
        if (*p == '_') *p = ' ';
}

/* ── Line files (favorites, recents) ───────────────────────────────────── */

typedef struct {
    char  *buf;
    char **line;
    int    n;
} lines_t;

static void lines_free(lines_t *l)
{
    heap_caps_free(l->buf);
    heap_caps_free(l->line);
    memset(l, 0, sizeof(*l));
}

static void lines_load(lines_t *l, const char *path)
{
    lines_free(l);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 256 * 1024 || !(l->buf = ps_alloc(sz + 1))) {
        fclose(f);
        return;
    }
    sz = (long)fread(l->buf, 1, sz, f);
    fclose(f);
    l->buf[sz] = 0;
    int cap = 1;
    for (long i = 0; i < sz; i++) cap += l->buf[i] == '\n';
    l->line = ps_alloc(cap * sizeof(char *));
    if (!l->line) return;
    for (char *p = l->buf; *p;) {
        char *e = strchr(p, '\n');
        if (e) *e = 0;
        size_t k = strlen(p);
        while (k && (p[k - 1] == '\r' || p[k - 1] == ' ')) p[--k] = 0;
        if (k) l->line[l->n++] = p;
        if (!e) break;
        p = e + 1;
    }
}

static int lines_find(const lines_t *l, const char *s)
{
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->line[i], s) == 0) return i;
    return -1;
}

/* Rewrites `path` as: [first] + every line of l except `drop`, at most max lines. */
static void lines_write(const lines_t *l, const char *path, const char *first, const char *drop, int max)
{
    mkdir(DATA_DIR, 0775);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "cannot write %s", path);
        return;
    }
    int n = 0;
    if (first) { fprintf(f, "%s\n", first); n++; }
    for (int i = 0; i < l->n && n < max; i++) {
        if (drop && strcmp(l->line[i], drop) == 0) continue;
        fprintf(f, "%s\n", l->line[i]);
        n++;
    }
    fclose(f);
}

/* ── Browser state ─────────────────────────────────────────────────────── */

enum { TAB_ALL, TAB_FAV, TAB_RECENT, TAB_COUNT };
static const char *const TAB_NAME[TAB_COUNT] = { "All", "Favorites", "Recent" };

enum { E_DIR = 1, E_SAVE_KNOWN = 2, E_SAVE = 4 };

typedef struct {
    const char *name;            /* TAB_ALL: file name; other tabs: full path */
    uint32_t    off;             /* while loading TAB_ALL: offset into pool   */
    uint8_t     flags;
} ent_t;

static struct {
    const papp_pick_cfg_t *cfg;
    char        root[PATH_LEN];
    char        dir[PATH_LEN];   /* TAB_ALL's folder */
    int         tab;
    ent_t      *ents;
    int         n;
    int        *view;            /* indices into ents passing the search */
    int         nview;
    char       *pool;
    size_t      pool_len, pool_cap;
    lines_t     favs, recents;
    char        search[SEARCH_LEN];
    int         sel, top;
    bool        kb;              /* on-screen keyboard shown */
    lx_canvas_t cv;
} S;

void lx_ui_reset(void)
{
    heap_caps_free(s_backdrop);
    s_backdrop = NULL;
    heap_caps_free(S.ents);
    heap_caps_free(S.view);
    heap_caps_free(S.pool);
    lines_free(&S.favs);
    lines_free(&S.recents);
    memset(&S, 0, sizeof(S));
}

/* name (a file name or a path) is in the comma-separated list */
static bool in_list(const char *list, const char *name)
{
    const char *slash = strrchr(name, '/');
    if (slash) name = slash + 1;
    size_t n = strlen(name);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ',');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k == n && strncasecmp(p, name, n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static bool ext_ok(const char *name)
{
    if ((S.cfg->flags & PAPP_PICK_EXCLUDE_F) && S.cfg->exclude && in_list(S.cfg->exclude, name))
        return false;
    const char *exts = S.cfg->extensions;
    if (!exts || !*exts) return true;
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    size_t n = strlen(dot);
    for (const char *p = exts; *p;) {
        const char *e = strchr(p, ',');
        size_t k = e ? (size_t)(e - p) : strlen(p);
        if (k == n && strncasecmp(p, dot, n) == 0) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}

static void full_path(const ent_t *e, char *out, size_t len)
{
    if (S.tab == TAB_ALL) snprintf(out, len, "%s/%s", S.dir, e->name);
    else snprintf(out, len, "%s", e->name);
}

static void save_path(const char *rom, char *out, size_t len)
{
    out[0] = 0;
    if (!S.cfg->save_dir || !*S.cfg->save_dir) return;
    char name[PATH_LEN];
    snprintf(name, sizeof(name), "%s", base_name(rom));
    if (S.cfg->flags & PAPP_PICK_SAVE_NOEXT_F) {
        char *dot = strrchr(name, '.');
        if (dot && dot != name) *dot = 0;
    }
    snprintf(out, len, "%s/%s%s", S.cfg->save_dir, name, S.cfg->save_ext ? S.cfg->save_ext : ".sav");
}

static bool has_save(ent_t *e)
{
    if (e->flags & E_DIR) return false;
    if (!(e->flags & E_SAVE_KNOWN)) {
        char rom[PATH_LEN], sv[PATH_LEN];
        full_path(e, rom, sizeof(rom));
        save_path(rom, sv, sizeof(sv));
        struct stat st;
        if (sv[0] && stat(sv, &st) == 0 && st.st_size > 0) e->flags |= E_SAVE;
        e->flags |= E_SAVE_KNOWN;
    }
    return e->flags & E_SAVE;
}

static bool is_fav(const ent_t *e)
{
    if (e->flags & E_DIR) return false;
    char rom[PATH_LEN];
    full_path(e, rom, sizeof(rom));
    return lines_find(&S.favs, rom) >= 0;
}

static int ent_cmp(const void *a, const void *b)
{
    const ent_t *x = a, *y = b;
    if ((x->flags & E_DIR) != (y->flags & E_DIR)) return (y->flags & E_DIR) - (x->flags & E_DIR);
    return strcasecmp(base_name(x->name), base_name(y->name));
}

static bool add_ent(const char *name, uint8_t flags, bool pooled)
{
    if (S.n >= MAX_ENTRIES) return false;
    if (S.n % 256 == 0) {
        ent_t *ne = ps_realloc(S.ents, (S.n + 256) * sizeof(ent_t));
        if (!ne) return false;
        S.ents = ne;
    }
    ent_t *e = &S.ents[S.n];
    memset(e, 0, sizeof(*e));
    e->flags = flags;
    if (pooled) {
        size_t k = strlen(name) + 1;
        if (S.pool_len + k > S.pool_cap) {
            size_t cap = S.pool_cap ? S.pool_cap * 2 : 16384;
            while (cap < S.pool_len + k) cap *= 2;
            char *np = ps_realloc(S.pool, cap);
            if (!np) return false;
            S.pool = np;
            S.pool_cap = cap;
        }
        memcpy(S.pool + S.pool_len, name, k);
        e->off = S.pool_len;
        S.pool_len += k;
    } else {
        e->name = name;
    }
    S.n++;
    return true;
}

static void apply_filter(void)
{
    const ent_t *keep = (S.nview && S.sel < S.nview) ? &S.ents[S.view[S.sel]] : NULL;
    heap_caps_free(S.view);
    S.view = ps_alloc((S.n ? S.n : 1) * sizeof(int));
    S.nview = 0;
    int sel = 0;
    char disp[PATH_LEN];
    for (int i = 0; i < S.n && S.view; i++) {
        pretty(base_name(S.ents[i].name), S.ents[i].flags & E_DIR, disp, sizeof(disp));
        if (!ci_contains(disp, S.search)) continue;
        if (&S.ents[i] == keep) sel = S.nview;
        S.view[S.nview++] = i;
    }
    S.sel = sel;
    S.top = 0;
}

static void select_name(const char *name)
{
    for (int i = 0; i < S.nview; i++)
        if (strcmp(base_name(S.ents[S.view[i]].name), name) == 0) { S.sel = i; return; }
}

static void load_tab(void)
{
    S.n = 0;
    S.pool_len = 0;
    S.nview = 0;
    if (S.tab == TAB_ALL) {
        DIR *d = opendir(S.dir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d))) {
                if (de->d_name[0] == '.') continue;
                bool dir = de->d_type == DT_DIR;
                if (dir && (S.cfg->flags & PAPP_PICK_NO_DIRS_F)) continue;
                if (!dir && !ext_ok(de->d_name)) continue;
                if (!add_ent(de->d_name, dir ? E_DIR : 0, true)) break;
            }
            closedir(d);
        }
        for (int i = 0; i < S.n; i++) S.ents[i].name = S.pool + S.ents[i].off;
    } else {
        const lines_t *l = S.tab == TAB_FAV ? &S.favs : &S.recents;
        size_t rl = strlen(S.root);
        for (int i = 0; i < l->n; i++)
            if (strncmp(l->line[i], S.root, rl) == 0 && l->line[i][rl] == '/' && ext_ok(l->line[i]))
                add_ent(l->line[i], 0, false);
    }
    if (S.tab != TAB_RECENT && S.n > 1) qsort(S.ents, S.n, sizeof(ent_t), ent_cmp);
    S.sel = 0;
    apply_filter();
}

/* ── Drawing ───────────────────────────────────────────────────────────── */

static bool frame_begin(void)
{
    guard_in();
    uint16_t *fb = lx_comp_takeover_begin();
    if (!fb) {
        guard_out();
        return false;
    }
    const bsp_info_t *b = bsp_info();
    lx_canvas_init(&S.cv, fb, b->display_w, b->display_h, b->display_rot);
    return true;
}

static void frame_end(void)
{
    lx_comp_launcher_end();
    guard_out();
}

/* Text clipped to max_chars (an ellipsis marks the cut). */
static void text_clip(int x, int y, const char *s, uint16_t color, int max_chars)
{
    char buf[128];
    size_t n = strlen(s);
    if ((int)n > max_chars && max_chars > 3) {
        snprintf(buf, sizeof(buf), "%.*s...", max_chars - 3, s);
        s = buf;
    }
    lx_text(&S.cv, x, y, s, color, 2);
}

static void button(int x, int y, int w, int h, const char *label, bool hot)
{
    lx_fill(&S.cv, x, y, w, h, hot ? C_SEL : C_BTN);
    lx_text(&S.cv, x + (w - lx_text_width(label, 2)) / 2, y + (h - 32) / 2, label, C_TEXT, 2);
}

/* Footer buttons (touch targets), right-aligned */
typedef struct { const char *label; int act; int x, w; } fbtn_t;
enum { A_NONE, A_UP, A_DOWN, A_LEFT, A_RIGHT, A_OK, A_BACK, A_FAV, A_SEARCH, A_TABP, A_TABN, A_RUN,
       A_CHAR, A_DEL, A_CLEAR, A_PGUP, A_PGDN, A_TAP, A_ABORT };
static fbtn_t s_fbtn[] = { { "Y Search", A_SEARCH, 0, 0 }, { "X Fav", A_FAV, 0, 0 }, { "B Back", A_BACK, 0, 0 } };
#define N_FBTN (sizeof(s_fbtn) / sizeof(s_fbtn[0]))

static int list_top(void) { return HDR_H + (S.search[0] || S.kb ? BAR_H : 0); }

static int kb_top(void);

static int list_rows(void)
{
    int bottom = S.kb ? kb_top() : S.cv.h - FTR_H;
    return (bottom - list_top()) / ROW_H;
}

static void draw_list_body(void)
{
    lx_canvas_t *c = &S.cv;
    lx_fill(c, 0, 0, c->w, c->h, C_BG);

    /* Header: title + location, tabs on the right */
    lx_fill(c, 0, 0, c->w, HDR_H, C_HDR);
    char where[96];
    if (S.tab == TAB_ALL && strlen(S.dir) > strlen(S.root))
        snprintf(where, sizeof(where), "%s%s", S.cfg->title ? S.cfg->title : "", S.dir + strlen(S.root));
    else
        snprintf(where, sizeof(where), "%s", S.cfg->title ? S.cfg->title : "Browse");
    int tx = c->w - MARGIN;
    for (int t = TAB_COUNT - 1; t >= 0; t--) {
        int w = lx_text_width(TAB_NAME[t], 2) + 24;
        tx -= w;
        lx_fill(c, tx, 8, w, HDR_H - 16, t == S.tab ? C_SEL : C_HDR);
        lx_text(c, tx + 12, 12, TAB_NAME[t], t == S.tab ? C_TEXT : C_DIM, 2);
        tx -= 8;
    }
    text_clip(MARGIN, 12, where, C_TEXT, (tx - MARGIN) / CW - 1);

    /* Search bar */
    if (S.search[0] || S.kb) {
        char bar[80];
        snprintf(bar, sizeof(bar), "Search: %s%s", S.search, S.kb ? "_" : "");
        lx_fill(c, 0, HDR_H, c->w, BAR_H, C_BOX);
        lx_text(c, MARGIN, HDR_H + 4, bar, C_TEXT, 2);
        snprintf(bar, sizeof(bar), "%d match%s", S.nview, S.nview == 1 ? "" : "es");
        lx_text(c, c->w - MARGIN - lx_text_width(bar, 2), HDR_H + 4, bar, C_DIM, 2);
    }

    /* Rows */
    int rows = list_rows(), y0 = list_top();
    if (S.sel < S.top) S.top = S.sel;
    if (S.sel >= S.top + rows) S.top = S.sel - rows + 1;
    if (S.top < 0) S.top = 0;
    if (S.nview == 0) {
        const char *msg = S.search[0] ? "No matches" :
                          S.tab == TAB_FAV ? "No favorites yet (X adds one)" :
                          S.tab == TAB_RECENT ? "Nothing played yet" : "No files here";
        lx_text(c, MARGIN + 40, y0 + 16, msg, C_DIM, 2);
    }
    char disp[PATH_LEN];
    for (int r = 0; r < rows && S.top + r < S.nview; r++) {
        int i = S.top + r;
        ent_t *e = &S.ents[S.view[i]];
        int y = y0 + r * ROW_H;
        if (i == S.sel) lx_fill(c, 0, y, c->w, ROW_H, C_SEL);
        bool dir = e->flags & E_DIR;
        if (is_fav(e)) lx_text(c, MARGIN, y + 4, "*", C_FAV, 2);
        if (has_save(e)) lx_text(c, MARGIN + CW, y + 4, "S", C_SAVE, 2);
        pretty(base_name(e->name), dir, disp, sizeof(disp));
        if (dir) {
            char d[PATH_LEN + 4];
            snprintf(d, sizeof(d), "[%s]", disp);
            text_clip(MARGIN + 3 * CW, y + 4, d, C_DIR, (c->w - 2 * MARGIN) / CW - 3);
        } else {
            text_clip(MARGIN + 3 * CW, y + 4, disp, C_TEXT, (c->w - 2 * MARGIN) / CW - 3);
        }
    }

    /* Footer: position + buttons */
    if (!S.kb) {
        int fy = c->h - FTR_H;
        lx_fill(c, 0, fy, c->w, FTR_H, C_HDR);
        char pos[32];
        snprintf(pos, sizeof(pos), "%d / %d", S.nview ? S.sel + 1 : 0, S.nview);
        lx_text(c, MARGIN, fy + 8, pos, C_DIM, 2);
        int bx = c->w - MARGIN;
        for (int k = N_FBTN - 1; k >= 0; k--) {
            s_fbtn[k].w = lx_text_width(s_fbtn[k].label, 2) + 24;
            bx -= s_fbtn[k].w;
            s_fbtn[k].x = bx;
            button(bx, fy + 4, s_fbtn[k].w, FTR_H - 8, s_fbtn[k].label, false);
            bx -= 8;
        }
    }
}

/* ── Input → actions ───────────────────────────────────────────────────── */

typedef struct { int act; int ch; int x, y; } act_t;

static struct {
    int     rep_act;             /* held direction for auto-repeat */
    int     rep_code;            /* button bit or key usage that started it (+0x10000 for keys) */
    int64_t rep_next;
    bool    touch_down;
    bool    touch_held;          /* a finger down when the screen opened: ignored until lifted */
    int     tx0, ty0, tx, ty;
} I;

#define REPEAT_DELAY_US 350000
#define REPEAT_RATE_US   70000

static int pad_action(int bit)
{
    switch (bit) {
    case 0:  return A_OK;        /* A */
    case 1:  return A_BACK;      /* B */
    case 2:  return A_FAV;       /* X */
    case 3:  return A_SEARCH;    /* Y */
    case 4:  return A_TABP;      /* L1 */
    case 5:  return A_TABN;      /* R1 */
    case 6:  return A_PGUP;      /* L2 */
    case 7:  return A_PGDN;      /* R2 */
    case 9:  return A_RUN;       /* START */
    case 16: return A_UP;
    case 17: return A_DOWN;
    case 18: return A_LEFT;
    case 19: return A_RIGHT;
    default: return A_NONE;
    }
}

static int key_action(int usage)
{
    switch (usage) {
    case 0x52: return A_UP;
    case 0x51: return A_DOWN;
    case 0x50: return A_LEFT;
    case 0x4F: return A_RIGHT;
    case 0x28: case 0x58: return A_OK;     /* Enter, keypad Enter */
    case 0x29: return A_BACK;              /* Esc */
    case 0x2A: return A_DEL;               /* Backspace */
    case 0x4C: return A_CLEAR;             /* Delete */
    case 0x4B: return A_PGUP;
    case 0x4E: return A_PGDN;
    case 0x2B: return A_TABN;              /* Tab */
    default:   return A_NONE;
    }
}

static bool repeats(int act)
{
    return act == A_UP || act == A_DOWN || act == A_LEFT || act == A_RIGHT || act == A_PGUP ||
           act == A_PGDN || act == A_DEL;
}

static bool poll_action(act_t *a)
{
    int64_t now = esp_timer_get_time();
    papp_input_event_t ev;
    while (lx_input_poll_event(&ev)) {
        int act = A_NONE, code = -1;
        if (ev.type == PAPP_EV_BUTTON_DOWN || ev.type == PAPP_EV_BUTTON_UP) {
            act = pad_action(ev.code);
            code = ev.code;
        } else if (ev.type == PAPP_EV_KEY_DOWN || ev.type == PAPP_EV_KEY_UP) {
            act = key_action(ev.code);
            code = 0x10000 + ev.code;
            if (act == A_NONE && ev.type == PAPP_EV_KEY_DOWN && ev.text >= 32 && ev.text < 127) {
                *a = (act_t){ .act = A_CHAR, .ch = ev.text };
                return true;
            }
        }
        if (act == A_NONE) continue;
        if (ev.type == PAPP_EV_BUTTON_UP || ev.type == PAPP_EV_KEY_UP) {
            if (I.rep_code == code) I.rep_act = A_NONE;
            continue;
        }
        if (repeats(act)) {
            I.rep_act = act;
            I.rep_code = code;
            I.rep_next = now + REPEAT_DELAY_US;
        }
        *a = (act_t){ .act = act, .ch = code >= 0x10000 ? -1 : 0 };     /* ch -1: from a keyboard */
        return true;
    }
    if (I.rep_act != A_NONE && now >= I.rep_next) {
        I.rep_next = now + REPEAT_RATE_US;
        *a = (act_t){ .act = I.rep_act };
        return true;
    }

    /* Touch: tap = short press that didn't move; vertical swipe = page */
    if (bsp_info()->caps & BSP_CAP_TOUCH) {
        bsp_touch_point_t p[1];
        int n = bsp_touch_read(p, 1);
        if (n == 0) I.touch_held = false;
        if (n > 0 && I.touch_held) {
            /* the touch that opened the screen (an app's edge tap): its release is no tap here */
        } else if (n > 0) {
            if (!I.touch_down) { I.tx0 = p[0].x; I.ty0 = p[0].y; }
            I.touch_down = true;
            I.tx = p[0].x;
            I.ty = p[0].y;
        } else if (n == 0 && I.touch_down) {
            I.touch_down = false;
            int dx = I.tx - I.tx0, dy = I.ty - I.ty0;
            if (abs(dy) > 60 && abs(dy) > abs(dx)) {
                *a = (act_t){ .act = dy < 0 ? A_PGDN : A_PGUP };
                return true;
            }
            if (abs(dx) < 24 && abs(dy) < 24) {
                *a = (act_t){ .act = A_TAP, .x = I.tx0, .y = I.ty0 };
                return true;
            }
        }
    }
    return false;
}

static act_t wait_action(volatile bool *abort)
{
    act_t a;
    for (;;) {
        if (abort && *abort) return (act_t){ .act = A_ABORT };
        if (poll_action(&a)) return a;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static void input_reset(void)
{
    memset(&I, 0, sizeof(I));
    bsp_touch_point_t p[1];
    I.touch_held = (bsp_info()->caps & BSP_CAP_TOUCH) && bsp_touch_read(p, 1) > 0;
    lx_input_flush();
}

/* ── Modal pieces: menu, confirm, message ──────────────────────────────── */

typedef struct { const char *label; int id; } item_t;


static void paint_background(void)
{
    if (S.cfg) {
        draw_list_body();
    } else if (s_backdrop) {
        memcpy(S.cv.fb, s_backdrop, (size_t)S.cv.w * S.cv.h * 2);
        /* darken it so the box stands out: halve every channel */
        uint16_t *p = S.cv.fb;
        for (size_t i = 0, n = (size_t)S.cv.w * S.cv.h; i < n; i++) p[i] = (p[i] >> 1) & 0x7BEF;
    } else {
        lx_fill(&S.cv, 0, 0, S.cv.w, S.cv.h, C_BG);
    }
}

static void backdrop_capture(void)
{
    if (S.cfg || s_backdrop) return;
    const bsp_info_t *b = bsp_info();
    guard_in();
    s_backdrop = ps_alloc((size_t)b->display_w * b->display_h * 2);
    if (s_backdrop) lx_comp_copy_visible(s_backdrop);
    guard_out();
}

static void backdrop_release(void)
{
    guard_in();
    heap_caps_free(s_backdrop);
    s_backdrop = NULL;
    guard_out();
}

#define BOX_W     560
#define ITEM_H    48

static void draw_box_title(int bx, int by, int bw, const char *title)
{
    lx_fill(&S.cv, bx, by, bw, 48, C_HDR);
    text_clip(bx + 16, by + 8, title, C_TEXT, (bw - 32) / CW);
}

static int menu_geometry(int n, int *bx, int *by)
{
    int h = 48 + 8 + n * ITEM_H + 8;
    *bx = (S.cv.w - BOX_W) / 2;
    *by = (S.cv.h - h) / 2;
    return h;
}

static void draw_menu(const char *title, const item_t *it, int n, int sel)
{
    if (!frame_begin()) return;
    paint_background();
    int bx, by, h = menu_geometry(n, &bx, &by);
    lx_fill(&S.cv, bx - 2, by - 2, BOX_W + 4, h + 4, C_DIM);
    lx_fill(&S.cv, bx, by, BOX_W, h, C_BOX);
    draw_box_title(bx, by, BOX_W, title);
    for (int i = 0; i < n; i++)
        button(bx + 16, by + 56 + i * ITEM_H, BOX_W - 32, ITEM_H - 6, it[i].label, i == sel);
    frame_end();
}

/* Returns the chosen item's id, or -1 (back / abort). */
static int run_menu(const char *title, const item_t *it, int n, int sel, volatile bool *abort)
{
    for (;;) {
        draw_menu(title, it, n, sel);
        act_t a = wait_action(abort);
        switch (a.act) {
        case A_UP:    sel = (sel + n - 1) % n; break;
        case A_DOWN:  sel = (sel + 1) % n; break;
        case A_OK:
        case A_RUN:   return it[sel].id;
        case A_BACK:
        case A_ABORT: return -1;
        case A_TAP: {
            int bx, by;
            menu_geometry(n, &bx, &by);
            int k = (a.y - by - 56) / ITEM_H;
            if (a.x >= bx && a.x < bx + BOX_W && a.y >= by + 56 && k >= 0 && k < n) return it[k].id;
            if (a.x < bx || a.x >= bx + BOX_W) return -1;       /* tap outside closes */
            break;
        }
        default: break;
        }
    }
}

/* Word-wraps text into the box; returns the number of lines drawn (or that would be). */
static int wrap_text(int x, int y, int max_chars, const char *text, bool draw)
{
    char line[64];
    int lines = 0;
    const char *p = text;
    while (*p) {
        int n = 0, cut = -1;
        while (p[n] && p[n] != '\n' && n < max_chars) {
            if (p[n] == ' ') cut = n;
            n++;
        }
        if (p[n] && p[n] != '\n' && cut > 0) n = cut;
        snprintf(line, sizeof(line), "%.*s", n, p);
        if (draw) lx_text(&S.cv, x, y + lines * 36, line, C_TEXT, 2);
        lines++;
        p += n;
        while (*p == ' ' || *p == '\n') p++;
    }
    return lines;
}

static void message_box(const char *title, const char *text, const char *button_label, bool over_list)
{
    (void)over_list;
    if (!frame_begin()) return;
    paint_background();
    int chars = (BOX_W - 32) / CW;
    int lines = wrap_text(0, 0, chars, text, false);
    int h = 48 + 16 + lines * 36 + 16 + ITEM_H + 8;
    int bx = (S.cv.w - BOX_W) / 2, by = (S.cv.h - h) / 2;
    lx_fill(&S.cv, bx - 2, by - 2, BOX_W + 4, h + 4, C_DIM);
    lx_fill(&S.cv, bx, by, BOX_W, h, C_BOX);
    draw_box_title(bx, by, BOX_W, title);
    wrap_text(bx + 16, by + 64, chars, text, true);
    button(bx + BOX_W / 2 - 100, by + h - ITEM_H - 4, 200, ITEM_H - 6, button_label, true);
    frame_end();
}

static bool confirm(const char *title, const char *question, volatile bool *abort)
{
    item_t it[] = { { "No", 0 }, { "Yes", 1 } };
    (void)question;
    return run_menu(title, it, 2, 0, abort) == 1;
}

void lx_ui_message(const char *title, const char *text, volatile bool *abort)
{
    input_reset();
    backdrop_capture();
    message_box(title ? title : "", text ? text : "", "OK", true);
    for (;;) {
        act_t a = wait_action(abort);
        if (a.act == A_OK || a.act == A_BACK || a.act == A_TAP || a.act == A_ABORT || a.act == A_RUN) break;
    }
    backdrop_release();
    lx_input_flush();
}

int lx_ui_menu(const char *title, const char *const *items, int n, int initial, volatile bool *abort)
{
    item_t it[LX_UI_MENU_MAX];
    if (n > LX_UI_MENU_MAX) n = LX_UI_MENU_MAX;
    if (n <= 0) return -1;
    for (int i = 0; i < n; i++) it[i] = (item_t){ items[i] ? items[i] : "", i };
    input_reset();
    backdrop_capture();
    int r = run_menu(title ? title : "", it, n, initial >= 0 && initial < n ? initial : 0, abort);
    backdrop_release();
    lx_input_flush();
    return r;
}

/* ── Volume overlay (Y in emulators) ───────────────────────────────────── */

#define VOL_W     560
#define VOL_H     176
#define VOL_STEP  10
#define VOL_IDLE_US 3000000

static void vol_geometry(int *bx, int *by, int *barx, int *bary, int *barw)
{
    *bx = (S.cv.w - VOL_W) / 2;
    *by = (S.cv.h - VOL_H) / 2;
    *barx = *bx + 40;
    *bary = *by + 76;
    *barw = VOL_W - 80;
}

static void draw_volume(int level)
{
    if (!frame_begin()) return;
    paint_background();
    int bx, by, barx, bary, barw;
    vol_geometry(&bx, &by, &barx, &bary, &barw);
    lx_fill(&S.cv, bx - 2, by - 2, VOL_W + 4, VOL_H + 4, C_DIM);
    lx_fill(&S.cv, bx, by, VOL_W, VOL_H, C_BOX);
    draw_box_title(bx, by, VOL_W, "Volume");
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", level);
    lx_text(&S.cv, bx + VOL_W - 16 - lx_text_width(pct, 2), by + 8, pct, C_TEXT, 2);
    lx_fill(&S.cv, barx, bary, barw, 28, C_BTN);
    lx_fill(&S.cv, barx, bary, barw * level / 100, 28, level ? C_SAVE : C_BTN);
    for (int i = 1; i < 100 / VOL_STEP; i++)                /* step ticks */
        lx_fill(&S.cv, barx + barw * i * VOL_STEP / 100, bary, 2, 28, C_BOX);
    lx_text(&S.cv, bx + 40, by + VOL_H - 44, "< >  adjust      B / Y  close", C_DIM, 2);
    frame_end();
}

static void save_volume_task(void *arg)
{
    nvs_handle_t h;
    if (nvs_open("launcher", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "volume", (uint8_t)(intptr_t)arg);  /* same key as the home screen */
        nvs_commit(h);
        nvs_close(h);
    }
    vTaskDelete(NULL);
}

/* NVS writes flash with the cache off, and the caller may be an app thread with
 * its stack in PSRAM (Quake, Duke3D): that aborts. Write from an internal stack. */
static void save_volume(int level)
{
    xTaskCreate(save_volume_task, "vol_save", 4096, (void *)(intptr_t)level, 5, NULL);
}

int lx_ui_volume(volatile bool *abort)
{
    int level = lx_audio_master_volume_get(), start = level;
    input_reset();
    backdrop_capture();
    int64_t idle_since = esp_timer_get_time();
    bool done = false;
    while (!done) {
        draw_volume(level);
        act_t a;
        for (;;) {
            if ((abort && *abort) || esp_timer_get_time() - idle_since > VOL_IDLE_US) { done = true; break; }
            if (poll_action(&a)) break;
            vTaskDelay(pdMS_TO_TICKS(15));
        }
        if (done) break;
        idle_since = esp_timer_get_time();
        switch (a.act) {
        case A_LEFT: case A_DOWN: case A_DEL: case A_PGDN:
            level = level - VOL_STEP < 0 ? 0 : level - VOL_STEP;
            break;
        case A_RIGHT: case A_UP: case A_PGUP:
            level = level + VOL_STEP > 100 ? 100 : level + VOL_STEP;
            break;
        case A_TAP: {
            int bx, by, barx, bary, barw;
            vol_geometry(&bx, &by, &barx, &bary, &barw);
            if (a.x < bx || a.x >= bx + VOL_W || a.y < by || a.y >= by + VOL_H) { done = true; break; }
            if (a.y >= bary - 20 && a.y < bary + 48) {
                int v = (a.x - barx) * 100 / barw;
                v = (v + VOL_STEP / 2) / VOL_STEP * VOL_STEP;
                level = v < 0 ? 0 : v > 100 ? 100 : v;
            }
            break;
        }
        case A_NONE:
            break;
        default:                /* A, B, X, Y, Start: close */
            done = true;
            break;
        }
        lx_audio_master_volume(level);
    }
    if (level != start) save_volume(level);
    backdrop_release();
    lx_input_flush();
    return level;
}

/* ── On-screen keyboard ────────────────────────────────────────────────── */

static const char *const KB_ROWS[] = { "1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM._'" };
#define KB_COLS   10
#define KB_ROWN   4
#define KEY_W     72
#define KEY_H     46
enum { KB_SPACE, KB_DEL, KB_CLEAR, KB_DONE, KB_NSPECIAL };
static const char *const KB_SPECIAL[KB_NSPECIAL] = { "Space", "Del", "Clear", "Done" };

static int kb_top(void) { return S.cv.h - (KB_ROWN + 1) * (KEY_H + 4) - 8; }
static int kb_left(void) { return (S.cv.w - KB_COLS * (KEY_W + 4)) / 2; }

static void draw_keyboard(int r, int col)
{
    if (!frame_begin()) return;
    draw_list_body();
    lx_canvas_t *c = &S.cv;
    int y0 = kb_top(), x0 = kb_left();
    lx_fill(c, 0, y0 - 8, c->w, c->h - y0 + 8, C_BOX);
    char k[2] = { 0, 0 };
    for (int i = 0; i < KB_ROWN; i++)
        for (int j = 0; j < KB_COLS; j++) {
            k[0] = KB_ROWS[i][j];
            button(x0 + j * (KEY_W + 4), y0 + i * (KEY_H + 4), KEY_W, KEY_H, k[0] == ' ' ? "_" : k,
                   i == r && j == col);
        }
    int sw = (KB_COLS * (KEY_W + 4)) / KB_NSPECIAL;
    for (int j = 0; j < KB_NSPECIAL; j++)
        button(x0 + j * sw, y0 + KB_ROWN * (KEY_H + 4), sw - 4, KEY_H, KB_SPECIAL[j], r == KB_ROWN && j == col);
    frame_end();
}

static void search_changed(void)
{
    apply_filter();
}

static void keyboard(volatile bool *abort)
{
    S.kb = true;
    int r = 1, kx = 0;          /* kx: column on the 10-wide rows; the special row maps it to 4 keys */
    for (;;) {
        int col = r < KB_ROWN ? kx : kx * KB_NSPECIAL / KB_COLS;
        draw_keyboard(r, col);
        act_t a = wait_action(abort);
        int press = -1;                     /* key to activate: row*16+col */
        switch (a.act) {
        case A_UP:    r = (r + KB_ROWN) % (KB_ROWN + 1); break;
        case A_DOWN:  r = (r + 1) % (KB_ROWN + 1); break;
        case A_LEFT:
        case A_RIGHT: {
            int d = a.act == A_LEFT ? -1 : 1;
            if (r < KB_ROWN) kx = (kx + d + KB_COLS) % KB_COLS;
            else kx = (((col + d + KB_NSPECIAL) % KB_NSPECIAL) * KB_COLS + KB_NSPECIAL / 2) / KB_NSPECIAL;
            break;
        }
        case A_OK:    press = r * 16 + col; break;
        case A_DEL:
        case A_FAV:   press = KB_ROWN * 16 + KB_DEL; break;        /* X = delete */
        case A_CLEAR: press = KB_ROWN * 16 + KB_CLEAR; break;
        case A_CHAR: {
            size_t n = strlen(S.search);
            if (n < SEARCH_LEN - 1) { S.search[n] = (char)a.ch; S.search[n + 1] = 0; search_changed(); }
            break;
        }
        case A_BACK: case A_SEARCH: case A_RUN: case A_ABORT:
            S.kb = false;
            return;
        case A_TAP: {
            int y0 = kb_top(), x0 = kb_left();
            int i = (a.y - y0) / (KEY_H + 4);
            if (a.y < y0 - 8) { S.kb = false; return; }            /* tap on the list closes */
            if (i >= 0 && i < KB_ROWN) {
                int j = (a.x - x0) / (KEY_W + 4);
                if (a.x >= x0 && j >= 0 && j < KB_COLS) { r = i; kx = j; press = i * 16 + j; }
            } else if (i == KB_ROWN) {
                int j = (a.x - x0) / ((KB_COLS * (KEY_W + 4)) / KB_NSPECIAL);
                if (a.x >= x0 && j >= 0 && j < KB_NSPECIAL) {
                    r = i;
                    kx = (j * KB_COLS + KB_NSPECIAL / 2) / KB_NSPECIAL;
                    press = i * 16 + j;
                }
            }
            break;
        }
        default: break;
        }
        if (press < 0) continue;
        int pr = press / 16, pc = press % 16;
        size_t n = strlen(S.search);
        if (pr < KB_ROWN) {
            if (n < SEARCH_LEN - 1) { S.search[n] = KB_ROWS[pr][pc]; S.search[n + 1] = 0; }
        } else if (pc == KB_SPACE) {
            if (n < SEARCH_LEN - 1) { S.search[n] = ' '; S.search[n + 1] = 0; }
        } else if (pc == KB_DEL) {
            if (n) S.search[n - 1] = 0;
        } else if (pc == KB_CLEAR) {
            S.search[0] = 0;
        } else {
            S.kb = false;
            search_changed();
            return;
        }
        search_changed();
    }
}

/* ── Browser ───────────────────────────────────────────────────────────── */

enum { M_RUN = 1, M_RESUME, M_RESTART, M_DELSAVE, M_FAV, M_BACK };

static void toggle_fav(const char *rom)
{
    guard_in();
    if (lines_find(&S.favs, rom) >= 0) lines_write(&S.favs, FAV_FILE, NULL, rom, 100000);
    else lines_write(&S.favs, FAV_FILE, rom, NULL, 100000);
    lines_load(&S.favs, FAV_FILE);
    guard_out();
    if (S.tab == TAB_FAV) {
        guard_in();
        load_tab();
        guard_out();
    }
}

/* Per-ROM menu. Returns PAPP_PICK_* to finish, or 0 to stay in the list. */
static int rom_menu(ent_t *e, volatile bool *abort)
{
    if (s_real_names) return PAPP_PICK_RUN;     /* the file manager has its own menu */
    char rom[PATH_LEN], disp[PATH_LEN];
    full_path(e, rom, sizeof(rom));
    pretty(base_name(rom), false, disp, sizeof(disp));
    for (;;) {
        item_t it[6];
        int n = 0;
        guard_in();
        bool saved = has_save(e), fav = is_fav(e);
        guard_out();
        if (saved) {
            it[n++] = (item_t){ "Resume", M_RESUME };
            it[n++] = (item_t){ "Restart", M_RESTART };
            it[n++] = (item_t){ "Delete save", M_DELSAVE };
        } else {
            it[n++] = (item_t){ "Run", M_RUN };
        }
        it[n++] = (item_t){ fav ? "Remove from favorites" : "Add to favorites", M_FAV };
        it[n++] = (item_t){ "Back", M_BACK };
        int id = run_menu(disp, it, n, 0, abort);
        switch (id) {
        case M_RUN:     return PAPP_PICK_RUN;
        case M_RESUME:  return PAPP_PICK_RESUME;
        case M_RESTART: return PAPP_PICK_RESTART;
        case M_FAV:     toggle_fav(rom); break;
        case M_DELSAVE:
            if (confirm("Delete the save?", disp, abort)) {
                char sv[PATH_LEN];
                save_path(rom, sv, sizeof(sv));
                guard_in();
                remove(sv);
                e->flags &= ~(E_SAVE | E_SAVE_KNOWN);
                guard_out();
            }
            break;
        default:
            return 0;
        }
        if (abort && *abort) return 0;
    }
}

static void enter_dir(const char *name)
{
    size_t n = strlen(S.dir);
    snprintf(S.dir + n, sizeof(S.dir) - n, "/%s", name);
    S.search[0] = 0;
    guard_in();
    load_tab();
    guard_out();
}

static void leave_dir(void)
{
    char *slash = strrchr(S.dir, '/');
    if (!slash || strlen(S.dir) <= strlen(S.root)) return;
    char from[PATH_LEN];
    snprintf(from, sizeof(from), "%s", slash + 1);
    *slash = 0;
    S.search[0] = 0;
    guard_in();
    load_tab();
    guard_out();
    select_name(from);
}

static void switch_tab(int tab)
{
    S.tab = (tab + TAB_COUNT) % TAB_COUNT;
    S.search[0] = 0;
    guard_in();
    load_tab();
    guard_out();
}

static void draw_loading(const char *what)
{
    if (!frame_begin()) return;
    lx_fill(&S.cv, 0, 0, S.cv.w, S.cv.h, C_BG);
    lx_fill(&S.cv, 0, 0, S.cv.w, HDR_H, C_HDR);
    lx_text(&S.cv, MARGIN, 12, S.cfg->title ? S.cfg->title : "Browse", C_TEXT, 2);
    lx_text(&S.cv, MARGIN, HDR_H + 24, what, C_DIM, 2);
    frame_end();
}

int lx_ui_pick_file(const papp_pick_cfg_t *cfg, papp_pick_result_t *out, volatile bool *abort)
{
    memset(out, 0, sizeof(*out));
    if (!cfg || !cfg->root) return PAPP_PICK_CANCEL;
    lx_ui_reset();
    S.cfg = cfg;
    snprintf(S.root, sizeof(S.root), "%s", cfg->root);
    size_t rl = strlen(S.root);
    while (rl > 1 && S.root[rl - 1] == '/') S.root[--rl] = 0;
    snprintf(S.dir, sizeof(S.dir), "%s", S.root);
    input_reset();
    draw_loading("Loading...");

    /* Start where the user left off: the most recent ROM under this root */
    guard_in();
    lines_load(&S.favs, FAV_FILE);
    lines_load(&S.recents, RECENT_FILE);
    char last[PATH_LEN] = "";
    for (int i = 0; i < S.recents.n; i++)
        if (strncmp(S.recents.line[i], S.root, rl) == 0 && S.recents.line[i][rl] == '/') {
            snprintf(last, sizeof(last), "%s", S.recents.line[i]);
            break;
        }
    s_real_names = cfg->flags & PAPP_PICK_FILES_F;
    if (s_real_names) last[0] = 0;
    if (last[0]) {
        snprintf(S.dir, sizeof(S.dir), "%s", last);
        *strrchr(S.dir, '/') = 0;
        struct stat st;
        if (stat(S.dir, &st) != 0) snprintf(S.dir, sizeof(S.dir), "%s", S.root);
    }
    load_tab();
    guard_out();
    if (last[0]) select_name(base_name(last));

    int result = PAPP_PICK_CANCEL;
    char rom[PATH_LEN] = "";
    for (;;) {
        if (frame_begin()) {
            draw_list_body();
            frame_end();
        }
        act_t a = wait_action(abort);
        int rows = list_rows();
        ent_t *e = S.nview ? &S.ents[S.view[S.sel]] : NULL;
        int act = a.act;

        if (act == A_TAP) {
            act = A_NONE;
            if (a.y < HDR_H) {                                  /* tabs */
                int tx = S.cv.w - MARGIN;
                for (int t = TAB_COUNT - 1; t >= 0; t--) {
                    int w = lx_text_width(TAB_NAME[t], 2) + 24;
                    tx -= w;
                    if (a.x >= tx && a.x < tx + w) { switch_tab(t); break; }
                    tx -= 8;
                }
            } else if (a.y >= S.cv.h - FTR_H) {                 /* footer buttons */
                for (size_t k = 0; k < N_FBTN; k++)
                    if (a.x >= s_fbtn[k].x && a.x < s_fbtn[k].x + s_fbtn[k].w) act = s_fbtn[k].act;
            } else if (a.y >= list_top()) {                     /* rows */
                int i = S.top + (a.y - list_top()) / ROW_H;
                if (i < S.nview) {
                    if (i == S.sel) act = A_OK;
                    else S.sel = i;
                }
            } else {
                act = A_SEARCH;                                 /* the search bar */
            }
            e = S.nview ? &S.ents[S.view[S.sel]] : NULL;
        }

        switch (act) {
        case A_ABORT:
            goto done;
        case A_UP:    if (S.nview) S.sel = (S.sel + S.nview - 1) % S.nview; break;
        case A_DOWN:  if (S.nview) S.sel = (S.sel + 1) % S.nview; break;
        case A_LEFT:
        case A_PGUP:  S.sel = S.sel - rows < 0 ? 0 : S.sel - rows; break;
        case A_RIGHT:
        case A_PGDN:  S.sel = S.sel + rows >= S.nview ? (S.nview ? S.nview - 1 : 0) : S.sel + rows; break;
        case A_TABP:  switch_tab(S.tab - 1); break;
        case A_TABN:  switch_tab(S.tab + 1); break;
        case A_SEARCH: keyboard(abort); break;
        case A_CHAR: {
            size_t n = strlen(S.search);
            if (n < SEARCH_LEN - 1) { S.search[n] = (char)a.ch; S.search[n + 1] = 0; search_changed(); }
            break;
        }
        case A_DEL:
            if (S.search[0]) { S.search[strlen(S.search) - 1] = 0; search_changed(); }
            break;
        case A_CLEAR:
            S.search[0] = 0;
            search_changed();
            break;
        case A_FAV:
            if (e && !(e->flags & E_DIR)) {
                full_path(e, rom, sizeof(rom));
                toggle_fav(rom);
            }
            break;
        case A_BACK:
            if (S.search[0]) { S.search[0] = 0; search_changed(); }
            else if (S.tab == TAB_ALL && strlen(S.dir) > strlen(S.root)) leave_dir();
            else goto done;
            break;
        case A_OK:
        case A_RUN:
            if (!e) break;
            if (e->flags & E_DIR) { enter_dir(e->name); break; }
            guard_in();
            bool saved = has_save(e);
            guard_out();
            result = act == A_RUN ? (saved ? PAPP_PICK_RESUME : PAPP_PICK_RUN) : rom_menu(e, abort);
            if (result) {
                full_path(e, rom, sizeof(rom));
                goto done;
            }
            break;
        default:
            break;
        }
    }

done:
    if (result != PAPP_PICK_CANCEL && !(abort && *abort)) {
        out->action = result;
        snprintf(out->path, sizeof(out->path), "%s", rom);
        save_path(rom, out->save_path, sizeof(out->save_path));
        guard_in();
        if (!s_real_names) lines_write(&S.recents, RECENT_FILE, rom, rom, MAX_RECENT);
        guard_out();
        ESP_LOGI(TAG, "picked %s (%s)", rom, result == PAPP_PICK_RESUME ? "resume" :
                 result == PAPP_PICK_RESTART ? "restart" : "run");
    } else {
        result = PAPP_PICK_CANCEL;
    }
    guard_in();
    lx_ui_reset();
    guard_out();
    lx_input_flush();
    return result;
}

/* ── Wi-Fi picker (UI v4) ──────────────────────────────────────────────── */

#define WF_ROW_H   48
#define WF_PASS    64           /* WPA: up to 63 characters */
#define WF_BTN_W   160
#define WF_SCAN_US   15000000
#define WF_JOIN_US   45000000

static const char *const PW_ROWS[3][KB_ROWN] = {
    { "1234567890", "qwertyuiop", "asdfghjkl@", "zxcvbnm.-_" },
    { "1234567890", "QWERTYUIOP", "ASDFGHJKL@", "ZXCVBNM.-_" },
    { "!@#$%^&*()", "~`-_=+[]{}", "\\|;:'\",.<>", "/?        " },     /* a blank is no key */
};
enum { PW_PAGE, PW_SPACE, PW_DEL, PW_SHOW, PW_BACK, PW_JOIN, PW_NSPECIAL };

static struct {
    lx_net_ap_t aps[LX_NET_MAX_APS];
    int  n, sel, top;
    char pass[WF_PASS];
} W;

static int wf_rows(void) { return (S.cv.h - HDR_H - FTR_H - 8) / WF_ROW_H; }

static bool wf_is_current(const char *ssid)
{
    return lx_net_state() == LX_NET_CONNECTED && strcmp(lx_net_ssid(), ssid) == 0;
}

static void wf_header(const char *title)
{
    lx_fill(&S.cv, 0, 0, S.cv.w, S.cv.h, C_BG);
    lx_fill(&S.cv, 0, 0, S.cv.w, HDR_H, C_HDR);
    text_clip(MARGIN, (HDR_H - 32) / 2, title, C_TEXT, (S.cv.w - 2 * MARGIN) / CW);
}

static void wf_bars(int x, int y, int rssi)
{
    int n = rssi >= -55 ? 4 : rssi >= -67 ? 3 : rssi >= -75 ? 2 : 1;
    for (int i = 0; i < 4; i++) {
        int h = 8 + i * 6;
        lx_fill(&S.cv, x + i * 9, y + 32 - h, 6, h, i < n ? C_TEXT : C_BTN);
    }
}

static void wf_lock(int x, int y, uint16_t c)
{
    lx_fill(&S.cv, x + 3, y, 10, 3, c);
    lx_fill(&S.cv, x + 3, y, 3, 10, c);
    lx_fill(&S.cv, x + 10, y, 3, 10, c);
    lx_fill(&S.cv, x, y + 9, 16, 13, c);
}

static void draw_wifi_list(const char *status)
{
    if (!frame_begin()) return;
    lx_canvas_t *c = &S.cv;
    wf_header("Choose a Wi-Fi network");
    if (status) {
        wrap_text(MARGIN, HDR_H + 24, (c->w - 2 * MARGIN) / CW, status, true);
    } else {
        int rows = wf_rows();
        for (int i = W.top; i < W.n && i < W.top + rows; i++) {
            const lx_net_ap_t *ap = &W.aps[i];
            int y = HDR_H + 4 + (i - W.top) * WF_ROW_H;
            if (i == W.sel) lx_fill(c, 0, y, c->w, WF_ROW_H - 4, C_SEL);
            wf_bars(MARGIN, y + 4, ap->rssi);
            int right = c->w - MARGIN - 16;
            if (ap->secure) wf_lock(right, y + 10, C_DIM);
            if (wf_is_current(ap->ssid)) {
                right -= 9 * CW + 12;
                lx_text(c, right, y + 6, "connected", C_SAVE, 2);
            }
            text_clip(MARGIN + 48, y + 6, ap->ssid, C_TEXT, (right - MARGIN - 48 - 12) / CW);
        }
    }
    int fy = c->h - FTR_H;
    lx_fill(c, 0, fy, c->w, FTR_H, C_BOX);
    lx_text(c, MARGIN, fy + 8, "A Join  Y Scan  B Cancel", C_DIM, 2);
    button(c->w - MARGIN - 2 * WF_BTN_W - 8, fy + 4, WF_BTN_W, FTR_H - 8, "Scan", false);
    button(c->w - MARGIN - WF_BTN_W, fy + 4, WF_BTN_W, FTR_H - 8, "Cancel", false);
    frame_end();
}

/* Returns false when the user backs out (or the app must exit) while it looks */
static bool wf_scan(volatile bool *abort)
{
    draw_wifi_list("Looking for networks...");
    guard_in();
    lx_net_scan();
    guard_out();
    int64_t until = esp_timer_get_time() + WF_SCAN_US;
    int n = -1;
    while (n < 0 && esp_timer_get_time() < until) {
        if (abort && *abort) return false;
        act_t a;
        if (poll_action(&a) && a.act == A_BACK) return false;
        vTaskDelay(pdMS_TO_TICKS(100));
        guard_in();
        n = lx_net_scan_results(W.aps, LX_NET_MAX_APS);
        guard_out();
    }
    W.n = n < 0 ? 0 : n;
    W.sel = W.top = 0;
    for (int i = 0; i < W.n; i++)           /* start on the network in use */
        if (wf_is_current(W.aps[i].ssid)) W.sel = i;
    return true;
}

/* ── Password keyboard: lower case, upper case, symbols ── */

static int pw_spec_w(void) { return (KB_COLS * (KEY_W + 4)) / PW_NSPECIAL; }

static void draw_password(const char *ssid, int page, int r, int col, bool show)
{
    if (!frame_begin()) return;
    lx_canvas_t *c = &S.cv;
    char title[64];
    snprintf(title, sizeof(title), "Password for %s", ssid);
    wf_header(title);
    int fx = MARGIN, fy = HDR_H + 12, fw = c->w - 2 * MARGIN;
    lx_fill(c, fx, fy, fw, 48, C_BOX);
    char shown[WF_PASS + 1];
    size_t n = strlen(W.pass);
    for (size_t i = 0; i < n; i++) shown[i] = show ? W.pass[i] : '*';
    shown[n] = '_';
    shown[n + 1] = 0;
    int fit = (fw - 24) / CW;
    const char *t = (int)n + 1 > fit ? shown + (n + 1 - fit) : shown;     /* the end, where you type */
    lx_text(c, fx + 12, fy + 8, t, C_TEXT, 2);
    lx_text(c, MARGIN, fy + 60, "X Del  Y abc/ABC/#+=  START Join  B Back", C_DIM, 2);

    int y0 = kb_top(), x0 = kb_left();
    lx_fill(c, 0, y0 - 8, c->w, c->h - y0 + 8, C_BOX);
    char k[2] = { 0, 0 };
    for (int i = 0; i < KB_ROWN; i++)
        for (int j = 0; j < KB_COLS; j++) {
            k[0] = PW_ROWS[page][i][j];
            if (k[0] != ' ') button(x0 + j * (KEY_W + 4), y0 + i * (KEY_H + 4), KEY_W, KEY_H, k, i == r && j == col);
        }
    static const char *const next_page[3] = { "ABC", "#+=", "abc" };
    const char *spec[PW_NSPECIAL] = { next_page[page], "Space", "Del", show ? "Hide" : "Show", "Back", "Join" };
    int sw = pw_spec_w();
    for (int j = 0; j < PW_NSPECIAL; j++)
        button(x0 + j * sw, y0 + KB_ROWN * (KEY_H + 4), sw - 4, KEY_H, spec[j], r == KB_ROWN && j == col);
    frame_end();
}

static void pw_add(char ch)
{
    size_t n = strlen(W.pass);
    if (n < WF_PASS - 1) { W.pass[n] = ch; W.pass[n + 1] = 0; }
}

/* Edits W.pass. Returns 1 join, 0 back, -1 abort */
static int wf_password(const char *ssid, volatile bool *abort)
{
    static int page;            /* stays as the user left it */
    bool show = false;
    int r = 1, kx = 0;
    for (;;) {
        int col = r < KB_ROWN ? kx : kx * PW_NSPECIAL / KB_COLS;
        draw_password(ssid, page, r, col, show);
        act_t a = wait_action(abort);
        int press = -1;                     /* row*16+col */
        switch (a.act) {
        case A_UP:    r = (r + KB_ROWN) % (KB_ROWN + 1); break;
        case A_DOWN:  r = (r + 1) % (KB_ROWN + 1); break;
        case A_LEFT:
        case A_RIGHT: {
            int d = a.act == A_LEFT ? -1 : 1;
            if (r < KB_ROWN) kx = (kx + d + KB_COLS) % KB_COLS;
            else kx = (((col + d + PW_NSPECIAL) % PW_NSPECIAL) * KB_COLS + PW_NSPECIAL / 2) / PW_NSPECIAL;
            break;
        }
        case A_OK:
            if (a.ch < 0) return 1;         /* Enter on a keyboard */
            press = r * 16 + col;
            break;
        case A_RUN:    return 1;
        case A_DEL:
        case A_FAV:    press = KB_ROWN * 16 + PW_DEL; break;
        case A_SEARCH: press = KB_ROWN * 16 + PW_PAGE; break;
        case A_CLEAR:  W.pass[0] = 0; break;
        case A_CHAR:   pw_add((char)a.ch); break;
        case A_BACK:   return 0;
        case A_ABORT:  return -1;
        case A_TAP: {
            int y0 = kb_top(), x0 = kb_left();
            int i = (a.y - y0) / (KEY_H + 4);
            if (a.y < y0) break;
            if (i >= 0 && i < KB_ROWN) {
                int j = (a.x - x0) / (KEY_W + 4);
                if (a.x >= x0 && j >= 0 && j < KB_COLS) { r = i; kx = j; press = i * 16 + j; }
            } else if (i == KB_ROWN) {
                int j = (a.x - x0) / pw_spec_w();
                if (a.x >= x0 && j >= 0 && j < PW_NSPECIAL) {
                    r = i;
                    kx = (j * KB_COLS + PW_NSPECIAL / 2) / PW_NSPECIAL;
                    press = i * 16 + j;
                }
            }
            break;
        }
        default: break;
        }
        if (press < 0) continue;
        int pr = press / 16, pc = press % 16;
        if (pr < KB_ROWN) {
            char ch = PW_ROWS[page][pr][pc];
            if (ch != ' ') pw_add(ch);
        } else if (pc == PW_PAGE) {
            page = (page + 1) % 3;
        } else if (pc == PW_SPACE) {
            pw_add(' ');
        } else if (pc == PW_DEL) {
            size_t n = strlen(W.pass);
            if (n) W.pass[n - 1] = 0;
        } else if (pc == PW_SHOW) {
            show = !show;
        } else if (pc == PW_BACK) {
            return 0;
        } else {
            return 1;
        }
    }
}

/* Joins and waits. Returns 1 connected (and saved), 0 failed (told), 2 cancelled, -1 abort */
static int wf_join(const char *ssid, volatile bool *abort)
{
    char text[96];
    snprintf(text, sizeof(text), "Joining %s ...", ssid);
    message_box("Wi-Fi", text, "Cancel", false);
    guard_in();
    lx_net_connect(ssid, W.pass);
    guard_out();
    int64_t until = esp_timer_get_time() + WF_JOIN_US;
    for (;;) {
        if (abort && *abort) {
            lx_net_restore();
            return -1;
        }
        act_t a;
        if (poll_action(&a) && (a.act == A_BACK || a.act == A_OK || a.act == A_TAP)) {
            lx_net_restore();
            return 2;
        }
        lx_net_state_t st = lx_net_state();
        if (st == LX_NET_CONNECTED && strcmp(lx_net_ssid(), ssid) == 0) {
            snprintf(text, sizeof(text), "Connected to %s. Saved as this board's network.", ssid);
            message_box("Wi-Fi", text, "OK", false);
            vTaskDelay(pdMS_TO_TICKS(1200));
            return 1;
        }
        if (st == LX_NET_FAILED || st == LX_NET_NO_RADIO || esp_timer_get_time() > until) {
            lx_net_restore();
            snprintf(text, sizeof(text), "Could not join %s. Check the password and try again.", ssid);
            message_box("Wi-Fi", text, "OK", false);
            for (;;) {
                act_t b = wait_action(abort);
                if (b.act == A_ABORT) return -1;
                if (b.act == A_OK || b.act == A_BACK || b.act == A_TAP || b.act == A_RUN) break;
            }
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

int lx_ui_wifi(volatile bool *abort)
{
    input_reset();
    memset(&W, 0, sizeof(W));
    int r = 0;
    bool scan = true;
    for (;;) {
        if (scan) {
            scan = false;
            if (!wf_scan(abort)) { r = abort && *abort ? -1 : 0; break; }
        }
        draw_wifi_list(W.n ? NULL : "No networks found. Press Y or Scan to look again.");
        /* Wait for input; redraw when the connection changes (the "connected" mark) */
        bool was = lx_net_state() == LX_NET_CONNECTED;
        act_t a;
        for (;;) {
            if (abort && *abort) { a = (act_t){ .act = A_ABORT }; break; }
            if (poll_action(&a)) break;
            vTaskDelay(pdMS_TO_TICKS(15));
            if ((lx_net_state() == LX_NET_CONNECTED) != was) { a = (act_t){ .act = A_NONE }; break; }
        }
        int rows = wf_rows(), pick = -1;
        switch (a.act) {
        case A_UP:     if (W.sel > 0) W.sel--; break;
        case A_DOWN:   if (W.sel < W.n - 1) W.sel++; break;
        case A_PGUP:   W.sel = W.sel > rows ? W.sel - rows : 0; break;
        case A_PGDN:   W.sel = W.sel + rows < W.n ? W.sel + rows : (W.n ? W.n - 1 : 0); break;
        case A_OK:
        case A_RUN:    pick = W.sel; break;
        case A_SEARCH: scan = true; break;
        case A_BACK:   r = 0; goto out;
        case A_ABORT:  r = -1; goto out;
        case A_TAP:
            if (a.y >= S.cv.h - FTR_H) {
                if (a.x >= S.cv.w - MARGIN - WF_BTN_W) { r = 0; goto out; }
                if (a.x >= S.cv.w - MARGIN - 2 * WF_BTN_W - 8) scan = true;
            } else if (a.y >= HDR_H) {
                int i = W.top + (a.y - HDR_H - 4) / WF_ROW_H;
                if (i >= 0 && i < W.n) pick = W.sel = i;
            }
            break;
        default: break;
        }
        if (W.sel < W.top) W.top = W.sel;
        if (W.sel >= W.top + rows) W.top = W.sel - rows + 1;
        if (pick < 0 || pick >= W.n) continue;

        const lx_net_ap_t *ap = &W.aps[pick];
        if (wf_is_current(ap->ssid)) { r = 1; goto out; }
        memset(W.pass, 0, sizeof(W.pass));
        for (;;) {
            if (ap->secure) {
                int k = wf_password(ap->ssid, abort);
                if (k < 0) { r = -1; goto out; }
                if (k == 0) break;                      /* back to the list */
            }
            int j = wf_join(ap->ssid, abort);
            if (j == 1 || j < 0) { r = j; goto out; }
            if (j == 2 || !ap->secure) break;           /* cancelled, or an open network failed */
        }                                               /* failed: the password again, as typed */
    }
out:
    memset(W.pass, 0, sizeof(W.pass));
    lx_input_flush();
    return r;
}
