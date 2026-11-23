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
 */
#include "sdkconfig.h"

#if CONFIG_BT_BLUEDROID_ENABLED && CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID

#include <stdio.h>
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
#define N_BULKIN 2
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
    char     name[40];
} dongle_t;

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
static const char *s_why = "";

static uint8_t  s_ev[1 + 2 + 255];
static int      s_ev_len;               /* bytes after the type byte */
static uint8_t  s_acl[1 + ACL_BUF];
static int      s_acl_len;

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
static void fix_features(void)
{
    uint8_t *e = s_ev;
    if (e[1] != 0x0E || e[2] < 14 || e[4] != 0x04 || e[5] != 0x10 || e[6] || e[7] || e[8]) return;
    if (!(e[13] & 0x40)) return;                            /* page 0 byte 4 bit 6: LE supported */
    e[8] = 1;
    if (!s_fake_p1) ESP_LOGW(TAG, "dongle reports no features page 1: answering it here");
    s_fake_p1 = true;
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
            if (s_probe) {
                /* only the reply to the probe's command: a late reply to an earlier one is dropped */
                if (s_ev[1] == 0x0E && s_ev[2] >= 3 && (s_ev[4] | s_ev[5] << 8) == s_probe_op) {
                    memcpy(s_pev, s_ev, 1 + s_ev_len);
                    s_probe = false;
                    xSemaphoreGive(s_probe_sem);
                }
            } else {
                fix_features();
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

static void in_done(usb_transfer_t *t)
{
    INF_DEC();
    bool evt = t == s_d.evt;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) {
        if (evt) evt_feed(t->data_buffer, t->actual_num_bytes);
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

static void usb_send(uint8_t *data, uint16_t len)
{
    if (len < 1) return;
    uint8_t type = data[0];
    bool live = s_present && s_streaming && !s_d.gone;
    if (!live) {
        if (type == H4_CMD && len >= 4) refuse(data);
        return;
    }
    if (type == H4_CMD && s_fake_p1 && len >= 5 && data[1] == 0x04 && data[2] == 0x10 && data[4] >= 1) {
        uint8_t r[] = { H4_EVT, 0x0E, 14, 1, 0x04, 0x10, 0, data[4], 1,
                        data[4] == 1 ? 0x07 : 0, 0, 0, 0, 0, 0, 0, 0 };
        if (s_host_cb) s_host_cb->notify_host_recv(r, sizeof(r));
        return;
    }
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
        if (len - 1 > ACL_MAX || !xSemaphoreTake(s_acl_free, pdMS_TO_TICKS(500))) {
            ESP_LOGW(TAG, "ACL packet dropped (%u bytes)", len);
            return;
        }
        xSemaphoreTake(s_mx, portMAX_DELAY);
        usb_transfer_t *t = NULL;
        for (int i = 0; i < N_ACL && !t; i++) if (!s_d.acl_busy[i]) { s_d.acl_busy[i] = true; t = s_d.acl[i]; }
        xSemaphoreGive(s_mx);
        if (!t) return;
        memcpy(t->data_buffer, data + 1, len - 1);
        t->num_bytes = len - 1;
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
bool lx_bt_usb_present(void) { return s_present && !s_d.gone; }
const char *lx_bt_usb_name(void) { return s_d.name; }

/* Sends a command without parameters and waits for its Command Complete (into s_pev) */
static bool probe(uint16_t op, int ms)
{
    if (!s_probe_sem) s_probe_sem = xSemaphoreCreateBinary();
    xSemaphoreTake(s_probe_sem, 0);
    s_probe_op = op;
    s_probe = true;
    uint8_t cmd[] = { H4_CMD, op & 0xFF, op >> 8, 0 };
    usb_send(cmd, sizeof(cmd));
    bool got = xSemaphoreTake(s_probe_sem, pdMS_TO_TICKS(ms)) == pdTRUE;
    s_probe = false;
    if (!got) ESP_LOGW(TAG, "no reply to command %04x in %d ms", op, ms);
    return got;
}

bool lx_bt_usb_open(void)
{
    if (!lx_bt_usb_present()) return false;
    s_ev_len = s_acl_len = 0;
    s_streaming = true;
    INF_INC();
    if (usb_host_transfer_submit(s_d.evt) != ESP_OK) {
        INF_DEC();
        s_streaming = false;
        return false;
    }
    for (int i = 0; i < N_BULKIN; i++) {
        INF_INC();
        if (usb_host_transfer_submit(s_d.in[i]) != ESP_OK) INF_DEC();
    }
    /* Read Local Supported Features: Bluedroid (BLE built in) asserts on a controller without LE,
     * so such a dongle (Bluetooth 2.x/3.0) is not handed to it */
    s_fake_p1 = false;
    s_why = "";
    bool got = probe(0x0C03, 3000) && probe(0x1003, 2000);    /* HCI Reset first: a dongle can be slow to wake */
    if (!got || s_pev[1] != 0x0E || s_pev[2] < 12 || s_pev[4] != 0x03 || s_pev[5] != 0x10 || s_pev[6]) {
        ESP_LOGW(TAG, "%s: no answer to Read Local Supported Features", s_d.name);
        s_why = "The USB Bluetooth dongle does not answer";
        return false;
    }
    if (!(s_pev[7 + 4] & 0x40)) {
        ESP_LOGW(TAG, "%s: no Bluetooth LE (Bluetooth 2.x/3.0): not used", s_d.name);
        s_why = "This dongle is Bluetooth 2.x/3.0 (no LE): use a Bluetooth 4.0 or newer dongle";
        return false;
    }
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
    usb_transfer_t **all[] = { &d->evt, &d->in[0], &d->in[1], &d->cmd[0], &d->cmd[1],
                               &d->acl[0], &d->acl[1], &d->acl[2], &d->acl[3] };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        if (*all[i]) { usb_host_transfer_free(*all[i]); *all[i] = NULL; }
}

static bool alloc_all(dongle_t *d, uint16_t mps_in)
{
    int bulk = mps_in && BULK_IN % mps_in == 0 ? BULK_IN : (BULK_IN / (mps_in ? mps_in : 64) + 1) * (mps_in ? mps_in : 64);
    bool ok = usb_host_transfer_alloc(d->mps_evt, 0, &d->evt) == ESP_OK;
    for (int i = 0; i < N_BULKIN && ok; i++) ok = usb_host_transfer_alloc(bulk, 0, &d->in[i]) == ESP_OK;
    for (int i = 0; i < N_CMD && ok; i++) ok = usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + CMD_MAX, 0, &d->cmd[i]) == ESP_OK;
    for (int i = 0; i < N_ACL && ok; i++) ok = usb_host_transfer_alloc(ACL_MAX, 0, &d->acl[i]) == ESP_OK;
    if (!ok) return false;
    d->evt->device_handle = d->dev;
    d->evt->bEndpointAddress = d->ep_evt;
    d->evt->callback = in_done;
    d->evt->num_bytes = d->mps_evt;
    for (int i = 0; i < N_BULKIN; i++) {
        d->in[i]->device_handle = d->dev;
        d->in[i]->bEndpointAddress = d->ep_in;
        d->in[i]->callback = in_done;
        d->in[i]->num_bytes = bulk;
    }
    for (int i = 0; i < N_CMD; i++) {
        d->cmd[i]->device_handle = d->dev;
        d->cmd[i]->bEndpointAddress = 0;
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
        !find_bt(cfg, &d, &mps_in)) {
        usb_host_device_close(s_client, dev);       /* not a Bluetooth controller */
        return;
    }
    product(dev, dd, d.name, sizeof(d.name));
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
    s_present = true;
    xSemaphoreGive(s_mx);
    ESP_LOGI(TAG, "%04X:%04X %s: Bluetooth controller (events 0x%02X, ACL 0x%02X/0x%02X)",
             dd->idVendor, dd->idProduct, d.name, d.ep_evt, d.ep_in, d.ep_out);
    if (dd->idVendor == 0x0BDA || dd->idVendor == 0x2357 || dd->idVendor == 0x0B05)
        ESP_LOGW(TAG, "a Realtek chip, it may need a firmware download (not done yet): Classic may not work");
    lx_bt_speaker_dongle(true);
}

static void close_dongle(void)
{
    dongle_t *d = &s_d;
    d->gone = true;
    s_streaming = false;
    lx_bt_transport_lost();                 /* Bluedroid stops; its commands are refused meanwhile */
    uint8_t eps[] = { d->ep_evt, d->ep_in, d->ep_out };
    for (int i = 0; i < 3; i++) {
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
                usb_host_endpoint_halt(s_d.dev, eps[i]);
                usb_host_endpoint_flush(s_d.dev, eps[i]);
            }
            for (int i = 0; i < 10 && s_d.inflight > 0; i++) usb_host_client_handle_events(s_client, pdMS_TO_TICKS(10));
            for (int i = 0; i < 2; i++) usb_host_endpoint_clear(s_d.dev, eps[i]);
            ESP_LOGW(TAG, "restarted after a transfer error");
            if (s_streaming) {
                s_ev_len = s_acl_len = 0;
                INF_INC();
                if (usb_host_transfer_submit(s_d.evt) != ESP_OK) INF_DEC();
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
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = { .client_event_callback = client_cb, .callback_arg = NULL },
    };
    if (!s_mx || !s_cmd_free || !s_acl_free || usb_host_client_register(&cfg, &s_client) != ESP_OK) {
        ESP_LOGW(TAG, "no USB host client: dongles not supported (USB host not installed?)");
        return;
    }
    xTaskCreatePinnedToCore(client_task, "lx_bt_usb", 4096, NULL, 6, NULL, 0);
}

#endif
