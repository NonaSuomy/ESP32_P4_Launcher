/*
 * USB Bluetooth dongle as Bluedroid's controller (Bluetooth Core Vol 4 Part B, the USB transport):
 * interface class E0/01/01, HCI commands as class control requests, events on the interrupt IN
 * endpoint, ACL data on the bulk endpoints. SCO (isochronous, interface 1) is not used.
 *
 * A third client of the launcher's USB host (after the HID driver and XInput). The board's port runs
 * at full speed, which every dongle supports. Bluedroid hands over and takes H4 packets (a type
 * byte first): the type byte is dropped on the way down and put back on the way up.
 *
 * Tested: a counterfeit CSR8510 A10 (0A12:0001, see fix_features) with an A2DP speaker. Broadcom BCM20702
 * should work as it comes; Realtek chips
 * (RTL8761B and others) want a firmware download that is not done here yet.
 *
 * Also the project's own ESP32 dongle (github.com/giltal/ESP32_BT_Dongle): a classic ESP32 dev board,
 * controller-only firmware with HCI H4 on UART0, reached through the board's USB-serial chip (CH34x or
 * CP210x, driven here with their vendor requests: 921600 8N1, DTR/RTS released). H4 keeps the type
 * byte on the wire: packets go out as Bluedroid gives them and the byte stream coming in is cut into
 * packets here (h4_feed). Such a chip is taken only after the dongle answers its identify command
 * (0xFC7E, "LXBT"): any other serial gadget is left alone.
 */
#include "sdkconfig.h"

#if CONFIG_BT_BLUEDROID_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID

#include <stdio.h>
#include "esp_timer.h"
#include <string.h>
#include "lx_bt_priv.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lx_bt_usb";

#define H4_CMD   0x01
#define H4_ACL   0x02
#define H4_EVT   0x04
#define N_CMD    2
#define N_ACL    4
#define N_BULKIN 4              /* kept queued: a serial chip's FIFO is small (92 bytes/ms at 921600) */
#define CMD_MAX  (3 + 255)
#define ACL_MAX  1100           /* biggest ACL packet sent; dongles report ~1021 */
#define BULK_IN  1024           /* a multiple of the 64-byte packet */
#define ACL_BUF  2048           /* reassembly: 4-byte header + up to the dongle's ACL size */

typedef struct {
    usb_device_handle_t dev;
    uint8_t  addr, iface;
    uint8_t  ep_evt, ep_in, ep_out;
    uint16_t mps_evt;
    usb_transfer_t *evt, *in[N_BULKIN], *cmd[N_CMD], *acl[N_ACL];
    bool     cmd_busy[N_CMD], acl_busy[N_ACL];
    volatile int inflight;      /* transfers submitted, callback not yet run */
    volatile bool gone, failed;
    uint8_t  serial;            /* 0 = USB Bluetooth dongle, else the ESP32 dongle's chip: SER_CH34X / SER_CP210X */
    char     name[40];
} dongle_t;

#define SER_CH34X  1
#define SER_CP210X 2
#define SER_BAUD   921600       /* the ESP32 dongle's boot rate */

/* Transfers in flight: counted up by Bluedroid's HCI task and the lx_bt task, down by the client task */
#define INF_INC() __atomic_add_fetch(&s_d.inflight, 1, __ATOMIC_RELAXED)
#define INF_DEC() __atomic_sub_fetch(&s_d.inflight, 1, __ATOMIC_RELAXED)

static usb_host_client_handle_t s_client;
static SemaphoreHandle_t s_mx, s_cmd_free, s_acl_free;
static dongle_t s_d;
static volatile bool s_present, s_streaming;
static uint8_t s_new[8];
static volatile int s_nnew;
static const esp_bluedroid_hci_driver_callbacks_t *s_host_cb;

/* Probe before Bluedroid starts (lx_bt_usb_open): the next event goes to s_pev, not to the stack */
static volatile bool s_probe;
static volatile uint16_t s_probe_op;
static uint8_t  s_pev[1 + 2 + 255];
static SemaphoreHandle_t s_probe_sem;
static bool     s_fake_p1;              /* features page 1 is answered here (see fix_features) */
static bool     s_fake_le;              /* Bluetooth 2.x/3.0 dongle: LE answered here (see usb_send) */
static volatile bool s_opening;         /* lx_bt_usb_open's probes: other events dropped */
static uint32_t s_sent;                 /* ACL packets sent (status) */
static uint8_t  s_le_share;             /* ACL buffers given to LE here (see fix_buffers), 0 = none */
#define LE_SHARE 4
static const char *s_why = "";

static uint8_t  s_ev[1 + 2 + 255];
static int      s_ev_len;               /* bytes after the type byte */
static uint8_t  s_acl[1 + ACL_BUF];
static int      s_acl_len;
static uint8_t  s_h4;                   /* serial: 0 = waiting for a type byte, H4_EVT / H4_ACL = in that packet */
static volatile bool s_discard;         /* serial: drop what comes in (the ESP32's boot banner) */
static uint32_t s_resync;               /* serial: bytes skipped looking for a packet start */

/* ── Up: events and ACL data to Bluedroid ───────────────────────────────── */

static void deliver(uint8_t *p, int len)
{
    if (s_host_cb && s_streaming) s_host_cb->notify_host_recv(p, (uint16_t)len);
}

/* Counterfeit CSR8510 dongles (0A12:0001, HCI revision = LMP subversion 0x1d86) answer Read Local
 * Extended Features page 0 with "last page 0" though they support LE and set the extended features
 * bit. Bluedroid looks for the LE host bit on page 1, finds no page 1, decides there is no BLE and
 * then asserts (BLE is built in). Such a reply says "last page 1" here, and page 1 is answered
 * here too (usb_send) with the host bits Bluedroid has just set: SSP, LE, LE + BR/EDR. */
/* Classic calls (pages) and their outcome in the log: the dongle has held some for 25 s or more */
static void trace_calls(void)
{
    const uint8_t *e = s_ev;
    if (e[1] == 0x0F && e[2] >= 4 && e[5] == 0x05 && e[6] == 0x04)                 /* Command Status */
        ESP_LOGI(TAG, "call accepted (status 0x%02X, %d commands free)", e[3], e[4]);
    else if (e[1] == 0x03 && e[2] >= 11)                                           /* Connection Complete */
        ESP_LOGI(TAG, "call %02X:%02X:%02X:%02X:%02X:%02X: %s (0x%02X)", e[11], e[10], e[9], e[8], e[7], e[6],
                 e[3] ? "failed" : "connected", e[3]);
    else if (e[1] == 0x04 && e[2] >= 10)                                           /* Connection Request */
        ESP_LOGI(TAG, "%02X:%02X:%02X:%02X:%02X:%02X calls us", e[8], e[7], e[6], e[5], e[4], e[3]);
    else if (e[1] == 0x12 && e[2] >= 8)                                            /* Role Change */
        ESP_LOGI(TAG, "%02X:%02X:%02X:%02X:%02X:%02X: board is %s (0x%02X)", e[9], e[8], e[7], e[6], e[5], e[4],
                 e[10] ? "slave" : "master", e[3]);
}

/* An old dongle (before Bluetooth 1.2/2.1) refuses a command it doesn't know (Read Local Supported
 * Commands, Read Local Extended Features, ...) with Command Status. Bluedroid waits for a Command
 * Complete and asserts on a failed one, so the refusal becomes a success here (controller,
 * information and status commands: all answer with Command Complete): supported commands none,
 * extended features from the dongle's own features (+ LE, see s_fake_le), anything else zeros. */
static uint8_t s_feat0[8];              /* the old dongle's LMP features (+ LE) */
static uint8_t s_ext_page;              /* page of the last Read Local Extended Features */

static void fix_status(void)
{
    uint8_t *e = s_ev;
    if (!s_fake_le || e[1] != 0x0F || e[2] < 4 || !e[3]) return;
    uint8_t ogf = e[6] >> 2;
    if (ogf != 0x03 && ogf != 0x04 && ogf != 0x05) return;
    uint8_t st = e[3], n = e[4], lo = e[5], hi = e[6];
    memset(e + 1, 0, 2 + 4 + 64);
    e[1] = 0x0E; e[2] = 4 + 64; e[3] = n; e[4] = lo; e[5] = hi; e[6] = 0;
    if (lo == 0x04 && hi == 0x10) {                         /* Read Local Extended Features */
        e[7] = s_ext_page;
        e[8] = 1;
        if (s_ext_page == 0) memcpy(e + 9, s_feat0, 8);
        else if (s_ext_page == 1) e[9] = 0x07;              /* host: SSP, LE, LE + BR/EDR */
    }
    s_ev_len = 2 + 4 + 64;
    ESP_LOGW(TAG, "command %02X%02X unknown to the dongle (0x%02X): answered here", hi, lo, st);
}

static void fix_features(void)
{
    uint8_t *e = s_ev;
    if (s_fake_le && e[1] == 0x0E && e[2] >= 12 && e[4] == 0x03 && e[5] == 0x10 && !e[6])
        e[7 + 4] |= 0x40;                                   /* Read Local Supported Features: "LE" */
    if (s_fake_le && e[1] == 0x0E && e[2] >= 14 && e[4] == 0x04 && e[5] == 0x10 && !e[6] && !e[7]) {
        e[9 + 4] |= 0x40;                                   /* extended page 0: "LE", and a page 1 */
        if (!e[8]) e[8] = 1;
        s_fake_p1 = true;
        return;
    }
    if (e[1] != 0x0E || e[2] < 14 || e[4] != 0x04 || e[5] != 0x10 || e[6] || e[7] || e[8]) return;
    if (!(e[13] & 0x40)) return;                            /* page 0 byte 4 bit 6: LE supported */
    e[8] = 1;
    if (!s_fake_p1) ESP_LOGW(TAG, "dongle reports no features page 1: answering it here");
    s_fake_p1 = true;
}

/*
 * A dongle with no LE buffers of its own (LE Read Buffer Size = 0, the CSR8510: 10 ACL buffers in
 * all) shares them, and Bluedroid then gives all LE links together a single transmit credit. The
 * dongle sometimes holds an LE packet for seconds; with one credit that stops every BLE pad's
 * requests (MTU, service search, encryption, pairing). So the buffers are split here: LE_SHARE for
 * LE, the rest for Classic, by adjusting the two replies; the dongle is never given more packets
 * than it has buffers. The split also keeps the dongle working with a speaker and a Classic pad
 * linked: given all 10, the speaker's stream stopped for good when a DualShock 4 connected, and the
 * dongle reset (hardware error 0x28).
 */
static void fix_buffers(void)
{
    uint8_t *e = s_ev;
    if (!s_le_share || e[1] != 0x0E || e[2] < 4 || e[6]) return;
    uint16_t op = (uint16_t)(e[4] | e[5] << 8);
    if (op == 0x1005 && e[2] >= 11) {                        /* Read Buffer Size: ACL count at 10..11 */
        uint16_t n = (uint16_t)(e[10] | e[11] << 8);
        if (n > s_le_share + 2) { n -= s_le_share; e[10] = (uint8_t)n; e[11] = (uint8_t)(n >> 8); }
        else s_le_share = 0;
    } else if (op == 0x2002 && e[2] >= 7 && !e[9] && !(e[7] | e[8])) {  /* LE Read Buffer Size: none */
        e[7] = 251; e[8] = 0;                                /* the ACL buffers hold 310 bytes */
        e[9] = s_le_share;
        ESP_LOGI(TAG, "dongle has no LE buffers: %d of its ACL buffers go to LE", s_le_share);
    }
}

/* ACL packets sent per link and not yet completed by the controller (Number of Completed Packets) */
#define N_TRACK 8
static struct { uint16_t h; int16_t n; int64_t t; } s_out[N_TRACK];

static void out_count(uint16_t h, int d)
{
    int free_i = -1;
    for (int i = 0; i < N_TRACK; i++) {
        if (s_out[i].n && s_out[i].h == h) {
            s_out[i].n += d;
            if (s_out[i].n < 0) s_out[i].n = 0;
            if (d > 0) s_out[i].t = esp_timer_get_time();
            return;
        }
        if (!s_out[i].n && free_i < 0) free_i = i;
    }
    if (d > 0 && free_i >= 0) { s_out[free_i].h = h; s_out[free_i].n = (int16_t)d; s_out[free_i].t = esp_timer_get_time(); }
}

static void count_out(void)
{
    if (s_ev[1] == 0x13 && s_ev[2] >= 1) {                     /* Number of Completed Packets */
        int n = s_ev[3];
        for (int i = 0; i < n && 4 + 4 * i + 3 < 1 + s_ev_len + 1; i++) {
            const uint8_t *q = &s_ev[4 + 4 * i];
            out_count((uint16_t)((q[0] | q[1] << 8) & 0x0FFF), -(int)(q[2] | q[3] << 8));
        }
    } else if (s_ev[1] == 0x05 && s_ev[2] >= 4 && s_ev[3] == 0) {  /* Disconnection Complete */
        uint16_t h = (uint16_t)((s_ev[4] | s_ev[5] << 8) & 0x0FFF);
        for (int i = 0; i < N_TRACK; i++) if (s_out[i].h == h) s_out[i].n = 0;
    }
}

int lx_bt_usb_unacked(char *buf, int size)
{
    int o = 0, total = 0;
    int64_t now = esp_timer_get_time();
    buf[0] = 0;
    for (int i = 0; i < N_TRACK; i++) {
        if (!s_out[i].n) continue;
        total += s_out[i].n;
        if (o < size - 24) o += snprintf(buf + o, size - o, "%s0x%x:%d/%dms", o ? " " : "", s_out[i].h, s_out[i].n,
                                         (int)((now - s_out[i].t) / 1000));
    }
    return total;
}

/* Interrupt IN: one packet per transfer; an event longer than the endpoint's 16 bytes comes in pieces */
static void evt_feed(const uint8_t *p, int n)
{
    while (n > 0) {
        int want = s_ev_len < 2 ? 2 - s_ev_len : 2 + s_ev[2] - s_ev_len;
        int k = n < want ? n : want;
        memcpy(&s_ev[1 + s_ev_len], p, k);
        s_ev_len += k;
        p += k;
        n -= k;
        if (s_ev_len >= 2 && s_ev_len == 2 + s_ev[2]) {
            s_ev[0] = H4_EVT;
            if (s_probe && s_ev[1] == 0x0E && s_ev[2] >= 3 && (s_ev[4] | s_ev[5] << 8) == s_probe_op) {
                memcpy(s_pev, s_ev, 1 + s_ev_len);      /* the probe's reply: kept here */
                s_probe = false;
                xSemaphoreGive(s_probe_sem);
            } else if (s_probe && s_opening) {
                /* while opening, a late reply to an earlier probe is dropped */
            } else {
                fix_status();
                fix_features();
                fix_buffers();
                count_out();
                trace_calls();
                deliver(s_ev, 1 + s_ev_len);
            }
            s_ev_len = 0;
        }
    }
}

/* Bulk IN: a byte stream of ACL packets (handle 2, length 2, data) */
static void acl_feed(const uint8_t *p, int n)
{
    while (n > 0) {
        int total = s_acl_len >= 4 ? 4 + (s_acl[3] | s_acl[4] << 8) : 4;
        if (total > ACL_BUF) {
            ESP_LOGW(TAG, "ACL packet of %d bytes: dropped", total);
            s_acl_len = 0;                  /* lost sync; the rest of this transfer goes too */
            return;
        }
        int k = total - s_acl_len;
        if (k > n) k = n;
        memcpy(&s_acl[1 + s_acl_len], p, k);
        s_acl_len += k;
        p += k;
        n -= k;
        if (s_acl_len >= 4 && s_acl_len == 4 + (s_acl[3] | s_acl[4] << 8)) {
            s_acl[0] = H4_ACL;
            deliver(s_acl, 1 + s_acl_len);
            s_acl_len = 0;
        }
    }
}

/* Serial (H4): a type byte, then an event or ACL packet; evt_feed / acl_feed take the bytes after it
 * and are back at 0 when the packet is complete. Bytes that can't start a packet are skipped. */
static void h4_feed(const uint8_t *p, int n)
{
    if (s_discard) return;
    while (n > 0) {
        if (!s_h4) {
            if (*p == H4_EVT || *p == H4_ACL) s_h4 = *p;
            else s_resync++;
            p++;
            n--;
            continue;
        }
        int want;
        if (s_h4 == H4_EVT) want = s_ev_len < 2 ? 2 - s_ev_len : 2 + s_ev[2] - s_ev_len;
        else want = s_acl_len < 4 ? 4 - s_acl_len : 4 + (s_acl[3] | s_acl[4] << 8) - s_acl_len;
        int k = n < want ? n : want;
        if (s_h4 == H4_EVT) evt_feed(p, k);
        else acl_feed(p, k);
        p += k;
        n -= k;
        if (s_h4 == H4_EVT ? s_ev_len == 0 : s_acl_len == 0) s_h4 = 0;    /* complete (or dropped) */
    }
}

static void in_done(usb_transfer_t *t)
{
    INF_DEC();
    bool evt = t == s_d.evt;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        if (s_d.serial) h4_feed(t->data_buffer, t->actual_num_bytes);
        else if (evt) evt_feed(t->data_buffer, t->actual_num_bytes);
        else acl_feed(t->data_buffer, t->actual_num_bytes);
    }
    if (s_d.gone || !s_streaming || t->status == USB_TRANSFER_STATUS_NO_DEVICE ||
        t->status == USB_TRANSFER_STATUS_CANCELED)
        return;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        s_d.failed = true;                  /* the endpoint is halted: the client task clears it */
        return;
    }
    INF_INC();
    if (usb_host_transfer_submit(t) != ESP_OK) {
        INF_DEC();
        s_d.failed = true;
    }
}

/* ── Down: commands and ACL data from Bluedroid ─────────────────────────── */

static void out_done(usb_transfer_t *t)
{
    INF_DEC();
    if (t->status != USB_TRANSFER_STATUS_COMPLETED && t->status != USB_TRANSFER_STATUS_CANCELED &&
        t->status != USB_TRANSFER_STATUS_NO_DEVICE)
        ESP_LOGW(TAG, "%s transfer failed: %d", t->bEndpointAddress ? "ACL" : "command", t->status);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    for (int i = 0; i < N_CMD; i++) if (s_d.cmd[i] == t) { s_d.cmd_busy[i] = false; xSemaphoreGive(s_cmd_free); }
    for (int i = 0; i < N_ACL; i++) if (s_d.acl[i] == t) { s_d.acl_busy[i] = false; xSemaphoreGive(s_acl_free); }
    xSemaphoreGive(s_mx);
    if (s_host_cb && s_host_cb->notify_host_send_available) s_host_cb->notify_host_send_available();
}

/* The dongle is gone but Bluedroid still talks: every command is refused (Command Status
 * "command disallowed", no parameters to parse), so the stack can shut down without waiting */
static void refuse(const uint8_t *cmd)
{
    uint8_t r[] = { H4_EVT, 0x0F, 4, 0x0C, 1, cmd[1], cmd[2] };
    if (s_host_cb) s_host_cb->notify_host_recv(r, sizeof(r));
}

static bool probe_p(uint16_t op, const uint8_t *par, int n, int ms);

static void usb_send(uint8_t *data, uint16_t len)
{
    if (len < 1) return;
    uint8_t type = data[0];
    bool live = s_present && s_streaming && !s_d.gone;
    if (!live) {
        if (type == H4_CMD && len >= 4) refuse(data);
        return;
    }
    /* A Bluetooth 2.x/3.0 dongle (no LE): Bluedroid, built with BLE, asserts without it, so its LE
     * commands are answered here: done, nothing to report (zeros). Classic works as on any dongle;
     * BLE pads and scans find nothing. */
    if (type == H4_CMD && len >= 5 && data[1] == 0x04 && data[2] == 0x10) s_ext_page = data[4];
    if (type == H4_CMD && s_fake_le && len >= 4 && ((data[2] >> 2) == 0x08 ||
                                                    (data[1] == 0x6D && data[2] == 0x0C))) {  /* LE, Write LE Host Support */
        uint8_t r[3 + 4 + 16] = { H4_EVT, 0x0E, 4 + 16, 1, data[1], data[2], 0 };
        if (data[1] == 0x02 && data[2] == 0x20) { r[7] = 251; r[9] = 2; }   /* LE Read Buffer Size: 2 x 251 */
        if (s_host_cb) s_host_cb->notify_host_recv(r, sizeof(r));
        return;
    }
    if (type == H4_CMD && s_fake_p1 && len >= 5 && data[1] == 0x04 && data[2] == 0x10 && data[4] >= 1) {
        uint8_t r[] = { H4_EVT, 0x0E, 14, 1, 0x04, 0x10, 0, data[4], 1,
                        data[4] == 1 ? 0x07 : 0, 0, 0, 0, 0, 0, 0, 0 };
        if (s_host_cb) s_host_cb->notify_host_recv(r, sizeof(r));
        return;
    }
    /* No sniff mode: Bluedroid puts an idle-looking link in sniff (a gamepad's every ~34 ms: input
     * lag, and DualShock 4s dropped the link). The dongle runs on USB power, so the "sniff allowed"
     * bit is cleared from the link policies Bluedroid writes, and the controller refuses sniff. */
    if (type == H4_CMD && len >= 6 && data[2] == 0x08) {
        if (data[1] == 0x0F) data[4] &= ~0x04;                  /* Write Default Link Policy Settings */
        else if (data[1] == 0x0D && len >= 8) data[6] &= ~0x04; /* Write Link Policy Settings (handle, policy) */
    }
    /* Always master: Bluedroid accepts the first incoming link (a DualShock 4 coming back on PS, a
     * speaker calling at power-on) as slave, so with a second link the board sits in two piconets
     * (scatternet). This dongle copes badly: links drop (supervision timeout) and calls go unheard.
     * As master of every link (role switch on accept) both share one piconet the board schedules. */
    if (type == H4_CMD && len >= 11 && data[1] == 0x09 && data[2] == 0x04 && data[10] != 0x00) {   /* Accept Connection Request */
        data[10] = 0x00;
        ESP_LOGI(TAG, "accepting %02X:%02X:%02X:%02X:%02X:%02X as master", data[9], data[8], data[7], data[6], data[5], data[4]);
    }
    if (type == H4_CMD && len >= 10 && data[1] == 0x05 && data[2] == 0x04)    /* Create Connection */
        ESP_LOGI(TAG, "call %02X:%02X:%02X:%02X:%02X:%02X", data[9], data[8], data[7], data[6], data[5], data[4]);
    if (type == H4_CMD && len >= 10 && data[1] == 0x08 && data[2] == 0x04)
        ESP_LOGI(TAG, "call cancelled");
    if (type == H4_CMD) {
        if (len < 4 || len - 1 > CMD_MAX || !xSemaphoreTake(s_cmd_free, pdMS_TO_TICKS(500))) {
            ESP_LOGW(TAG, "command dropped");
            return;
        }
        xSemaphoreTake(s_mx, portMAX_DELAY);
        usb_transfer_t *t = NULL;
        for (int i = 0; i < N_CMD && !t; i++) if (!s_d.cmd_busy[i]) { s_d.cmd_busy[i] = true; t = s_d.cmd[i]; }
        xSemaphoreGive(s_mx);
        if (!t) return;
        if (s_d.serial) {                   /* H4 on the bulk OUT endpoint, type byte and all */
            memcpy(t->data_buffer, data, len);
            t->num_bytes = len;
            INF_INC();
            if (usb_host_transfer_submit(t) != ESP_OK) out_done(t);
            return;
        }
        usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
        s->bmRequestType = 0x20;            /* host to device, class, device */
        s->bRequest = 0;
        s->wValue = 0;
        s->wIndex = 0;
        s->wLength = len - 1;
        memcpy(t->data_buffer + sizeof(usb_setup_packet_t), data + 1, len - 1);
        t->num_bytes = sizeof(usb_setup_packet_t) + len - 1;
        INF_INC();
        if (usb_host_transfer_submit_control(s_client, t) != ESP_OK) out_done(t);
    } else if (type == H4_ACL) {
        if (len >= 3) out_count((uint16_t)((data[1] | data[2] << 8) & 0x0FFF), 1);
        s_sent++;
        if (len > ACL_MAX || !xSemaphoreTake(s_acl_free, pdMS_TO_TICKS(500))) {
            ESP_LOGW(TAG, "ACL packet dropped (%u bytes)", len);
            return;
        }
        xSemaphoreTake(s_mx, portMAX_DELAY);
        usb_transfer_t *t = NULL;
        for (int i = 0; i < N_ACL && !t; i++) if (!s_d.acl_busy[i]) { s_d.acl_busy[i] = true; t = s_d.acl[i]; }
        xSemaphoreGive(s_mx);
        if (!t) return;
        int skip = s_d.serial ? 0 : 1;      /* USB: the endpoint says it's ACL; serial: the type byte does */
        memcpy(t->data_buffer, data + skip, len - skip);
        t->num_bytes = len - skip;
        INF_INC();
        if (usb_host_transfer_submit(t) != ESP_OK) out_done(t);
    }
    /* SCO (0x03) and anything else: not on this transport */
}

static bool usb_send_available(void)
{
    if (!(s_present && s_streaming && !s_d.gone)) return true;     /* refused at once */
    return uxSemaphoreGetCount(s_cmd_free) > 0 && uxSemaphoreGetCount(s_acl_free) > 0;
}

static esp_err_t usb_register_cb(const esp_bluedroid_hci_driver_callbacks_t *cb)
{
    s_host_cb = cb;
    return ESP_OK;
}

static const esp_bluedroid_hci_driver_operations_t k_ops = {
    .send = usb_send,
    .check_send_available = usb_send_available,
    .register_host_callback = usb_register_cb,
};

const esp_bluedroid_hci_driver_operations_t *lx_bt_usb_ops(void) { return &k_ops; }
uint32_t lx_bt_usb_sent(void) { return s_sent; }
bool lx_bt_usb_present(void) { return s_present && !s_d.gone; }
const char *lx_bt_usb_name(void) { return s_d.name; }

/* Sends a command without parameters and waits for its Command Complete (into s_pev) */
static bool probe_p(uint16_t op, const uint8_t *par, int n, int ms)
{
    if (!s_probe_sem) s_probe_sem = xSemaphoreCreateBinary();
    xSemaphoreTake(s_probe_sem, 0);
    s_probe_op = op;
    s_probe = true;
    uint8_t cmd[4 + 8] = { H4_CMD, op & 0xFF, op >> 8, (uint8_t)n };
    memcpy(cmd + 4, par, n);
    usb_send(cmd, 4 + n);
    bool got = xSemaphoreTake(s_probe_sem, pdMS_TO_TICKS(ms)) == pdTRUE;
    s_probe = false;
    if (!got) ESP_LOGW(TAG, "no reply to command %04x in %d ms", op, ms);
    return got;
}

static bool probe(uint16_t op, int ms) { return probe_p(op, NULL, 0, ms); }

static bool open_stream(void);

bool lx_bt_usb_open(void)
{
    s_opening = true;
    bool ok = open_stream();
    s_opening = false;
    return ok;
}

static bool open_stream(void)
{
    if (!lx_bt_usb_present()) return false;
    s_ev_len = s_acl_len = 0;
    s_h4 = 0;
    s_streaming = true;
    if (s_d.evt) {
        INF_INC();
        if (usb_host_transfer_submit(s_d.evt) != ESP_OK) {
            INF_DEC();
            s_streaming = false;
            return false;
        }
    }
    for (int i = 0; i < N_BULKIN; i++) {
        INF_INC();
        if (usb_host_transfer_submit(s_d.in[i]) != ESP_OK) INF_DEC();
    }
    /* Read Local Supported Features: Bluedroid (BLE built in) asserts on a controller without LE,
     * so for such a dongle (Bluetooth 2.x/3.0) LE is answered here (s_fake_le) */
    s_fake_p1 = s_fake_le = false;
    s_le_share = 0;
    s_why = "";
    bool got = probe(0x0C03, 3000) && probe(0x1003, 2000);    /* HCI Reset first: a dongle can be slow to wake */
    if (!got || s_pev[1] != 0x0E || s_pev[2] < 12 || s_pev[4] != 0x03 || s_pev[5] != 0x10 || s_pev[6]) {
        ESP_LOGW(TAG, "%s: no answer to Read Local Supported Features", s_d.name);
        s_why = "The USB Bluetooth dongle does not answer";
        return false;
    }
    s_fake_le = !(s_pev[7 + 4] & 0x40);
    if (s_fake_le) {
        memcpy(s_feat0, s_pev + 7, 8);
        s_feat0[4] |= 0x40;
        ESP_LOGI(TAG, "features %02X %02X %02X %02X %02X %02X %02X %02X", s_pev[7], s_pev[8], s_pev[9], s_pev[10],
                 s_pev[11], s_pev[12], s_pev[13], s_pev[14]);
        ESP_LOGW(TAG, "%s: no Bluetooth LE (Bluetooth 2.x/3.0): Classic only (controllers over BLE won't connect)", s_d.name);
        return true;
    }
    /* LE Read Buffer Size: none of its own = shared with Classic (fix_buffers) */
    if (probe(0x2002, 1000) && s_pev[1] == 0x0E && s_pev[2] >= 7 && !s_pev[6] && !s_pev[9]) s_le_share = LE_SHARE;
    return true;
}

const char *lx_bt_usb_why(void) { return s_why; }

/* Bluedroid has stopped: the IN transfers stay queued until the next start, or are taken back
 * now; halting the endpoints returns them through their callbacks (no resubmit: not streaming) */
void lx_bt_usb_close(void)
{
    s_streaming = false;
    s_host_cb = NULL;
    xSemaphoreTake(s_mx, portMAX_DELAY);
    if (s_present && !s_d.gone) {
        uint8_t eps[] = { s_d.ep_evt, s_d.ep_in };
        for (int i = 0; i < 2; i++) {
            if (!eps[i]) continue;          /* serial: no event endpoint */
            usb_host_endpoint_halt(s_d.dev, eps[i]);
            usb_host_endpoint_flush(s_d.dev, eps[i]);
            usb_host_endpoint_clear(s_d.dev, eps[i]);
        }
    }
    xSemaphoreGive(s_mx);
}

/* ── Device bring-up (client task) ──────────────────────────────────────── */

static bool find_bt(const usb_config_desc_t *cfg, dongle_t *d, uint16_t *mps_in)
{
    const uint8_t *p = (const uint8_t *)cfg, *end = p + cfg->wTotalLength;
    bool in_bt = false, found = false;
    for (; p + 2 <= end && p[0]; p += p[0]) {
        if (p[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)p;
            in_bt = !found && i->bInterfaceClass == 0xE0 && i->bInterfaceSubClass == 0x01 &&
                    i->bInterfaceProtocol == 0x01 && i->bAlternateSetting == 0;
            if (in_bt) { d->iface = i->bInterfaceNumber; found = true; }
            else if (found) break;          /* the next interface (SCO) */
        } else if (in_bt && p[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)p;
            uint8_t type = e->bmAttributes & 0x03;
            if (type == USB_TRANSFER_TYPE_INTR && (e->bEndpointAddress & 0x80)) {
                d->ep_evt = e->bEndpointAddress;
                d->mps_evt = e->wMaxPacketSize & 0x7FF;
            } else if (type == USB_TRANSFER_TYPE_BULK) {
                if (e->bEndpointAddress & 0x80) { d->ep_in = e->bEndpointAddress; *mps_in = e->wMaxPacketSize & 0x7FF; }
                else d->ep_out = e->bEndpointAddress;
            }
        }
    }
    return found && d->ep_evt && d->ep_in && d->ep_out && d->mps_evt;
}

static void free_all(dongle_t *d)
{
    usb_transfer_t **all[1 + N_BULKIN + N_CMD + N_ACL], ***a = all;
    *a++ = &d->evt;
    for (int i = 0; i < N_BULKIN; i++) *a++ = &d->in[i];
    for (int i = 0; i < N_CMD; i++) *a++ = &d->cmd[i];
    for (int i = 0; i < N_ACL; i++) *a++ = &d->acl[i];
    for (usb_transfer_t ***q = all; q < a; q++)
        if (**q) { usb_host_transfer_free(**q); **q = NULL; }
}

static bool alloc_all(dongle_t *d, uint16_t mps_in)
{
    int bulk = mps_in && BULK_IN % mps_in == 0 ? BULK_IN : (BULK_IN / (mps_in ? mps_in : 64) + 1) * (mps_in ? mps_in : 64);
    bool ok = !d->mps_evt || usb_host_transfer_alloc(d->mps_evt, 0, &d->evt) == ESP_OK;  /* serial: no event endpoint */
    for (int i = 0; i < N_BULKIN && ok; i++) ok = usb_host_transfer_alloc(bulk, 0, &d->in[i]) == ESP_OK;
    for (int i = 0; i < N_CMD && ok; i++) ok = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + CMD_MAX, 0, &d->cmd[i]) == ESP_OK;
    for (int i = 0; i < N_ACL && ok; i++) ok = usb_host_transfer_alloc(ACL_MAX, 0, &d->acl[i]) == ESP_OK;
    if (!ok) return false;
    if (d->evt) {
        d->evt->device_handle = d->dev;
        d->evt->bEndpointAddress = d->ep_evt;
        d->evt->callback = in_done;
        d->evt->num_bytes = d->mps_evt;
    }
    for (int i = 0; i < N_BULKIN; i++) {
        d->in[i]->device_handle = d->dev;
        d->in[i]->bEndpointAddress = d->ep_in;
        d->in[i]->callback = in_done;
        d->in[i]->num_bytes = bulk;
    }
    for (int i = 0; i < N_CMD; i++) {
        d->cmd[i]->device_handle = d->dev;
        d->cmd[i]->bEndpointAddress = d->serial ? d->ep_out : 0;   /* serial: commands are bulk too */
        d->cmd[i]->callback = out_done;
    }
    for (int i = 0; i < N_ACL; i++) {
        d->acl[i]->device_handle = d->dev;
        d->acl[i]->bEndpointAddress = d->ep_out;
        d->acl[i]->callback = out_done;
    }
    return true;
}

static void product(usb_device_handle_t dev, const usb_device_desc_t *dd, char *out, size_t len)
{
    usb_device_info_t info;
    size_t n = 0;
    if (usb_host_device_info(dev, &info) == ESP_OK && info.str_desc_product) {
        const usb_str_desc_t *s = info.str_desc_product;
        for (int i = 0; i < (s->bLength - 2) / 2 && n + 1 < len; i++)
            out[n++] = s->wData[i] < 0x80 ? (char)s->wData[i] : '?';
    }
    out[n] = 0;
    if (!n) snprintf(out, len, "USB Bluetooth %04X:%04X", dd->idVendor, dd->idProduct);
}

/* ── The ESP32 dongle behind a USB-serial chip ─────────────────────────── */

/* CH340/CH341/CH9102 (1A86) or CP210x (10C4:EA60): the first interface's bulk endpoints */
static bool find_serial(const usb_device_desc_t *dd, const usb_config_desc_t *cfg, dongle_t *d, uint16_t *mps_in)
{
    if (dd->idVendor == 0x1A86 && (dd->idProduct == 0x7523 || dd->idProduct == 0x5523 || dd->idProduct == 0x55D4))
        d->serial = SER_CH34X;
    else if (dd->idVendor == 0x10C4 && dd->idProduct == 0xEA60)
        d->serial = SER_CP210X;
    else
        return false;
    const uint8_t *p = (const uint8_t *)cfg, *end = p + cfg->wTotalLength;
    bool found = false;
    for (; p + 2 <= end && p[0]; p += p[0]) {
        if (p[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)p;
            if (found) break;
            if (i->bAlternateSetting == 0) { d->iface = i->bInterfaceNumber; found = true; }
        } else if (found && p[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)p;
            if ((e->bmAttributes & 0x03) != USB_TRANSFER_TYPE_BULK) continue;
            if (e->bEndpointAddress & 0x80) { d->ep_in = e->bEndpointAddress; *mps_in = e->wMaxPacketSize & 0x7FF; }
            else d->ep_out = e->bEndpointAddress;
        }
    }
    if (!(found && d->ep_in && d->ep_out)) d->serial = 0;
    return d->serial != 0;
}

/* A transfer from the client task, waited for here (callbacks run in usb_host_client_handle_events) */
static volatile bool s_sync_done;
static void sync_cb(usb_transfer_t *t) { (void)t; s_sync_done = true; }

static bool sync_xfer(usb_transfer_t *t, bool control)
{
    usb_transfer_cb_t cb = t->callback;
    t->callback = sync_cb;
    s_sync_done = false;
    esp_err_t e = control ? usb_host_transfer_submit_control(s_client, t) : usb_host_transfer_submit(t);
    for (int i = 0; e == ESP_OK && !s_sync_done && i < 100; i++) usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    bool ok = e == ESP_OK && s_sync_done && t->status == USB_TRANSFER_STATUS_COMPLETED;
    if (s_sync_done || e != ESP_OK) t->callback = cb;      /* else it is still out: the device gets closed */
    return ok;
}

/* A vendor control request to the serial chip (data in or out) */
static bool ser_ctrl(uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, void *data, uint16_t n)
{
    usb_transfer_t *t = s_d.cmd[0];
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = rt;
    s->bRequest = req;
    s->wValue = val;
    s->wIndex = idx;
    s->wLength = n;
    if (!(rt & 0x80) && n) memcpy(t->data_buffer + sizeof(*s), data, n);
    t->num_bytes = sizeof(*s) + n;
    uint8_t ep = t->bEndpointAddress;
    t->bEndpointAddress = 0;
    bool ok = sync_xfer(t, true);
    t->bEndpointAddress = ep;
    if (ok && (rt & 0x80) && n) memcpy(data, t->data_buffer + sizeof(*s), n);
    if (!ok) ESP_LOGW(TAG, "%s: request %02X failed", s_d.name, req);
    return ok;
}

/* CH34x baud divisor, as in Linux (ch341.c): 48 MHz / prescaler / divisor; prescaler register in the
 * low byte (fact bit 2, ps bits 0..1), 256 - divisor in the high byte */
static uint16_t ch34x_divisor(uint32_t speed)
{
    const uint32_t clk = 48000000;
    int ps, fact = 1;
    for (ps = 3; ps > 0; ps--)
        if (speed > clk / ((1u << (12 - 3 * ps - 1)) * 512)) break;
    uint32_t clk_div = 1u << (12 - 3 * ps - fact);
    uint32_t div = clk / (clk_div * speed);
    if (div < 9 || div > 255) { div /= 2; clk_div *= 2; fact = 0; }
    if (16 * clk / (clk_div * div) - 16 * speed >= 16 * speed - 16 * clk / (clk_div * (div + 1))) div++;
    if (fact == 1 && div % 2 == 0) { div /= 2; fact = 0; }
    return (uint16_t)((0x100 - div) << 8 | fact << 2 | ps);
}

/* 921600 8N1, no flow control, DTR and RTS released (on an ESP32 dev board they drive EN and IO0) */
static bool serial_setup(void)
{
    if (s_d.serial == SER_CH34X) {
        uint8_t ver[2] = { 0 };
        if (!ser_ctrl(0xC0, 0x5F, 0, 0, ver, 2) || !ser_ctrl(0x40, 0xA1, 0, 0, NULL, 0)) return false;  /* version, init */
        /* bit 7: pass on what came in at once, not 32 bytes at a time (CH341A) */
        if (!ser_ctrl(0x40, 0x9A, 0x1312, ch34x_divisor(SER_BAUD) | 0x80, NULL, 0)) return false;
        if (ver[0] >= 0x30 && !ser_ctrl(0x40, 0x9A, 0x2518, 0xC3, NULL, 0)) return false;              /* LCR: 8N1, RX + TX on */
        return ser_ctrl(0x40, 0xA4, 0xFFFF, 0, NULL, 0);                    /* modem lines: inverted bits, all off */
    }
    uint32_t b = SER_BAUD;
    uint8_t baud[4] = { b & 0xFF, b >> 8 & 0xFF, b >> 16 & 0xFF, b >> 24 };
    uint16_t i = s_d.iface;
    return ser_ctrl(0x41, 0x00, 1, i, NULL, 0) &&                           /* IFC_ENABLE */
           ser_ctrl(0x41, 0x1E, 0, i, baud, 4) &&                           /* SET_BAUDRATE */
           ser_ctrl(0x41, 0x03, 0x0800, i, NULL, 0) &&                      /* SET_LINE_CTL: 8N1 */
           ser_ctrl(0x41, 0x07, 0x0300, i, NULL, 0);                        /* SET_MHS: DTR, RTS low */
}

/* RTS on or off (DTR stays off): on an ESP32 dev board RTS pulls EN low, a hardware reset */
static bool ser_rts(bool on)
{
    if (s_d.serial == SER_CH34X) return ser_ctrl(0x40, 0xA4, on ? (uint16_t)~0x40 : 0xFFFF, 0, NULL, 0);
    return ser_ctrl(0x41, 0x07, on ? 0x0302 : 0x0300, s_d.iface, NULL, 0);
}

/* Is it the ESP32 dongle? It is reset first (RTS pulse): a dongle that stayed powered while the board
 * restarted still has its old links and keeps sending, and its stream would be joined mid-packet. After
 * a reset the ROM prints its banner at 115200 and the controller is ready ~1 s later, so the first
 * 1.2 s of input are dropped. Then the identify command (0xFC7E) answers "LXBT", protocol, firmware
 * version, baud, flags, links. The stream stops again afterwards and starts with Bluedroid
 * (open_stream). */
static bool serial_identify(void)
{
    s_h4 = 0;
    s_discard = true;
    s_streaming = true;
    for (int i = 0; i < N_BULKIN; i++) {
        INF_INC();
        if (usb_host_transfer_submit(s_d.in[i]) != ESP_OK) INF_DEC();
    }
    if (ser_rts(true)) {
        for (int i = 0; i < 10 && !s_d.gone; i++) usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
        ser_rts(false);
    }
    for (int i = 0; i < 120 && !s_d.gone; i++) usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    bool ok = false;
    for (int a = 0; a < 3 && !ok && !s_d.gone; a++) {
        s_discard = false;
        s_h4 = 0;
        s_ev_len = s_acl_len = 0;
        s_probe_op = 0xFC7E;
        s_probe = true;
        usb_transfer_t *t = s_d.acl[0];
        static const uint8_t c[] = { H4_CMD, 0x7E, 0xFC, 0 };
        memcpy(t->data_buffer, c, sizeof(c));
        t->num_bytes = sizeof(c);
        if (!sync_xfer(t, false)) break;
        for (int i = 0; i < 60 && s_probe; i++) usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
        ok = !s_probe && s_pev[2] >= 19 && !s_pev[6] && !memcmp(s_pev + 7, "LXBT", 4);
    }
    s_probe = false;
    if (ok)
        snprintf(s_d.name, sizeof(s_d.name), "ESP32 dongle %u.%u.%u (%s)", s_pev[12], s_pev[13], s_pev[14],
                 s_d.serial == SER_CH34X ? "CH34x" : "CP210x");
    s_streaming = false;
    usb_host_endpoint_halt(s_d.dev, s_d.ep_in);
    usb_host_endpoint_flush(s_d.dev, s_d.ep_in);
    for (int i = 0; i < 20 && s_d.inflight > 0; i++) usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
    usb_host_endpoint_clear(s_d.dev, s_d.ep_in);
    return ok;
}

static void open_dongle(uint8_t addr)
{
    if (s_present) return;                  /* one dongle at a time */
    usb_device_handle_t dev;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) return;
    const usb_config_desc_t *cfg;
    const usb_device_desc_t *dd;
    dongle_t d = { .dev = dev, .addr = addr };
    uint16_t mps_in = 64;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || usb_host_get_device_descriptor(dev, &dd) != ESP_OK ||
        (!find_bt(cfg, &d, &mps_in) && !find_serial(dd, cfg, (d = (dongle_t){ .dev = dev, .addr = addr }, &d), &mps_in))) {
        usb_host_device_close(s_client, dev);       /* not a Bluetooth controller */
        return;
    }
    if (d.serial) snprintf(d.name, sizeof(d.name), "USB serial %04X:%04X", dd->idVendor, dd->idProduct);
    else product(dev, dd, d.name, sizeof(d.name));
    if (usb_host_interface_claim(s_client, dev, d.iface, 0) != ESP_OK) {
        ESP_LOGW(TAG, "%s: cannot claim interface %d", d.name, d.iface);
        usb_host_device_close(s_client, dev);
        return;
    }
    if (!alloc_all(&d, mps_in)) {
        ESP_LOGE(TAG, "%s: no memory for transfers", d.name);
        free_all(&d);
        usb_host_interface_release(s_client, dev, d.iface);
        usb_host_device_close(s_client, dev);
        return;
    }
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_d = d;
    while (xSemaphoreTake(s_cmd_free, 0)) {}
    while (xSemaphoreTake(s_acl_free, 0)) {}
    for (int i = 0; i < N_CMD; i++) xSemaphoreGive(s_cmd_free);
    for (int i = 0; i < N_ACL; i++) xSemaphoreGive(s_acl_free);
    xSemaphoreGive(s_mx);
    if (d.serial && !(serial_setup() && serial_identify())) {
        ESP_LOGI(TAG, "%04X:%04X: a USB-serial chip, not the ESP32 Bluetooth dongle", dd->idVendor, dd->idProduct);
        xSemaphoreTake(s_mx, portMAX_DELAY);
        free_all(&s_d);
        usb_host_interface_release(s_client, dev, s_d.iface);
        usb_host_device_close(s_client, dev);
        memset(&s_d, 0, sizeof(s_d));
        xSemaphoreGive(s_mx);
        return;
    }
    s_present = true;
    d = s_d;
    ESP_LOGI(TAG, "%04X:%04X %s: Bluetooth controller (events 0x%02X, ACL 0x%02X/0x%02X)",
             dd->idVendor, dd->idProduct, d.name, d.ep_evt, d.ep_in, d.ep_out);
    if (dd->idVendor == 0x0BDA || dd->idVendor == 0x2357 || dd->idVendor == 0x0B05)
        ESP_LOGW(TAG, "a Realtek chip, it may need a firmware download (not done yet): Classic may not work");
    lx_bt_speaker_dongle(true);
    lx_bt_pads_dongle(true);
}

static void close_dongle(void)
{
    dongle_t *d = &s_d;
    d->gone = true;
    s_streaming = false;
    lx_bt_transport_lost();                 /* Bluedroid stops; its commands are refused meanwhile */
    uint8_t eps[] = { d->ep_evt, d->ep_in, d->ep_out };
    for (int i = 0; i < 3; i++) {
        if (!eps[i]) continue;
        usb_host_endpoint_halt(d->dev, eps[i]);
        usb_host_endpoint_flush(d->dev, eps[i]);
    }
    /* The flushed transfers come back through their callbacks; only then may they be freed */
    for (int i = 0; i < 40 && d->inflight > 0; i++) usb_host_client_handle_events(s_client, pdMS_TO_TICKS(25));
    if (d->inflight > 0) ESP_LOGW(TAG, "%d transfers did not come back", d->inflight);
    xSemaphoreTake(s_mx, portMAX_DELAY);
    s_present = false;
    free_all(d);
    usb_host_interface_release(s_client, d->dev, d->iface);
    usb_host_device_close(s_client, d->dev);
    ESP_LOGI(TAG, "%s gone", d->name);
    memset(d, 0, sizeof(*d));
    xSemaphoreGive(s_mx);
    lx_bt_speaker_dongle(false);
    lx_bt_pads_dongle(false);
}

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (s_nnew < (int)sizeof(s_new)) s_new[s_nnew++] = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        if (s_present && msg->dev_gone.dev_hdl == s_d.dev) s_d.gone = true;
    }
}

static void client_task(void *arg)
{
    uint8_t addrs[8];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) == ESP_OK)    /* plugged in before we registered */
        for (int i = 0; i < n; i++) open_dongle(addrs[i]);
    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(50));
        while (s_nnew) open_dongle(s_new[--s_nnew]);
        if (s_present && s_d.gone && s_d.dev) close_dongle();
        else if (s_present && s_d.failed) {
            /* A failed IN transfer halts its endpoint: clear it and go on */
            s_d.failed = false;
            uint8_t eps[] = { s_d.ep_evt, s_d.ep_in };
            for (int i = 0; i < 2; i++) {
                if (!eps[i]) continue;
                usb_host_endpoint_halt(s_d.dev, eps[i]);
                usb_host_endpoint_flush(s_d.dev, eps[i]);
            }
            for (int i = 0; i < 10 && s_d.inflight > 0; i++) usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
            for (int i = 0; i < 2; i++) if (eps[i]) usb_host_endpoint_clear(s_d.dev, eps[i]);
            ESP_LOGW(TAG, "restarted after a transfer error");
            if (s_streaming) {
                s_ev_len = s_acl_len = 0;
                s_h4 = 0;
                if (s_d.evt) {
                    INF_INC();
                    if (usb_host_transfer_submit(s_d.evt) != ESP_OK) INF_DEC();
                }
                for (int i = 0; i < N_BULKIN; i++) {
                    INF_INC();
                    if (usb_host_transfer_submit(s_d.in[i]) != ESP_OK) INF_DEC();
                }
            }
        }
    }
}

void lx_bt_usb_init(void)
{
    s_mx = xSemaphoreCreateMutex();
    s_cmd_free = xSemaphoreCreateCounting(N_CMD, 0);
    s_acl_free = xSemaphoreCreateCounting(N_ACL, 0);
    if (!s_probe_sem) s_probe_sem = xSemaphoreCreateBinary();   /* evt_feed gives it (serial_identify) */
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = { .client_event_callback = client_cb, .callback_arg = NULL },
    };
    if (!s_mx || !s_cmd_free || !s_acl_free || usb_host_client_register(&cfg, &s_client) != ESP_OK) {
        ESP_LOGW(TAG, "no USB host client: dongles not supported (USB host not installed?)");
        return;
    }
    /* above lwIP (18) and the C6 link: a Wi-Fi download must not hold up the dongle's transfers */
    xTaskCreatePinnedToCore(client_task, "lx_bt_usb", 4096, NULL, 19, NULL, 0);
}

#endif
