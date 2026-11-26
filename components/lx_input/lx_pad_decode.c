/*
 * Gamepad report decoding shared by the USB HID source (lx_input_usb.c) and the HID pads of other
 * transports (lx_input_hidpad.c, Bluetooth).
 */
#include "lx_input_priv.h"
#include "papp_abi.h"

uint32_t lx_pad_hat(uint8_t hat)
{
    static const uint32_t map[8] = {
        PAPP_BTN_UP, PAPP_BTN_UP | PAPP_BTN_RIGHT, PAPP_BTN_RIGHT, PAPP_BTN_DOWN | PAPP_BTN_RIGHT,
        PAPP_BTN_DOWN, PAPP_BTN_DOWN | PAPP_BTN_LEFT, PAPP_BTN_LEFT, PAPP_BTN_UP | PAPP_BTN_LEFT,
    };
    return hat <= 7 ? map[hat] : 0;
}

uint32_t lx_pad_axes_dpad(uint8_t x, uint8_t y, uint8_t lo, uint8_t hi)
{
    uint32_t d = 0;
    if (y < lo) d |= PAPP_BTN_UP;
    else if (y > hi) d |= PAPP_BTN_DOWN;
    if (x < lo) d |= PAPP_BTN_LEFT;
    else if (x > hi) d |= PAPP_BTN_RIGHT;
    return d;
}

uint32_t lx_pad_ps_buttons(uint8_t hat_btn, uint8_t b2, uint8_t b3)
{
    uint32_t b = 0;
    if (hat_btn & 0x10) b |= PAPP_BTN_X;        /* Square   */
    if (hat_btn & 0x20) b |= PAPP_BTN_A;        /* Cross    */
    if (hat_btn & 0x40) b |= PAPP_BTN_B;        /* Circle   */
    if (hat_btn & 0x80) b |= PAPP_BTN_Y;        /* Triangle */
    if (b2 & 0x01) b |= PAPP_BTN_L1;
    if (b2 & 0x02) b |= PAPP_BTN_R1;
    if (b2 & 0x04) b |= PAPP_BTN_L2;
    if (b2 & 0x08) b |= PAPP_BTN_R2;
    if (b2 & 0x10) b |= PAPP_BTN_SELECT;
    if (b2 & 0x20) b |= PAPP_BTN_START;
    if (b2 & 0x40) b |= PAPP_BTN_L3;
    if (b2 & 0x80) b |= PAPP_BTN_R3;
    if (b3 & 0x01) b |= PAPP_BTN_HOME;
    if (b3 & 0x02) b |= PAPP_BTN_MISC;
    return b;
}

#define AX(v)   ((int16_t)(((int)(v) - 128) * 256))
#define TRIG(v) ((uint16_t)((v) * 257))

void lx_pad_ps4(const uint8_t *p, lx_pad_state_t *s)
{
    /* [id][LX][LY][RX][RY][hat+btn1][btn2][btn3][L2][R2] */
    s->lx = AX(p[1]); s->ly = AX(p[2]); s->rx = AX(p[3]); s->ry = AX(p[4]);
    s->b = lx_pad_hat(p[5] & 0x0F) | lx_pad_ps_buttons(p[5], p[6], p[7]);
    s->lt = TRIG(p[8]); s->rt = TRIG(p[9]);
}

void lx_pad_ps5(const uint8_t *p, lx_pad_state_t *s)
{
    /* [id][LX][LY][RX][RY][L2][R2][cnt][hat+btn1][btn2][btn3] */
    s->lx = AX(p[1]); s->ly = AX(p[2]); s->rx = AX(p[3]); s->ry = AX(p[4]);
    s->lt = TRIG(p[5]); s->rt = TRIG(p[6]);
    s->b = lx_pad_hat(p[8] & 0x0F) | lx_pad_ps_buttons(p[8], p[9], p[10]);
}

/* Default button order of generic pads; a saved mapping can change any of it */
static const uint32_t k_hid_buttons[] = {
    PAPP_BTN_X, PAPP_BTN_A, PAPP_BTN_B, PAPP_BTN_Y, PAPP_BTN_L1, PAPP_BTN_R1, PAPP_BTN_L2,
    PAPP_BTN_R2, PAPP_BTN_SELECT, PAPP_BTN_START, PAPP_BTN_L3, PAPP_BTN_R3, PAPP_BTN_HOME,
    PAPP_BTN_MISC,
};

bool lx_pad_layout(const hid_layout_t *l, const uint8_t *p, size_t len, lx_pad_state_t *out)
{
    hid_state_t s;
    if (!hid_layout_decode(l, p, len, &s)) return false;
    uint32_t b = s.hat >= 0 ? lx_pad_hat((uint8_t)s.hat) : 0;
    for (size_t i = 0; i < sizeof(k_hid_buttons) / sizeof(k_hid_buttons[0]); i++)
        if (s.buttons & (1u << i)) b |= k_hid_buttons[i];
    if (!l->hat.size) {
        if (s.axis[HID_AX_Y] < -16000) b |= PAPP_BTN_UP;
        else if (s.axis[HID_AX_Y] > 16000) b |= PAPP_BTN_DOWN;
        if (s.axis[HID_AX_X] < -16000) b |= PAPP_BTN_LEFT;
        else if (s.axis[HID_AX_X] > 16000) b |= PAPP_BTN_RIGHT;
    }
    /* Right stick is Z/Rz when present, then Rx/Ry are the triggers */
    bool zrz = l->axis[HID_AX_Z].size && l->axis[HID_AX_RZ].size;
    bool trig = zrz && l->axis[HID_AX_RX].size && l->axis[HID_AX_RY].size;
    out->b = b;
    out->lx = s.axis[HID_AX_X];
    out->ly = s.axis[HID_AX_Y];
    out->rx = zrz ? s.axis[HID_AX_Z] : s.axis[HID_AX_RX];
    out->ry = zrz ? s.axis[HID_AX_RZ] : s.axis[HID_AX_RY];
    out->lt = trig ? (uint16_t)(s.axis[HID_AX_RX] + 32768) : 0;
    out->rt = trig ? (uint16_t)(s.axis[HID_AX_RY] + 32768) : 0;
    return true;
}
