/*
 * NET interface: Wi-Fi (lx_net), lwIP sockets, TLS (mbedTLS) and an HTTP/1.1 client for apps.
 *
 * Sockets are non-blocking underneath: every wait is a select() in 100 ms slices that gives
 * up when the app is asked to exit, so a forced stop never waits on the network (the App
 * Manager only kills the app outside service calls). Names are looked up on a helper task
 * for the same reason: an abandoned lookup finishes there on its own. The first call holds
 * the radio for the run; teardown closes what the app left open and releases it.
 */
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "lx_services.h"
#include "lx_net.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include "psa/crypto.h"
#endif

static const char *TAG = "lx_svc_net";

#define SLICE_MS     100
#define DEF_TIMEOUT  15000
#define HOST_MAX     128
#define URL_MAX      2048            /* release downloads redirect to long URLs */
#define HDR_MAX      16384           /* GitHub redirects carry about 5 KB of headers */
#define BODY_BUF     2048

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    mbedtls_x509_crt    ca;
} tls_t;

struct papp_sock {
    bool    used;
    uint8_t type;
    int     fd;
    tls_t  *tls;
};

struct papp_http {
    bool              used;
    struct papp_sock *sock;
    char             *hdr;           /* reply headers, 0-terminated */
    uint8_t          *buf;           /* body bytes read ahead       */
    int               pos, len;
    long              length, left;  /* Content-Length, -1 unknown  */
    long              chunk_left;
    bool              chunked, eof, done;
    uint32_t          timeout;
};

static struct papp_sock s_socks[PAPP_NET_MAX_SOCKETS];
static struct papp_http s_http[PAPP_NET_MAX_HTTP];
static SemaphoreHandle_t s_lock;
static bool s_held;

/* ── Deadlines and waits ─────────────────────────────────────────────────── */

typedef struct { int64_t end; bool forever; } deadline_t;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static deadline_t dl_make(uint32_t ms)
{
    deadline_t d = { .forever = ms == PAPP_WAIT_FOREVER };
    d.end = now_ms() + (d.forever ? 0 : ms);
    return d;
}

static int64_t dl_slice(const deadline_t *d)        /* next wait, <= SLICE_MS; 0 = expired */
{
    if (d->forever) return SLICE_MS;
    int64_t left = d->end - now_ms();
    return left <= 0 ? 0 : left < SLICE_MS ? left : SLICE_MS;
}

static bool dl_over(const deadline_t *d) { return !d->forever && now_ms() >= d->end; }

static int map_errno(int e)
{
    switch (e) {
    case ECONNREFUSED: return PAPP_E_REFUSED;
    case ECONNRESET: case ECONNABORTED: case EPIPE: case ENOTCONN: return PAPP_E_RESET;
    case EHOSTUNREACH: case ENETUNREACH: case ENETDOWN: return PAPP_E_NOT_CONNECTED;
    case ENOMEM: case ENOBUFS: return PAPP_E_NOMEM;
    case ETIMEDOUT: return PAPP_E_TIMEOUT;
    case ENFILE: case EMFILE: return PAPP_E_LIMIT;
    case EINVAL: case EADDRINUSE: case EADDRNOTAVAIL: case EAFNOSUPPORT: return PAPP_E_ARG;
    default: return PAPP_E_IO;
    }
}

/* 1 when fd is readable (or writable), else PAPP_E_TIMEOUT / PAPP_E_EXIT / PAPP_E_IO */
static int wait_fd(int fd, bool write, const deadline_t *d)
{
    for (;;) {
        if (lx_svc_exit_flag) return PAPP_E_EXIT;
        int64_t slice = dl_slice(d);
        fd_set fs, es;
        FD_ZERO(&fs);
        FD_ZERO(&es);
        FD_SET(fd, &fs);
        FD_SET(fd, &es);
        struct timeval tv = { .tv_sec = 0, .tv_usec = (long)slice * 1000 };
        int r = select(fd + 1, write ? NULL : &fs, write ? &fs : NULL, &es, &tv);
        if (r > 0) return 1;
        if (r < 0 && errno != EINTR) return PAPP_E_IO;
        if (dl_over(d)) return PAPP_E_TIMEOUT;
    }
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int hold(void);

int lx_svc_net_hold(void) { return hold(); }

static int hold(void)
{
    if (s_held) return PAPP_E_OK;
    esp_err_t e = lx_net_app_hold(&lx_svc_exit_flag);
    if (e == ESP_OK) {
        s_held = true;
        return PAPP_E_OK;
    }
    return lx_svc_exit_flag ? PAPP_E_EXIT : PAPP_E_UNSUPPORTED;
}

static void to_sin(const papp_addr_t *a, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    sa->sin_port = htons(a->port);
    memcpy(&sa->sin_addr.s_addr, a->ip, 4);
}

static void from_sin(const struct sockaddr_in *sa, papp_addr_t *a)
{
    memset(a, 0, sizeof(*a));
    a->family = PAPP_AF_INET;
    a->port = ntohs(sa->sin_port);
    memcpy(a->ip, &sa->sin_addr.s_addr, 4);
}

/* ── Socket table ────────────────────────────────────────────────────────── */

static struct papp_sock *slot_new(int type, int fd)
{
    struct papp_sock *s = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < PAPP_NET_MAX_SOCKETS; i++) {
        if (!s_socks[i].used) {
            s = &s_socks[i];
            *s = (struct papp_sock){ .used = true, .type = (uint8_t)type, .fd = fd };
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return s;
}

static struct papp_sock *slot_get(papp_sock_t *h)
{
    uintptr_t p = (uintptr_t)h, b = (uintptr_t)s_socks;
    if (p < b || p >= b + sizeof(s_socks) || (p - b) % sizeof(s_socks[0])) return NULL;
    struct papp_sock *s = (struct papp_sock *)h;
    return s->used ? s : NULL;
}

static void tls_free(tls_t *t)
{
    if (!t) return;
    mbedtls_ssl_free(&t->ssl);
    mbedtls_ssl_config_free(&t->conf);
    mbedtls_x509_crt_free(&t->ca);
    free(t);
}

static void slot_close(struct papp_sock *s)
{
    if (s->tls) {
        mbedtls_ssl_close_notify(&s->tls->ssl);     /* best effort: the socket is non-blocking */
        tls_free(s->tls);
    }
    if (s->fd >= 0) close(s->fd);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *s = (struct papp_sock){ .fd = -1 };
    xSemaphoreGive(s_lock);
}

static int tls_count(void)
{
    int n = 0;
    for (int i = 0; i < PAPP_NET_MAX_SOCKETS; i++) n += s_socks[i].used && s_socks[i].tls;
    return n;
}

/* ── Names (helper task) ─────────────────────────────────────────────────── */

typedef struct {
    char              host[HOST_MAX];
    uint32_t          ip;
    int               err;
    SemaphoreHandle_t done;
    int               refs;          /* the caller and the task: the last one frees */
} dns_job_t;

static void dns_job_put(dns_job_t *j)
{
    if (__atomic_sub_fetch(&j->refs, 1, __ATOMIC_SEQ_CST) == 0) {
        vSemaphoreDelete(j->done);
        free(j);
    }
}

static void dns_task(void *arg)
{
    dns_job_t *j = arg;
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    if (getaddrinfo(j->host, NULL, &hints, &res) == 0 && res) {
        j->ip = ((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
        j->err = PAPP_E_OK;
    } else {
        j->err = PAPP_E_DNS;
    }
    if (res) freeaddrinfo(res);
    xSemaphoreGive(j->done);
    dns_job_put(j);
    vTaskDelete(NULL);
}

static int resolve_ip(const char *host, uint32_t *ip, const deadline_t *d)
{
    struct in_addr a;
    if (!host || !host[0]) return PAPP_E_ARG;
    if (inet_aton(host, &a)) {
        *ip = a.s_addr;
        return PAPP_E_OK;
    }
    if (strlen(host) >= HOST_MAX) return PAPP_E_ARG;
    dns_job_t *j = heap_caps_calloc(1, sizeof(*j), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!j) return PAPP_E_NOMEM;
    strcpy(j->host, host);
    j->refs = 2;
    j->done = xSemaphoreCreateBinary();
    if (!j->done || xTaskCreate(dns_task, "lx_dns", 4096, j, 5, NULL) != pdPASS) {
        if (j->done) vSemaphoreDelete(j->done);
        free(j);
        return PAPP_E_NOMEM;
    }
    int r;
    for (;;) {
        if (xSemaphoreTake(j->done, pdMS_TO_TICKS(dl_slice(d))) == pdTRUE) {
            r = j->err;
            if (r == PAPP_E_OK) *ip = j->ip;
            break;
        }
        if (lx_svc_exit_flag) { r = PAPP_E_EXIT; break; }
        if (dl_over(d)) { r = PAPP_E_TIMEOUT; break; }
    }
    dns_job_put(j);
    return r;
}

/* ── Plain and TLS I/O ───────────────────────────────────────────────────── */

static int tcp_connect(int fd, const struct sockaddr_in *sa, const deadline_t *d)
{
    if (connect(fd, (const struct sockaddr *)sa, sizeof(*sa)) == 0) return PAPP_E_OK;
    if (errno != EINPROGRESS) return map_errno(errno);
    int r = wait_fd(fd, true, d);
    if (r < 0) return r;
    int e = 0;
    socklen_t l = sizeof(e);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l);
    return e ? map_errno(e) : PAPP_E_OK;
}

static int tls_err(int ret)
{
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
    if (ret == MBEDTLS_ERR_NET_CONN_RESET || ret == MBEDTLS_ERR_NET_RECV_FAILED ||
        ret == MBEDTLS_ERR_NET_SEND_FAILED) return PAPP_E_RESET;
    if (ret == MBEDTLS_ERR_SSL_ALLOC_FAILED) return PAPP_E_NOMEM;
    return PAPP_E_TLS;
}

static int sock_send(struct papp_sock *s, const void *buf, size_t len, const deadline_t *d)
{
    for (;;) {
        int n, r;
        if (s->tls) {
            n = mbedtls_ssl_write(&s->tls->ssl, buf, len);
            if (n >= 0) return n;
            if (n == MBEDTLS_ERR_SSL_WANT_READ) r = wait_fd(s->fd, false, d);
            else if (n == MBEDTLS_ERR_SSL_WANT_WRITE) r = wait_fd(s->fd, true, d);
            else return tls_err(n) ? tls_err(n) : PAPP_E_RESET;
        } else {
            n = send(s->fd, buf, len, MSG_DONTWAIT);
            if (n >= 0) return n;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return map_errno(errno);
            r = wait_fd(s->fd, true, d);
        }
        if (r < 0) return r;
    }
}

static int send_all(struct papp_sock *s, const void *buf, size_t len, const deadline_t *d)
{
    const uint8_t *p = buf;
    while (len) {
        int n = sock_send(s, p, len, d);
        if (n < 0) return n;
        p += n;
        len -= (size_t)n;
    }
    return PAPP_E_OK;
}

static int sock_recv(struct papp_sock *s, void *buf, size_t len, const deadline_t *d)
{
    for (;;) {
        int n, r;
        if (s->tls) {
            n = mbedtls_ssl_read(&s->tls->ssl, buf, len);
            if (n >= 0) return n;
            if (n == MBEDTLS_ERR_SSL_WANT_READ) r = wait_fd(s->fd, false, d);
            else if (n == MBEDTLS_ERR_SSL_WANT_WRITE) r = wait_fd(s->fd, true, d);
#ifdef MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET
            else if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) continue;
#endif
            else return tls_err(n);
        } else {
            n = recv(s->fd, buf, len, MSG_DONTWAIT);
            if (n >= 0) return n;
            if (errno != EAGAIN && errno != EWOULDBLOCK) return map_errno(errno);
            r = wait_fd(s->fd, false, d);
        }
        if (r < 0) return r;
    }
}

static int bio_send(void *ctx, const unsigned char *b, size_t n)
{
    int r = send((int)(intptr_t)ctx, b, n, MSG_DONTWAIT);
    if (r >= 0) return r;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return errno == ECONNRESET || errno == EPIPE ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int bio_recv(void *ctx, unsigned char *b, size_t n)
{
    int r = recv((int)(intptr_t)ctx, b, n, MSG_DONTWAIT);
    if (r >= 0) return r;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
    return errno == ECONNRESET ? MBEDTLS_ERR_NET_CONN_RESET : MBEDTLS_ERR_NET_RECV_FAILED;
}

static int rng(void *p, unsigned char *b, size_t n)
{
    esp_fill_random(b, n);
    return 0;
}

/* Resolve, connect and (tls) handshake within one deadline */
static struct papp_sock *open_stream(const char *host, uint16_t port, bool tls, const papp_tls_cfg_t *cfg,
                                     const deadline_t *d, int *err)
{
    *err = PAPP_E_OK;
    if (tls && tls_count() >= PAPP_NET_MAX_TLS) { *err = PAPP_E_LIMIT; return NULL; }
    uint32_t ip;
    int r = resolve_ip(host, &ip, d);
    if (r) { *err = r; return NULL; }
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) { *err = map_errno(errno); return NULL; }
    set_nonblock(fd);
    struct papp_sock *s = slot_new(PAPP_SOCK_TCP, fd);
    if (!s) {
        close(fd);
        *err = PAPP_E_LIMIT;
        return NULL;
    }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = ip };
    r = tcp_connect(fd, &sa, d);
    if (r == PAPP_E_OK && tls) {
        tls_t *t = calloc(1, sizeof(*t));
        if (!t) {
            r = PAPP_E_NOMEM;
            goto fail;
        }
        s->tls = t;
#if defined(MBEDTLS_PSA_CRYPTO_C)
        psa_crypto_init();
#endif
        mbedtls_ssl_init(&t->ssl);
        mbedtls_ssl_config_init(&t->conf);
        mbedtls_x509_crt_init(&t->ca);
        int ret = mbedtls_ssl_config_defaults(&t->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                              MBEDTLS_SSL_PRESET_DEFAULT);
        mbedtls_ssl_conf_rng(&t->conf, rng, NULL);
        if (!ret) {
            if (cfg && cfg->insecure) {
                mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_NONE);
            } else if (cfg && cfg->ca_pem) {
                ret = mbedtls_x509_crt_parse(&t->ca, (const unsigned char *)cfg->ca_pem,
                                             cfg->ca_len ? cfg->ca_len : strlen(cfg->ca_pem) + 1);
                mbedtls_ssl_conf_ca_chain(&t->conf, &t->ca, NULL);
                mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            } else {
                ret = esp_crt_bundle_attach(&t->conf) == ESP_OK ? 0 : -1;
                mbedtls_ssl_conf_authmode(&t->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
            }
        }
#if defined(MBEDTLS_SSL_ALPN)
        if (!ret && cfg && cfg->alpn) ret = mbedtls_ssl_conf_alpn_protocols(&t->conf, cfg->alpn);
#endif
        if (!ret) ret = mbedtls_ssl_setup(&t->ssl, &t->conf);
        if (!ret) ret = mbedtls_ssl_set_hostname(&t->ssl, host);
        if (ret) {
            ESP_LOGW(TAG, "TLS setup failed: -0x%x", -ret);
            r = ret == MBEDTLS_ERR_SSL_ALLOC_FAILED ? PAPP_E_NOMEM : PAPP_E_ARG;
            goto fail;
        }
        mbedtls_ssl_set_bio(&t->ssl, (void *)(intptr_t)fd, bio_send, bio_recv, NULL);
        for (;;) {
            ret = mbedtls_ssl_handshake(&t->ssl);
            if (ret == 0) {
                r = PAPP_E_OK;
                break;
            }
            if (ret == MBEDTLS_ERR_SSL_WANT_READ) r = wait_fd(fd, false, d);
            else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) r = wait_fd(fd, true, d);
            else {
                ESP_LOGW(TAG, "TLS handshake with %s failed: -0x%x", host, -ret);
                r = ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED ? PAPP_E_CERT
                    : tls_err(ret) ? tls_err(ret) : PAPP_E_RESET;
            }
            if (r < 0) goto fail;
        }
    }
    if (r == PAPP_E_OK) return s;
fail:
    slot_close(s);
    *err = r;
    return NULL;
}

/* ── Wi-Fi ───────────────────────────────────────────────────────────────── */

static int map_state(lx_net_state_t st)
{
    switch (st) {
    case LX_NET_IDLE: return PAPP_NET_IDLE;
    case LX_NET_SCANNING: return lx_net_joining() ? PAPP_NET_CONNECTING : PAPP_NET_IDLE;
    case LX_NET_CONNECTING: return PAPP_NET_CONNECTING;
    case LX_NET_CONNECTED: return PAPP_NET_CONNECTED;
    case LX_NET_FAILED: return PAPP_NET_FAILED;
    default: return PAPP_NET_OFF;
    }
}

static int wait_connected(uint32_t timeout_ms)
{
    deadline_t d = dl_make(timeout_ms);
    for (;;) {
        lx_net_state_t st = lx_net_state();
        if (st == LX_NET_CONNECTED) return PAPP_E_OK;
        if (st == LX_NET_NO_RADIO) return PAPP_E_UNSUPPORTED;
        if (st == LX_NET_FAILED || !lx_net_joining()) return PAPP_E_NOT_CONNECTED;
        if (lx_svc_exit_flag) return PAPP_E_EXIT;
        if (dl_over(&d)) return PAPP_E_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static int net_up(uint32_t timeout_ms)
{
    SVC_ENTER();
    int r = hold();
    if (!r) r = wait_connected(timeout_ms);
    SVC_LEAVE();
    return r;
}

static void net_info(papp_net_info_t *o)
{
    if (!o) return;
    SVC_ENTER();
    hold();
    lx_net_info_t li;
    lx_net_info(&li);
    memset(o, 0, sizeof(*o));
    o->state = (uint8_t)map_state(li.state);
    o->rssi = li.rssi;
    o->channel = li.channel;
    o->ap_clients = li.ap_clients;
    o->ap_running = li.ap_running;
    memcpy(o->ssid, li.ssid, sizeof(o->ssid));
    memcpy(o->mac, li.mac, 6);
    memcpy(o->ip, &li.ip, 4);
    memcpy(o->mask, &li.mask, 4);
    memcpy(o->gateway, &li.gw, 4);
    memcpy(o->dns, &li.dns, 4);
    memcpy(o->ap_ip, &li.ap_ip, 4);
    SVC_LEAVE();
}

static int net_scan(papp_net_ap_t *out, int max, uint32_t timeout_ms)
{
    if (!out || max <= 0) return PAPP_E_ARG;
    SVC_ENTER();
    int r = hold();
    if (!r) {
        lx_net_scan();
        deadline_t d = dl_make(timeout_ms);
        lx_net_ap_t aps[LX_NET_MAX_APS];
        for (;;) {
            int n = lx_net_scan_results(aps, LX_NET_MAX_APS);
            if (n >= 0) {
                if (n > max) n = max;
                for (int i = 0; i < n; i++) {
                    memset(&out[i], 0, sizeof(out[i]));
                    memcpy(out[i].ssid, aps[i].ssid, sizeof(out[i].ssid));
                    out[i].rssi = aps[i].rssi;
                    out[i].channel = aps[i].channel;
                    out[i].secure = aps[i].secure;
                }
                r = n;
                break;
            }
            if (lx_svc_exit_flag) { r = PAPP_E_EXIT; break; }
            if (dl_over(&d)) { r = PAPP_E_TIMEOUT; break; }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    SVC_LEAVE();
    return r;
}

static int net_join(const char *ssid, const char *password, uint32_t timeout_ms)
{
    size_t pl = password ? strlen(password) : 0;
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pl && (pl < 8 || pl > 64))) return PAPP_E_ARG;
    SVC_ENTER();
    int r = hold();
    if (!r) {
        esp_err_t e = lx_net_app_join(ssid, password, &lx_svc_exit_flag);
        r = e == ESP_OK ? wait_connected(timeout_ms) : lx_svc_exit_flag ? PAPP_E_EXIT : PAPP_E_UNSUPPORTED;
    }
    SVC_LEAVE();
    return r;
}

static bool net_poll_event(papp_net_event_t *out)
{
    if (!out) return false;
    lx_net_app_event_t e;
    if (!lx_net_app_event(&e)) return false;
    *out = (papp_net_event_t){ .type = e.type, .code = e.code, .time_ms = e.time_ms };
    return true;
}

static int net_ap_start(const char *ssid, const char *password, uint8_t channel, uint8_t max_clients)
{
    size_t pl = password ? strlen(password) : 0;
    if (!ssid || !ssid[0] || strlen(ssid) > 32 || (pl && (pl < 8 || pl > 63)) || channel > 13 || max_clients > 10)
        return PAPP_E_ARG;
    SVC_ENTER();
    int r = hold();
    if (!r) {
        esp_err_t e = lx_net_ap_start(ssid, password, channel, max_clients, &lx_svc_exit_flag);
        r = e == ESP_OK ? PAPP_E_OK : lx_svc_exit_flag ? PAPP_E_EXIT
            : e == ESP_ERR_TIMEOUT ? PAPP_E_TIMEOUT : PAPP_E_UNSUPPORTED;
    }
    SVC_LEAVE();
    return r;
}

static void net_ap_stop(void)
{
    SVC_ENTER();
    if (s_held) lx_net_ap_stop();
    SVC_LEAVE();
}

static int net_resolve(const char *host, papp_addr_t *out, uint32_t timeout_ms)
{
    if (!out) return PAPP_E_ARG;
    SVC_ENTER();
    int r = hold();
    uint32_t ip = 0;
    if (!r) {
        deadline_t d = dl_make(timeout_ms);
        r = resolve_ip(host, &ip, &d);
    }
    if (!r) {
        memset(out, 0, sizeof(*out));
        out->family = PAPP_AF_INET;
        memcpy(out->ip, &ip, 4);
    }
    SVC_LEAVE();
    return r;
}

/* ── Sockets ─────────────────────────────────────────────────────────────── */

static papp_sock_t *net_socket(int type, int *err)
{
    SVC_ENTER();
    struct papp_sock *s = NULL;
    int r = hold();
    if (!r && type != PAPP_SOCK_TCP && type != PAPP_SOCK_UDP) r = PAPP_E_ARG;
    if (!r) {
        int fd = socket(AF_INET, type == PAPP_SOCK_TCP ? SOCK_STREAM : SOCK_DGRAM,
                        type == PAPP_SOCK_TCP ? IPPROTO_TCP : IPPROTO_UDP);
        if (fd < 0) {
            r = map_errno(errno);
        } else {
            set_nonblock(fd);
            s = slot_new(type, fd);
            if (!s) {
                close(fd);
                r = PAPP_E_LIMIT;
            }
        }
    }
    if (err) *err = r;
    SVC_LEAVE();
    return (papp_sock_t *)s;
}

static int net_connect(papp_sock_t *h, const papp_addr_t *to, uint32_t timeout_ms)
{
    SVC_ENTER();
    struct papp_sock *s = slot_get(h);
    int r = PAPP_E_ARG;
    if (s && !s->tls && to && to->family == PAPP_AF_INET) {
        struct sockaddr_in sa;
        to_sin(to, &sa);
        deadline_t d = dl_make(timeout_ms);
        r = tcp_connect(s->fd, &sa, &d);
    }
    SVC_LEAVE();
    return r;
}

static int net_bind(papp_sock_t *h, uint16_t port)
{
    SVC_ENTER();
    struct papp_sock *s = slot_get(h);
    int r = PAPP_E_ARG;
    if (s) {
        struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
        r = bind(s->fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 ? PAPP_E_OK : map_errno(errno);
    }
    SVC_LEAVE();
    return r;
}

static int net_listen(papp_sock_t *h, int backlog)
{
    SVC_ENTER();
    struct papp_sock *s = slot_get(h);
    int r = PAPP_E_ARG;
    if (s && s->type == PAPP_SOCK_TCP)
        r = listen(s->fd, backlog > 0 ? backlog : 4) == 0 ? PAPP_E_OK : map_errno(errno);
    SVC_LEAVE();
    return r;
}

static papp_sock_t *net_accept(papp_sock_t *h, papp_addr_t *from, uint32_t timeout_ms, int *err)
{
    SVC_ENTER();
    struct papp_sock *s = slot_get(h), *c = NULL;
    int r = s && s->type == PAPP_SOCK_TCP ? PAPP_E_OK : PAPP_E_ARG;
    deadline_t d = dl_make(timeout_ms);
    while (!r) {
        struct sockaddr_in sa;
        socklen_t l = sizeof(sa);
        int fd = accept(s->fd, (struct sockaddr *)&sa, &l);
        if (fd >= 0) {
            set_nonblock(fd);
            c = slot_new(PAPP_SOCK_TCP, fd);
            if (!c) {
                close(fd);
                r = PAPP_E_LIMIT;
            } else if (from) {
                from_sin(&sa, from);
            }
            break;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            r = map_errno(errno);
            break;
        }
        int w = wait_fd(s->fd, false, &d);
        if (w < 0) r = w;
    }
    if (err) *err = r;
    SVC_LEAVE();
    return (papp_sock_t *)c;
}

static int net_send(papp_sock_t *h, const void *buf, size_t len, uint32_t timeout_ms)
{
    SVC_ENTER();
    struct papp_sock *s = slot_get(h);
    int r = PAPP_E_ARG;
    if (s && (buf || !len)) {
        deadline_t d = dl_make(timeout_ms);
        r = len ? sock_send(s, buf, len, &d) : 0;
    }
    SVC_LEAVE();
    return r;
}

static int net_recv(papp_sock_t *h, void *buf, size_t len, uint32_t timeout_ms)
{
    SVC_ENTER();
    struct papp_sock *s = slot_get(h);
    int r = PAPP_E_ARG;
    if (s && buf && len) {
        deadline_t d = dl_make(timeout_ms);
        r = sock_recv(s, buf, len, &d);
    }
    SVC_LEAVE();
    return r;
}

static int net_sendto(papp_sock_t *h, const void *buf, size_t len, const papp_addr_t *to)
{
    SVC_ENTER();
    struct papp_sock *s = slot_get(h);
    int r = PAPP_E_ARG;
    if (s && s->type == PAPP_SOCK_UDP && to && to->family == PAPP_AF_INET && (buf || !len)) {
        struct sockaddr_in sa;
        to_sin(to, &sa);
        deadline_t d = dl_make(1000);
        for (;;) {
            int n = sendto(s->fd, buf, len, MSG_DONTWAIT, (struct sockaddr *)&sa, sizeof(sa));
            if (n >= 0) { r = n; break; }
            if (errno != EAGAIN && errno != EWOULDBLOCK) { r = map_errno(errno); break; }
            r = wait_fd(s->fd, true, &d);
            if (r < 0) break;
        }
    }
    SVC_LEAVE();
    return r;
}

static int net_recvfrom(papp_sock_t *h, void *buf, size_t len, papp_addr_t *from, uint32_t timeout_ms)
{
    SVC_ENTER();
    struct papp_sock *s = slot_get(h);
    int r = PAPP_E_ARG;
    if (s && s->type == PAPP_SOCK_UDP && buf && len) {
        deadline_t d = dl_make(timeout_ms);
        for (;;) {
            struct sockaddr_in sa;
            socklen_t l = sizeof(sa);
            int n = recvfrom(s->fd, buf, len, MSG_DONTWAIT, (struct sockaddr *)&sa, &l);
            if (n >= 0) {
                if (from) from_sin(&sa, from);
                r = n;
                break;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) { r = map_errno(errno); break; }
            r = wait_fd(s->fd, false, &d);
            if (r < 0) break;
        }
    }
    SVC_LEAVE();
    return r;
}

static int net_set_opt(papp_sock_t *h, int opt, int value)
{
    SVC_ENTER();
    struct papp_sock *s = slot_get(h);
    int r = PAPP_E_ARG, e = -1;
    if (s) {
        int one = value ? 1 : 0;
        switch (opt) {
        case PAPP_SO_NODELAY:
            if (s->type == PAPP_SOCK_TCP) e = setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            break;
        case PAPP_SO_KEEPALIVE:
            if (s->type == PAPP_SOCK_TCP) {
                e = setsockopt(s->fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
                if (!e && value > 0) {
                    int intvl = 5, cnt = 3;
                    setsockopt(s->fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, sizeof(value));
                    setsockopt(s->fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                    setsockopt(s->fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
                }
            }
            break;
        case PAPP_SO_BROADCAST:
            if (s->type == PAPP_SOCK_UDP) e = setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
            break;
        case PAPP_SO_REUSEADDR:
            e = setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            break;
        case PAPP_SO_MCAST_JOIN:
        case PAPP_SO_MCAST_LEAVE:
            if (s->type == PAPP_SOCK_UDP) {
                struct ip_mreq m = { .imr_multiaddr.s_addr = (uint32_t)value, .imr_interface.s_addr = htonl(INADDR_ANY) };
                e = setsockopt(s->fd, IPPROTO_IP, opt == PAPP_SO_MCAST_JOIN ? IP_ADD_MEMBERSHIP : IP_DROP_MEMBERSHIP,
                               &m, sizeof(m));
            }
            break;
        }
        r = e == 0 ? PAPP_E_OK : PAPP_E_ARG;
    }
    SVC_LEAVE();
    return r;
}

static int net_poll(papp_poll_t *set, int n, uint32_t timeout_ms)
{
    if (!set || n <= 0 || n > PAPP_NET_MAX_SOCKETS) return PAPP_E_ARG;
    SVC_ENTER();
    deadline_t d = dl_make(timeout_ms);
    int r;
    for (;;) {
        fd_set rs, ws, es;
        FD_ZERO(&rs);
        FD_ZERO(&ws);
        FD_ZERO(&es);
        int maxfd = -1, ready = 0;
        for (int i = 0; i < n; i++) {
            set[i].got = 0;
            struct papp_sock *s = slot_get(set[i].sock);
            if (!s) {
                set[i].got = PAPP_POLL_ERR;
                ready++;
                continue;
            }
            if ((set[i].want & PAPP_POLL_IN) && s->tls && mbedtls_ssl_get_bytes_avail(&s->tls->ssl) > 0)
                set[i].got |= PAPP_POLL_IN;          /* already decrypted */
            if (set[i].want & PAPP_POLL_IN) FD_SET(s->fd, &rs);
            if (set[i].want & PAPP_POLL_OUT) FD_SET(s->fd, &ws);
            FD_SET(s->fd, &es);
            if (s->fd > maxfd) maxfd = s->fd;
            ready += set[i].got != 0;
        }
        struct timeval tv = { .tv_sec = 0, .tv_usec = ready ? 0 : (long)dl_slice(&d) * 1000 };
        int sr = maxfd >= 0 ? select(maxfd + 1, &rs, &ws, &es, &tv) : 0;
        if (sr < 0 && errno != EINTR) { r = PAPP_E_IO; break; }
        ready = 0;
        for (int i = 0; i < n; i++) {
            struct papp_sock *s = slot_get(set[i].sock);
            if (s && sr > 0) {
                if (FD_ISSET(s->fd, &rs)) set[i].got |= PAPP_POLL_IN;
                if (FD_ISSET(s->fd, &ws)) set[i].got |= PAPP_POLL_OUT;
                if (FD_ISSET(s->fd, &es)) set[i].got |= PAPP_POLL_ERR;
            }
            ready += set[i].got != 0;
        }
        if (ready) { r = ready; break; }
        if (lx_svc_exit_flag) { r = PAPP_E_EXIT; break; }
        if (dl_over(&d)) { r = 0; break; }
    }
    SVC_LEAVE();
    return r;
}

static int net_local_addr(papp_sock_t *h, papp_addr_t *out)
{
    SVC_ENTER();
    struct papp_sock *s = slot_get(h);
    int r = PAPP_E_ARG;
    if (s && out) {
        struct sockaddr_in sa;
        socklen_t l = sizeof(sa);
        if (getsockname(s->fd, (struct sockaddr *)&sa, &l) == 0) {
            from_sin(&sa, out);
            r = PAPP_E_OK;
        }
    }
    SVC_LEAVE();
    return r;
}

static void net_close(papp_sock_t *h)
{
    SVC_ENTER();
    struct papp_sock *s = slot_get(h);
    if (s) {
        for (int i = 0; i < PAPP_NET_MAX_HTTP; i++)       /* an HTTP request's socket stays its own */
            if (s_http[i].used && s_http[i].sock == s) s = NULL;
        if (s) slot_close(s);
    }
    SVC_LEAVE();
}

static papp_sock_t *net_tls_connect(const char *host, uint16_t port, const papp_tls_cfg_t *cfg, int *err)
{
    SVC_ENTER();
    struct papp_sock *s = NULL;
    int r = hold();
    if (!r && (!host || !port)) r = PAPP_E_ARG;
    if (!r) {
        deadline_t d = dl_make(cfg && cfg->timeout_ms ? cfg->timeout_ms : DEF_TIMEOUT);
        s = open_stream(host, port, true, cfg, &d, &r);
    }
    if (err) *err = r;
    SVC_LEAVE();
    return (papp_sock_t *)s;
}

/* ── HTTP/1.1 client ─────────────────────────────────────────────────────── */

static struct papp_http *http_get(papp_http_t *h)
{
    uintptr_t p = (uintptr_t)h, b = (uintptr_t)s_http;
    if (p < b || p >= b + sizeof(s_http) || (p - b) % sizeof(s_http[0])) return NULL;
    struct papp_http *x = (struct papp_http *)h;
    return x->used ? x : NULL;
}

static void http_free(struct papp_http *h)
{
    if (h->sock) slot_close(h->sock);
    free(h->hdr);
    free(h->buf);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *h = (struct papp_http){ 0 };
    xSemaphoreGive(s_lock);
}

/* "https://host:port/path?q" -> parts; path points into url ("" means "/") */
static int parse_url(const char *url, bool *tls, char *host, uint16_t *port, const char **path)
{
    if (!strncasecmp(url, "https://", 8)) { *tls = true; *port = 443; url += 8; }
    else if (!strncasecmp(url, "http://", 7)) { *tls = false; *port = 80; url += 7; }
    else return PAPP_E_ARG;
    size_t n = strcspn(url, ":/?#");
    if (!n || n >= HOST_MAX) return PAPP_E_ARG;
    memcpy(host, url, n);
    host[n] = 0;
    url += n;
    if (*url == ':') {
        *port = (uint16_t)atoi(url + 1);
        url += 1 + strspn(url + 1, "0123456789");
        if (!*port) return PAPP_E_ARG;
    }
    *path = url;
    return PAPP_E_OK;
}

/* Value of header `name` in the reply, or NULL; *len = its length */
static const char *find_header(const char *hdr, const char *name, int *len)
{
    size_t nl = strlen(name);
    const char *p = strstr(hdr, "\r\n");
    while (p && p[2] && !(p[2] == '\r' && p[3] == '\n')) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *v = p + nl + 1;
            while (*v == ' ' || *v == '\t') v++;
            const char *e = strstr(v, "\r\n");
            *len = e ? (int)(e - v) : (int)strlen(v);
            return v;
        }
        p = strstr(p, "\r\n");
    }
    return NULL;
}

static int body_raw(struct papp_http *h, void *dst, size_t n)
{
    if (h->pos >= h->len) {
        if (h->eof) return 0;
        deadline_t d = dl_make(h->timeout);
        int r = sock_recv(h->sock, h->buf, BODY_BUF, &d);
        if (r < 0) return r;
        if (r == 0) {
            h->eof = true;
            return 0;
        }
        h->pos = 0;
        h->len = r;
    }
    int k = h->len - h->pos < (int)n ? h->len - h->pos : (int)n;
    memcpy(dst, h->buf + h->pos, (size_t)k);
    h->pos += k;
    return k;
}

static int body_line(struct papp_http *h, char *line, int max)    /* chunk size lines */
{
    int n = 0;
    for (;;) {
        char c;
        int r = body_raw(h, &c, 1);
        if (r < 0) return r;
        if (r == 0) return PAPP_E_RESET;
        if (c == '\n') break;
        if (c != '\r' && n < max - 1) line[n++] = c;
    }
    line[n] = 0;
    return n;
}

static int http_request_once(struct papp_http *h, const char *method, const char *url, const papp_http_req_t *req,
                             bool send_body, char *location, int *status)
{
    bool tls;
    char host[HOST_MAX];
    uint16_t port;
    const char *path;
    int r = parse_url(url, &tls, host, &port, &path);
    if (r) return r;
    deadline_t d = dl_make(h->timeout);
    papp_tls_cfg_t tc = { .insecure = req->insecure };
    h->sock = open_stream(host, port, tls, &tc, &d, &r);
    if (!h->sock) return r;

    size_t cap = strlen(method) + strlen(path) + strlen(host) + (req->headers ? strlen(req->headers) : 0) + 256;
    char *q = malloc(cap);
    if (!q) return PAPP_E_NOMEM;
    bool std_port = port == (tls ? 443 : 80);
    int n = snprintf(q, cap, "%s %s%s HTTP/1.1\r\nHost: %s", method, *path == '/' ? "" : "/", path, host);
    if (!std_port) n += snprintf(q + n, cap - n, ":%u", port);
    n += snprintf(q + n, cap - n, "\r\nUser-Agent: P4-Launcher-App\r\nConnection: close\r\n");
    if (req->headers) n += snprintf(q + n, cap - n, "%s", req->headers);
    if (send_body) n += snprintf(q + n, cap - n, "Content-Length: %u\r\n", req->body ? (unsigned)req->body_len : 0u);
    n += snprintf(q + n, cap - n, "\r\n");
    d = dl_make(h->timeout);
    r = send_all(h->sock, q, (size_t)n, &d);
    free(q);
    if (!r && send_body && req->body && req->body_len) r = send_all(h->sock, req->body, req->body_len, &d);
    if (r) return r;

    /* Headers: up to the blank line; what follows is the start of the body */
    int got = 0;
    char *end = NULL;
    while (!end) {
        if (got >= HDR_MAX - 1) return PAPP_E_HTTP;
        d = dl_make(h->timeout);
        r = sock_recv(h->sock, h->hdr + got, (size_t)(HDR_MAX - 1 - got), &d);
        if (r < 0) return r;
        if (r == 0) return PAPP_E_HTTP;
        got += r;
        h->hdr[got] = 0;
        end = strstr(h->hdr, "\r\n\r\n");
    }
    int head = (int)(end - h->hdr) + 4;
    h->len = got - head;
    h->pos = 0;
    memcpy(h->buf, h->hdr + head, (size_t)h->len);
    h->hdr[head] = 0;
    if (strncmp(h->hdr, "HTTP/1.", 7) || !isdigit((unsigned char)h->hdr[9])) return PAPP_E_HTTP;
    *status = atoi(h->hdr + 9);

    int vl;
    const char *v = find_header(h->hdr, "Location", &vl);
    location[0] = 0;
    if (v && vl > 0 && vl < URL_MAX - HOST_MAX - 16) {
        if (!strncasecmp(v, "http://", 7) || !strncasecmp(v, "https://", 8)) {
            memcpy(location, v, (size_t)vl);
            location[vl] = 0;
        } else {                                    /* relative to this server */
            int k = snprintf(location, URL_MAX, "%s://%s", tls ? "https" : "http", host);
            if (!std_port) k += snprintf(location + k, URL_MAX - k, ":%u", port);
            snprintf(location + k, URL_MAX - k, "%s%.*s", *v == '/' ? "" : "/", vl, v);
        }
    }
    h->length = -1;
    if ((v = find_header(h->hdr, "Content-Length", &vl))) h->length = atol(v);
    h->left = h->length;
    h->chunked = (v = find_header(h->hdr, "Transfer-Encoding", &vl)) && vl >= 7 && !strncasecmp(v, "chunked", 7);
    h->chunk_left = 0;
    h->eof = h->done = false;
    if (!strcasecmp(method, "HEAD") || *status == 204 || *status == 304) h->done = true;
    return PAPP_E_OK;
}

static papp_http_t *net_http_open(const papp_http_req_t *req, int *status)
{
    SVC_ENTER();
    struct papp_http *h = NULL;
    char *url = NULL, *location = NULL;
    int r = hold(), st = 0;
    if (!r && (!req || !req->url || strlen(req->url) >= URL_MAX)) r = PAPP_E_ARG;
    if (!r) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < PAPP_NET_MAX_HTTP && !h; i++)
            if (!s_http[i].used) {
                h = &s_http[i];
                *h = (struct papp_http){ .used = true };
            }
        xSemaphoreGive(s_lock);
        if (!h) r = PAPP_E_LIMIT;
    }
    if (!r) {
        h->timeout = req->timeout_ms ? req->timeout_ms : DEF_TIMEOUT;
        h->hdr = malloc(HDR_MAX);
        h->buf = malloc(HDR_MAX > BODY_BUF ? HDR_MAX : BODY_BUF);   /* also holds the body after the headers */
        url = malloc(URL_MAX);
        location = malloc(URL_MAX);
        if (!h->hdr || !h->buf || !url || !location) r = PAPP_E_NOMEM;
    }
    if (!r) {
        strcpy(url, req->url);
        const char *method = req->method ? req->method : "GET";
        bool body = req->body || !strcasecmp(method, "POST") || !strcasecmp(method, "PUT") || !strcasecmp(method, "PATCH");
        int hops = req->max_redirects ? req->max_redirects : 5;
        for (;;) {
            r = http_request_once(h, method, url, req, body, location, &st);
            if (r) break;
            bool redirect = (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && location[0];
            if (!redirect) break;
            if (hops-- <= 0) { r = PAPP_E_HTTP; break; }
            if (st == 303 || ((st == 301 || st == 302) && strcasecmp(method, "GET") && strcasecmp(method, "HEAD"))) {
                method = "GET";
                body = false;
            }
            slot_close(h->sock);
            h->sock = NULL;
            strcpy(url, location);
        }
    }
    free(url);
    free(location);
    if (r && h) {
        http_free(h);
        h = NULL;
    }
    if (status) *status = r ? r : st;
    SVC_LEAVE();
    return (papp_http_t *)h;
}

static int net_http_header(papp_http_t *hh, const char *name, char *out, size_t out_len)
{
    SVC_ENTER();
    struct papp_http *h = http_get(hh);
    int r = -1, vl;
    const char *v = h && name ? find_header(h->hdr, name, &vl) : NULL;
    if (v) {
        r = vl;
        if (out && out_len) {
            size_t k = (size_t)vl < out_len - 1 ? (size_t)vl : out_len - 1;
            memcpy(out, v, k);
            out[k] = 0;
        }
    }
    SVC_LEAVE();
    return r;
}

static long net_http_length(papp_http_t *hh)
{
    struct papp_http *h = http_get(hh);
    return h && !h->chunked ? h->length : -1;
}

static int net_http_read(papp_http_t *hh, void *buf, size_t len)
{
    SVC_ENTER();
    struct papp_http *h = http_get(hh);
    int r = PAPP_E_ARG;
    if (h && buf && len) {
        if (h->done) {
            r = 0;
        } else if (!h->chunked) {
            size_t want = h->length >= 0 && (long)len > h->left ? (size_t)h->left : len;
            r = want ? body_raw(h, buf, want) : 0;
            if (r > 0 && h->length >= 0) h->left -= r;
            if (r == 0 && h->length >= 0 && h->left > 0) r = PAPP_E_RESET;    /* cut short */
            if (r == 0 || (h->length >= 0 && h->left == 0)) h->done = r >= 0;
        } else {
            char line[24];
            r = 0;
            if (h->chunk_left == 0) {
                r = body_line(h, line, sizeof(line));
                if (r >= 0) {
                    h->chunk_left = strtol(line, NULL, 16);
                    if (h->chunk_left <= 0) {             /* last chunk: skip the trailer */
                        while ((r = body_line(h, line, sizeof(line))) > 0) {}
                        h->done = true;
                    }
                    r = r < 0 ? r : 0;
                }
            }
            if (r >= 0 && !h->done) {
                size_t want = (long)len > h->chunk_left ? (size_t)h->chunk_left : len;
                r = body_raw(h, buf, want);
                if (r == 0) r = PAPP_E_RESET;
                if (r > 0 && (h->chunk_left -= r) == 0) {
                    int e = body_line(h, line, sizeof(line));      /* CRLF after the chunk */
                    if (e < 0) r = e;
                }
            }
        }
    }
    SVC_LEAVE();
    return r;
}

static void net_http_close(papp_http_t *hh)
{
    SVC_ENTER();
    struct papp_http *h = http_get(hh);
    if (h) http_free(h);
    SVC_LEAVE();
}

/* ── Interface, run bookkeeping ──────────────────────────────────────────── */

const papp_if_net_t lx_if_net = {
    .hdr = { PAPP_IF_NET, PAPP_IF_NET_VER, sizeof(papp_if_net_t) },
    .up = net_up,
    .info = net_info,
    .scan = net_scan,
    .join = net_join,
    .poll_event = net_poll_event,
    .ap_start = net_ap_start,
    .ap_stop = net_ap_stop,
    .resolve = net_resolve,
    .socket = net_socket,
    .connect = net_connect,
    .bind = net_bind,
    .listen = net_listen,
    .accept = net_accept,
    .send = net_send,
    .recv = net_recv,
    .sendto = net_sendto,
    .recvfrom = net_recvfrom,
    .set_opt = net_set_opt,
    .poll = net_poll,
    .local_addr = net_local_addr,
    .close = net_close,
    .tls_connect = net_tls_connect,
    .http_open = net_http_open,
    .http_header = net_http_header,
    .http_length = net_http_length,
    .http_read = net_http_read,
    .http_close = net_http_close,
};

void lx_svc_net_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    for (int i = 0; i < PAPP_NET_MAX_SOCKETS; i++) s_socks[i].fd = -1;
}

int lx_svc_net_end_run(void)
{
    int n = 0;
    for (int i = 0; i < PAPP_NET_MAX_HTTP; i++)
        if (s_http[i].used) {
            http_free(&s_http[i]);
            n++;
        }
    for (int i = 0; i < PAPP_NET_MAX_SOCKETS; i++)
        if (s_socks[i].used) {
            slot_close(&s_socks[i]);
            n++;
        }
    if (s_held) {
        s_held = false;
        lx_net_app_release();
    }
    return n;
}
