/*
 * USB HID host source for the input manager: gamepads and boot keyboards.
 * Report parsers ported from RetroESP32_P4 components/gamepad (PS3, PS4, PS5,
 * generic, Dual PSX adaptor), extended to several devices at once.
 *
 * Tasks: usb_host (library events), HID host driver task (callbacks), and
 * lx_hid (opens devices — needs delays, so not in the callback).
 */
#include "lx_input.h"
#include "lx_input_priv.h"
#include "lx_hid_parse.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "hal/usb_dwc_hal.h"
#include "hal/usb_dwc_ll.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "lx_hid";

/* ── USB host BNA workaround ────────────────────────────────────────────────
 * IDF 5.5 (v5.5.2 to v5.5.6) usb_dwc_hal_chan_decode_intr() asserts that every channel error
 * comes with CHHLTD. BNA (the DMA fetched a descriptor that wasn't ready, seen
 * when apps load both cores and the PPA) arrives without it, so the assert
 * panics in the USB interrupt. Wrapped at link time (CMakeLists.txt): an error
 * without a halt is reported as a transfer error, at once if the channel has
 * already stopped (every BNA seen so far: disabling it would raise no CHHLTD),
 * else on the CHHLTD that disabling it raises. The URB fails and interface_cb
 * restarts the device; the pad is back within ~50 ms. */

#define CHAN_ERR_MSK (USB_DWC_LL_INTR_CHAN_STALL | USB_DWC_LL_INTR_CHAN_BBLEER | \
                      USB_DWC_LL_INTR_CHAN_BNAINTR | USB_DWC_LL_INTR_CHAN_XCS_XACT_ERR)

static uint32_t s_chan_err[16];             /* deferred error bits per channel */
static volatile uint32_t s_bna_count;       /* channel errors without a halt (logged by lx_hid) */

usb_dwc_hal_chan_event_t __real_usb_dwc_hal_chan_decode_intr(usb_dwc_hal_chan_t *chan);

/* The channel has stopped with error bits `e`: report it as IDF's error path would */
static IRAM_ATTR usb_dwc_hal_chan_event_t chan_stopped_with_error(usb_dwc_hal_chan_t *chan, uint32_t e)
{
    chan->flags.active = 0;
    if (chan->flags.halt_requested) {
        /* The HCD asked for this halt meanwhile and waits for HALT_REQ;
           it treats a halted transfer as failed anyway */
        chan->flags.halt_requested = 0;
        return USB_DWC_HAL_CHAN_EVENT_HALT_REQ;
    }
    /* Not USB_DWC_HAL_CHAN_ERROR_BNA: the HCD maps it to ERROR_URB_NOT_AVAIL,
       which _buffer_parse_error() aborts on. A transfer error fails the URB. */
    chan->error = (e & USB_DWC_LL_INTR_CHAN_STALL)  ? USB_DWC_HAL_CHAN_ERROR_STALL :
                  (e & USB_DWC_LL_INTR_CHAN_BBLEER) ? USB_DWC_HAL_CHAN_ERROR_PKT_BBL :
                                                      USB_DWC_HAL_CHAN_ERROR_XCS_XACT;
    return USB_DWC_HAL_CHAN_EVENT_ERROR;
}

IRAM_ATTR usb_dwc_hal_chan_event_t __wrap_usb_dwc_hal_chan_decode_intr(usb_dwc_hal_chan_t *chan)
{
    volatile usb_dwc_host_chan_regs_t *r = chan->regs;
    uint32_t in = r->hcint_reg.val;
    int i = chan->flags.chan_idx;

    if ((in & CHAN_ERR_MSK) && !(in & USB_DWC_LL_INTR_CHAN_CHHLTD)) {
        r->hcint_reg.val = in & CHAN_ERR_MSK;           /* write 1 to clear: only the errors */
        s_bna_count++;
        if (!usb_dwc_ll_hcchar_chan_is_enabled(r)) {
            /* Already stopped (as after a BNA): disabling it raises no CHHLTD */
            uint32_t e = s_chan_err[i] | (in & CHAN_ERR_MSK);
            s_chan_err[i] = 0;
            return chan_stopped_with_error(chan, e);
        }
        s_chan_err[i] |= in & CHAN_ERR_MSK;
        usb_dwc_ll_hcchar_disable_chan(r);              /* CHHLTD follows */
        return USB_DWC_HAL_CHAN_EVENT_NONE;
    }
    if (s_chan_err[i] && (in & USB_DWC_LL_INTR_CHAN_CHHLTD)) {
        uint32_t e = s_chan_err[i] | (in & CHAN_ERR_MSK);
        s_chan_err[i] = 0;
        r->hcint_reg.val = in;
        return chan_stopped_with_error(chan, e);
    }
    return __real_usb_dwc_hal_chan_decode_intr(chan);
}

#define MAX_DEVS 8

typedef enum { DEV_FREE = 0, DEV_PAD, DEV_KBD, DEV_IGNORED } dev_kind_t;

typedef enum {
    FMT_UNKNOWN = 0, FMT_PS3, FMT_PS4, FMT_PS5, FMT_GENERIC, FMT_PSX_ADAPTOR, FMT_HID, FMT_SWITCH_PRO,
} pad_fmt_t;

typedef struct {
    hid_host_device_handle_t handle;
    dev_kind_t kind;
    int player;
    pad_fmt_t fmt;
    int detect_count;
    uint16_t vid, pid;
    bool waking;                    /* DS3 asleep: its wake-up press isn't HOME */
    volatile bool switch_report_seen;
    bool has_layout;
    hid_layout_t layout;            /* from the report descriptor */
} dev_t;

static dev_t s_devs[MAX_DEVS];
static QueueHandle_t s_connect_q;
static QueueHandle_t s_restart_q;          /* devices whose transfer failed */

static dev_t *find_dev(hid_host_device_handle_t h)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (s_devs[i].kind != DEV_FREE && s_devs[i].handle == h) return &s_devs[i];
    return NULL;
}

/* ── Gamepad report parsing ─────────────────────────────────────────────── */

static uint32_t hat_to_dpad(uint8_t hat)
{
    static const uint32_t map[8] = {
        PAPP_BTN_UP, PAPP_BTN_UP | PAPP_BTN_RIGHT, PAPP_BTN_RIGHT, PAPP_BTN_DOWN | PAPP_BTN_RIGHT,
        PAPP_BTN_DOWN, PAPP_BTN_DOWN | PAPP_BTN_LEFT, PAPP_BTN_LEFT, PAPP_BTN_UP | PAPP_BTN_LEFT,
    };
    return hat <= 7 ? map[hat] : 0;
}

static uint32_t axes_to_dpad(uint8_t x, uint8_t y, uint8_t lo, uint8_t hi)
{
    uint32_t d = 0;
    if (y < lo) d |= PAPP_BTN_UP;
    else if (y > hi) d |= PAPP_BTN_DOWN;
    if (x < lo) d |= PAPP_BTN_LEFT;
    else if (x > hi) d |= PAPP_BTN_RIGHT;
    return d;
}

static uint32_t ps_buttons(uint8_t hat_btn, uint8_t b2, uint8_t b3)
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

static bool centred(uint8_t v, uint8_t lo, uint8_t hi) { return v >= lo && v <= hi; }

static pad_fmt_t detect_format(const dev_t *d, const uint8_t *p, int len)
{
    /* Official Switch Pro controllers emit zero-filled startup packets before
     * report mode 0x30 is selected. Identify them by USB identity first. */
    if (d->vid == 0x057E && d->pid == 0x2009) return FMT_SWITCH_PRO;
    if (len == 8 && d->vid == 0x0810 && d->pid == 0x0001) return FMT_PSX_ADAPTOR;
    if (d->vid == 0x054C && d->pid == 0x0268 && len >= 49) return FMT_PS3;
    if (d->has_layout && d->vid != 0x054C) return FMT_HID;  /* the pad says what it sends */
    if (len < 10 || p[0] != 0x01) return FMT_GENERIC;
    if (len >= 49) {
        bool ps3_axes = centred(p[6], 0x60, 0xA0) && centred(p[7], 0x60, 0xA0) &&
                        centred(p[8], 0x60, 0xA0) && centred(p[9], 0x60, 0xA0);
        bool ps4_axes = centred(p[1], 0x70, 0x90) && centred(p[2], 0x70, 0x90);
        if (ps3_axes && !ps4_axes && p[1] == 0x00) return FMT_PS3;
    }
    if (centred(p[1], 0x70, 0x90) && centred(p[2], 0x70, 0x90) &&
        centred(p[3], 0x70, 0x90) && centred(p[4], 0x70, 0x90)) {
        if (p[5] == 0x08 && p[8] != 0x08) return FMT_PS4;
        if (p[5] == 0x00 && p[8] == 0x08) return FMT_PS5;
    }
    uint8_t h5 = p[5] & 0x0F, h8 = p[8] & 0x0F;
    if (h5 <= 8 && h8 > 8) return FMT_PS4;
    if (h8 <= 8 && h5 > 8) return FMT_PS5;
    return FMT_PS4;
}

#define AX(v)  ((int16_t)(((int)(v) - 128) * 256))
#define TRIG(v) ((uint16_t)((v) * 257))

/* Default button order of generic pads; a saved mapping can change any of it */
static const uint32_t k_hid_buttons[] = {
    PAPP_BTN_X, PAPP_BTN_A, PAPP_BTN_B, PAPP_BTN_Y, PAPP_BTN_L1, PAPP_BTN_R1, PAPP_BTN_L2,
    PAPP_BTN_R2, PAPP_BTN_SELECT, PAPP_BTN_START, PAPP_BTN_L3, PAPP_BTN_R3, PAPP_BTN_HOME,
    PAPP_BTN_MISC,
};

static void parse_hid(dev_t *d, const uint8_t *p, int len)
{
    hid_state_t s;
    if (!hid_layout_decode(&d->layout, p, (size_t)len, &s)) return;
    uint32_t b = s.hat >= 0 ? hat_to_dpad((uint8_t)s.hat) : 0;
    for (size_t i = 0; i < sizeof(k_hid_buttons) / sizeof(k_hid_buttons[0]); i++)
        if (s.buttons & (1u << i)) b |= k_hid_buttons[i];
    /* This wired Switch controller reports as a generic HID pad rather than
     * the Switch Pro 0x30 protocol. Its face-button usages are in Xbox
     * position order, while Launcher actions follow the printed Nintendo
     * labels (Switch A = confirm, Switch B = back). */
    if (d->vid == 0x20D6 && d->pid == 0xA713) {
        uint32_t ab = b & (PAPP_BTN_A | PAPP_BTN_B);
        uint32_t xy = b & (PAPP_BTN_X | PAPP_BTN_Y);
        b &= ~(PAPP_BTN_A | PAPP_BTN_B | PAPP_BTN_X | PAPP_BTN_Y);
        if (ab & PAPP_BTN_A) b |= PAPP_BTN_B;
        if (ab & PAPP_BTN_B) b |= PAPP_BTN_A;
        if (xy & PAPP_BTN_X) b |= PAPP_BTN_Y;
        if (xy & PAPP_BTN_Y) b |= PAPP_BTN_X;
    }
    if (!d->layout.hat.size) {
        if (s.axis[HID_AX_Y] < -16000) b |= PAPP_BTN_UP;
        else if (s.axis[HID_AX_Y] > 16000) b |= PAPP_BTN_DOWN;
        if (s.axis[HID_AX_X] < -16000) b |= PAPP_BTN_LEFT;
        else if (s.axis[HID_AX_X] > 16000) b |= PAPP_BTN_RIGHT;
    }
    /* Right stick is Z/Rz when present, then Rx/Ry are the triggers */
    bool zrz = d->layout.axis[HID_AX_Z].size && d->layout.axis[HID_AX_RZ].size;
    bool trig = zrz && d->layout.axis[HID_AX_RX].size && d->layout.axis[HID_AX_RY].size;
    lx_input_pad_update(d->player, b, s.axis[HID_AX_X], s.axis[HID_AX_Y],
                        zrz ? s.axis[HID_AX_Z] : s.axis[HID_AX_RX],
                        zrz ? s.axis[HID_AX_RZ] : s.axis[HID_AX_RY],
                        trig ? (uint16_t)(s.axis[HID_AX_RX] + 32768) : 0,
                        trig ? (uint16_t)(s.axis[HID_AX_RY] + 32768) : 0);
}

static void parse_pad(dev_t *d, const uint8_t *p, int len)
{
    if (d->fmt == FMT_UNKNOWN) {
        if (len < 4) return;
        d->fmt = detect_format(d, p, len);
        static const char *const names[] = { "?", "PS3", "PS4", "PS5", "generic", "PSX adaptor", "HID descriptor", "Switch Pro" };
        ESP_LOGI(TAG, "player %d report format: %s (%d bytes)", d->player + 1, names[d->fmt], len);
    }
    if (d->fmt == FMT_HID) {
        parse_hid(d, p, len);
        return;
    }

    uint32_t b = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint16_t lt = 0, rt = 0;
    switch (d->fmt) {
    case FMT_SWITCH_PRO: {
        /* Standard wired Pro report 0x30: 24-bit buttons and four packed
         * 12-bit stick axes. Ignore startup packets until the mode is active. */
        if (len < 13 || p[0] != 0x30) return;
        d->switch_report_seen = true;
        uint32_t sb = (uint32_t)p[3] | ((uint32_t)p[4] << 8) | ((uint32_t)p[5] << 16);
        /* Preserve the labels printed on a Nintendo controller. The launcher
         * uses A to confirm and B to go back, so mapping by Xbox face-button
         * position would make Switch A act as Back. */
        if (sb & (1u << 3)) b |= PAPP_BTN_A; /* Switch A, right */
        if (sb & (1u << 2)) b |= PAPP_BTN_B; /* Switch B, bottom */
        if (sb & (1u << 1)) b |= PAPP_BTN_X; /* Switch X, top */
        if (sb & (1u << 0)) b |= PAPP_BTN_Y; /* Switch Y, left */
        if (sb & (1u << 22)) b |= PAPP_BTN_L1;
        if (sb & (1u << 6)) b |= PAPP_BTN_R1;
        if (sb & (1u << 23)) { b |= PAPP_BTN_L2; lt = 0xFFFF; }
        if (sb & (1u << 7)) { b |= PAPP_BTN_R2; rt = 0xFFFF; }
        if (sb & (1u << 8)) b |= PAPP_BTN_SELECT;
        if (sb & (1u << 9)) b |= PAPP_BTN_START;
        if (sb & (1u << 11)) b |= PAPP_BTN_L3;
        if (sb & (1u << 10)) b |= PAPP_BTN_R3;
        if (sb & (1u << 12)) b |= PAPP_BTN_HOME;
        if (sb & (1u << 16)) b |= PAPP_BTN_DOWN;
        if (sb & (1u << 17)) b |= PAPP_BTN_UP;
        if (sb & (1u << 18)) b |= PAPP_BTN_RIGHT;
        if (sb & (1u << 19)) b |= PAPP_BTN_LEFT;
        uint16_t slx = p[6] | ((uint16_t)(p[7] & 0x0F) << 8);
        uint16_t sly = (p[7] >> 4) | ((uint16_t)p[8] << 4);
        uint16_t srx = p[9] | ((uint16_t)(p[10] & 0x0F) << 8);
        uint16_t sry = (p[10] >> 4) | ((uint16_t)p[11] << 4);
        lx = (int16_t)(((int)(slx >> 4) - 128) * 256);
        ly = (int16_t)((128 - (int)(sly >> 4)) * 256);
        rx = (int16_t)(((int)(srx >> 4) - 128) * 256);
        ry = (int16_t)(((int)(sry >> 4) - 128) * 256);
        break;
    }
    case FMT_PS3:
        /* [01][00][btn1][btn2][ps][00][LX][LY][RX][RY] ... pressures from [18] */
        if (len < 10) return;
        lx = AX(p[6]); ly = AX(p[7]); rx = AX(p[8]); ry = AX(p[9]);
        if (p[2] & 0x10) b |= PAPP_BTN_UP;
        if (p[2] & 0x20) b |= PAPP_BTN_RIGHT;
        if (p[2] & 0x40) b |= PAPP_BTN_DOWN;
        if (p[2] & 0x80) b |= PAPP_BTN_LEFT;
        if (p[2] & 0x01) b |= PAPP_BTN_SELECT;
        if (p[2] & 0x02) b |= PAPP_BTN_L3;
        if (p[2] & 0x04) b |= PAPP_BTN_R3;
        if (p[2] & 0x08) b |= PAPP_BTN_START;
        if (p[3] & 0x01) b |= PAPP_BTN_L2;
        if (p[3] & 0x02) b |= PAPP_BTN_R2;
        if (p[3] & 0x04) b |= PAPP_BTN_L1;
        if (p[3] & 0x08) b |= PAPP_BTN_R1;
        if (p[3] & 0x10) b |= PAPP_BTN_Y;       /* Triangle */
        if (p[3] & 0x20) b |= PAPP_BTN_B;       /* Circle   */
        if (p[3] & 0x40) b |= PAPP_BTN_A;       /* Cross    */
        if (p[3] & 0x80) b |= PAPP_BTN_X;       /* Square   */
        if (p[4] & 0x01) b |= PAPP_BTN_HOME;
        if (len >= 20) { lt = TRIG(p[18]); rt = TRIG(p[19]); }
        break;
    case FMT_PS4:
        /* [01][LX][LY][RX][RY][hat+btn1][btn2][btn3][L2][R2] */
        if (len < 10) return;
        lx = AX(p[1]); ly = AX(p[2]); rx = AX(p[3]); ry = AX(p[4]);
        b = hat_to_dpad(p[5] & 0x0F) | ps_buttons(p[5], p[6], p[7]);
        lt = TRIG(p[8]); rt = TRIG(p[9]);
        break;
    case FMT_PS5:
        /* [01][LX][LY][RX][RY][L2][R2][cnt][hat+btn1][btn2][btn3] */
        if (len < 11) return;
        lx = AX(p[1]); ly = AX(p[2]); rx = AX(p[3]); ry = AX(p[4]);
        lt = TRIG(p[5]); rt = TRIG(p[6]);
        b = hat_to_dpad(p[8] & 0x0F) | ps_buttons(p[8], p[9], p[10]);
        break;
    case FMT_PSX_ADAPTOR:
        /* [01][80][80][HatX][HatY][XABY][Sel,Start|LX,RX][00] */
        lx = AX(p[1]); ly = AX(p[2]); rx = AX(p[3]); ry = AX(p[4]);
        b = axes_to_dpad(p[3], p[4], 64, 192) | ps_buttons(p[5], p[6], p[7]);
        break;
    default: {
        /* [LX][LY][RX][RY][Z][hat|XABY][L1 R1 . . Sel Start][..], optional report id */
        int o = (len > 8 && p[0] >= 0x01 && p[0] <= 0x0F) ? 1 : 0;
        if (len - o < 4) return;
        lx = AX(p[o]); ly = AX(p[o + 1]); rx = AX(p[o + 2]); ry = AX(p[o + 3]);
        b = axes_to_dpad(p[o], p[o + 1], 0x20, 0xE0);
        if (len - o >= 6) {
            uint8_t h = p[o + 5];
            if ((h & 0x0F) <= 7) b |= hat_to_dpad(h & 0x0F);
            if (h & 0x10) b |= PAPP_BTN_X;
            if (h & 0x20) b |= PAPP_BTN_A;
            if (h & 0x40) b |= PAPP_BTN_B;
            if (h & 0x80) b |= PAPP_BTN_Y;
        }
        if (len - o >= 7) {
            uint8_t s = p[o + 6];
            if (s & 0x01) b |= PAPP_BTN_L1;
            if (s & 0x02) b |= PAPP_BTN_R1;
            if (s & 0x10) b |= PAPP_BTN_SELECT;
            if (s & 0x20) b |= PAPP_BTN_START;
        }
        break;
    }
    }
    /* A genuine DS3 stays silent after connecting until it is pressed once,
     * in practice with PS (= HOME). That press wakes the pad; it must not
     * also stop the running app. HOME counts again once PS was released. */
    if (d->waking) {
        if (b & PAPP_BTN_HOME) b &= ~PAPP_BTN_HOME;
        else {
            d->waking = false;
            ESP_LOGI(TAG, "player %d: DS3 is awake", d->player + 1);
        }
    }
    lx_input_pad_update(d->player, b, lx, ly, rx, ry, lt, rt);
}

/* ── HID callbacks ──────────────────────────────────────────────────────── */

static void close_dev(dev_t *d)
{
    if (d->kind == DEV_PAD) lx_input_pad_detach(d->player);
    if (d->kind == DEV_KBD) lx_input_kbd_detach();
    hid_host_device_close(d->handle);
    memset(d, 0, sizeof(*d));
}

static void interface_cb(hid_host_device_handle_t h, const hid_host_interface_event_t ev, void *arg)
{
    dev_t *d = find_dev(h);
    switch (ev) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t data[64];
        size_t len = 0;
        if (!d || hid_host_device_get_raw_input_report_data(h, data, sizeof(data), &len) != ESP_OK || !len)
            break;
        if (d->kind == DEV_PAD) parse_pad(d, data, (int)len);
        else if (d->kind == DEV_KBD && len >= 8) lx_input_kbd_report(data[0], data + 2);
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        if (d) close_dev(d);
        else hid_host_device_close(h);
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        /* The device going away (DISCONNECTED follows from the USB core), or a
           failed transfer: hid_host doesn't resubmit, so lx_hid restarts it */
        ESP_LOGW(TAG, "transfer error");
        if (d) xQueueSend(s_restart_q, &h, 0);
        break;
    default:
        break;
    }
}

static void driver_cb(hid_host_device_handle_t h, const hid_host_driver_event_t ev, void *arg)
{
    if (ev == HID_HOST_DRIVER_EVENT_CONNECTED) xQueueSend(s_connect_q, &h, 0);
}

/* ── Device bring-up ────────────────────────────────────────────────────── */

/* Top-level collection Generic Desktop / Joystick or Gamepad? */
static bool desc_is_gamepad(const uint8_t *desc, size_t len)
{
    /* Any collection: a receiver may list a mouse or keyboard before the pad */
    for (size_t i = 0; i + 3 < len; i++) {
        if (desc[i] == 0x05 && desc[i + 1] == 0x01 && desc[i + 2] == 0x09 &&
            (desc[i + 3] == 0x04 || desc[i + 3] == 0x05))
            return true;
    }
    return false;
}

/* Genuine DualShock 3 only streams after a feature request; it may still need
 * one physical button press (see the reference project's notes). */
static void ds3_enable(hid_host_device_handle_t h)
{
    static uint8_t led[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02,
        0xff, 0x27, 0x10, 0x00, 0x32, 0xff, 0x27, 0x10, 0x00, 0x32,
        0xff, 0x27, 0x10, 0x00, 0x32, 0xff, 0x27, 0x10, 0x00, 0x32,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    uint8_t f2[17];
    size_t f2_len = sizeof(f2);
    hid_class_request_get_report(h, HID_REPORT_TYPE_FEATURE, 0xF2, f2, &f2_len);
    uint8_t enable[4] = { 0x42, 0x0C, 0x00, 0x00 };
    esp_err_t err = hid_class_request_set_report(h, HID_REPORT_TYPE_FEATURE, 0xF4, enable, sizeof(enable));
    led[9] = 0x02;
    hid_class_request_set_report(h, HID_REPORT_TYPE_OUTPUT, 0x01, led, sizeof(led));
    ESP_LOGI(TAG, "DS3 operational mode %s — it stays silent until pressed once (PS is fine)",
             err == ESP_OK ? "enabled" : "request failed");
}

static void switch_pro_enable(hid_host_device_handle_t h)
{
    static const uint8_t commands[] = { 0x02, 0x03, 0x02, 0x04 };
    for (size_t i = 0; i < sizeof(commands); i++) {
        uint8_t report[] = { 0x80, commands[i] };
        esp_err_t err = hid_host_device_send_report(h, report, sizeof(report), 1000);
        ESP_LOGI(TAG, "Switch Pro USB handshake %u: %s", (unsigned)i + 1,
                 err == ESP_OK ? "OK" : esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    uint8_t mode[] = { 0x01, 0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0x03, 0x30 };
    esp_err_t err = hid_host_device_send_report(h, mode, sizeof(mode), 1000);
    ESP_LOGI(TAG, "Switch Pro report mode 0x30: %s", err == ESP_OK ? "OK" : esp_err_to_name(err));
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void product_name(hid_host_device_handle_t h, uint16_t vid, uint16_t pid, char *out, size_t len)
{
    if (vid == 0x057E && pid == 0x2009) { snprintf(out, len, "Switch Pro"); return; }
    if (vid == 0x054C && pid == 0x0268) { snprintf(out, len, "DualShock 3"); return; }
    if (vid == 0x054C && (pid == 0x05C4 || pid == 0x09CC)) { snprintf(out, len, "DualShock 4"); return; }
    if (vid == 0x054C && pid == 0x0CE6) { snprintf(out, len, "DualSense"); return; }
    hid_host_dev_info_t info;
    size_t n = 0;
    if (hid_host_get_device_info(h, &info) == ESP_OK) {
        for (int i = 0; i < HID_STR_DESC_MAX_LENGTH && info.iProduct[i] && n + 1 < len; i++)
            out[n++] = info.iProduct[i] < 0x80 ? (char)info.iProduct[i] : '?';
    }
    out[n] = 0;
    if (!n) snprintf(out, len, "Gamepad %04X:%04X", vid, pid);
}

static void open_device(hid_host_device_handle_t h)
{
    dev_t *d = NULL;
    for (int i = 0; i < MAX_DEVS; i++) if (s_devs[i].kind == DEV_FREE) { d = &s_devs[i]; break; }
    hid_host_dev_params_t params;
    if (!d || hid_host_device_get_params(h, &params) != ESP_OK) return;

    const hid_host_device_config_t cfg = { .callback = interface_cb };
    if (hid_host_device_open(h, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "open failed");
        return;
    }
    hid_host_dev_info_t info = { 0 };
    hid_host_get_device_info(h, &info);
    *d = (dev_t){ .handle = h, .kind = DEV_IGNORED, .player = -1, .vid = info.VID, .pid = info.PID };

    if (params.proto == HID_PROTOCOL_KEYBOARD) {
        d->kind = DEV_KBD;
        if (params.sub_class == HID_SUBCLASS_BOOT_INTERFACE)
            hid_class_request_set_protocol(h, HID_REPORT_PROTOCOL_BOOT);
        hid_class_request_set_idle(h, 0, 0);
    } else if (params.proto == HID_PROTOCOL_NONE) {
        size_t dlen = 0;
        const uint8_t *desc = hid_host_get_report_descriptor(h, &dlen);
        /* No descriptor: assume a gamepad, like the reference did */
        if ((d->vid == 0x057E && d->pid == 0x2009) || !desc || desc_is_gamepad(desc, dlen))
            d->kind = DEV_PAD;
        else ESP_LOG_BUFFER_HEX_LEVEL(TAG, desc, dlen < 128 ? dlen : 128, ESP_LOG_DEBUG);   /* to see what it is */
        if (desc && d->kind == DEV_PAD) {
            d->has_layout = hid_layout_parse(desc, dlen, &d->layout);
            ESP_LOGI(TAG, "report descriptor %u bytes: %s, %d buttons, hat %d, id %d", (unsigned)dlen,
                     d->has_layout ? "usable" : "not usable", d->layout.buttons,
                     d->layout.hat.size != 0, d->layout.report_id);
        }
    }
    ESP_LOGI(TAG, "%04X:%04X iface proto %d -> %s", d->vid, d->pid, params.proto,
             d->kind == DEV_PAD ? "gamepad" : d->kind == DEV_KBD ? "keyboard" : "ignored");

    /* PS4 pads can droop VBUS on configuration; let the supply settle (reference) */
    if (d->kind == DEV_PAD) vTaskDelay(pdMS_TO_TICKS(500));

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < 3 && err != ESP_OK; attempt++) {
        err = hid_host_device_start(h);
        if (err != ESP_OK) vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start failed: %s", esp_err_to_name(err));
        hid_host_device_close(h);
        memset(d, 0, sizeof(*d));
        return;
    }

    if (d->kind == DEV_PAD) {
        char name[24];
        product_name(h, d->vid, d->pid, name, sizeof(name));
        d->player = lx_input_pad_attach(name, d->vid, d->pid);
        if (d->player < 0) d->kind = DEV_IGNORED;           /* all player slots taken */
        else if (d->vid == 0x054C && d->pid == 0x0268) {
            d->waking = true;
            ds3_enable(h);
        } else if (d->vid == 0x057E && d->pid == 0x2009) {
            ESP_LOGI(TAG, "Switch Pro detected; initializing wired USB HID transport");
            for (int attempt = 1; attempt <= 3; ++attempt) {
                switch_pro_enable(h);
                vTaskDelay(pdMS_TO_TICKS(150));
                if (d->switch_report_seen) {
                    ESP_LOGI(TAG, "Switch Pro input active after handshake attempt %d", attempt);
                    break;
                }
                ESP_LOGW(TAG, "No Switch Pro 0x30 report after attempt %d", attempt);
            }
        }
    } else if (d->kind == DEV_KBD) {
        lx_input_kbd_attach();
    }
}

/* After a failed transfer: give a disconnect time to arrive, then restart the
 * interface (halt, flush, clear the endpoint, resubmit) if the device is still here */
static void restart_device(hid_host_device_handle_t h)
{
    vTaskDelay(pdMS_TO_TICKS(50));
    if (!find_dev(h)) return;
    esp_err_t e = hid_host_device_stop(h);
    if (e == ESP_OK) e = hid_host_device_start(h);
    ESP_LOGW(TAG, "restarted device after a transfer error: %s", esp_err_to_name(e));
}

static void hid_task(void *arg)
{
    uint32_t bna_seen = 0;
    for (;;) {
        hid_host_device_handle_t h;
        if (xQueueReceive(s_connect_q, &h, pdMS_TO_TICKS(200))) open_device(h);
        while (xQueueReceive(s_restart_q, &h, 0)) restart_device(h);
        if (s_bna_count != bna_seen) {
            bna_seen = s_bna_count;
            ESP_LOGW(TAG, "USB channel error without halt (BNA workaround), %lu so far", (unsigned long)bna_seen);
        }
    }
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t cfg = { .intr_flags = ESP_INTR_FLAG_LEVEL3 };
    esp_err_t err = usb_host_install(&cfg);
    xTaskNotify((TaskHandle_t)arg, err, eSetValueWithOverwrite);
    if (err != ESP_OK) vTaskDelete(NULL);
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

esp_err_t lx_input_usb_start(void)
{
    s_connect_q = xQueueCreate(8, sizeof(hid_host_device_handle_t));
    s_restart_q = xQueueCreate(8, sizeof(hid_host_device_handle_t));
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_host", 4096, xTaskGetCurrentTaskHandle(), 5, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    uint32_t err = ESP_ERR_TIMEOUT;
    xTaskNotifyWait(0, 0, &err, pdMS_TO_TICKS(3000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB host install failed: %s", esp_err_to_name(err));
        return err;
    }
    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 6,
        .stack_size = 4096,
        .core_id = 0,
        .callback = driver_cb,
    };
    esp_err_t e = hid_host_install(&hid_cfg);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "HID host install failed: %s", esp_err_to_name(e));
        return e;
    }
    xTaskCreatePinnedToCore(hid_task, "lx_hid", 4096, NULL, 6, NULL, 1);
    if (lx_input_xinput_start() != ESP_OK) ESP_LOGW(TAG, "XInput client failed");
    ESP_LOGI(TAG, "USB HID host ready");
    return ESP_OK;
}
