/*
 * Game of Life (education): simple rules make patterns that move, blink and grow.
 *
 * The grid is made of cells, each alive or dead. Every generation all cells change at once:
 *   - a live cell with 2 or 3 live neighbours stays alive, otherwise it dies (alone or crowded);
 *   - a dead cell with exactly 3 live neighbours comes alive.
 * Each cell has 8 neighbours; the grid wraps around (the right edge touches the left one).
 *
 * Touch the grid to draw cells (or rub them out, if you start on a live one). Buttons: Play /
 * Pause, Step (one generation), Clear, Random, speed, and ready-made patterns placed in the
 * middle: the glider walks, the blinker blinks, the glider gun fires gliders for ever.
 * Pad: A play / pause, B step, Y random, L1 / R1 speed, X exits.
 */
#include <stdio.h>
#include <string.h>
#include "edu_ui.h"

PAPP_MANIFEST(.id = "edu.life", .name = "Game of Life", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define CELL 8                         /* pixels per cell */
#define GW 64                          /* grid: 64 x 44 cells = 512 x 352 pixels */
#define GH 44
#define GX 20                          /* grid position on the screen */
#define GY 112
#define SIDE_X 548
#define SIDE_W 232

enum { ID_PLAY = 1, ID_STEP, ID_CLEAR, ID_RANDOM, ID_SPEED, ID_GLIDER, ID_BLINKER, ID_GUN };

static uint8_t s_grid[GH][GW], s_next[GH][GW];
static uint8_t s_age[GH][GW];          /* generations a cell has been alive: new cells look brighter */
static bool s_run, s_quit;
static int s_speed = 10;               /* generations per second, 1..60 */
static uint32_t s_gen, s_live;
static uint32_t s_rng = 2463534242u;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static void count(void)
{
    s_live = 0;
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) s_live += s_grid[y][x];
}

/* One generation: count each cell's neighbours (wrapping around), apply the rules */
static void step(void)
{
    for (int y = 0; y < GH; y++) {
        int up = (y + GH - 1) % GH, down = (y + 1) % GH;
        for (int x = 0; x < GW; x++) {
            int left = (x + GW - 1) % GW, right = (x + 1) % GW;
            int n = s_grid[up][left] + s_grid[up][x] + s_grid[up][right] +
                    s_grid[y][left] + s_grid[y][right] +
                    s_grid[down][left] + s_grid[down][x] + s_grid[down][right];
            s_next[y][x] = s_grid[y][x] ? (n == 2 || n == 3) : (n == 3);
        }
    }
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            s_age[y][x] = s_next[y][x] ? (s_age[y][x] < 255 ? s_age[y][x] + 1 : 255) : 0;
            s_grid[y][x] = s_next[y][x];
        }
    s_gen++;
    count();
}

static void clear(void)
{
    memset(s_grid, 0, sizeof(s_grid));
    memset(s_age, 0, sizeof(s_age));
    s_gen = 0;
    s_live = 0;
}

static void randomize(void)
{
    clear();
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) s_grid[y][x] = rnd() % 100 < 28;
    count();
}

/* A pattern as text rows ('O' = alive), placed with its middle at the middle of the grid */
static void place(const char *const *rows, int n)
{
    int w = (int)strlen(rows[0]);
    int x0 = (GW - w) / 2, y0 = (GH - n) / 2;
    for (int j = 0; j < n; j++)
        for (int i = 0; rows[j][i]; i++)
            if (rows[j][i] == 'O') {
                s_grid[(y0 + j + GH) % GH][(x0 + i + GW) % GW] = 1;
                s_age[(y0 + j + GH) % GH][(x0 + i + GW) % GW] = 0;
            }
    count();
}

static const char *const glider[] = { ".O.", "..O", "OOO" };
static const char *const blinker[] = { "OOO" };
static const char *const gun[] = {      /* Gosper's glider gun: a new glider every 30 generations */
    "........................O...........",
    "......................O.O...........",
    "............OO......OO............OO",
    "...........O...O....OO............OO",
    "OO........O.....O...OO..............",
    "OO........O...O.OO....O.O...........",
    "..........O.....O.......O...........",
    "...........O...O....................",
    "............OO......................",
};

static void act(int id, int x)
{
    switch (id) {
    case ID_PLAY: s_run = !s_run; break;
    case ID_STEP: s_run = false; step(); break;
    case ID_CLEAR: s_run = false; clear(); break;
    case ID_RANDOM: randomize(); break;
    case ID_SPEED: {                    /* the slider: 1..60 along its width */
        int v = 1 + (x - (SIDE_X + 16)) * 59 / (SIDE_W - 32);
        s_speed = v < 1 ? 1 : v > 60 ? 60 : v;
        break;
    }
    case ID_GLIDER: place(glider, 3); break;
    case ID_BLINKER: place(blinker, 1); break;
    case ID_GUN: place(gun, 9); break;
    case UI_ID_CLOSE: s_quit = true; break;
    }
}

/* Touch on the grid: draw, or rub out when the stroke started on a live cell */
static void touch_grid(papp_surface_t *s)
{
    static int mode = -1;               /* -1 not touching, 0 rub out, 1 draw */
    static int last_x = -1, last_y = -1;
    papp_touch_point_t p;
    if (papp_disp_touch(s, &p, 1) <= 0) {
        mode = -1;
        return;
    }
    int cx = (p.x - GX) / CELL, cy = (p.y - GY) / CELL;
    if (p.x < GX || p.y < GY || cx >= GW || cy >= GH) {
        if (mode < 0) mode = -2;        /* started outside: a button, not drawing */
        return;
    }
    if (mode == -2) return;
    if (mode < 0) {
        mode = !s_grid[cy][cx];
        last_x = cx;
        last_y = cy;
    }
    /* fill the cells between the last point and this one, so a fast stroke has no gaps */
    int dx = cx - last_x, dy = cy - last_y, n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    for (int i = 0; i <= n; i++) {
        int x = n ? last_x + dx * i / n : cx, y = n ? last_y + dy * i / n : cy;
        s_grid[y][x] = (uint8_t)mode;
        s_age[y][x] = 0;
    }
    last_x = cx;
    last_y = cy;
    count();
}

static void draw_grid(void)
{
    int stride;
    uint16_t *px = ui_pixels(&stride);
    const uint16_t dead = UI_RGB(0x0B1730), line = UI_RGB(0x12223F);
    for (int y = 0; y < GH; y++)
        for (int x = 0; x < GW; x++) {
            uint16_t c = dead;
            if (s_grid[y][x]) {
                int a = s_age[y][x];            /* newborn: white-green, settled: green, old: teal */
                c = a < 2 ? UI_RGB(0xC8FFD8) : a < 10 ? C_GREEN : UI_RGB(0x2BB3A0);
            }
            uint16_t *row = &px[(GY + y * CELL) * stride + GX + x * CELL];
            for (int j = 0; j < CELL; j++, row += stride)
                for (int i = 0; i < CELL; i++) row[i] = (i == CELL - 1 || j == CELL - 1) ? line : c;
        }
}

static void draw(papp_surface_t *s)
{
    ui_begin(s);
    char status[96];
    snprintf(status, sizeof(status), "Generation %lu     Live cells %lu     %d per second",
             (unsigned long)s_gen, (unsigned long)s_live, s_speed);
    ui_bar("Game of Life", NULL, 0, 0, status, s_run ? C_GREEN : C_ORANGE, s_run);
    draw_grid();

    ui_rrect(SIDE_X, GY, SIDE_W, GH * CELL, 18, C_PANEL);
    int x = SIDE_X + 16, w = SIDE_W - 32;
    ui_button(x, 124, w, 48, s_run ? "Pause" : "Play", s_run ? C_ORANGE : C_BLUE, ID_PLAY);
    ui_button(x, 180, w / 2 - 4, 40, "Step", C_RAISED, ID_STEP);
    ui_button(x + w / 2 + 4, 180, w / 2 - 4, 40, "Clear", C_RAISED, ID_CLEAR);
    ui_button(x, 228, w, 40, "Random", C_RAISED, ID_RANDOM);

    ui_text_l(ui_small, x, 288, "SPEED", C_FAINT);              /* slider */
    int kx = x + (s_speed - 1) * w / 59;
    ui_rrect(x, 304, w, 8, 4, C_BORDER);
    ui_rrect(x, 304, kx - x, 8, 4, C_BLUE);
    ui_disc(kx, 308, 12, ui_pressed_color(ID_SPEED, C_TEXT), 255);
    ui_hit(x - 12, 288, w + 24, 40, ID_SPEED, UI_DRAG);

    ui_text_l(ui_small, x, 342, "PATTERNS", C_FAINT);
    ui_button(x, 358, w, 32, "Glider", C_RAISED, ID_GLIDER);
    ui_button(x, 395, w, 32, "Blinker", C_RAISED, ID_BLINKER);
    ui_button(x, 432, w, 32, "Glider gun", C_RAISED, ID_GUN);
    ui_end(s);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    papp_surface_t *s = ui_open();
    if (!s) return 1;
    s_rng ^= (uint32_t)papp_time_us();
    place(gun, 9);                      /* something to watch: press Play */

    uint32_t prev = 0xFFFFFFFFu;        /* nothing counts as pressed until released once */
    int64_t next = 0, speed_at = 0;
    while (!papp_exit_requested() && !s_quit) {
        int64_t now = papp_time_us();
        papp_gamepad_t gp = { 0 };
        papp_input_gamepad(0, &gp);
        uint32_t press = gp.buttons & ~prev;
        prev = gp.buttons;
        if (press & PAPP_BTN_X) break;
        if (press & PAPP_BTN_A) s_run = !s_run;
        if (press & PAPP_BTN_B) { s_run = false; step(); }
        if (press & PAPP_BTN_Y) randomize();
        if ((gp.buttons & (PAPP_BTN_L1 | PAPP_BTN_R1)) && now >= speed_at) {
            if (gp.buttons & PAPP_BTN_L1 && s_speed > 1) s_speed--;
            if (gp.buttons & PAPP_BTN_R1 && s_speed < 60) s_speed++;
            speed_at = now + ((press & (PAPP_BTN_L1 | PAPP_BTN_R1)) ? 300000 : 50000);
        }

        touch_grid(s);
        ui_touch(s, act);
        if (s_run && now >= next) {
            step();
            next = now + 1000000 / s_speed;
        }
        draw(s);
    }
    papp_disp_surface_destroy(s);
    return 0;
}
