/*
 * XInput (Xbox 360 protocol) pads for the input manager: wired Xbox 360 pads
 * and receivers in X-input mode, such as 8BitDo's. They use a vendor interface
 * (class 0xFF, subclass 0x5D, protocol 0x01) that the HID driver never sees, so
 * this is a second USB host client. It claims only that interface; the HID
 * driver keeps any HID interface of the same device.
 *
 * Report: [0]=0x00 [1]=0x14 [2..3]=buttons [4]=LT [5]=RT [6..13]=LX LY RX RY (int16, Y up)
 */
#include "lx_input.h"
#include "lx_input_priv.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "usb/usb_host.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lx_xinput";

#define MAX_PADS 4

typedef struct {
    usb_device_handle_t dev;
    uint8_t addr, iface;
    usb_transfer_t *in;
    int player;
    bool gone;
    bool failed;                    /* IN transfer failed: endpoint halted, client_task restarts it */
} xpad_t;

static usb_host_client_handle_t s_client;
static xpad_t s_pads[MAX_PADS];
static uint8_t s_new[8];
static volatile int s_nnew;

static void on_report(xpad_t *p, const uint8_t *r, int len)
{
    if (len < 14 || r[0] != 0x00) return;          /* 0x01 LED / 0x03 rumble status, ... */
    static const struct { uint8_t byte, bit; uint32_t btn; } k[] = {
        { 2, 0x01, PAPP_BTN_UP },   { 2, 0x02, PAPP_BTN_DOWN }, { 2, 0x04, PAPP_BTN_LEFT },
        { 2, 0x08, PAPP_BTN_RIGHT }, { 2, 0x10, PAPP_BTN_START }, { 2, 0x20, PAPP_BTN_SELECT },
        { 2, 0x40, PAPP_BTN_L3 },   { 2, 0x80, PAPP_BTN_R3 },   { 3, 0x01, PAPP_BTN_L1 },
        { 3, 0x02, PAPP_BTN_R1 },   { 3, 0x04, PAPP_BTN_HOME }, { 3, 0x10, PAPP_BTN_A },
        { 3, 0x20, PAPP_BTN_B },    { 3, 0x40, PAPP_BTN_X },    { 3, 0x80, PAPP_BTN_Y },
    };
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++)
        if (r[k[i].byte] & k[i].bit) b |= k[i].btn;
    if (r[4] > 0x80) b |= PAPP_BTN_L2;              /* triggers also count as buttons */
    if (r[5] > 0x80) b |= PAPP_BTN_R2;
    int16_t lx = (int16_t)(r[6] | r[7] << 8), ly = (int16_t)(r[8] | r[9] << 8);
    int16_t rx = (int16_t)(r[10] | r[11] << 8), ry = (int16_t)(r[12] | r[13] << 8);
    /* Y is up-positive here, down-positive for apps; -32768 has no opposite */
    ly = ly == -32768 ? 32767 : -ly;
    ry = ry == -32768 ? 32767 : -ry;
    lx_input_pad_update(p->player, b, lx, ly, rx, ry, (uint16_t)(r[4] * 257), (uint16_t)(r[5] * 257));
}

static void in_done(usb_transfer_t *t)
{
    xpad_t *p = t->context;
    if (t->status == USB_TRANSFER_STATUS_COMPLETED) on_report(p, t->data_buffer, t->actual_num_bytes);
    if (p->gone || t->status == USB_TRANSFER_STATUS_NO_DEVICE || t->status == USB_TRANSFER_STATUS_CANCELED) return;
    if (t->status != USB_TRANSFER_STATUS_COMPLETED) {
        p->failed = true;           /* the endpoint is halted: a submit would fail */
        return;
    }
    if (usb_host_transfer_submit(t) != ESP_OK) ESP_LOGW(TAG, "resubmit failed");
}

static void ctrl_done(usb_transfer_t *t)
{
    ESP_LOGD(TAG, "start request: status %d, %d bytes", t->status, t->actual_num_bytes);
    usb_host_transfer_free(t);
}

/* Some third-party 360 pads (8BitDo among them) stay silent until this vendor
 * request, as Linux's xpad sends it: IN, vendor, interface, 0x01, 0x0100, 20 bytes */
static void start_request(usb_device_handle_t dev, uint8_t iface)
{
    usb_transfer_t *t;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + 20, 0, &t) != ESP_OK) return;
    usb_setup_packet_t *s = (usb_setup_packet_t *)t->data_buffer;
    s->bmRequestType = 0xC1;
    s->bRequest = 0x01;
    s->wValue = 0x0100;
    s->wIndex = iface;
    s->wLength = 20;
    t->num_bytes = sizeof(usb_setup_packet_t) + 20;
    t->device_handle = dev;
    t->bEndpointAddress = 0;
    t->callback = ctrl_done;
    t->context = NULL;
    if (usb_host_transfer_submit_control(s_client, t) != ESP_OK) usb_host_transfer_free(t);
}

static void out_done(usb_transfer_t *t)
{
    ESP_LOGD(TAG, "LED command: status %d", t->status);
    usb_host_transfer_free(t);
}

/* Player LED (1-4 lit): 360 pads, and clones that wait for it before reporting */
static void set_led(usb_device_handle_t dev, uint8_t ep, int player)
{
    usb_transfer_t *t;
    if (usb_host_transfer_alloc(32, 0, &t) != ESP_OK) return;
    t->data_buffer[0] = 0x01;
    t->data_buffer[1] = 0x03;
    t->data_buffer[2] = 0x06 + (player & 3);
    t->num_bytes = 3;
    t->device_handle = dev;
    t->bEndpointAddress = ep;
    t->callback = out_done;
    t->context = NULL;
    if (usb_host_transfer_submit(t) != ESP_OK) usb_host_transfer_free(t);
}

static void close_pad(xpad_t *p)
{
    if (p->player >= 0) lx_input_pad_detach(p->player);
    if (p->in) {
        usb_host_endpoint_halt(p->dev, p->in->bEndpointAddress);
        usb_host_endpoint_flush(p->dev, p->in->bEndpointAddress);
        usb_host_endpoint_clear(p->dev, p->in->bEndpointAddress);
    }
    usb_host_interface_release(s_client, p->dev, p->iface);
    if (p->in) usb_host_transfer_free(p->in);
    usb_host_device_close(s_client, p->dev);
    memset(p, 0, sizeof(*p));
}

static void product(usb_device_handle_t dev, char *out, size_t len)
{
    usb_device_info_t info;
    size_t n = 0;
    if (usb_host_device_info(dev, &info) == ESP_OK && info.str_desc_product) {
        const usb_str_desc_t *s = info.str_desc_product;
        for (int i = 0; i < (s->bLength - 2) / 2 && n + 1 < len; i++)
            out[n++] = s->wData[i] < 0x80 ? (char)s->wData[i] : '?';
    }
    out[n] = 0;
    if (!n) snprintf(out, len, "Xbox pad");
}

/* Finds the XInput gamepad interface and its interrupt IN endpoint */
static bool find_xinput(const usb_config_desc_t *cfg, uint8_t *iface, uint8_t *ep, uint16_t *mps, uint8_t *out)
{
    const uint8_t *d = (const uint8_t *)cfg, *end = d + cfg->wTotalLength;
    bool in_xinput = false, found = false;
    for (; d + 2 <= end && d[0]; d += d[0]) {
        if (d[1] == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *i = (const usb_intf_desc_t *)d;
            ESP_LOGD(TAG, "interface %d alt %d: class %02X/%02X/%02X", i->bInterfaceNumber, i->bAlternateSetting,
                     i->bInterfaceClass, i->bInterfaceSubClass, i->bInterfaceProtocol);
            in_xinput = i->bInterfaceClass == 0xFF && i->bInterfaceSubClass == 0x5D &&
                        i->bInterfaceProtocol == 0x01 && i->bAlternateSetting == 0;
            if (in_xinput) *iface = i->bInterfaceNumber;
        } else if (in_xinput && d[1] == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            const usb_ep_desc_t *e = (const usb_ep_desc_t *)d;
            if ((e->bmAttributes & 0x03) != USB_TRANSFER_TYPE_INTR) continue;
            if (e->bEndpointAddress & 0x80) {
                if (!found) {
                    *ep = e->bEndpointAddress;
                    *mps = e->wMaxPacketSize & 0x7FF;
                    found = true;
                }
            } else if (!*out) {
                *out = e->bEndpointAddress;
            }
        }
    }
    return found;
}

static void open_pad(uint8_t addr)
{
    xpad_t *p = NULL;
    for (int i = 0; i < MAX_PADS; i++) if (s_pads[i].dev && s_pads[i].addr == addr) return;    /* already open */
    for (int i = 0; i < MAX_PADS; i++) if (!s_pads[i].dev) { p = &s_pads[i]; break; }
    usb_device_handle_t dev;
    esp_err_t e = p ? usb_host_device_open(s_client, addr, &dev) : ESP_ERR_NO_MEM;
    ESP_LOGD(TAG, "device %d: open %s", addr, esp_err_to_name(e));
    if (e != ESP_OK) return;
    const usb_config_desc_t *cfg;
    const usb_device_desc_t *dd;
    uint8_t iface = 0, ep = 0, out = 0;
    uint16_t mps = 0;
    usb_device_info_t info;
    if (usb_host_get_device_descriptor(dev, &dd) == ESP_OK && usb_host_device_info(dev, &info) == ESP_OK) {
        static const char *const speed[] = { "low", "full", "high" };
        if (info.parent.dev_hdl) {
            usb_device_info_t up;
            ESP_LOGI(TAG, "USB device %d: %04X:%04X class %02X, %s speed, on port %d of hub %d", addr,
                     dd->idVendor, dd->idProduct, dd->bDeviceClass, speed[info.speed % 3], info.parent.port_num,
                     usb_host_device_info(info.parent.dev_hdl, &up) == ESP_OK ? up.dev_addr : 0);
        } else {
            ESP_LOGI(TAG, "USB device %d: %04X:%04X class %02X, %s speed, on the board's port", addr,
                     dd->idVendor, dd->idProduct, dd->bDeviceClass, speed[info.speed % 3]);
        }
    }
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK || usb_host_get_device_descriptor(dev, &dd) != ESP_OK ||
        !find_xinput(cfg, &iface, &ep, &mps, &out)) {
        usb_host_device_close(s_client, dev);           /* not ours */
        return;
    }
    *p = (xpad_t){ .dev = dev, .addr = addr, .iface = iface, .player = -1 };
    if (usb_host_interface_claim(s_client, dev, iface, 0) != ESP_OK ||
        usb_host_transfer_alloc(mps < 32 ? 32 : mps, 0, &p->in) != ESP_OK) {
        ESP_LOGW(TAG, "%04X:%04X: cannot claim the XInput interface", dd->idVendor, dd->idProduct);
        p->in = NULL;
        close_pad(p);
        return;
    }
    p->in->device_handle = dev;
    p->in->bEndpointAddress = ep;
    p->in->callback = in_done;
    p->in->context = p;
    p->in->num_bytes = mps < 32 ? 32 : mps;
    char name[24];
    product(dev, name, sizeof(name));
    p->player = lx_input_pad_attach(name, dd->idVendor, dd->idProduct);
    ESP_LOGI(TAG, "%04X:%04X %s: XInput pad, player %d", dd->idVendor, dd->idProduct, name, p->player + 1);
    if (p->player >= 0) {
        start_request(dev, iface);
        if (out) set_led(dev, out, p->player);
    }
    if (p->player < 0 || usb_host_transfer_submit(p->in) != ESP_OK) close_pad(p);
}

static void client_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    if (msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (s_nnew < (int)sizeof(s_new)) s_new[s_nnew++] = msg->new_dev.address;
    } else if (msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        for (int i = 0; i < MAX_PADS; i++)
            if (s_pads[i].dev == msg->dev_gone.dev_hdl) s_pads[i].gone = true;
    }
}

static void client_task(void *arg)
{
    uint8_t addrs[8];
    int n = 0;
    if (usb_host_device_addr_list_fill(sizeof(addrs), addrs, &n) == ESP_OK)    /* plugged in before we registered */
        for (int i = 0; i < n; i++) open_pad(addrs[i]);
    for (;;) {
        usb_host_client_handle_events(s_client, pdMS_TO_TICKS(50));
        while (s_nnew) open_pad(s_new[--s_nnew]);   /* opening isn't allowed in the callback */
        for (int i = 0; i < MAX_PADS; i++)
            if (s_pads[i].dev && s_pads[i].gone) {
                ESP_LOGI(TAG, "player %d: XInput pad gone", s_pads[i].player + 1);
                close_pad(&s_pads[i]);
            } else if (s_pads[i].dev && s_pads[i].failed) {
                /* A failed transfer (e.g. the BNA workaround in lx_input_usb.c) halts
                   the endpoint: clear it and poll again */
                xpad_t *p = &s_pads[i];
                uint8_t ep = p->in->bEndpointAddress;
                p->failed = false;
                usb_host_endpoint_halt(p->dev, ep);
                usb_host_endpoint_flush(p->dev, ep);
                usb_host_endpoint_clear(p->dev, ep);
                esp_err_t e = usb_host_transfer_submit(p->in);
                ESP_LOGW(TAG, "player %d: restarted after a transfer error: %s", p->player + 1, esp_err_to_name(e));
            }
    }
}

esp_err_t lx_input_xinput_start(void)
{
    const usb_host_client_config_t cfg = {
        .is_synchronous = false,
        .max_num_event_msg = 8,
        .async = { .client_event_callback = client_cb, .callback_arg = NULL },
    };
    esp_err_t e = usb_host_client_register(&cfg, &s_client);
    if (e != ESP_OK) return e;
    return xTaskCreatePinnedToCore(client_task, "lx_xinput", 4096, NULL, 6, NULL, 0) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
