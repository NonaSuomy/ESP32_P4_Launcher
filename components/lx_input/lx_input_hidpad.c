/*
 * HID gamepads that arrive by another transport than USB (Bluetooth Classic, lx_bt_pads.c): the
 * transport hands over the report descriptor once and then every input report.
 *
 * Sony and Nintendo pads send their own formats over Bluetooth and are read by model:
 *   DualShock 4 / DualSense: report 0x01 (the short report both send until asked for more, same
 *   layout as the DS4's USB report), DS4 0x11 (full, 2 bytes later), DualSense 0x31 (full, the
 *   USB layout 1 byte later);
 *   Switch Pro (and pads that pose as one, 8BitDo in Switch mode): 0x3F (simple HID mode, what it
 *   sends after connecting) and 0x30 (full mode).
 * Others: the report descriptor (lx_hid_parse), as for generic USB pads; a saved mapping fixes
 * the button order.
 */
#include "lx_input.h"
#include "lx_input_priv.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "lx_hidpad";

enum { K_HID, K_SONY, K_SWITCH, K_NONE };

struct lx_hidpad {
    int      player;
    int      kind;
    bool     has_layout;
    hid_layout_t layout;
    uint8_t  last_id;           /* report id of the last report seen, for the log */
    /* report timing, for lx_input_hidpad_stats (DevLink "bt status"): a link that stalls shows */
    int64_t  t_last, t_window;
    uint32_t n, gap_max_us, stalls, stalls_use;
    int64_t  t_change;                      /* last input change */
    lx_pad_state_t prev;
    bool     have_prev;
};

static int16_t stick16(const uint8_t *p, bool invert)
{
    int v = (int)(p[0] | p[1] << 8) - 32768;
    if (invert) v = -v - 1;
    return (int16_t)v;
}

static int16_t stick12(int v, bool invert)
{
    v = (v - 2048) * 16;
    if (invert) v = -v;
    return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
}

/* Nintendo's positions: B south, A east, Y west, X north = PAPP A, B, X, Y */
static bool decode_switch(const uint8_t *p, size_t len, lx_pad_state_t *s)
{
    memset(s, 0, sizeof(*s));
    if (p[0] == 0x3F && len >= 12) {
        uint32_t b = lx_pad_hat(p[3]);
        if (p[1] & 0x01) b |= PAPP_BTN_A;
        if (p[1] & 0x02) b |= PAPP_BTN_B;
        if (p[1] & 0x04) b |= PAPP_BTN_X;
        if (p[1] & 0x08) b |= PAPP_BTN_Y;
        if (p[1] & 0x10) b |= PAPP_BTN_L1;
        if (p[1] & 0x20) b |= PAPP_BTN_R1;
        if (p[1] & 0x40) b |= PAPP_BTN_L2;
        if (p[1] & 0x80) b |= PAPP_BTN_R2;
        if (p[2] & 0x01) b |= PAPP_BTN_SELECT;
        if (p[2] & 0x02) b |= PAPP_BTN_START;
        if (p[2] & 0x04) b |= PAPP_BTN_L3;
        if (p[2] & 0x08) b |= PAPP_BTN_R3;
        if (p[2] & 0x10) b |= PAPP_BTN_HOME;
        if (p[2] & 0x20) b |= PAPP_BTN_MISC;
        s->b = b;
        s->lx = stick16(p + 4, false);  s->ly = stick16(p + 6, false);
        s->rx = stick16(p + 8, false);  s->ry = stick16(p + 10, false);
        s->lt = p[1] & 0x40 ? 65535 : 0;
        s->rt = p[1] & 0x80 ? 65535 : 0;
        return true;
    }
    if (p[0] == 0x30 && len >= 12) {
        /* [30][timer][battery][right][shared][left][LS x3][RS x3]; sticks 12 bit, up = more */
        uint8_t r = p[3], m = p[4], l = p[5];
        uint32_t b = 0;
        if (r & 0x01) b |= PAPP_BTN_X;          /* Y */
        if (r & 0x02) b |= PAPP_BTN_Y;          /* X */
        if (r & 0x04) b |= PAPP_BTN_A;          /* B */
        if (r & 0x08) b |= PAPP_BTN_B;          /* A */
        if (r & 0x40) b |= PAPP_BTN_R1;
        if (r & 0x80) b |= PAPP_BTN_R2;
        if (m & 0x01) b |= PAPP_BTN_SELECT;
        if (m & 0x02) b |= PAPP_BTN_START;
        if (m & 0x04) b |= PAPP_BTN_R3;
        if (m & 0x08) b |= PAPP_BTN_L3;
        if (m & 0x10) b |= PAPP_BTN_HOME;
        if (m & 0x20) b |= PAPP_BTN_MISC;
        if (l & 0x01) b |= PAPP_BTN_DOWN;
        if (l & 0x02) b |= PAPP_BTN_UP;
        if (l & 0x04) b |= PAPP_BTN_RIGHT;
        if (l & 0x08) b |= PAPP_BTN_LEFT;
        if (l & 0x40) b |= PAPP_BTN_L1;
        if (l & 0x80) b |= PAPP_BTN_L2;
        s->b = b;
        s->lx = stick12(p[6] | (p[7] & 0x0F) << 8, false);
        s->ly = stick12(p[7] >> 4 | p[8] << 4, true);
        s->rx = stick12(p[9] | (p[10] & 0x0F) << 8, false);
        s->ry = stick12(p[10] >> 4 | p[11] << 4, true);
        s->lt = l & 0x80 ? 65535 : 0;
        s->rt = r & 0x80 ? 65535 : 0;
        return true;
    }
    return false;
}

static bool decode_sony(const uint8_t *p, size_t len, lx_pad_state_t *s)
{
    memset(s, 0, sizeof(*s));
    if (p[0] == 0x01 && len >= 10) { lx_pad_ps4(p, s); return true; }
    if (p[0] == 0x11 && len >= 12) { lx_pad_ps4(p + 2, s); return true; }     /* DS4 full */
    if (p[0] == 0x31 && len >= 12) { lx_pad_ps5(p + 1, s); return true; }     /* DualSense full */
    return false;
}

lx_hidpad_t *lx_input_hidpad_open(const char *name, uint16_t vid, uint16_t pid, const uint8_t *desc, size_t len)
{
    lx_hidpad_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->has_layout = desc && len && hid_layout_parse(desc, len, &h->layout);
    if (vid == 0x054C) h->kind = K_SONY;
    else if (vid == 0x057E && pid == 0x2009) h->kind = K_SWITCH;
    else h->kind = h->has_layout ? K_HID : K_NONE;
    h->player = lx_input_pad_attach(name, vid, pid);
    if (h->player < 0) {
        free(h);
        return NULL;
    }
    static const char *const kinds[] = { "HID descriptor", "Sony", "Switch Pro", "unknown format" };
    ESP_LOGI(TAG, "player %d: %s, read as %s", h->player + 1, name, kinds[h->kind]);
    return h;
}

/* an input change someone made: a button, or a stick / trigger moved past its noise */
static bool moved(const lx_pad_state_t *a, const lx_pad_state_t *b)
{
    const int D = 3000;                     /* of +-32767: about 9 % */
    return a->b != b->b || abs(a->lx - b->lx) > D || abs(a->ly - b->ly) > D || abs(a->rx - b->rx) > D ||
           abs(a->ry - b->ry) > D || abs((int)a->lt - (int)b->lt) > 2 * D || abs((int)a->rt - (int)b->rt) > 2 * D;   /* triggers 0..65535 */
}

uint32_t lx_input_hidpad_still_ms(lx_hidpad_t *h)
{
    if (!h) return UINT32_MAX;
    return h->t_change ? (uint32_t)((esp_timer_get_time() - h->t_change) / 1000) : 0;
}

void lx_input_hidpad_report(lx_hidpad_t *h, const uint8_t *p, size_t len)
{
    if (!h || !p || !len) return;
    lx_pad_state_t s;
    bool ok = false;
    switch (h->kind) {
    case K_SONY:   ok = decode_sony(p, len, &s); break;
    case K_SWITCH: ok = decode_switch(p, len, &s); break;
    case K_HID:    ok = lx_pad_layout(&h->layout, p, len, &s); break;
    default:       break;
    }
    if (!ok && h->kind != K_HID && h->has_layout) ok = lx_pad_layout(&h->layout, p, len, &s);
    if (p[0] != h->last_id) {               /* once per report kind: what the pad sends */
        h->last_id = p[0];
        ESP_LOGI(TAG, "player %d: report %02X, %u bytes%s", h->player + 1, p[0], (unsigned)len, ok ? "" : ", not read");
    }
    int64_t now = esp_timer_get_time();
    if (h->t_last) {
        uint32_t g = (uint32_t)(now - h->t_last);
        if (g > h->gap_max_us) h->gap_max_us = g;
        if (g >= LX_HIDPAD_STALL_US) {
            h->stalls++;
            if (h->t_change && h->t_last - h->t_change < LX_HIDPAD_IN_USE_US) h->stalls_use++;
        }
    }
    if (ok && (!h->have_prev || moved(&h->prev, &s))) {
        h->t_change = now;                  /* the first report counts: the pad was just switched on */
        h->prev = s;
        h->have_prev = true;
    }
    if (!h->t_window) h->t_window = now;
    h->t_last = now;
    h->n++;
    if (ok) lx_input_pad_update(h->player, s.b, s.lx, s.ly, s.rx, s.ry, s.lt, s.rt);
}

void lx_input_hidpad_stats(lx_hidpad_t *h, lx_hidpad_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!h) return;
    int64_t now = esp_timer_get_time();
    out->player = h->player;
    out->reports = h->n;
    out->ms = h->t_window ? (uint32_t)((now - h->t_window) / 1000) : 0;
    out->gap_max_ms = h->gap_max_us / 1000;
    out->stalls = h->stalls;
    out->stalls_in_use = h->stalls_use;
    out->idle_ms = h->t_last ? (uint32_t)((now - h->t_last) / 1000) : 0;
    out->still_ms = lx_input_hidpad_still_ms(h);
    h->n = h->gap_max_us = h->stalls = h->stalls_use = 0;
    h->t_window = now;
}

void lx_input_hidpad_close(lx_hidpad_t *h)
{
    if (!h) return;
    lx_input_pad_detach(h->player);
    free(h);
}
