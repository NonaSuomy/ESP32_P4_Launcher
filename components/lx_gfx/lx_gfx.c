#include "lx_gfx.h"

#include <stdbool.h>
#include <string.h>
#include "font8x16.h"

void lx_canvas_init(lx_canvas_t *c, uint16_t *fb, int panel_w, int panel_h, int rot)
{
    c->fb = fb;
    c->pw = panel_w;
    c->ph = panel_h;
    c->rot = rot;
    /* A half-turn preserves the logical width and height. Only a quarter-turn
     * swaps them; treating 180 like 90/270 made the AIO file picker portrait. */
    bool quarter_turn = rot == 90 || rot == 270;
    c->w = quarter_turn ? panel_h : panel_w;
    c->h = quarter_turn ? panel_w : panel_h;
}

static inline void put(lx_canvas_t *c, int x, int y, uint16_t color)
{
    int px, py;
    switch (c->rot) {
    case 90:  px = y;             py = c->w - 1 - x; break;
    case 270: px = c->h - 1 - y;  py = x;            break;
    case 180: px = c->pw - 1 - x; py = c->ph - 1 - y; break;
    default:  px = x;             py = y;            break;
    }
    c->fb[py * c->pw + px] = color;
}

void lx_fill(lx_canvas_t *c, int x, int y, int w, int h, uint16_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (w <= 0 || h <= 0) return;

    if (c->rot == 0) {
        for (int j = 0; j < h; j++) {
            uint16_t *row = c->fb + (y + j) * c->pw + x;
            for (int i = 0; i < w; i++) row[i] = color;
        }
        return;
    }
    /* Rotated: a logical rect is a panel rect too — fill it row by row in panel space */
    int px0, py0, pw, ph;
    if (c->rot == 180) {
        px0 = c->pw - x - w;
        py0 = c->ph - y - h;
        pw = w; ph = h;
    } else {
        if (c->rot == 270) { px0 = c->h - y - h; py0 = x; }
        else               { px0 = y;            py0 = c->w - x - w; }
        pw = h; ph = w;
    }
    for (int j = 0; j < ph; j++) {
        uint16_t *row = c->fb + (py0 + j) * c->pw + px0;
        for (int i = 0; i < pw; i++) row[i] = color;
    }
}

int lx_text(lx_canvas_t *c, int x, int y, const char *s, uint16_t fg, int scale)
{
    for (; *s; s++, x += 8 * scale) {
        unsigned ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = FONT_8x16[ch - 32];
        for (int row = 0; row < 16; row++) {
            for (int col = 0; col < 8; col++) {
                if (!(g[row] & (0x80 >> col))) continue;
                int gx = x + col * scale, gy = y + row * scale;
                if (scale == 1) {
                    if (gx >= 0 && gx < c->w && gy >= 0 && gy < c->h) put(c, gx, gy, fg);
                } else {
                    lx_fill(c, gx, gy, scale, scale, fg);
                }
            }
        }
    }
    return x;
}

int lx_text_width(const char *s, int scale)
{
    return (int)strlen(s) * 8 * scale;
}
