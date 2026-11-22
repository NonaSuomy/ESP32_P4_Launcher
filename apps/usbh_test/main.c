/*
 * usbh_test: the USBH interface (raw USB host). Lists the devices on the port, and for each one
 * reads its device descriptor again through a control transfer, walks its configuration
 * descriptor and tries to claim every interface (one a launcher driver holds must refuse).
 *   no args   : that, once
 *   "watch"   : then reports devices coming and going until stopped
 *   "picoboot": also asks an RP2040/RP2350 bootloader (2e8a:0003/000f) or a One ROM (1209:f540..f542)
 *               for its picoboot status (control IN 0x42 on the picoboot interface)
 */
#include "papp/papp.h"
#include <stdio.h>
#include <string.h>

PAPP_MANIFEST(.id = "test.usbh", .name = "USBH test", .version = PAPP_APP_VERSION, .main_stack = 16384,
              .flags = PAPP_MF_HIDDEN);

static const char *speed(int s) { return s == 2 ? "high" : s == 1 ? "full" : "low"; }

static void probe(const papp_if_usbh_t *u, const papp_usb_info_t *i, bool picoboot)
{
    papp_log("dev %u: %04x:%04x %s speed, class %02x, %u interfaces, \"%s\" \"%s\" serial \"%s\"\n", i->addr, i->vid,
             i->pid, speed(i->speed), i->dev_class, i->num_interfaces, i->manufacturer, i->product, i->serial);
    int err = 0;
    papp_usb_dev_t *d = u->open(i->addr, &err);
    if (!d) { papp_log("  open failed: %d\n", err); return; }
    uint8_t dd[18] = { 0 };
    int n = u->control(d, 0x80, 6, 0x0100, 0, dd, sizeof(dd), 1000);      /* GET_DESCRIPTOR(device) */
    papp_log("  GET_DESCRIPTOR(device): %d bytes, idVendor %04x idProduct %04x %s\n", n, dd[8] | dd[9] << 8,
             dd[10] | dd[11] << 8, n == 18 && (dd[8] | dd[9] << 8) == i->vid ? "(matches)" : "(MISMATCH)");
    uint8_t cfg[512];
    int total = u->config_descriptor(d, cfg, sizeof(cfg));
    papp_log("  configuration: %d bytes\n", total);
    int pb_intf = -1;
    for (int off = 0; off + 2 <= total && off < (int)sizeof(cfg) && cfg[off];) {
        const uint8_t *p = cfg + off;
        if (p[1] == 4 && p[3] == 0) {                                     /* interface, alternate 0 */
            int r = u->claim(d, p[2], 0);
            papp_log("  intf %u class %02x/%02x/%02x, %u eps: claim %s\n", p[2], p[5], p[6], p[7], p[4],
                     r == 0 ? "ok" : r == PAPP_E_IO ? "refused (held)" : "failed");
            if (r == 0 && p[5] == 0xFF && p[6] == 0 && p[7] == 0 && p[4] == 2) pb_intf = p[2];   /* picoboot-shaped */
            if (r == 0 && !(picoboot && pb_intf == p[2])) u->release(d, p[2]);
        } else if (p[1] == 5) {
            papp_log("    ep %02x attr %02x mps %u\n", p[2], p[3], p[4] | p[5] << 8);
        }
        off += p[0];
    }
    if (picoboot && pb_intf >= 0) {
        uint8_t st[16] = { 0 };
        int r = u->control(d, 0xC1, 0x42, 0, (uint16_t)pb_intf, st, 16, 1000);
        papp_log("  picoboot status on intf %d: %d bytes, token %lu status %u cmd %02x busy %u\n", pb_intf, r,
                 (unsigned long)(st[0] | st[1] << 8 | st[2] << 16 | (uint32_t)st[3] << 24), st[4] | st[5] << 8, st[8], st[9]);
    }
    u->close(d);
}

static bool streq(const char *a, const char *b) { return strcmp(a, b) == 0; }

int main(int argc, char **argv)
{
    const papp_if_usbh_t *u = papp_usbh();
    if (!u) { papp_log("no USBH interface in this launcher\n"); return 2; }
    bool watch = false, picoboot = false;
    for (int a = 1; a < argc; a++) {
        if (streq(argv[a], "watch")) watch = true;
        if (streq(argv[a], "picoboot")) picoboot = true;
    }
    papp_usb_info_t devs[8];
    int n = u->list(devs, 8);
    papp_log("USBH v%u: %d device%s\n", (unsigned)u->hdr.version, n, n == 1 ? "" : "s");
    for (int i = 0; i < n; i++) probe(u, &devs[i], picoboot);
    uint32_t gen = u->generation();
    while (watch && !papp_exit_requested()) {
        papp_delay_ms(100);
        if (u->generation() == gen) continue;
        gen = u->generation();
        papp_delay_ms(300);                                               /* let it finish enumerating */
        n = u->list(devs, 8);
        papp_log("-- the bus changed: %d device%s\n", n, n == 1 ? "" : "s");
        for (int i = 0; i < n; i++) probe(u, &devs[i], picoboot);
    }
    return 0;
}
