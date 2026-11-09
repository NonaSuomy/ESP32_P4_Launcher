/*
 * ble_test — BLE interface test.
 *   args: [mode] ...
 *   periph [secs]          (default, 120 s) a peripheral named "gution-p4" with the services the
 *                          PC tools in ESP32P4_Gution4_7project/tools expect: a control service
 *                          (mode, level, led, label; plus tick, indications only) and a Nordic UART
 *                          console ("status", "help", "tick",
 *                          "mode N", "level N", "led N", "label text")
 *   scan [secs]            scans (5 s), lists what it heard; exit code 1 if nothing
 *   central [name] [secs]  finds a peripheral by name (gution-p4: another board running
 *                          periph), connects, discovers, reads, asks "status" over the UART
 *                          service; exit code = failed checks
 *   hang                   waits for an event that never comes: STOP must end it at once
 */
#include "papp/papp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PAPP_MANIFEST(.id = "test.ble", .name = "BLE test", .version = PAPP_APP_VERSION, .main_stack = 16384,
              .flags = PAPP_MF_HIDDEN);

static const papp_if_ble_t *ble;
static int s_fail;

#define CHECK(cond, ...) do { if (cond) papp_log("ok   " __VA_ARGS__); else { s_fail++; papp_log("FAIL " __VA_ARGS__); } } while (0)

static const char *addr_str(const papp_ble_addr_t *a)
{
    static char b[2][24];
    static int k;
    k ^= 1;
    snprintf(b[k], sizeof(b[k]), "%02x:%02x:%02x:%02x:%02x:%02x", a->a[5], a->a[4], a->a[3], a->a[2], a->a[1],
             a->a[0]);
    return b[k];
}

static const char *uuid_str(const papp_ble_uuid_t *u)
{
    static char b[2][40];
    static int k;
    k ^= 1;
    if (u->len == 2) snprintf(b[k], sizeof(b[k]), "%04x", u->u[0] | u->u[1] << 8);
    else if (u->len == 4) snprintf(b[k], sizeof(b[k]), "%08lx", (unsigned long)(u->u[0] | u->u[1] << 8 | u->u[2] << 16 | (uint32_t)u->u[3] << 24));
    else {
        char *p = b[k];
        for (int i = 15; i >= 0; i--) {
            p += sprintf(p, "%02x", u->u[i]);
            if (i == 12 || i == 10 || i == 8 || i == 6) *p++ = '-';
        }
        *p = 0;
    }
    return b[k];
}

static papp_ble_uuid_t U(const char *s)
{
    papp_ble_uuid_t u;
    papp_ble_uuid_parse(s, &u);
    return u;
}

static bool uuid_eq(const papp_ble_uuid_t *a, const papp_ble_uuid_t *b)
{
    return a->len == b->len && !memcmp(a->u, b->u, a->len);
}

#define CTRL_SVC   "4f3a0001-9d2b-4c7e-a1f5-8b6d3e0c7a21"
#define CTRL_MODE  "4f3a0002-9d2b-4c7e-a1f5-8b6d3e0c7a21"
#define CTRL_LEVEL "4f3a0003-9d2b-4c7e-a1f5-8b6d3e0c7a21"
#define CTRL_LED   "4f3a0004-9d2b-4c7e-a1f5-8b6d3e0c7a21"
#define CTRL_LABEL "4f3a0005-9d2b-4c7e-a1f5-8b6d3e0c7a21"
#define CTRL_TICK  "4f3a0006-9d2b-4c7e-a1f5-8b6d3e0c7a21"    /* ours: indications only */
#define NUS_SVC    "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX     "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_TX     "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

/* ── Peripheral ─────────────────────────────────────────────────────────── */

static uint16_t h_mode, h_level, h_led, h_label, h_tick, h_rx, h_tx;
static uint32_t s_tick;
static uint8_t  s_mode = 1, s_level = 5, s_led;
static char     s_label[33] = "hello from a PAPP";
static uint16_t s_mtu[8];
static int      s_conns, s_writes, s_cmds;

static void uart_send(papp_ble_conn_t c, const char *text)
{
    int chunk = (c < 8 && s_mtu[c] > 23 ? s_mtu[c] : 23) - 3;
    int n = (int)strlen(text);
    for (int i = 0; i < n; i += chunk) {
        int k = n - i < chunk ? n - i : chunk;
        int r = ble->notify(c, h_tx, text + i, k);
        if (r) {
            papp_log("TX notify -> %d\n", r);
            break;
        }
    }
}

static void command(papp_ble_conn_t c, char *line)
{
    char out[160];
    s_cmds++;
    papp_log("command %s\n", line);
    if (!strcmp(line, "status")) {
        static const char *const modes[] = { "Idle", "Run", "Test" };
        snprintf(out, sizeof(out), "mode=%s level=%u led=%s label=\"%s\" uptime=%llds conns=%d\r\n",
                 modes[s_mode % 3], s_level, s_led ? "on" : "off", s_label, papp_time_us() / 1000000, s_conns);
    } else if (!strcmp(line, "help")) {
        snprintf(out, sizeof(out), "status | mode 0-2 | level 0-255 | led 0/1 | label text | tick\r\n");
    } else if (!strncmp(line, "mode ", 5)) {
        s_mode = (uint8_t)(atoi(line + 5) % 3);
        ble->notify(PAPP_BLE_CONN_ALL, h_mode, &s_mode, 1);
        snprintf(out, sizeof(out), "ok mode=%u\r\n", s_mode);
    } else if (!strncmp(line, "level ", 6)) {
        s_level = (uint8_t)atoi(line + 6);
        ble->notify(PAPP_BLE_CONN_ALL, h_level, &s_level, 1);
        snprintf(out, sizeof(out), "ok level=%u\r\n", s_level);
    } else if (!strncmp(line, "led ", 4)) {
        s_led = (uint8_t)(atoi(line + 4) != 0);
        ble->notify(PAPP_BLE_CONN_ALL, h_led, &s_led, 1);
        snprintf(out, sizeof(out), "ok led=%u\r\n", s_led);
    } else if (!strncmp(line, "label ", 6)) {
        snprintf(s_label, sizeof(s_label), "%s", line + 6);
        ble->notify(PAPP_BLE_CONN_ALL, h_label, s_label, strlen(s_label));
        snprintf(out, sizeof(out), "ok label=\"%s\"\r\n", s_label);
    } else if (!strcmp(line, "tick")) {
        s_tick++;
        int r = ble->notify(PAPP_BLE_CONN_ALL, h_tick, &s_tick, 4);
        snprintf(out, sizeof(out), "ok tick=%lu (indicate %d)\r\n", (unsigned long)s_tick, r);
    } else {
        snprintf(out, sizeof(out), "unknown: %s (try help)\r\n", line);
    }
    uart_send(c, out);
}

static int periph(int secs)
{
    static papp_ble_chr_t ctrl[5], nus[2];
    ctrl[0] = (papp_ble_chr_t){ U(CTRL_MODE),  PAPP_BLE_READ | PAPP_BLE_WRITE | PAPP_BLE_NOTIFY, 1, &h_mode };
    ctrl[1] = (papp_ble_chr_t){ U(CTRL_LEVEL), PAPP_BLE_READ | PAPP_BLE_WRITE | PAPP_BLE_NOTIFY, 1, &h_level };
    ctrl[2] = (papp_ble_chr_t){ U(CTRL_LED),   PAPP_BLE_READ | PAPP_BLE_WRITE | PAPP_BLE_NOTIFY, 1, &h_led };
    ctrl[3] = (papp_ble_chr_t){ U(CTRL_LABEL), PAPP_BLE_READ | PAPP_BLE_NOTIFY, 32, &h_label };
    ctrl[4] = (papp_ble_chr_t){ U(CTRL_TICK),  PAPP_BLE_READ | PAPP_BLE_INDICATE, 4, &h_tick };
    nus[0] = (papp_ble_chr_t){ U(NUS_RX), PAPP_BLE_WRITE | PAPP_BLE_WRITE_NR, 244, &h_rx };
    nus[1] = (papp_ble_chr_t){ U(NUS_TX), PAPP_BLE_NOTIFY, 244, &h_tx };
    papp_ble_svc_t svcs[2] = { { U(CTRL_SVC), ctrl, 5 }, { U(NUS_SVC), nus, 2 } };

    ble->set_name("gution-p4");
    int64_t t = papp_time_us();
    int r = ble->gatt_register(svcs, 2);
    papp_log("gatt_register -> %d (%lld ms): handles mode %u level %u led %u label %u rx %u tx %u\n", r,
             (papp_time_us() - t) / 1000, h_mode, h_level, h_led, h_label, h_rx, h_tx);
    if (r) return 1;
    ble->set_value(h_mode, &s_mode, 1);
    ble->set_value(h_level, &s_level, 1);
    ble->set_value(h_led, &s_led, 1);
    ble->set_value(h_label, s_label, strlen(s_label));
    papp_ble_addr_t me;
    if (!ble->own_addr(&me)) papp_log("own address %s (type %d)\n", addr_str(&me), me.type);

    uint8_t adv[31], rsp[31];
    int an = 0, rn = 0;
    papp_ble_ad_flags(adv, &an);
    papp_ble_ad_name(adv, &an, "gution-p4");
    papp_ble_uuid_t nu = U(NUS_SVC);
    papp_ble_ad_uuid(rsp, &rn, &nu);
    papp_ble_adv_cfg_t ac = { .adv = adv, .adv_len = (uint8_t)an, .rsp = rsp, .rsp_len = (uint8_t)rn,
                              .connectable = 1 };
    r = ble->adv_start(&ac);
    papp_log("advertising as gution-p4 -> %d, for %d s\n", r, secs);
    if (r) return 1;

    int64_t end = papp_time_us() + (int64_t)secs * 1000000;
    char line[256];
    while (!papp_exit_requested() && papp_time_us() < end) {
        papp_ble_event_t e;
        if (!ble->wait_event(&e, 500)) continue;
        switch (e.type) {
        case PAPP_BLE_EV_CONNECTED:
            if (e.code) { papp_log("connect failed %d\n", e.code); break; }
            s_conns++;
            if (e.conn < 8) s_mtu[e.conn] = 23;
            papp_log("connected %u from %s (%d)\n", e.conn, addr_str(&e.addr), s_conns);
            if (s_conns < PAPP_BLE_MAX_CONN) ble->adv_start(&ac);     /* stay visible for more */
            break;
        case PAPP_BLE_EV_DISCONNECTED:
            s_conns--;
            papp_log("disconnected %u reason 0x%x\n", e.conn, e.code);
            ble->adv_start(&ac);
            break;
        case PAPP_BLE_EV_MTU:
            if (e.conn < 8) s_mtu[e.conn] = e.code;
            papp_log("MTU %u on %u\n", e.code, e.conn);
            break;
        case PAPP_BLE_EV_SUBSCRIBE:
            papp_log("subscribe handle %u mode %u (conn %u)\n", e.handle, e.code, e.conn);
            break;
        case PAPP_BLE_EV_ADV_DONE:
            break;
        case PAPP_BLE_EV_WRITE:
            s_writes++;
            if (e.handle == h_rx) {
                int n = e.len < sizeof(line) - 1 ? e.len : sizeof(line) - 1;
                memcpy(line, e.data, n);
                line[n] = 0;
                for (char *p = line, *q; *p; p = q) {      /* one command per line */
                    q = p + strcspn(p, "\r\n");
                    if (*q) *q++ = 0;
                    if (*p) command(e.conn, p);
                }
            } else {
                papp_log("write handle %u, %u bytes, first %u\n", e.handle, e.len, e.len ? e.data[0] : 0);
                if (e.handle == h_mode && e.len) s_mode = e.data[0];
                if (e.handle == h_level && e.len) s_level = e.data[0];
                if (e.handle == h_led && e.len) s_led = e.data[0];
            }
            break;
        default:
            papp_log("event %u\n", e.type);
        }
    }
    papp_log("periph done: %d writes, %d commands, %d connected now\n", s_writes, s_cmds, s_conns);
    return 0;
}

/* ── Central ────────────────────────────────────────────────────────────── */

static int scan(int secs)
{
    papp_ble_scan_cfg_t sc = { .active = 1, .filter_duplicates = 1, .duration_ms = (uint32_t)secs * 1000 };
    int r = ble->scan_start(&sc);
    papp_log("scan %d s -> %d\n", secs, r);
    if (r) return 1;
    int n = 0, named = 0;
    papp_ble_event_t e;
    while (!papp_exit_requested() && ble->wait_event(&e, (uint32_t)secs * 1000 + 2000)) {
        if (e.type == PAPP_BLE_EV_SCAN_DONE) break;
        if (e.type != PAPP_BLE_EV_ADV) continue;
        n++;
        int nl = 0;
        const uint8_t *nm = papp_ble_ad_find(e.data, e.len, 0x09, &nl);
        if (!nm) nm = papp_ble_ad_find(e.data, e.len, 0x08, &nl);
        char name[32] = "";
        if (nm) {
            snprintf(name, sizeof(name), "%.*s", nl, (const char *)nm);
            named++;
        }
        papp_log("  %s type %d rssi %d adv %u %s\n", addr_str(&e.addr), e.addr.type, e.rssi, e.code, name);
    }
    papp_log("scan: %d reports, %d with a name\n", n, named);
    return n ? 0 : 1;
}

static bool find(const char *want, int secs, papp_ble_addr_t *out)
{
    papp_ble_scan_cfg_t sc = { .active = 1, .filter_duplicates = 1, .duration_ms = (uint32_t)secs * 1000 };
    if (ble->scan_start(&sc)) return false;
    papp_ble_event_t e;
    bool found = false;
    while (!found && !papp_exit_requested() && ble->wait_event(&e, (uint32_t)secs * 1000 + 2000)) {
        if (e.type == PAPP_BLE_EV_SCAN_DONE) break;
        if (e.type != PAPP_BLE_EV_ADV) continue;
        int nl = 0;
        const uint8_t *nm = papp_ble_ad_find(e.data, e.len, 0x09, &nl);
        if (nm && nl == (int)strlen(want) && !memcmp(nm, want, nl)) {
            *out = e.addr;
            found = true;
        }
    }
    ble->scan_stop();
    return found;
}

static int central(const char *name, int secs)
{
    papp_ble_addr_t peer;
    int64_t t = papp_time_us();
    bool f = find(name, secs, &peer);
    CHECK(f, "found %s at %s (%lld ms)\n", name, f ? addr_str(&peer) : "-", (papp_time_us() - t) / 1000);
    if (!f) return s_fail;

    papp_ble_conn_t c;
    t = papp_time_us();
    int r = ble->connect(&peer, 8000, &c);
    CHECK(r == 0, "connect -> %d, conn %u (%lld ms)\n", r, c, (papp_time_us() - t) / 1000);
    if (r) return s_fail;
    r = ble->mtu_exchange(c, 5000);
    CHECK(r >= 23, "MTU -> %d\n", r);
    int mtu = r >= 23 ? r : 23;

    static papp_ble_found_svc_t svcs[12];
    static papp_ble_found_chr_t chrs[40];
    int ns = 0, nc = 0;
    t = papp_time_us();
    r = ble->discover(c, svcs, 12, &ns, chrs, 40, &nc, 15000);
    CHECK(r == 0 && ns >= 2, "discover -> %d: %d services, %d characteristics (%lld ms)\n", r, ns, nc,
          (papp_time_us() - t) / 1000);
    for (int i = 0; i < ns; i++) papp_log("     service %s %u..%u\n", uuid_str(&svcs[i].uuid), svcs[i].start, svcs[i].end);
    uint16_t rx = 0, tx = 0, tx_cccd = 0, label = 0, level = 0, tick = 0, tick_cccd = 0;
    papp_ble_uuid_t u_tick = U(CTRL_TICK);
    papp_ble_uuid_t u_rx = U(NUS_RX), u_tx = U(NUS_TX), u_label = U(CTRL_LABEL), u_level = U(CTRL_LEVEL);
    for (int i = 0; i < nc; i++) {
        papp_log("     chr %s handle %u cccd %u flags 0x%x svc %u\n", uuid_str(&chrs[i].uuid), chrs[i].handle,
                 chrs[i].cccd, chrs[i].flags, chrs[i].svc_index);
        if (uuid_eq(&chrs[i].uuid, &u_rx)) rx = chrs[i].handle;
        if (uuid_eq(&chrs[i].uuid, &u_tx)) { tx = chrs[i].handle; tx_cccd = chrs[i].cccd; }
        if (uuid_eq(&chrs[i].uuid, &u_label)) label = chrs[i].handle;
        if (uuid_eq(&chrs[i].uuid, &u_level)) level = chrs[i].handle;
        if (uuid_eq(&chrs[i].uuid, &u_tick)) { tick = chrs[i].handle; tick_cccd = chrs[i].cccd; }
    }
    CHECK(rx && tx && tx_cccd, "UART service: rx %u tx %u cccd %u\n", rx, tx, tx_cccd);

    char buf[64];
    if (label) {
        r = ble->read(c, label, buf, sizeof(buf) - 1, 5000);
        if (r >= 0) buf[r] = 0;
        CHECK(r > 0, "read label -> %d \"%s\"\n", r, r > 0 ? buf : "");
    }
    if (level) {
        uint8_t v = 42;
        r = ble->write(c, level, &v, 1, true, 5000);
        uint8_t back = 0;
        int r2 = ble->read(c, level, &back, 1, 5000);
        CHECK(r == 0 && r2 == 1 && back == 42, "write level 42 -> %d, read back %d -> %u\n", r, r2, back);
    }
    if (rx && tx_cccd) {
        r = ble->subscribe(c, tx_cccd, 1, 5000);
        CHECK(r == 0, "subscribe TX -> %d\n", r);
        r = ble->write(c, rx, "status\r\n", 8, false, 5000);
        CHECK(r == 0, "write \"status\" (no response) -> %d\n", r);
        char reply[256];
        int got = 0;
        int64_t end = papp_time_us() + 3000000;
        papp_ble_event_t e;
        while (papp_time_us() < end && !papp_exit_requested()) {
            if (!ble->wait_event(&e, 200)) {
                if (got && memchr(reply, '\n', got)) break;
                continue;
            }
            if (e.type == PAPP_BLE_EV_NOTIFY && e.handle == tx && got + e.len < (int)sizeof(reply)) {
                memcpy(reply + got, e.data, e.len);
                got += e.len;
            }
        }
        reply[got] = 0;
        CHECK(got > 0 && strstr(reply, "level=42"), "UART reply (%d bytes, MTU %d): %s", got, mtu, got ? reply : "\n");
    }
    if (rx && tick && tick_cccd) {                  /* indications: tick over the console */
        r = ble->subscribe(c, tick_cccd, 2, 5000);
        CHECK(r == 0, "subscribe tick (indicate) -> %d\n", r);
        int seen = 0, ind = 0;
        uint32_t last = 0;
        for (int k = 0; k < 3; k++) {
            ble->write(c, rx, "tick\r\n", 6, false, 5000);
            int64_t end = papp_time_us() + 2000000;
            papp_ble_event_t e;
            while (papp_time_us() < end && ble->wait_event(&e, 200)) {
                if (e.type == PAPP_BLE_EV_NOTIFY && e.handle == tick && e.len == 4) {
                    seen++;
                    ind += e.code == 1;
                    memcpy(&last, e.data, 4);
                    break;
                }
            }
        }
        CHECK(seen == 3 && ind == 3, "3 ticks -> %d received, %d as indications, last %lu\n", seen, ind,
              (unsigned long)last);
    }
    ble->disconnect(c);
    papp_ble_event_t e;
    bool gone = false;
    int64_t end = papp_time_us() + 3000000;
    while (!gone && papp_time_us() < end && ble->wait_event(&e, 500)) gone = e.type == PAPP_BLE_EV_DISCONNECTED;
    CHECK(gone, "disconnected (reason 0x%x)\n", gone ? e.code : 0);
    papp_log("central: %d failed\n", s_fail);
    return s_fail;
}

int main(int argc, char **argv)
{
    ble = papp_ble();
    if (!ble) {
        papp_log("no BLE interface on this launcher/board\n");
        return 2;
    }
    int64_t t = papp_time_us();
    bool ok = ble->available();
    papp_log("BLE available -> %d (%lld ms)\n", ok, (papp_time_us() - t) / 1000);
    if (!ok) return 3;

    const char *mode = argc > 1 ? argv[1] : "periph";
    if (!strcmp(mode, "scan")) return scan(argc > 2 ? atoi(argv[2]) : 5);
    if (!strcmp(mode, "central")) return central(argc > 2 ? argv[2] : "gution-p4", argc > 3 ? atoi(argv[3]) : 10);
    if (!strcmp(mode, "hang")) {
        papp_ble_event_t e;
        papp_log("waiting forever\n");
        ble->wait_event(&e, PAPP_WAIT_FOREVER);
        papp_log("wait ended (exit requested %d)\n", papp_exit_requested());
        return 0;
    }
    return periph(argc > 2 ? atoi(argv[2]) : 120);
}
