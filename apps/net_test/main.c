/*
 * net_test — NET interface test.
 *   args: [mode] [seconds]
 *   all (default)  Wi-Fi up, DNS, TCP, UDP (a DNS query by hand), TLS, HTTPS with redirects,
 *                  HTTP, scan; exit code = failed checks
 *   serve [secs]   TCP server on port 8080 (curl http://<board>:8080/): answers each request
 *   ap [secs]      SoftAP "P4-net-test" (password 12345678) beside the station, logs who joins
 *   hang           blocks in recv with no timeout: STOP must still end it at once
 *   join ssid pass joins another network for this run, then runs the checks
 *   fetch url [method] [body]  one HTTP(S) request: prints the status and the reply (body sent as JSON)
 */
#include "papp/papp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PAPP_MANIFEST(.id = "test.net", .name = "Net test", .version = PAPP_APP_VERSION, .main_stack = 16384,
              .flags = PAPP_MF_HIDDEN);

static const papp_if_net_t *net;
static int s_fail;

#define CHECK(cond, ...) do { if (cond) papp_log("ok   " __VA_ARGS__); else { s_fail++; papp_log("FAIL " __VA_ARGS__); } } while (0)

static const char *ip_str(const uint8_t *ip)
{
    static char b[4][16];
    static int k;
    k = (k + 1) & 3;
    snprintf(b[k], sizeof(b[k]), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return b[k];
}

static void show_info(void)
{
    papp_net_info_t i;
    net->info(&i);
    papp_log("state %d, ssid %s, rssi %d, channel %d, ip %s, gw %s, dns %s, mac %02x:%02x:%02x:%02x:%02x:%02x\n",
             i.state, i.ssid, i.rssi, i.channel, ip_str(i.ip), ip_str(i.gateway), ip_str(i.dns),
             i.mac[0], i.mac[1], i.mac[2], i.mac[3], i.mac[4], i.mac[5]);
    if (i.ap_running) papp_log("SoftAP %s, %d stations\n", ip_str(i.ap_ip), i.ap_clients);
}

static void drain_events(void)
{
    papp_net_event_t e;
    while (net->poll_event(&e)) papp_log("event %d code %d at %lu ms\n", e.type, e.code, (unsigned long)e.time_ms);
}

static void test_dns(void)
{
    papp_addr_t a;
    int64_t t = papp_time_us();
    int r = net->resolve("github.com", &a, 8000);
    CHECK(r == 0, "resolve github.com -> %s (%d, %lld ms)\n", r ? "-" : ip_str(a.ip), r, (papp_time_us() - t) / 1000);
    r = net->resolve("no-such-host.invalid", &a, 8000);
    CHECK(r == PAPP_E_DNS, "resolve a missing name -> %d\n", r);
}

static void test_tcp(void)
{
    int err;
    papp_sock_t *s = net->socket(PAPP_SOCK_TCP, &err);
    papp_addr_t a = { .family = PAPP_AF_INET, .port = 80, .ip = { 1, 1, 1, 1 } };
    int r = s ? net->connect(s, &a, 5000) : err;
    CHECK(r == 0, "tcp connect 1.1.1.1:80 (%d)\n", r);
    if (!r) {
        const char *q = "HEAD / HTTP/1.0\r\nHost: 1.1.1.1\r\n\r\n";
        r = net->send(s, q, strlen(q), 3000);
        char buf[128] = { 0 };
        int n = net->recv(s, buf, sizeof(buf) - 1, 5000);
        char *e = strchr(buf, '\r');
        if (e) *e = 0;
        CHECK(n > 0 && !strncmp(buf, "HTTP/1.", 7), "tcp reply \"%s\" (%d)\n", buf, n);
        papp_addr_t l;
        if (net->local_addr(s, &l) == 0) papp_log("     local %s:%u\n", ip_str(l.ip), l.port);
    }
    if (s) net->close(s);

    s = net->socket(PAPP_SOCK_TCP, &err);
    papp_addr_t closed = { .family = PAPP_AF_INET, .port = 81, .ip = { 1, 1, 1, 1 } };
    int64_t t = papp_time_us();
    r = s ? net->connect(s, &closed, 1500) : err;
    CHECK(r == PAPP_E_TIMEOUT || r == PAPP_E_REFUSED, "tcp to a closed port -> %d after %lld ms\n", r,
          (papp_time_us() - t) / 1000);
    if (s) net->close(s);
}

static void test_udp(void)
{
    /* DNS query for example.com, type A, sent by hand to 8.8.8.8 */
    static const uint8_t q[] = { 0x12, 0x34, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                                 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1 };
    int err;
    papp_sock_t *s = net->socket(PAPP_SOCK_UDP, &err);
    papp_addr_t a = { .family = PAPP_AF_INET, .port = 53, .ip = { 8, 8, 8, 8 } }, from;
    int r = s ? net->sendto(s, q, sizeof(q), &a) : err;
    uint8_t buf[512];
    int n = r > 0 ? net->recvfrom(s, buf, sizeof(buf), &from, 3000) : r;
    CHECK(n > 12 && buf[0] == 0x12 && buf[1] == 0x34, "udp dns query: %d bytes from %s:%u, %d answers\n", n,
          n > 0 ? ip_str(from.ip) : "-", n > 0 ? from.port : 0, n > 12 ? (buf[6] << 8 | buf[7]) : 0);
    if (s) {
        papp_poll_t p = { .sock = s, .want = PAPP_POLL_IN };
        int64_t t = papp_time_us();
        r = net->poll(&p, 1, 300);
        CHECK(r == 0 && (papp_time_us() - t) >= 250000, "poll with nothing to read -> %d after %lld ms\n", r,
              (papp_time_us() - t) / 1000);
        net->close(s);
    }
}

static void test_tls(void)
{
    int err;
    int64_t t = papp_time_us();
    size_t before = papp_mem_free_size(PAPP_MEM_FAST);
    papp_sock_t *s = net->tls_connect("github.com", 443, NULL, &err);
    CHECK(s != NULL, "tls github.com (%d, %lld ms, internal RAM -%u bytes)\n", s ? 0 : err, (papp_time_us() - t) / 1000,
          (unsigned)(before - papp_mem_free_size(PAPP_MEM_FAST)));
    if (s) {
        const char *q = "HEAD / HTTP/1.1\r\nHost: github.com\r\nConnection: close\r\n\r\n";
        int r = net->send(s, q, strlen(q), 5000);
        char buf[256] = { 0 };
        int n = net->recv(s, buf, sizeof(buf) - 1, 5000);
        char *e = strchr(buf, '\r');
        if (e) *e = 0;
        CHECK(r > 0 && n > 0 && !strncmp(buf, "HTTP/1.1", 8), "tls reply \"%s\"\n", buf);
        net->close(s);
    }
    papp_tls_cfg_t bad = { .ca_pem = "-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n" };
    s = net->tls_connect("github.com", 443, &bad, &err);
    CHECK(!s, "tls with a bad CA is refused (%d)\n", err);
    if (s) net->close(s);
}

static void test_http(const char *url, const char *want)
{
    int status;
    int64_t t = papp_time_us();
    papp_http_req_t req = { .url = url };
    papp_http_t *h = net->http_open(&req, &status);
    if (!h) {
        CHECK(0, "http %s -> %d\n", url, status);
        return;
    }
    char te[32] = "", ct[64] = "";
    net->http_header(h, "Transfer-Encoding", te, sizeof(te));
    net->http_header(h, "Content-Type", ct, sizeof(ct));
    long len = net->http_length(h);
    static char body[8192];
    int got = 0, n;
    while ((n = net->http_read(h, body + got, sizeof(body) - 1 - got)) > 0) got += n;
    body[got] = 0;
    net->http_close(h);
    CHECK(status == 200 && n == 0 && (len < 0 || len == got) && (!want || strstr(body, want)),
          "http %s -> %d, %d bytes (length %ld%s%s, %s), %lld ms\n", url, status, got, len, te[0] ? ", " : "", te, ct,
          (papp_time_us() - t) / 1000);
    if (want && strstr(body, want)) {
        char *v = strstr(body, want);
        char line[64];
        snprintf(line, sizeof(line), "%.60s", v);
        char *e = strchr(line, '\n');
        if (e) *e = 0;
        papp_log("     %s\n", line);
    }
}

static void test_post(void)
{
    int status;
    const char *body = "{\"board\":\"p4\",\"n\":42}";
    papp_http_req_t req = { .method = "POST", .url = "https://httpbin.org/post", .body = body, .body_len = strlen(body),
                            .headers = "Content-Type: application/json\r\n" };
    papp_http_t *h = net->http_open(&req, &status);
    static char buf[4096];
    int got = 0, n = 0;
    if (h) {
        while ((n = net->http_read(h, buf + got, sizeof(buf) - 1 - got)) > 0) got += n;
        net->http_close(h);
    }
    buf[got] = 0;
    CHECK(h && status == 200 && strstr(buf, "\"n\": 42"), "http POST echoed back (%d, %d bytes)\n", status, got);
}

/* Server and client in one: connect to our own address (the backlog takes the connection),
   then accept, answer and read the answer */
static void test_server_self(void)
{
    int err;
    papp_net_info_t i;
    net->info(&i);
    papp_sock_t *l = net->socket(PAPP_SOCK_TCP, &err);
    net->set_opt(l, PAPP_SO_REUSEADDR, 1);
    int r = net->bind(l, 8081);
    if (!r) r = net->listen(l, 2);
    papp_sock_t *c = net->socket(PAPP_SOCK_TCP, &err);
    papp_addr_t me = { .family = PAPP_AF_INET, .port = 8081 };
    memcpy(me.ip, i.ip, 4);
    if (!r) r = net->connect(c, &me, 3000);
    if (!r) r = net->send(c, "ping", 4, 1000) == 4 ? 0 : -99;
    papp_addr_t from;
    papp_sock_t *a = r ? NULL : net->accept(l, &from, 3000, &err);
    char b[8] = { 0 };
    if (a && net->recv(a, b, 4, 2000) == 4 && !strcmp(b, "ping")) net->send(a, "pong", 4, 1000);
    char b2[8] = { 0 };
    int n = a ? net->recv(c, b2, 4, 2000) : -1;
    CHECK(a && n == 4 && !strcmp(b2, "pong"), "tcp server: accepted %s:%u, ping -> \"%s\" (%d)\n",
          a ? ip_str(from.ip) : "-", a ? from.port : 0, b2, r ? r : err);
    if (a) net->close(a);
    net->close(c);
    net->close(l);
}

static void test_scan(void)
{
    papp_net_ap_t aps[12];
    int n = net->scan(aps, 12, 10000);
    CHECK(n > 0, "scan: %d networks\n", n);
    for (int i = 0; i < n && i < 6; i++)
        papp_log("     %-24s ch %2d rssi %d%s\n", aps[i].ssid, aps[i].channel, aps[i].rssi, aps[i].secure ? "" : " open");
}

static int serve(int secs)
{
    int err;
    papp_sock_t *l = net->socket(PAPP_SOCK_TCP, &err);
    net->set_opt(l, PAPP_SO_REUSEADDR, 1);
    int r = l ? net->bind(l, 8080) : err;
    if (!r) r = net->listen(l, 4);
    papp_net_info_t i;
    net->info(&i);
    papp_log("listening on %s:8080 (%d) for %d s\n", ip_str(i.ip), r, secs);
    int served = 0;
    papp_sock_t *u = net->socket(PAPP_SOCK_UDP, &err);       /* UDP 9999: logs what arrives, answers */
    net->set_opt(u, PAPP_SO_BROADCAST, 1);
    net->bind(u, 9999);
    int64_t end = papp_time_us() + (int64_t)secs * 1000000;
    while (!r && !papp_exit_requested() && papp_time_us() < end) {
        papp_addr_t from;
        char d[64];
        int dn;
        while ((dn = net->recvfrom(u, d, sizeof(d) - 1, &from, 0)) > 0) {
            d[dn] = 0;
            papp_log("udp from %s:%u \"%s\"\n", ip_str(from.ip), from.port, d);
            net->sendto(u, "unicast-reply", 13, &from);
            papp_addr_t all = { .family = PAPP_AF_INET, .port = from.port, .ip = { 255, 255, 255, 255 } };
            net->sendto(u, "broadcast-reply", 15, &all);
        }
        papp_sock_t *c = net->accept(l, &from, 500, &err);
        drain_events();
        if (!c) continue;
        char req[512];
        int n = net->recv(c, req, sizeof(req) - 1, 3000);
        if (n > 0) {
            req[n] = 0;
            char *e = strchr(req, '\r');
            if (e) *e = 0;
            char reply[256];
            int bl = snprintf(reply, sizeof(reply), "hello from a PAPP on %s, request %d: %s\n", papp_board_id(), served + 1, req);
            char head[128];
            int hl = snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", bl);
            net->send(c, head, hl, 2000);
            net->send(c, reply, bl, 2000);
            papp_log("served %s:%u \"%s\"\n", ip_str(from.ip), from.port, req);
            served++;
        }
        net->close(c);
    }
    return served;
}

static int softap(int secs)
{
    int r = net->ap_start("P4-net-test", "12345678", 0, 4);
    CHECK(r == 0, "SoftAP P4-net-test (%d)\n", r);
    show_info();
    if (!r) papp_log("served %d requests on the SoftAP\n", serve(secs));
    drain_events();
    show_info();
    net->ap_stop();
    return s_fail;
}

static int hang(void)
{
    int err;
    papp_sock_t *s = net->socket(PAPP_SOCK_UDP, &err);
    net->bind(s, 40000);
    papp_log("blocking in recv with no timeout; stop the app now\n");
    char b[16];
    int r = net->recvfrom(s, b, sizeof(b), NULL, PAPP_WAIT_FOREVER);
    papp_log("recv returned %d\n", r);
    return r == PAPP_E_EXIT ? 0 : 1;
}

/* fetch url [method] [body]: one request, prints the status and the reply (exit 0 on 2xx) */
static int fetch(const char *url, const char *method, const char *body)
{
    papp_http_req_t req = { .method = method, .url = url, .body = body, .body_len = body ? strlen(body) : 0,
                            .headers = body ? "Content-Type: application/json\r\n" : NULL };
    int status;
    int64_t t = papp_time_us();
    papp_http_t *h = net->http_open(&req, &status);
    papp_log("%s %s -> %d (%lld ms)\n", method ? method : "GET", url, status, (papp_time_us() - t) / 1000);
    if (!h) return 1;
    char buf[257];
    long total = 0;
    int n;
    while ((n = net->http_read(h, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        if (total < 600) papp_log("%s", buf);
        total += n;
    }
    papp_log("\n%ld bytes\n", total);
    net->http_close(h);
    return status >= 200 && status < 300 ? 0 : 2;
}

int main(int argc, char **argv)
{
    net = papp_net();
    if (!net) {
        papp_log("no NET interface (launcher too old)\n");
        return 100;
    }
    const char *mode = argc > 1 ? argv[1] : "all";
    if (!strcmp(mode, "join") && argc > 3) {
        int64_t t = papp_time_us();
        int r = net->join(argv[2], argv[3], 20000);
        CHECK(r == 0, "join %s (%d, %lld ms)\n", argv[2], r, (papp_time_us() - t) / 1000);
        if (argc > 4) {                     /* join ssid pass url: fetch it on that network */
            show_info();
            papp_net_info_t i;
            net->info(&i);
            int err;
            papp_sock_t *u = net->socket(PAPP_SOCK_UDP, &err);
            net->set_opt(u, PAPP_SO_BROADCAST, 1);
            net->bind(u, 9998);
            papp_addr_t gw = { .family = PAPP_AF_INET, .port = 9999 }, all = { .family = PAPP_AF_INET, .port = 9999, .ip = { 255, 255, 255, 255 } };
            memcpy(gw.ip, i.gateway, 4);
            for (int k = 0; k < 3; k++) {
                net->sendto(u, "broadcast", 9, &all);
                net->sendto(u, "unicast", 7, &gw);
                papp_delay_ms(300);
            }
            char d[64];
            papp_addr_t from;
            int dn;
            while ((dn = net->recvfrom(u, d, sizeof(d) - 1, &from, 1500)) > 0) {
                d[dn] = 0;
                papp_log("udp from %s:%u \"%s\"\n", ip_str(from.ip), from.port, d);
            }
            net->close(u);
            test_http(argv[4], "hello");
            return s_fail;
        }
    } else {
        int64_t t = papp_time_us();
        int r = net->up(20000);
        CHECK(r == 0, "up (%d, %lld ms)\n", r, (papp_time_us() - t) / 1000);
    }
    show_info();
    drain_events();
    int secs = argc > 2 ? atoi(argv[2]) : 60;
    if (!strcmp(mode, "serve")) return serve(secs);
    if (!strcmp(mode, "ap")) return softap(secs);
    if (!strcmp(mode, "hang")) return hang();
    if (!strcmp(mode, "fetch") && argc > 2) return fetch(argv[2], argc > 3 ? argv[3] : NULL, argc > 4 ? argv[4] : NULL);

    test_dns();
    test_tcp();
    test_udp();
    test_tls();
    test_http("https://github.com/giltal/ESP32_P4_Launcher/releases/latest/download/latest.json", "\"version\"");
    test_http("http://example.com/", "<title>");
    test_http("https://api.github.com/repos/giltal/ESP32_P4_Launcher", "\"full_name\"");
    test_http("https://httpbin.org/stream/5", "\"id\":4");          /* chunked */
    test_post();
    test_server_self();
    test_scan();
    drain_events();
    papp_log("%d failed\n", s_fail);
    return s_fail;
}
