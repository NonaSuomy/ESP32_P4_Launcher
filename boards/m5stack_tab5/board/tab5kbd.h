/*
 * The clip-on Tab5 Keyboard (M5Stack): its key events turned into keyboard reports (modifier byte and up to six
 * HID usages, as a USB boot keyboard sends), which the launcher already understands. Hardware-free, so it builds
 * on a computer too (tests/test_tab5kbd.c).
 *
 * The keyboard is 5 rows of 14 keys behind an I2C controller (board_keyboard.c reads it). Each event is a byte:
 * bit 7 down, bits 6..4 the row, bits 3..0 the column. Four keys are layer keys and send nothing themselves:
 *   Sym   the legends printed above the keys (! @ # ... : " { } |), and an Fn layer where a key has none
 *         (F1..F12 on the number row, Insert on Del, PageUp/PageDown/Home/End on the arrows)
 *   Aa    Shift while held; two quick taps turn Caps Lock on or off (kept here: the launcher has no Caps Lock)
 *   Ctrl, Alt (Sym+Alt is GUI)
 * The legends follow M5's own keyboard firmware (user_keyboard_handle.c, MIT).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TAB5KBD_ROWS 5
#define TAB5KBD_COLS 14
#define TAB5KBD_QUEUE 16
#define TAB5KBD_DOUBLE_TAP_MS 400

#define TAB5KBD_MOD_LCTRL  0x01
#define TAB5KBD_MOD_LSHIFT 0x02
#define TAB5KBD_MOD_LALT   0x04
#define TAB5KBD_MOD_LGUI   0x08

typedef struct { uint8_t mods; uint8_t keys[6]; } tab5kbd_report_t;

typedef struct {
    uint16_t held[TAB5KBD_ROWS];
    uint8_t sent[TAB5KBD_ROWS][TAB5KBD_COLS];      /* the usage each held key sent, for its release */
    uint8_t sent_shift[TAB5KBD_ROWS][TAB5KBD_COLS];
    bool sym, aa, ctrl, alt, caps, aa_other;
    uint32_t aa_down_at, aa_last_tap;
    tab5kbd_report_t now;                          /* the report as it stands */
    tab5kbd_report_t queue[TAB5KBD_QUEUE];         /* reports not yet taken, oldest first */
    int head, tail;
} tab5kbd_t;

/* usage, Shift as printed, then the same with Sym held */
typedef struct { uint8_t usage, shift, sym_usage, sym_shift; } tab5kbd_legend_t;

enum {
    K_ESC = 0x29, K_BKSP = 0x2A, K_TAB = 0x2B, K_SPACE = 0x2C, K_MINUS = 0x2D, K_EQUAL = 0x2E, K_LBRACE = 0x2F,
    K_RBRACE = 0x30, K_BSLASH = 0x31, K_SEMI = 0x33, K_APOS = 0x34, K_GRAVE = 0x35, K_COMMA = 0x36, K_DOT = 0x37,
    K_SLASH = 0x38, K_ENTER = 0x28, K_F1 = 0x3A, K_INSERT = 0x49, K_HOME = 0x4A, K_PGUP = 0x4B, K_DELETE = 0x4C,
    K_END = 0x4D, K_PGDN = 0x4E, K_RIGHT = 0x4F, K_LEFT = 0x50, K_DOWN = 0x51, K_UP = 0x52,
};

static const tab5kbd_legend_t tab5kbd_map[TAB5KBD_ROWS][TAB5KBD_COLS] = {
    /* esc 1 2 3 4 5 6 7 8 9 0 - + del   (Sym: F1..F12 on 1..+, Insert on del) */
    {{K_ESC, 0, K_ESC, 0}, {0x1E, 0, K_F1 + 0, 0}, {0x1F, 0, K_F1 + 1, 0}, {0x20, 0, K_F1 + 2, 0},
     {0x21, 0, K_F1 + 3, 0}, {0x22, 0, K_F1 + 4, 0}, {0x23, 0, K_F1 + 5, 0}, {0x24, 0, K_F1 + 6, 0},
     {0x25, 0, K_F1 + 7, 0}, {0x26, 0, K_F1 + 8, 0}, {0x27, 0, K_F1 + 9, 0}, {K_MINUS, 0, K_F1 + 10, 0},
     {K_EQUAL, 1, K_F1 + 11, 0}, {K_DELETE, 0, K_INSERT, 0}},
    /* ` ! @ # $ % ^ & * ( ) [ ] \   (Sym: ~ ? @ # $ % ^ & / < > { } |) */
    {{K_GRAVE, 0, K_GRAVE, 1}, {0x1E, 1, K_SLASH, 1}, {0x1F, 1, 0x1F, 1}, {0x20, 1, 0x20, 1},
     {0x21, 1, 0x21, 1}, {0x22, 1, 0x22, 1}, {0x23, 1, 0x23, 1}, {0x24, 1, 0x24, 1},
     {0x25, 1, K_SLASH, 0}, {0x26, 1, K_COMMA, 1}, {0x27, 1, K_DOT, 1}, {K_LBRACE, 0, K_LBRACE, 1},
     {K_RBRACE, 0, K_RBRACE, 1}, {K_BSLASH, 0, K_BSLASH, 1}},
    /* tab q w e r t y u i o p ; ' backspace   (Sym: ; -> :, ' -> ") */
    {{K_TAB, 0, K_TAB, 0}, {0x14, 0, 0x14, 0}, {0x1A, 0, 0x1A, 0}, {0x08, 0, 0x08, 0},
     {0x15, 0, 0x15, 0}, {0x17, 0, 0x17, 0}, {0x1C, 0, 0x1C, 0}, {0x18, 0, 0x18, 0},
     {0x0C, 0, 0x0C, 0}, {0x12, 0, 0x12, 0}, {0x13, 0, 0x13, 0}, {K_SEMI, 0, K_SEMI, 1},
     {K_APOS, 0, K_APOS, 1}, {K_BKSP, 0, K_BKSP, 0}},
    /* sym Aa a s d f g h j k l up _ enter   (Sym: up -> PageUp, _ -> =) */
    {{0, 0, 0, 0}, {0, 0, 0, 0}, {0x04, 0, 0x04, 0}, {0x16, 0, 0x16, 0},
     {0x07, 0, 0x07, 0}, {0x09, 0, 0x09, 0}, {0x0A, 0, 0x0A, 0}, {0x0B, 0, 0x0B, 0},
     {0x0D, 0, 0x0D, 0}, {0x0E, 0, 0x0E, 0}, {0x0F, 0, 0x0F, 0}, {K_UP, 0, K_PGUP, 0},
     {K_MINUS, 1, K_EQUAL, 0}, {K_ENTER, 0, K_ENTER, 0}},
    /* ctrl alt z x c v b n m . left down right space   (Sym: . -> , and Home/PageDown/End) */
    {{0, 0, 0, 0}, {0, 0, 0, 0}, {0x1D, 0, 0x1D, 0}, {0x1B, 0, 0x1B, 0},
     {0x06, 0, 0x06, 0}, {0x19, 0, 0x19, 0}, {0x05, 0, 0x05, 0}, {0x11, 0, 0x11, 0},
     {0x10, 0, 0x10, 0}, {K_DOT, 0, K_COMMA, 0}, {K_LEFT, 0, K_HOME, 0}, {K_DOWN, 0, K_PGDN, 0},
     {K_RIGHT, 0, K_END, 0}, {K_SPACE, 0, K_SPACE, 0}},
};

#define TAB5KBD_SYM_ROW 3
#define TAB5KBD_SYM_COL 0
#define TAB5KBD_AA_ROW 3
#define TAB5KBD_AA_COL 1
#define TAB5KBD_CTRL_ROW 4
#define TAB5KBD_CTRL_COL 0
#define TAB5KBD_ALT_ROW 4
#define TAB5KBD_ALT_COL 1

static inline void tab5kbd_reset(tab5kbd_t *k) { memset(k, 0, sizeof *k); }

/* The next report waiting, oldest first; false when there is none. */
static inline bool tab5kbd_take(tab5kbd_t *k, tab5kbd_report_t *out)
{
    if (k->tail == k->head) return false;
    *out = k->queue[k->tail];
    k->tail = (k->tail + 1) % TAB5KBD_QUEUE;
    return true;
}

/* Rebuilds the report from what is held and queues it if it changed. Shift comes from Aa, from the printed
 * legend of the key held last (! is Shift+1), and from Caps Lock on letters. */
static inline void tab5kbd_publish(tab5kbd_t *k, uint8_t key_shift, bool letter)
{
    tab5kbd_report_t r = {0, {0}};
    if (k->ctrl) r.mods |= TAB5KBD_MOD_LCTRL;
    if (k->alt) r.mods |= k->sym ? TAB5KBD_MOD_LGUI : TAB5KBD_MOD_LALT;
    bool shift = k->aa || key_shift;
    if (letter && k->caps) shift = !shift;
    if (shift) r.mods |= TAB5KBD_MOD_LSHIFT;
    int n = 0;
    for (int row = 0; row < TAB5KBD_ROWS; row++)
        for (int col = 0; col < TAB5KBD_COLS && n < 6; col++)
            if (k->sent[row][col]) r.keys[n++] = k->sent[row][col];
    if (!memcmp(&r, &k->now, sizeof r)) return;
    k->now = r;
    int next = (k->head + 1) % TAB5KBD_QUEUE;
    if (next == k->tail) k->tail = (k->tail + 1) % TAB5KBD_QUEUE;   /* full: the oldest goes */
    k->queue[k->head] = r;
    k->head = next;
}

/* One event byte from the keyboard, at now_ms. */
static inline void tab5kbd_event(tab5kbd_t *k, uint8_t ev, uint32_t now_ms)
{
    int row = (ev >> 4) & 7, col = ev & 0x0F;
    bool down = (ev & 0x80) != 0;
    if (row >= TAB5KBD_ROWS || col >= TAB5KBD_COLS) return;
    bool was = (k->held[row] >> col) & 1;
    if (was == down) return;
    if (down) k->held[row] |= (uint16_t)(1u << col);
    else k->held[row] &= (uint16_t)~(1u << col);

    if (row == TAB5KBD_SYM_ROW && col == TAB5KBD_SYM_COL) { k->sym = down; tab5kbd_publish(k, 0, false); return; }
    if (row == TAB5KBD_CTRL_ROW && col == TAB5KBD_CTRL_COL) { k->ctrl = down; tab5kbd_publish(k, 0, false); return; }
    if (row == TAB5KBD_ALT_ROW && col == TAB5KBD_ALT_COL) { k->alt = down; tab5kbd_publish(k, 0, false); return; }
    if (row == TAB5KBD_AA_ROW && col == TAB5KBD_AA_COL) {
        if (down) {
            k->aa = true; k->aa_other = false; k->aa_down_at = now_ms;
        } else {
            k->aa = false;
            if (!k->aa_other && now_ms - k->aa_down_at < TAB5KBD_DOUBLE_TAP_MS) {   /* a tap, not a held Shift */
                if (k->aa_last_tap && now_ms - k->aa_last_tap < TAB5KBD_DOUBLE_TAP_MS) { k->caps = !k->caps; k->aa_last_tap = 0; }
                else k->aa_last_tap = now_ms;
            }
        }
        tab5kbd_publish(k, 0, false);
        return;
    }
    if (down) {
        k->aa_other = true;
        const tab5kbd_legend_t *l = &tab5kbd_map[row][col];
        uint8_t usage = k->sym ? l->sym_usage : l->usage;
        uint8_t shift = k->sym ? l->sym_shift : l->shift;
        if (!usage) return;
        k->sent[row][col] = usage;
        k->sent_shift[row][col] = shift;
        tab5kbd_publish(k, shift, usage >= 0x04 && usage <= 0x1D);
    } else {
        if (!k->sent[row][col]) return;
        k->sent[row][col] = 0;
        k->sent_shift[row][col] = 0;
        /* What stays held keeps its own Shift. */
        uint8_t shift = 0; bool letter = false;
        for (int r = 0; r < TAB5KBD_ROWS; r++)
            for (int c = 0; c < TAB5KBD_COLS; c++)
                if (k->sent[r][c]) { shift = k->sent_shift[r][c]; letter = k->sent[r][c] >= 0x04 && k->sent[r][c] <= 0x1D; }
        tab5kbd_publish(k, shift, letter);
    }
}

/* Unplugged: whatever was held comes up. */
static inline void tab5kbd_release_all(tab5kbd_t *k)
{
    memset(k->held, 0, sizeof k->held);
    memset(k->sent, 0, sizeof k->sent);
    memset(k->sent_shift, 0, sizeof k->sent_shift);
    k->sym = k->aa = k->ctrl = k->alt = false;
    tab5kbd_publish(k, 0, false);
}
