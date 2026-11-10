/*
 * Touch Paint (education): draw with your fingers, several at once.
 *
 * The screen reports up to 5 touch points, each with its own number, so every finger draws its
 * own line. A line is a row of round dots close together (closer than the brush size, or there
 * would be gaps). Fill colours the area around the touch: it spreads to every neighbouring
 * pixel of the same colour (a flood fill, done a row at a time with a list of rows to visit).
 * Save writes a BMP picture to /sd/pictures (it opens on a PC too); Open loads one back.
 *
 * Pad: A / B pick the next / previous colour, L1 / R1 brush size, Y undo, START save,
 * SELECT open, X exits. Argument selftest: fills a ring inside and out, saves, exit code 0 = OK.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "edu_ui.h"

PAPP_MANIFEST(.id = "edu.paint", .name = "Touch Paint", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define CW 592                          /* the canvas */
#define CH 352
#define CX 20
#define CY 112
#define PX 624                          /* the panel */
#define PW 156
#define DIR "/sd/pictures"

enum { T_BRUSH, T_FILL, T_ERASER };
enum { ID_COLOR = 1, ID_SIZE = 20, ID_BRUSH = 30, ID_FILL, ID_ERASER, ID_UNDO, ID_CLEAR, ID_OPEN, ID_SAVE };

static const uint32_t colors[12] = {
    0x111111, 0xFFFFFF, 0xE53935, 0xFB8C00, 0xFDD835, 0x43A047,
    0x00ACC1, 0x1E88E5, 0x8E24AA, 0xEC407A, 0x8D6E63, 0x9E9E9E,
};
static const int sizes[3] = { 3, 8, 18 };            /* brush radius */

static uint16_t *s_canvas, *s_undo;
static int s_color = 7, s_size = 1, s_tool = T_BRUSH;
static bool s_quit, s_dirty_undo;
static char s_note[64];                               /* shown in the status line for a while */
static int64_t s_note_until;

static void note(const char *msg)
{
    snprintf(s_note, sizeof(s_note), "%s", msg);
    s_note_until = papp_time_us() + 3000000;
}

static void save_undo(void)
{
    memcpy(s_undo, s_canvas, CW * CH * 2);
    s_dirty_undo = true;
}

/* ── Drawing on the canvas ─────────────────────────────────────────────── */

static void dot(int cx, int cy, int r, uint16_t c)
{
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= CH) continue;
        int dy = y - cy, half = 0;
        while ((half + 1) * (half + 1) + dy * dy <= r * r) half++;
        int x0 = cx - half < 0 ? 0 : cx - half, x1 = cx + half >= CW ? CW - 1 : cx + half;
        for (int x = x0; x <= x1; x++) s_canvas[y * CW + x] = c;
    }
}

static void line(int x0, int y0, int x1, int y1, int r, uint16_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int len = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    int step = r / 2 > 0 ? r / 2 : 1;                 /* dots closer than the brush: no gaps */
    for (int i = 0; i <= len; i += step) dot(x0 + dx * i / (len ? len : 1), y0 + dy * i / (len ? len : 1), r, c);
    dot(x1, y1, r, c);
}

/* Flood fill: a row at a time. Each entry is a seed pixel; fill left and right of it as far as
 * the old colour goes, then queue the rows above and below. */
static void fill(int sx, int sy, uint16_t c)
{
    uint16_t old = s_canvas[sy * CW + sx];
    if (old == c) return;
    int cap = CW * CH / 4, n = 0;
    int32_t *stack = papp_alloc(cap * sizeof(int32_t), PAPP_MEM_LARGE, 4);
    if (!stack) return;
    stack[n++] = sy * CW + sx;
    while (n > 0) {
        int p = stack[--n], y = p / CW, x = p % CW;
        if (s_canvas[p] != old) continue;
        int l = x, r = x;
        while (l > 0 && s_canvas[y * CW + l - 1] == old) l--;
        while (r < CW - 1 && s_canvas[y * CW + r + 1] == old) r++;
        for (int i = l; i <= r; i++) s_canvas[y * CW + i] = c;
        for (int ny = y - 1; ny <= y + 1; ny += 2) {   /* one seed per run of old colour above / below */
            if (ny < 0 || ny >= CH) continue;
            bool in = false;
            for (int i = l; i <= r; i++) {
                bool o = s_canvas[ny * CW + i] == old;
                if (o && !in && n < cap) stack[n++] = ny * CW + i;
                in = o;
            }
        }
    }
    papp_free(stack);
}

/* ── BMP files ─────────────────────────────────────────────────────────── */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }

static void save_bmp(void)
{
    mkdir(DIR, 0777);
    char path[64];
    FILE *f = NULL;
    for (int i = 1; i < 1000; i++) {                  /* the first free name */
        snprintf(path, sizeof(path), DIR "/paint_%03d.bmp", i);
        FILE *t = fopen(path, "rb");
        if (!t) { f = fopen(path, "wb"); break; }
        fclose(t);
    }
    if (!f) { note("Could not save (is the card in?)"); return; }
    uint8_t h[54] = { 'B', 'M' };
    int row = CW * 3;                                 /* 1776: already a multiple of 4 */
    put32(h + 2, 54 + row * CH);
    put32(h + 10, 54);
    put32(h + 14, 40);
    put32(h + 18, CW);
    put32(h + 22, CH);                                /* positive: rows bottom-up */
    put16(h + 26, 1);
    put16(h + 28, 24);
    put32(h + 34, row * CH);
    fwrite(h, 1, 54, f);
    uint8_t *buf = malloc(row);
    bool ok = buf != NULL;
    for (int y = CH - 1; y >= 0 && ok; y--) {
        for (int x = 0; x < CW; x++) {                /* RGB565 -> blue, green, red bytes */
            uint16_t c = s_canvas[y * CW + x];
            buf[x * 3 + 0] = (uint8_t)((c & 0x1F) * 255 / 31);
            buf[x * 3 + 1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
            buf[x * 3 + 2] = (uint8_t)((c >> 11) * 255 / 31);
        }
        ok = fwrite(buf, 1, row, f) == (size_t)row;
    }
    free(buf);
    ok = fclose(f) == 0 && ok;
    char msg[64];
    snprintf(msg, sizeof(msg), ok ? "Saved %s" : "Could not save %s", path + sizeof(DIR));
    note(msg);
    papp_log("%s\n", msg);
}

static uint32_t get32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* Loads a 24 or 32-bit BMP into the canvas, centred (bigger pictures are cut) */
static bool load_bmp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t h[54];
    bool ok = fread(h, 1, 54, f) == 54 && h[0] == 'B' && h[1] == 'M';
    int w = ok ? (int32_t)get32(h + 18) : 0, hh = ok ? (int32_t)get32(h + 22) : 0;
    int bpp = h[28], comp = (int)get32(h + 30);
    if (!ok || w <= 0 || w > 8192 || hh == 0 || (bpp != 24 && bpp != 32) || (comp != 0 && comp != 3)) {
        fclose(f);
        return false;
    }
    bool top_down = hh < 0;
    if (top_down) hh = -hh;
    int row = (w * (bpp / 8) + 3) & ~3;
    uint8_t *buf = malloc(row);
    if (!buf) { fclose(f); return false; }
    save_undo();
    for (int i = 0; i < CW * CH; i++) s_canvas[i] = 0xFFFF;
    int ox = (CW - w) / 2, oy = (CH - hh) / 2;
    fseek(f, (long)get32(h + 10), SEEK_SET);
    for (int r = 0; r < hh; r++) {
        if (fread(buf, 1, row, f) != (size_t)row) break;
        int y = (top_down ? r : hh - 1 - r) + oy;
        if (y < 0 || y >= CH) continue;
        for (int x = 0; x < w; x++) {
            int cx = x + ox;
            if (cx < 0 || cx >= CW) continue;
            const uint8_t *p = &buf[x * (bpp / 8)];
            s_canvas[y * CW + cx] = PAPP_RGB565(p[2], p[1], p[0]);
        }
    }
    free(buf);
    fclose(f);
    return true;
}

static void open_picture(void)
{
    mkdir(DIR, 0777);
    papp_pick_cfg_t cfg = { .title = "Open a picture", .root = DIR, .extensions = ".bmp",
                            .flags = PAPP_PICK_FILES_F };
    papp_pick_result_t r = { 0 };
    if (papp_ui_pick_file(&cfg, &r) == PAPP_PICK_CANCEL) return;
    const char *name = strrchr(r.path, '/');
    char msg[64];
    snprintf(msg, sizeof(msg), load_bmp(r.path) ? "Opened %s" : "Can't open %s (24 or 32-bit BMP only)",
             name ? name + 1 : r.path);
    note(msg);
}

/* ── Touch ─────────────────────────────────────────────────────────────── */

typedef struct { bool down; int x, y; } finger_t;
static finger_t s_finger[256];                        /* by touch id */

static void touch_canvas(papp_surface_t *s)
{
    papp_touch_point_t pts[5];
    int n = papp_disp_touch(s, pts, 5);
    bool seen[256] = { 0 };
    uint16_t c = s_tool == T_ERASER ? 0xFFFF : UI_RGB(colors[s_color]);
    int r = sizes[s_size] * (s_tool == T_ERASER ? 2 : 1);
    for (int i = 0; i < n; i++) {
        finger_t *f = &s_finger[pts[i].id];
        int x = pts[i].x - CX, y = pts[i].y - CY;
        seen[pts[i].id] = true;
        bool inside = x >= 0 && x < CW && y >= 0 && y < CH;
        if (!f->down) {                               /* a new finger */
            f->down = true;
            f->x = inside ? x : -1;                   /* -1: started outside, not drawing */
            f->y = y;
            if (!inside) continue;
            save_undo();
            if (s_tool == T_FILL) fill(x, y, c);
            else dot(x, y, r, c);
            continue;
        }
        if (f->x < 0 || s_tool == T_FILL) continue;
        if (x < -r) x = -r;                           /* follow the finger off the edge a little */
        if (y < -r) y = -r;
        if (x > CW + r) x = CW + r;
        if (y > CH + r) y = CH + r;
        line(f->x, f->y, x, y, r, c);
        f->x = x;
        f->y = y;
    }
    for (int i = 0; i < 256; i++)
        if (!seen[i]) s_finger[i].down = false;
}

/* ── Screen ────────────────────────────────────────────────────────────── */

static void small_button(int x, int y, int w, int h, const char *label, bool on, int id)
{
    ui_rrect(x, y, w, h, h / 2, ui_pressed_color(id, on ? C_BLUE : C_RAISED));
    ui_text_c(ui_small, x + w / 2, y + h / 2, label, on ? C_TEXT : C_DIM);
    ui_hit(x, y, w, h, id, UI_TAP);
}

static void draw(papp_surface_t *s)
{
    ui_begin(s);
    char status[96];
    static const char *const tool[] = { "Brush", "Fill", "Eraser" };
    if (papp_time_us() < s_note_until) snprintf(status, sizeof(status), "%s", s_note);
    else snprintf(status, sizeof(status), "%s     Use several fingers at once", tool[s_tool]);
    ui_bar("Touch Paint", NULL, 0, 0, status, UI_RGB(colors[s_color]), false);

    int stride;
    uint16_t *px = ui_pixels(&stride);
    for (int y = 0; y < CH; y++) memcpy(&px[(CY + y) * stride + CX], &s_canvas[y * CW], CW * 2);

    ui_rrect(PX, CY, PW, CH, 18, C_PANEL);
    for (int i = 0; i < 12; i++) {                    /* colours: 4 x 3 */
        int cx = PX + 22 + (i % 4) * 37, cy = CY + 24 + (i / 4) * 37;
        if (i == s_color) ui_disc(cx, cy, 18, C_TEXT, 255);
        ui_disc(cx, cy, 15, UI_RGB(colors[i]), 255);
        ui_hit(cx - 18, cy - 18, 37, 37, ID_COLOR + i, UI_TAP);
    }
    for (int i = 0; i < 3; i++) {                     /* brush sizes */
        int cx = PX + 30 + i * 48, cy = CY + 152;
        if (i == s_size) ui_disc(cx, cy, 22, C_RAISED, 255);
        ui_disc(cx, cy, sizes[i] < 4 ? 4 : sizes[i], i == s_size ? C_TEXT : C_DIM, 255);
        ui_hit(cx - 24, cy - 24, 48, 48, ID_SIZE + i, UI_TAP);
    }
    int x = PX + 8, w = (PW - 24) / 2, x2 = x + w + 8;
    small_button(x, 296, w, 34, "Brush", s_tool == T_BRUSH, ID_BRUSH);
    small_button(x2, 296, w, 34, "Fill", s_tool == T_FILL, ID_FILL);
    small_button(x, 338, w, 34, "Eraser", s_tool == T_ERASER, ID_ERASER);
    small_button(x2, 338, w, 34, "Undo", false, ID_UNDO);
    small_button(x, 380, w, 34, "Clear", false, ID_CLEAR);
    small_button(x2, 380, w, 34, "Open", false, ID_OPEN);
    ui_button(x, 422, PW - 16, 36, "Save", C_GREEN, ID_SAVE);
    ui_end(s);
}

static void undo(void)
{
    if (!s_dirty_undo) return;
    for (int i = 0; i < CW * CH; i++) {               /* swap: Undo twice is Redo */
        uint16_t t = s_canvas[i];
        s_canvas[i] = s_undo[i];
        s_undo[i] = t;
    }
}

static int s_action;                                  /* Save and Open run after the frame */

static void act(int id, int x)
{
    (void)x;
    if (id >= ID_COLOR && id < ID_COLOR + 12) {
        s_color = id - ID_COLOR;
        if (s_tool == T_ERASER) s_tool = T_BRUSH;
    } else if (id >= ID_SIZE && id < ID_SIZE + 3) s_size = id - ID_SIZE;
    else if (id == ID_BRUSH) s_tool = T_BRUSH;
    else if (id == ID_FILL) s_tool = T_FILL;
    else if (id == ID_ERASER) s_tool = T_ERASER;
    else if (id == ID_UNDO) undo();
    else if (id == ID_CLEAR) {
        save_undo();
        for (int i = 0; i < CW * CH; i++) s_canvas[i] = 0xFFFF;
    } else if (id == ID_OPEN || id == ID_SAVE) s_action = id;
    else if (id == UI_ID_CLOSE) s_quit = true;
}

int main(int argc, char **argv)
{
    papp_surface_t *s = ui_open();
    s_canvas = papp_alloc(CW * CH * 2, PAPP_MEM_LARGE, 64);
    s_undo = papp_alloc(CW * CH * 2, PAPP_MEM_LARGE, 64);
    if (!s || !s_canvas || !s_undo) return 1;
    for (int i = 0; i < CW * CH; i++) s_canvas[i] = 0xFFFF;
    /* a little hello, so the canvas isn't blank */
    line(40, 300, 140, 200, 6, UI_RGB(0xE53935));
    line(140, 200, 240, 300, 6, UI_RGB(0xFB8C00));
    dot(470, 80, 34, UI_RGB(0xFDD835));

    if (argc > 1 && strcmp(argv[1], "selftest") == 0) {  /* fill inside and outside a ring, save */
        dot(300, 180, 60, 0);
        dot(300, 180, 50, 0xFFFF);
        int64_t t0 = papp_time_us();
        fill(300, 180, UI_RGB(0xE53935));
        fill(5, 5, UI_RGB(0x43A047));
        int64_t t = papp_time_us() - t0;
        int red = 0, green = 0, white = 0;
        for (int i = 0; i < CW * CH; i++) {
            red += s_canvas[i] == UI_RGB(0xE53935);
            green += s_canvas[i] == UI_RGB(0x43A047);
            white += s_canvas[i] == 0xFFFF;
        }
        papp_log("fill: red %d green %d white %d in %d ms\n", red, green, white, (int)(t / 1000));
        save_bmp();
        s_quit = true;
        s_action = white == 0 ? 0 : 2;                  /* the exit code */
    }

    uint32_t prev = 0xFFFFFFFFu;
    while (!papp_exit_requested() && !s_quit) {
        papp_gamepad_t gp = { 0 };
        papp_input_gamepad(0, &gp);
        uint32_t press = gp.buttons & ~prev;
        prev = gp.buttons;
        if (press & PAPP_BTN_X) break;
        if (press & PAPP_BTN_A) s_color = (s_color + 1) % 12;
        if (press & PAPP_BTN_B) s_color = (s_color + 11) % 12;
        if (press & PAPP_BTN_L1 && s_size > 0) s_size--;
        if (press & PAPP_BTN_R1 && s_size < 2) s_size++;
        if (press & PAPP_BTN_Y) undo();
        if (press & PAPP_BTN_START) s_action = ID_SAVE;
        if (press & PAPP_BTN_SELECT) s_action = ID_OPEN;

        touch_canvas(s);
        ui_touch(s, act);
        draw(s);
        if (s_action == ID_SAVE) save_bmp();
        else if (s_action == ID_OPEN) {
            open_picture();
            prev = 0xFFFFFFFFu;                       /* the button that closed the picker isn't ours */
        }
        s_action = 0;
    }
    papp_free(s_undo);
    papp_free(s_canvas);
    papp_disp_surface_destroy(s);
    return s_quit ? s_action : 0;
}
