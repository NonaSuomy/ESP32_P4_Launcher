/*
 * Speedtest: a native PAPP2, single-stream network test against Ookla servers.
 *
 * Server discovery and the test endpoints follow the public Speedtest.net
 * client protocol: /api/js/servers, /speedtest/latency.txt,
 * /speedtest/randomNxN.jpg and /speedtest/upload.php. This is a compact
 * diagnostic for the P4, not Ookla's licensed CLI/SDK and not a multi-stream
 * replacement for the Speedtest.net browser test.
 */
#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "papp/papp.h"
#include "edu_ui.h"

PAPP_MANIFEST(.id = "tool.speedtest", .name = "Speedtest", .version = PAPP_APP_VERSION,
              .main_stack = 16384);

#define DISCOVERY_URL "https://www.speedtest.net/api/js/servers?engine=js&https_functional=true&limit=12"
#define SERVER_LIMIT 12
#define RESPONSE_MAX 32768
#define DOWNLOAD_URL_MAX 256
#define UPLOAD_BYTES (512u * 1024u)
#define DOWNLOAD_SECONDS 7
#define SPEED_SAMPLES 64

enum { ACT_START = 1, ACT_CANCEL, ACT_CHANGE, ACT_TOGGLE_SERVER };
enum { ST_IDLE, ST_DISCOVER, ST_LATENCY, ST_DOWNLOAD, ST_UPLOAD, ST_DONE, ST_ERROR, ST_CANCELLED };

typedef struct {
    char url[192];
    char name[48];
    char sponsor[64];
    char id[20];
    double distance;
} server_t;

typedef struct {
    int state;
    int progress;                 /* 0..1000 */
    double ping_ms;
    double download_mbps;
    double upload_mbps;
    char server[96];
    char status[96];
} result_t;

static volatile int s_state = ST_IDLE;
static volatile int s_progress;
static volatile int s_cancel;
static volatile double s_ping_ms;
static volatile double s_download_mbps;
static volatile double s_upload_mbps;
static volatile double s_live_mbps;
static volatile int s_download_error;
static volatile int s_upload_error;
static double s_download_history[SPEED_SAMPLES];
static double s_upload_history[SPEED_SAMPLES];
static unsigned s_download_samples, s_upload_samples;
static server_t s_servers[SERVER_LIMIT];
static server_t s_selected_server;
static int s_server_count, s_selected_index;
static bool s_selected_valid;
static bool s_show_server = true;
enum {
    FOCUS_START = 0,
    FOCUS_CHANGE_SERVER,
    FOCUS_SERVER_LABEL,
    FOCUS_CLOSE,
    FOCUS_COUNT,
};
static int s_focus_control = FOCUS_START;
static char s_server[96];
static char s_status[96] = "Ready to test";
static papp_thread_t *s_worker;
static papp_mutex_t *s_result_lock;
static papp_surface_t *s_surface;
static bool s_quit;
static uint32_t s_prev_pad_buttons;
static bool s_prev_enter;

static void graph_reset(void)
{
    if (s_result_lock) papp_mutex_lock(s_result_lock);
    s_download_samples = s_upload_samples = 0;
    if (s_result_lock) papp_mutex_unlock(s_result_lock);
}

static void graph_add(int state, double mbps)
{
    if (s_result_lock) papp_mutex_lock(s_result_lock);
    double *values = state == ST_DOWNLOAD ? s_download_history : s_upload_history;
    unsigned *count = state == ST_DOWNLOAD ? &s_download_samples : &s_upload_samples;
    if (*count == SPEED_SAMPLES) {
        memmove(values, values + 1, (SPEED_SAMPLES - 1) * sizeof(values[0]));
        (*count)--;
    }
    values[(*count)++] = mbps;
    if (s_result_lock) papp_mutex_unlock(s_result_lock);
}

static void set_status(int state, int progress, const char *status)
{
    if (s_result_lock) papp_mutex_lock(s_result_lock);
    s_state = state;
    s_progress = progress;
    snprintf(s_status, sizeof(s_status), "%s", status);
    if (s_result_lock) papp_mutex_unlock(s_result_lock);
}

static bool cancelled(void)
{
    return s_cancel || papp_exit_requested();
}

static uint32_t nonce(void)
{
    uint32_t v = 0;
    if (!papp_random(&v, sizeof(v))) v = (uint32_t)papp_time_us();
    return v;
}

static void short_server_label(const server_t *server, char *out, size_t cap)
{
    char sponsor[64];
    snprintf(sponsor, sizeof(sponsor), "%s", server->sponsor[0] ? server->sponsor : "Ookla");
    char *space = strpbrk(sponsor, " \t");
    if (space) *space = 0;
    snprintf(out, cap, "%.24s / %.24s", sponsor,
             server->name[0] ? server->name : server->id);
}

/* Decode the small, flat JSON string fields returned by Ookla's server API. */
static bool json_string(const char *begin, const char *end, const char *key, char *out, size_t cap)
{
    char pattern[40];
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern)) return false;
    const char *p = begin;
    while (p < end) {
        p = strstr(p, pattern);
        if (!p || p >= end) return false;
        p += strlen(pattern);
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p++ != ':') continue;
        while (p < end && isspace((unsigned char)*p)) p++;
        if (p >= end || *p++ != '"') return false;
        size_t n = 0;
        while (p < end && *p != '"') {
            char c = *p++;
            if (c == '\\' && p < end) c = *p++;
            if (n + 1 < cap) out[n++] = c;
        }
        out[n] = 0;
        return p < end && *p == '"';
    }
    return false;
}

static double json_number(const char *begin, const char *end, const char *key, double fallback)
{
    char pattern[40];
    if (snprintf(pattern, sizeof(pattern), "\"%s\"", key) >= (int)sizeof(pattern)) return fallback;
    const char *p = strstr(begin, pattern);
    if (!p || p >= end) return fallback;
    p += strlen(pattern);
    while (p < end && (isspace((unsigned char)*p) || *p == ':')) p++;
    char *q = NULL;
    double d = strtod(p, &q);
    return q != p && q <= end ? d : fallback;
}

static int socket_send_all(const papp_if_net_t *net, papp_sock_t *sock, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len) {
        int n = net->send(sock, p, len, 10000);
        if (n <= 0) return n ? n : PAPP_E_RESET;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return PAPP_E_OK;
}

static int discover_servers(const papp_if_net_t *net, server_t *out, int max)
{
    int status = 0, len = 0;
    papp_http_req_t req = { .url = DISCOVERY_URL, .timeout_ms = 12000 };
    papp_http_t *h = net->http_open(&req, &status);
    if (!h || status != 200) {
        if (h) net->http_close(h);
        return -1;
    }
    char *body = malloc(RESPONSE_MAX);
    if (!body) { net->http_close(h); return -1; }
    while (len < RESPONSE_MAX - 1 && !cancelled()) {
        int n = net->http_read(h, body + len, RESPONSE_MAX - 1 - len);
        if (n <= 0) break;
        len += n;
    }
    net->http_close(h);
    body[len] = 0;

    int count = 0;
    const char *p = body;
    while (count < max && (p = strchr(p, '{')) != NULL) {
        const char *end = strchr(p, '}');
        if (!end) break;
        server_t *s = &out[count];
        memset(s, 0, sizeof(*s));
        bool ok = json_string(p, end, "url", s->url, sizeof(s->url)) &&
                  json_string(p, end, "id", s->id, sizeof(s->id));
        if (ok) {
            json_string(p, end, "name", s->name, sizeof(s->name));
            json_string(p, end, "sponsor", s->sponsor, sizeof(s->sponsor));
            s->distance = json_number(p, end, "d", 1e9);
            count++;
        }
        p = end + 1;
    }
    free(body);
    return count;
}

static bool server_url(const server_t *s, const char *leaf, char *out, size_t cap)
{
    size_t n = strlen(s->url);
    const char *suffix = strstr(s->url, "upload.php");
    if (!suffix) return false;
    size_t base = (size_t)(suffix - s->url);
    int wrote = snprintf(out, cap, "%.*s%s", (int)base, s->url, leaf);
    return wrote > 0 && (size_t)wrote < cap && base < n;
}

static double latency_probe(const papp_if_net_t *net, const server_t *s)
{
    char url[DOWNLOAD_URL_MAX];
    if (!server_url(s, "latency.txt", url, sizeof(url))) return -1;
    size_t n = strlen(url);
    snprintf(url + n, sizeof(url) - n, "?x=%08lx", (unsigned long)nonce());
    papp_http_req_t req = { .url = url, .headers = "Cache-Control: no-cache\r\n",
                            .timeout_ms = 4000, .max_redirects = 2 };
    int status = 0;
    int64_t start = papp_time_us();
    papp_http_t *h = net->http_open(&req, &status);
    if (!h) return -1;
    char tiny[16];
    int got = net->http_read(h, tiny, sizeof(tiny));
    net->http_close(h);
    if (status != 200 || got <= 0) return -1;
    return (papp_time_us() - start) / 1000.0;
}

static double select_server(const papp_if_net_t *net, server_t *selected)
{
    server_t servers[SERVER_LIMIT];
    int count = discover_servers(net, servers, SERVER_LIMIT);
    if (count <= 0) return -1;
    if (s_result_lock) papp_mutex_lock(s_result_lock);
    memcpy(s_servers, servers, (size_t)count * sizeof(servers[0]));
    s_server_count = count;
    if (s_result_lock) papp_mutex_unlock(s_result_lock);
    double best = 1e9;
    int best_i = -1;
    /* The endpoint returns nearby candidates first; probe a small set to choose by RTT. */
    for (int i = 0; i < count && i < 5 && !cancelled(); i++) {
        char label[96];
        snprintf(label, sizeof(label), "Finding a nearby server (%d/%d)", i + 1, count < 5 ? count : 5);
        set_status(ST_LATENCY, (i * 1000) / (count < 5 ? count : 5), label);
        double ms = latency_probe(net, &servers[i]);
        if (ms >= 0 && ms < best) { best = ms; best_i = i; }
    }
    if (best_i < 0) return -1;
    *selected = servers[best_i];
    if (s_result_lock) papp_mutex_lock(s_result_lock);
    s_selected_server = *selected;
    s_selected_index = best_i;
    s_selected_valid = true;
    short_server_label(selected, s_server, sizeof(s_server));
    if (s_result_lock) papp_mutex_unlock(s_result_lock);
    return best;
}

static double run_download(const papp_if_net_t *net, const server_t *server)
{
    char url[DOWNLOAD_URL_MAX];
    /* Ookla servers expose a fixed image-size ladder; arbitrary dimensions 404. */
    if (!server_url(server, "random2000x2000.jpg", url, sizeof(url))) return -1;
    size_t n = strlen(url);
    snprintf(url + n, sizeof(url) - n, "?x=%08lx", (unsigned long)nonce());
    int64_t start = papp_time_us();
    uint64_t total = 0;
    uint64_t sample_bytes = 0;
    int64_t sample_start = start;
    int requests = 0;
    do {
        if (cancelled()) return -1;
        if (requests++) {
            size_t z = strlen(url);
            snprintf(url + z, sizeof(url) - z, "&r=%08lx", (unsigned long)nonce());
        }
        int status = 0;
        papp_http_req_t req = { .url = url, .headers = "Cache-Control: no-cache\r\n",
                                .timeout_ms = 12000, .max_redirects = 2 };
        papp_http_t *h = net->http_open(&req, &status);
        if (!h || status != 200) {
            s_download_error = status;
            papp_log("speedtest: download request failed: status=%d url=%s\n", status, url);
            if (h) net->http_close(h);
            return -1;
        }
        uint8_t buf[2048];
        int got = 0;
        while (!cancelled() && (got = net->http_read(h, buf, sizeof(buf))) > 0) {
            total += (uint32_t)got;
            int64_t elapsed = papp_time_us() - start;
            sample_bytes += (uint32_t)got;
            int64_t sample_elapsed = papp_time_us() - sample_start;
            if (sample_elapsed >= 200000) {
                double rate = (double)sample_bytes * 8.0 / ((double)sample_elapsed / 1000000.0) / 1000000.0;
                s_live_mbps = rate;
                graph_add(ST_DOWNLOAD, rate);
                sample_bytes = 0;
                sample_start = papp_time_us();
            }
            int progress = 100 + (int)(elapsed * 550 / (DOWNLOAD_SECONDS * 1000000LL));
            if (progress > 650) progress = 650;
            set_status(ST_DOWNLOAD, progress, "Measuring download (single stream)");
        }
        net->http_close(h);
        if (cancelled()) return -1;
        if (got < 0) return -1;
    } while (papp_time_us() - start < DOWNLOAD_SECONDS * 1000000LL && requests < 8);
    double seconds = (papp_time_us() - start) / 1000000.0;
    return seconds > 0 ? (double)total * 8.0 / seconds / 1000000.0 : -1;
}

static double run_upload(const papp_if_net_t *net, const server_t *server)
{
    char url[DOWNLOAD_URL_MAX], host[128], path[128];
    if (!server_url(server, "upload.php", url, sizeof(url))) return -1;
    /* Discovery explicitly requests HTTPS-capable servers. The HTTP helper
     * transparently upgraded downloads, but a raw socket did not follow the
     * server's HTTP redirect and timed out on port 80. Use TLS directly. */
    bool tls = true;
    /* Keep the source URL's actual scheme offset even though upload is
     * intentionally upgraded to TLS for HTTPS-capable discovered servers. */
    const char *authority = url + (strncmp(url, "https://", 8) == 0 ? 8 : 7);
    const char *slash = strchr(authority, '/');
    if (!slash || (size_t)(slash - authority) >= sizeof(host)) return -1;
    snprintf(host, sizeof(host), "%.*s", (int)(slash - authority), authority);
    snprintf(path, sizeof(path), "%s?x=%08lx", slash, (unsigned long)nonce());
    char *colon = strchr(host, ':');
    uint16_t port = tls ? 443 : 80;
    if (colon) { *colon++ = 0; int p = atoi(colon); if (p <= 0 || p > 65535) return -1; port = (uint16_t)p; }

    uint8_t *payload = papp_alloc(UPLOAD_BYTES, PAPP_MEM_LARGE, 64);
    if (!payload) return -1;
    static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char prefix[] = "content1=";
    memcpy(payload, prefix, sizeof(prefix) - 1);
    uint32_t seed = nonce() | 1u;
    for (size_t i = sizeof(prefix) - 1; i < UPLOAD_BYTES; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        payload[i] = (uint8_t)alphabet[seed % (sizeof(alphabet) - 1)];
    }
    int err = 0, transport_error = 0;
    papp_sock_t *sock;
    if (tls) sock = net->tls_connect(host, port, NULL, &err);
    else {
        papp_addr_t addr = { .family = PAPP_AF_INET, .port = port };
        err = net->resolve(host, &addr, 8000);
        sock = err ? NULL : net->socket(PAPP_SOCK_TCP, &err);
        if (sock) {
            int connect_result = net->connect(sock, &addr, 8000);
            if (connect_result != PAPP_E_OK) { err = connect_result; net->close(sock); sock = NULL; }
        }
    }
    int64_t start = papp_time_us();
    int status = 0;
    size_t bytes_sent = 0;
    if (sock) {
        char header[512];
        char host_header[160];
        if (port != 80 && port != 443) snprintf(host_header, sizeof(host_header), "%s:%u", host, port);
        else snprintf(host_header, sizeof(host_header), "%s", host);
        int hn = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: PAPP2-Speedtest/1.0\r\n"
            "Accept: */*\r\nConnection: close\r\n"
            "Content-Type: application/x-www-form-urlencoded\r\nCache-Control: no-cache\r\n"
            "Content-Length: %u\r\n\r\n", path, host_header, (unsigned)UPLOAD_BYTES);
        int send_result = hn > 0 && (size_t)hn < sizeof(header)
            ? socket_send_all(net, sock, header, (size_t)hn) : PAPP_E_ARG;
        bool sent = send_result == PAPP_E_OK;
        if (!sent) transport_error = send_result;
        size_t offset = 0;
        uint64_t sample_bytes = 0;
        int64_t sample_start = start;
        while (sent && offset < UPLOAD_BYTES && !cancelled()) {
            size_t chunk = UPLOAD_BYTES - offset;
            if (chunk > 4096) chunk = 4096;
            int n = net->send(sock, payload + offset, chunk, 10000);
            if (n <= 0) { sent = false; transport_error = n ? n : PAPP_E_RESET; break; }
            offset += (size_t)n;
            bytes_sent = offset;
            sample_bytes += (uint32_t)n;
            int64_t sample_elapsed = papp_time_us() - sample_start;
            if (sample_elapsed >= 150000) {
                double rate = (double)sample_bytes * 8.0 / ((double)sample_elapsed / 1000000.0) / 1000000.0;
                s_live_mbps = rate;
                graph_add(ST_UPLOAD, rate);
                sample_bytes = 0;
                sample_start = papp_time_us();
            }
            int progress = 700 + (int)(offset * 280 / UPLOAD_BYTES);
            set_status(ST_UPLOAD, progress, "Measuring live upload speed");
        }
        if ((offset == UPLOAD_BYTES || transport_error) && !cancelled()) {
            char response[256] = {0};
            size_t used = 0;
            while (used < sizeof(response) - 1 && !strstr(response, "\r\n")) {
                int n = net->recv(sock, response + used, sizeof(response) - 1 - used, 1500);
                if (n <= 0) { if (n < 0) transport_error = n; else transport_error = PAPP_E_RESET; break; }
                used += (size_t)n;
                response[used] = 0;
            }
            if (used && sscanf(response, "HTTP/%*s %d", &status) != 1) status = 0;
            if (used) papp_log("speedtest: upload server response: %.96s\n", response);
        }
        net->close(sock);
    }
    int64_t elapsed = papp_time_us() - start;
    papp_free(payload);
    if (!sock || status < 200 || status >= 300 || elapsed <= 0) {
        s_upload_error = status ? status : (transport_error ? transport_error : (err ? err : PAPP_E_HTTP));
        papp_log("speedtest: upload request failed: status=%d transport=%d connect=%d sent=%u/%u url=%s\n",
                 status, transport_error, err, (unsigned)bytes_sent, (unsigned)UPLOAD_BYTES, url);
        return -1;
    }
    return (double)UPLOAD_BYTES * 8.0 / ((double)elapsed / 1000000.0) / 1000000.0;
}

static void format_server_label(const server_t *server)
{
    if (s_result_lock) papp_mutex_lock(s_result_lock);
    short_server_label(server, s_server, sizeof(s_server));
    if (s_result_lock) papp_mutex_unlock(s_result_lock);
}

static int change_server_thread(void *arg)
{
    (void)arg;
    s_cancel = 0;
    const papp_if_net_t *net = papp_net();
    if (!net) { set_status(ST_ERROR, 0, "This launcher has no app networking"); return -1; }
    set_status(ST_DISCOVER, 0, "Finding available servers");
    if (net->up(15000) != PAPP_E_OK) { set_status(ST_ERROR, 0, "No Wi-Fi connection"); return -1; }

    if (s_server_count <= 0) {
        server_t found[SERVER_LIMIT];
        int count = discover_servers(net, found, SERVER_LIMIT);
        if (count <= 0) { set_status(ST_ERROR, 0, "Could not find Speedtest servers"); return -1; }
        if (s_result_lock) papp_mutex_lock(s_result_lock);
        memcpy(s_servers, found, (size_t)count * sizeof(found[0]));
        s_server_count = count;
        if (s_result_lock) papp_mutex_unlock(s_result_lock);
    }
    if (s_result_lock) papp_mutex_lock(s_result_lock);
    if (s_server_count <= 0) {
        if (s_result_lock) papp_mutex_unlock(s_result_lock);
        set_status(ST_ERROR, 0, "No servers available");
        return -1;
    }
    int next = s_selected_valid ? (s_selected_index + 1) % s_server_count : 0;
    s_selected_index = next;
    s_selected_server = s_servers[next];
    s_selected_valid = true;
    server_t selected = s_selected_server;
    if (s_result_lock) papp_mutex_unlock(s_result_lock);
    format_server_label(&selected);
    set_status(ST_DONE, 1000, "Server changed; ready to test");
    return 0;
}

static int test_thread(void *arg)
{
    (void)arg;
    s_cancel = 0;
    s_ping_ms = s_download_mbps = s_upload_mbps = 0;
    s_live_mbps = 0;
    s_download_error = 0;
    s_upload_error = 0;
    graph_reset();
    const papp_if_net_t *net = papp_net();
    if (!net) { set_status(ST_ERROR, 0, "This launcher has no app networking"); return -1; }
    set_status(ST_DISCOVER, 0, "Connecting to Wi-Fi");
    if (net->up(15000) != PAPP_E_OK) { set_status(ST_ERROR, 0, "No Wi-Fi connection"); return -1; }
    if (cancelled()) { set_status(ST_CANCELLED, 0, "Cancelled"); return 0; }

    set_status(ST_DISCOVER, 0, "Discovering nearby Ookla servers");
    server_t server;
    bool selected_valid;
    if (s_result_lock) papp_mutex_lock(s_result_lock);
    selected_valid = s_selected_valid;
    if (selected_valid) server = s_selected_server;
    if (s_result_lock) papp_mutex_unlock(s_result_lock);
    double ping;
    if (selected_valid) {
        set_status(ST_LATENCY, 0, "Checking selected server");
        ping = latency_probe(net, &server);
        format_server_label(&server);
    } else {
        ping = select_server(net, &server);
    }
    if (cancelled()) { set_status(ST_CANCELLED, 0, "Cancelled"); return 0; }
    if (ping < 0) { set_status(ST_ERROR, 0, "Could not find a Speedtest server"); return -1; }
    s_ping_ms = ping;

    set_status(ST_DOWNLOAD, 100, "Measuring download (single stream)");
    s_live_mbps = 0;
    double down = run_download(net, &server);
    if (cancelled()) { set_status(ST_CANCELLED, 0, "Cancelled"); return 0; }
    if (down < 0) {
        char msg[96];
        if (s_download_error > 0) snprintf(msg, sizeof(msg), "Download failed (HTTP %d)", s_download_error);
        else snprintf(msg, sizeof(msg), "Download connection failed (%d)", s_download_error);
        set_status(ST_ERROR, 0, msg);
        return -1;
    }
    s_download_mbps = down;

    set_status(ST_UPLOAD, 700, "Measuring upload (single stream)");
    s_live_mbps = 0;
    double up = run_upload(net, &server);
    if (cancelled()) { set_status(ST_CANCELLED, 0, "Cancelled"); return 0; }
    if (up < 0) {
        char msg[96];
        if (s_upload_error > 0) snprintf(msg, sizeof(msg), "Upload failed (HTTP %d)", s_upload_error);
        else if (s_upload_error == PAPP_E_TIMEOUT) snprintf(msg, sizeof(msg), "Upload timed out connecting to server");
        else if (s_upload_error == PAPP_E_TLS || s_upload_error == PAPP_E_CERT) snprintf(msg, sizeof(msg), "Upload TLS connection failed");
        else if (s_upload_error == PAPP_E_DNS) snprintf(msg, sizeof(msg), "Upload DNS lookup failed");
        else if (s_upload_error == PAPP_E_REFUSED) snprintf(msg, sizeof(msg), "Upload server refused connection");
        else snprintf(msg, sizeof(msg), "Upload network error (%d)", s_upload_error);
        set_status(ST_ERROR, 0, msg);
        return -1;
    }
    s_upload_mbps = up;
    set_status(ST_DONE, 1000, "Test complete - single-stream result");
    papp_log("speedtest: server=%s ping=%.2fms down=%.3fMbps up=%.3fMbps\n",
             s_server, s_ping_ms, s_download_mbps, s_upload_mbps);
    return 0;
}

static void act(int id, int x)
{
    (void)x;
    if (id == UI_ID_CLOSE) { s_quit = true; s_focus_control = FOCUS_CLOSE; }
    else if (id == ACT_START) {
        s_focus_control = FOCUS_START;
        if (!s_worker) {
            s_cancel = 0;
            s_worker = papp_thread_create(test_thread, NULL,
                &(papp_thread_cfg_t){ .name = "ookla_test", .stack_size = 16384, .prio = PAPP_PRIO_NORMAL, .core = -1 });
            if (!s_worker) set_status(ST_ERROR, 0, "Could not start the test worker");
        }
    } else if (id == ACT_CHANGE) {
        s_focus_control = FOCUS_CHANGE_SERVER;
        if (!s_worker) {
            s_cancel = 0;
            s_worker = papp_thread_create(change_server_thread, NULL,
                &(papp_thread_cfg_t){ .name = "server_select", .stack_size = 12288, .prio = PAPP_PRIO_NORMAL, .core = -1 });
            if (!s_worker) set_status(ST_ERROR, 0, "Could not start server selection");
        }
    } else if (id == ACT_TOGGLE_SERVER) {
        s_show_server = !s_show_server;
        s_focus_control = FOCUS_SERVER_LABEL;
    } else if (id == ACT_CANCEL && s_worker) {
        s_cancel = 1;
        if (s_result_lock) papp_mutex_lock(s_result_lock);
        snprintf(s_status, sizeof(s_status), "Cancelling after current request...");
        if (s_result_lock) papp_mutex_unlock(s_result_lock);
    }
}

static void move_focus(uint32_t direction, bool running)
{
    static const int center_x[FOCUS_COUNT] = { 146, 639, 712, 760 };
    static const int center_y[FOCUS_COUNT] = { 444, 444, 400, 32 };
    if (direction != PAPP_BTN_LEFT && direction != PAPP_BTN_RIGHT &&
        direction != PAPP_BTN_UP && direction != PAPP_BTN_DOWN) return;

    int current = s_focus_control;
    int best = -1;
    long best_distance = LONG_MAX;
    for (int candidate = 0; candidate < FOCUS_COUNT; candidate++) {
        if (candidate == current || (running && candidate == FOCUS_CHANGE_SERVER)) continue;
        int dx = center_x[candidate] - center_x[current];
        int dy = center_y[candidate] - center_y[current];
        int primary, lateral;
        if (direction == PAPP_BTN_LEFT) { if (dx >= 0) continue; primary = -dx; lateral = dy; }
        else if (direction == PAPP_BTN_RIGHT) { if (dx <= 0) continue; primary = dx; lateral = dy; }
        else if (direction == PAPP_BTN_UP) { if (dy >= 0) continue; primary = -dy; lateral = dx; }
        else { if (dy <= 0) continue; primary = dy; lateral = dx; }

        /* Favor the requested direction while still choosing the nearest
         * target when controls are staggered (for example Server → Close). */
        long distance = (long)primary * primary + 4L * lateral * lateral;
        if (distance < best_distance) { best = candidate; best_distance = distance; }
    }
    if (best >= 0) s_focus_control = best;
}

static void draw_history_graph(int x, int y, int w, int h, const double *samples, unsigned count, uint16_t tint)
{
    ui_fill(x, y, w, h, ui_mix(C_PANEL, C_BG, 96));
    ui_line(x, y + h / 2, x + w, y + h / 2, 1, C_BORDER);
    if (count < 2) return;
    double max_value = 0;
    for (unsigned i = 0; i < count; i++) if (samples[i] > max_value) max_value = samples[i];
    if (max_value < 0.1) max_value = 1.0;
    max_value *= 1.12;
    for (unsigned i = 1; i < count; i++) {
        float x0 = x + (float)(i - 1) * (w - 1) / (float)(count - 1);
        float x1 = x + (float)i * (w - 1) / (float)(count - 1);
        float y0 = y + h - 2 - (float)(samples[i - 1] / max_value) * (h - 4);
        float y1 = y + h - 2 - (float)(samples[i] / max_value) * (h - 4);
        ui_line(x0, y0, x1, y1, 2, ui_mix(C_PANEL, tint, 150));
    }
}

static void draw_stat(int y, const char *label, const char *value, uint16_t tint,
                      const double *samples, unsigned count, bool graph)
{
    int h = graph ? 78 : 58;
    ui_rrect(440, y, 324, h, 14, C_PANEL);
    ui_rrect(440, y, 5, h, 2, tint);
    ui_text_l(ui_small, 460, y + 21, label, C_DIM);
    ui_text_r(ui_med, 744, y + 27, value, C_TEXT);
    if (graph) draw_history_graph(456, y + 38, 292, 34, samples, count, tint);
}

static void draw_gauge(double value, double scale, const char *phase, uint16_t tint)
{
    const float cx = 245, cy = 272, radius = 112;
    ui_disc(cx, cy, 129, C_PANEL, 255);
    (void)phase;
    for (int i = 0; i < 60; i++) {
        float a = (-135.0f + 270.0f * (float)i / 59.0f) * 0.01745329252f;
        float r0 = (i % 5 == 0) ? radius - 14 : radius - 7;
        uint16_t col = (i <= (int)(59.0 * fmin(fmax(value / scale, 0.0), 1.0))) ? tint : C_BORDER;
        ui_line(cx + cosf(a) * r0, cy + sinf(a) * r0,
                cx + cosf(a) * radius, cy + sinf(a) * radius, i % 5 == 0 ? 3 : 1, col);
    }
    double ratio = fmin(fmax(value / scale, 0.0), 1.0);
    float needle = (-135.0f + (float)(270.0 * ratio)) * 0.01745329252f;
    ui_line(cx, cy, cx + cosf(needle) * 78, cy + sinf(needle) * 78, 5, C_TEXT);
    ui_disc(cx, cy, 9, tint, 255);
    char number[24];
    snprintf(number, sizeof(number), value > 0.05 ? "%.1f" : "—", value);
    ui_text_c(ui_med, (int)cx, 306, number, C_TEXT);
    ui_text_c(ui_small, (int)cx, 343, "Mbps", C_DIM);
}

static void draw_outline_arc(float cx, float cy, float radius, float start_deg, float end_deg, uint16_t color)
{
    float prev_x = cx + cosf(start_deg * 0.01745329252f) * radius;
    float prev_y = cy + sinf(start_deg * 0.01745329252f) * radius;
    for (int i = 1; i <= 12; i++) {
        float deg = start_deg + (end_deg - start_deg) * (float)i / 12.0f;
        float x = cx + cosf(deg * 0.01745329252f) * radius;
        float y = cy + sinf(deg * 0.01745329252f) * radius;
        ui_line(prev_x, prev_y, x, y, 2, color);
        prev_x = x;
        prev_y = y;
    }
}

static void draw_round_outline(int x, int y, int w, int h, uint16_t color)
{
    /* Trace the same outer pill geometry as ui_button(), with every arc
     * tangent sharing its endpoint with the adjoining straight segment. */
    float radius = (float)h / 2.0f;
    float left = (float)x, top = (float)y;
    float right = (float)(x + w), bottom = (float)(y + h);
    ui_line(left + radius, top, right - radius, top, 2, color);
    draw_outline_arc(x + w - radius, y + radius, radius, -90, 0, color);
    ui_line(right, top + radius, right, bottom - radius, 2, color);
    draw_outline_arc(x + w - radius, y + h - radius, radius, 0, 90, color);
    ui_line(right - radius, bottom, left + radius, bottom, 2, color);
    draw_outline_arc(x + radius, y + h - radius, radius, 90, 180, color);
    ui_line(left, bottom - radius, left, top + radius, 2, color);
    draw_outline_arc(x + radius, y + radius, radius, 180, 270, color);
    /* Raster line caps can round adjacent float endpoints in opposite
     * directions. Small joins close those sub-pixel seams cleanly. */
    ui_disc(right - radius, top, 1.5f, color, 255);
    ui_disc(right, top + radius, 1.5f, color, 255);
    ui_disc(right, bottom - radius, 1.5f, color, 255);
    ui_disc(right - radius, bottom, 1.5f, color, 255);
    ui_disc(left + radius, bottom, 1.5f, color, 255);
    ui_disc(left, bottom - radius, 1.5f, color, 255);
    ui_disc(left, top + radius, 1.5f, color, 255);
    ui_disc(left + radius, top, 1.5f, color, 255);
}

static void draw_close_outline(uint16_t color)
{
    float previous_x = 760 + 27;
    float previous_y = 32;
    for (int i = 1; i <= 32; i++) {
        float angle = (float)i * (6.28318530718f / 32.0f);
        float x = 760 + cosf(angle) * 27;
        float y = 32 + sinf(angle) * 27;
        ui_line(previous_x, previous_y, x, y, 2, color);
        previous_x = x;
        previous_y = y;
    }
}

static void draw_screen(void)
{
    if (!s_surface) return;
    result_t r;
    double download_history[SPEED_SAMPLES], upload_history[SPEED_SAMPLES];
    unsigned download_samples, upload_samples;
    if (s_result_lock) papp_mutex_lock(s_result_lock);
    r = (result_t){ .state = s_state, .progress = s_progress, .ping_ms = s_ping_ms,
                    .download_mbps = s_download_mbps, .upload_mbps = s_upload_mbps };
    snprintf(r.server, sizeof(r.server), "%s", s_server);
    snprintf(r.status, sizeof(r.status), "%s", s_status);
    download_samples = s_download_samples;
    upload_samples = s_upload_samples;
    memcpy(download_history, s_download_history, download_samples * sizeof(double));
    memcpy(upload_history, s_upload_history, upload_samples * sizeof(double));
    if (s_result_lock) papp_mutex_unlock(s_result_lock);
    double live = s_live_mbps;

    ui_begin(s_surface);
    ui_text_l(ui_large, 36, 48, "Network Speedtest", C_TEXT);
    ui_disc(760, 32, 22, ui_pressed_color(UI_ID_CLOSE, C_RAISED), 255);
    ui_line(752, 24, 768, 40, 3, C_TEXT);
    ui_line(768, 24, 752, 40, 3, C_TEXT);
    ui_hit(728, 0, 72, 64, UI_ID_CLOSE, UI_TAP);
    ui_text_l(ui_small, 38, 92, "Ookla-compatible server - one connection per direction", C_DIM);
    ui_text_n(ui_small, 38, ui_line_top(ui_small, 120), r.status, (int)strlen(r.status),
              r.state == ST_ERROR ? C_RED : C_IN, 760);

    char value[32];
    snprintf(value, sizeof(value), r.ping_ms > 0 ? "%.0f ms" : "—", r.ping_ms);
    draw_gauge(live, live < 10 ? 10 : live < 25 ? 25 : live < 50 ? 50 : live < 100 ? 100 : live < 250 ? 250 : 500,
               r.state == ST_DOWNLOAD ? "DOWNLOAD" : r.state == ST_UPLOAD ? "UPLOAD" : "LIVE SPEED", r.state == ST_UPLOAD ? C_GREEN : C_OUT);
    draw_stat(145, "Latency", value, C_ORANGE, NULL, 0, false);
    snprintf(value, sizeof(value), r.download_mbps > 0 ? "%.1f Mbps" : "—", r.download_mbps);
    draw_stat(207, "Download", value, C_OUT, download_history, download_samples, true);
    snprintf(value, sizeof(value), r.upload_mbps > 0 ? "%.1f Mbps" : "—", r.upload_mbps);
    draw_stat(291, "Upload", value, C_GREEN, upload_history, upload_samples, true);
    const char *server_label = s_show_server
        ? (r.server[0] ? r.server : "Auto server") : "Server hidden";
    ui_text_n(ui_small, 440, ui_line_top(ui_small, 401), server_label,
              (int)strlen(server_label), C_DIM, 752);
    ui_button(660, 382, 104, 36, s_show_server ? "Hide" : "Show", C_RAISED, ACT_TOGGLE_SERVER);

    bool running = r.state == ST_DISCOVER || r.state == ST_LATENCY || r.state == ST_DOWNLOAD || r.state == ST_UPLOAD;
    if (!running) ui_button(36, 420, 220, 48, "Start Test", C_BLUE, ACT_START);
    else ui_button(36, 420, 220, 48, "Cancel", C_RED, ACT_CANCEL);
    ui_button(514, 420, 250, 48, "Change Server", C_RAISED, ACT_CHANGE);
    ui_text_c(ui_small, 385, 440, "D-pad + A", C_DIM);
    if (s_focus_control == FOCUS_CLOSE) {
        draw_close_outline(C_ORANGE);
    } else if (!running && s_focus_control == FOCUS_START) {
        draw_round_outline(36, 420, 220, 48, C_ORANGE);
    } else if (!running && s_focus_control == FOCUS_CHANGE_SERVER) {
        draw_round_outline(514, 420, 250, 48, C_ORANGE);
    } else if (s_focus_control == FOCUS_SERVER_LABEL) {
        draw_round_outline(660, 382, 104, 36, C_ORANGE);
    }
    ui_end(s_surface);
}

int main(int argc, char **argv)
{
    bool autotest = false;
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], "--autotest") == 0) autotest = true;
    s_result_lock = papp_mutex_create(false);
    s_surface = ui_open();
    if (!s_surface) { papp_ui_message("Speedtest", "This app needs a display."); return 1; }
    papp_input_flush();
    papp_gamepad_t pad = { 0 };
    if (papp_input_gamepad(0, &pad)) s_prev_pad_buttons = pad.buttons;
    ui_touch_reset();
    if (autotest) act(ACT_START, 0);
    while (!papp_exit_requested() && !s_quit) {
        ui_touch(s_surface, act);
        uint32_t buttons = papp_input_gamepad(0, &pad) ? pad.buttons : 0;
        uint32_t pressed = buttons & ~s_prev_pad_buttons;
        s_prev_pad_buttons = buttons;
        bool running = s_state == ST_DISCOVER || s_state == ST_LATENCY || s_state == ST_DOWNLOAD || s_state == ST_UPLOAD;
        if (pressed & PAPP_BTN_LEFT) move_focus(PAPP_BTN_LEFT, running);
        else if (pressed & PAPP_BTN_RIGHT) move_focus(PAPP_BTN_RIGHT, running);
        else if (pressed & PAPP_BTN_UP) move_focus(PAPP_BTN_UP, running);
        else if (pressed & PAPP_BTN_DOWN) move_focus(PAPP_BTN_DOWN, running);
        if (pressed & PAPP_BTN_A) {
            if (s_focus_control == FOCUS_CLOSE) act(UI_ID_CLOSE, 0);
            else if (s_focus_control == FOCUS_START) act(running ? ACT_CANCEL : ACT_START, 0);
            else if (s_focus_control == FOCUS_CHANGE_SERVER && !running) act(ACT_CHANGE, 0);
            else if (s_focus_control == FOCUS_SERVER_LABEL) act(ACT_TOGGLE_SERVER, 0);
        }
        if (pressed & PAPP_BTN_B) act(ACT_CANCEL, 0);
        if (pressed & PAPP_BTN_X) s_quit = true;
        if (pressed & PAPP_BTN_Y) act(ACT_CHANGE, 0);
        bool enter = papp_input_key_down(0x28);  /* HID Enter */
        if (enter && !s_prev_enter) act(ACT_START, 0);
        s_prev_enter = enter;
        if (s_worker && (s_state == ST_DONE || s_state == ST_ERROR || s_state == ST_CANCELLED)) {
            papp_thread_join(s_worker, PAPP_WAIT_FOREVER, NULL);
            s_worker = NULL;
        }
        draw_screen();
        papp_delay_ms(30);
    }
    s_cancel = 1;
    if (s_worker) papp_thread_join(s_worker, PAPP_WAIT_FOREVER, NULL);
    if (s_result_lock) papp_mutex_destroy(s_result_lock);
    if (s_surface) papp_disp_surface_destroy(s_surface);
    return 0;
}
