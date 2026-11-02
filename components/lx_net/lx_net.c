#include "lx_net.h"
#include "bsp.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "ping/ping_sock.h"

static const char *TAG = "lx_net";

#define NS            "net"
#define MAX_RETRIES   4
#define TASK_STACK    6144

typedef enum { CMD_SCAN, CMD_CONNECT, CMD_FORGET, CMD_PAUSE, CMD_RESUME, CMD_RETRY } cmd_t;

static const struct { const char *name, *posix; } k_zones[] = {
    { "Not set", NULL },
    { "UTC", "UTC0" },
    { "London", "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Central Europe", "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Israel", "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "Eastern Europe", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "India", "IST-5:30" },
    { "China", "CST-8" },
    { "Japan", "JST-9" },
    { "Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "New York", "EST5EDT,M3.2.0,M11.1.0" },
    { "Chicago", "CST6CDT,M3.2.0,M11.1.0" },
    { "Denver", "MST7MDT,M3.2.0,M11.1.0" },
    { "Los Angeles", "PST8PDT,M3.2.0,M11.1.0" },
};
#define ZONES ((int)(sizeof(k_zones) / sizeof(k_zones[0])))

static QueueHandle_t s_q;
static volatile lx_net_state_t s_state = LX_NET_OFF;
static bool s_started, s_paused, s_want, s_save, s_sntp;
static int s_retries, s_tz, s_drops, s_last_reason;
static char s_ssid[33], s_pass[65], s_ip[16];
static lx_net_ap_t s_aps[LX_NET_MAX_APS];
static volatile int s_ap_count = -1;

static void post(cmd_t c)
{
    if (s_q) xQueueSend(s_q, &c, 0);
}

/* ── Saved network ───────────────────────────────────────────── */

static void creds_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t a = sizeof(s_ssid), b = sizeof(s_pass);
    if (nvs_get_str(h, "ssid", s_ssid, &a) != ESP_OK) s_ssid[0] = 0;
    if (nvs_get_str(h, "pass", s_pass, &b) != ESP_OK) s_pass[0] = 0;
    uint8_t tz;
    if (nvs_get_u8(h, "tz", &tz) == ESP_OK && tz < ZONES) s_tz = tz;
    nvs_close(h);
}

static void creds_save(bool keep)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (keep) {
        nvs_set_str(h, "ssid", s_ssid);
        nvs_set_str(h, "pass", s_pass);
    } else {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
    }
    nvs_commit(h);
    nvs_close(h);
}

/* ── Events ──────────────────────────────────────────────────── */

/* Network time is real UTC: keep it in the board's RTC for the next power-on */
static void on_sntp_sync(struct timeval *tv)
{
    if (bsp_rtc_set(tv->tv_sec) == ESP_OK) ESP_LOGI(TAG, "time from NTP, saved to the RTC");
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip[0] = 0;
        const wifi_event_sta_disconnected_t *dis = data;
        ESP_LOGW(TAG, "disconnected, reason %d, rssi %d", dis->reason, dis->rssi);
        s_drops++;
        s_last_reason = dis->reason;
        if (s_paused || !s_want) return;
        if (++s_retries > MAX_RETRIES && s_save) {      /* a saved network is worth waiting for */
            s_want = false;
            s_state = LX_NET_FAILED;
            ESP_LOGW(TAG, "could not join %s", s_ssid);
            return;
        }
        s_state = LX_NET_CONNECTING;
        post(CMD_RETRY);
    } else if (base == IP_EVENT && (id == IP_EVENT_STA_GOT_IP || id == IP_EVENT_ETH_GOT_IP)) {
        const ip_event_got_ip_t *ev = data;
        if(id==IP_EVENT_STA_GOT_IP)snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        if(id==IP_EVENT_STA_GOT_IP)s_state = LX_NET_CONNECTED;
        wifi_ap_record_t ap;            /* the C6 may rejoin a network it saved itself (another firmware's) */
        if (id==IP_EVENT_STA_GOT_IP && !s_ssid[0] && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            snprintf(s_ssid, sizeof(s_ssid), "%s", (char *)ap.ssid);
        ESP_LOGI(TAG, "connected to %s, %s", s_ssid, s_ip);
        esp_netif_dns_info_t dns = { 0 };
        esp_netif_get_dns_info(ev->esp_netif, ESP_NETIF_DNS_MAIN, &dns);
        ESP_LOGI(TAG, "DNS " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        esp_netif_dns_info_t backup = { .ip.type = ESP_IPADDR_TYPE_V4 };
        backup.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
        esp_netif_set_dns_info(ev->esp_netif, dns.ip.u_addr.ip4.addr ? ESP_NETIF_DNS_BACKUP : ESP_NETIF_DNS_MAIN, &backup);
        if (id==IP_EVENT_STA_GOT_IP && s_save) {
            s_save = false;
            creds_save(true);
        }
        if (!s_sntp) {
            s_sntp = true;
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            cfg.sync_cb = on_sntp_sync;
            esp_netif_sntp_init(&cfg);
        }
    }
}

static bool radio_start(void)
{
    if (s_started) return true;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t e = esp_wifi_init(&cfg);
    if (e == ESP_OK) {
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        esp_wifi_set_mode(WIFI_MODE_STA);
        e = esp_wifi_start();
    }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "radio start failed: %s", esp_err_to_name(e));
        s_state = LX_NET_NO_RADIO;
        return false;
    }
    s_started = true;
    if (s_state == LX_NET_OFF) s_state = LX_NET_IDLE;
    return true;
}

static void join(void)
{
    wifi_config_t c = { 0 };
    memcpy(c.sta.ssid, s_ssid, strnlen(s_ssid, sizeof(c.sta.ssid)));   /* may fill the field */
    memcpy(c.sta.password, s_pass, strnlen(s_pass, sizeof(c.sta.password)));
    esp_wifi_set_config(WIFI_IF_STA, &c);
    esp_wifi_connect();
}

static void do_scan(void)
{
    s_ap_count = -1;
    if (!radio_start()) {
        s_ap_count = 0;
        return;
    }
    lx_net_state_t was = s_state;
    if (was == LX_NET_CONNECTING) esp_wifi_disconnect();    /* a scan is refused during a join */
    s_state = LX_NET_SCANNING;
    uint16_t found = 0, n = LX_NET_MAX_APS;
    wifi_ap_record_t *rec = calloc(n, sizeof(*rec));
    if (rec && esp_wifi_scan_start(NULL, true) == ESP_OK) {
        esp_wifi_scan_get_ap_num(&found);
        esp_wifi_scan_get_ap_records(&n, rec);
    } else {
        n = 0;
    }
    int out = 0;
    for (int i = 0; i < n; i++) {           /* strongest first; one line per name */
        bool dup = !rec[i].ssid[0];
        for (int j = 0; j < out; j++) dup |= strcmp(s_aps[j].ssid, (char *)rec[i].ssid) == 0;
        if (dup) continue;
        snprintf(s_aps[out].ssid, sizeof(s_aps[out].ssid), "%s", (char *)rec[i].ssid);
        s_aps[out].rssi = rec[i].rssi;
        s_aps[out].secure = rec[i].authmode != WIFI_AUTH_OPEN;
        out++;
    }
    free(rec);
    s_state = s_state == LX_NET_SCANNING ? was : s_state;
    s_ap_count = out;
}

static void net_task(void *arg)
{
    if (s_ssid[0]) {
        s_want = true;
        s_state = LX_NET_CONNECTING;
        if (radio_start()) join();
    }
    for (;;) {
        cmd_t c;
        if (!xQueueReceive(s_q, &c, portMAX_DELAY)) continue;
        switch (c) {
        case CMD_SCAN:
            do_scan();
            break;
        case CMD_CONNECT:
            s_want = true;
            s_retries = 0;
            s_state = LX_NET_CONNECTING;
            if (s_started && !s_paused) esp_wifi_disconnect();
            if (radio_start()) join();
            break;
        case CMD_RETRY:
            vTaskDelay(pdMS_TO_TICKS(s_retries > MAX_RETRIES ? 15000 : 1500));
            if (s_want && !s_paused) join();
            break;
        case CMD_FORGET:
            s_want = false;
            s_ssid[0] = s_pass[0] = 0;
            creds_save(false);
            if (s_started) {
                esp_wifi_disconnect();
                esp_wifi_stop();
                esp_wifi_restore();     /* and whatever the C6 saved for itself, or it rejoins at boot */
                esp_wifi_set_mode(WIFI_MODE_STA);
                esp_wifi_start();
            }
            s_state = s_started ? LX_NET_IDLE : LX_NET_OFF;
            break;
        case CMD_PAUSE:
            if (s_started && !s_paused) {
                s_paused = true;
                esp_wifi_stop();
            }
            break;
        case CMD_RESUME:
            if (s_paused) {
                s_paused = false;
                esp_wifi_start();
                if (s_want) {
                    s_retries = 0;
                    s_state = LX_NET_CONNECTING;
                    join();
                } else if (s_state != LX_NET_FAILED) {
                    s_state = LX_NET_IDLE;
                }
            }
            break;
        }
    }
}

/* ── Public ──────────────────────────────────────────────────── */

esp_err_t lx_net_init(void)
{
    esp_netif_init();
    esp_err_t loop=esp_event_loop_create_default();
    if(loop!=ESP_OK && loop!=ESP_ERR_INVALID_STATE)return loop;
    esp_event_handler_register(IP_EVENT,IP_EVENT_ETH_GOT_IP,on_event,NULL);
    esp_err_t wired=bsp_ethernet_init();
    if(wired!=ESP_OK && wired!=ESP_ERR_NOT_SUPPORTED)ESP_LOGW(TAG,"Ethernet init: %s",esp_err_to_name(wired));
    creds_load();
    if (k_zones[s_tz].posix) {
        setenv("TZ", k_zones[s_tz].posix, 1);
        tzset();
    }
    s_q = xQueueCreate(8, sizeof(cmd_t));
    if (!s_q) return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(net_task, "lx_net", TASK_STACK, NULL, 4, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

lx_net_state_t lx_net_state(void) { return bsp_ethernet_ip()[0]?LX_NET_CONNECTED:s_state; }
const char *lx_net_ssid(void)     { return bsp_ethernet_ip()[0]?"Ethernet":s_ssid; }
const char *lx_net_ip(void)       { const char *eth=bsp_ethernet_ip();return eth[0]?eth:s_ip; }

void lx_net_scan(void)
{
    s_ap_count = -1;
    post(CMD_SCAN);
}

int lx_net_scan_results(lx_net_ap_t *out, int max)
{
    int n = s_ap_count;
    if (n < 0) return -1;
    if (n > max) n = max;
    memcpy(out, s_aps, (size_t)n * sizeof(*out));
    return n;
}

void lx_net_connect(const char *ssid, const char *password)
{
    snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
    snprintf(s_pass, sizeof(s_pass), "%s", password ? password : "");
    s_save = true;
    post(CMD_CONNECT);
}

void lx_net_forget(void)      { post(CMD_FORGET); }
void lx_net_pause(bool pause) { post(pause ? CMD_PAUSE : CMD_RESUME); }

int         lx_net_tz_count(void)      { return ZONES; }
const char *lx_net_tz_name(int index)  { return index >= 0 && index < ZONES ? k_zones[index].name : ""; }
int         lx_net_tz_get(void)        { return s_tz; }

void lx_net_time_sync(void)
{
    if (s_sntp) esp_sntp_restart();         /* on_sntp_sync saves it to the RTC */
}

void lx_net_tz_set(int index)
{
    if (index < 0 || index >= ZONES) return;
    s_tz = index;
    if (k_zones[index].posix) setenv("TZ", k_zones[index].posix, 1);
    else unsetenv("TZ");
    tzset();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "tz", (uint8_t)index);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ── Diagnostic (DevLink "net") ──────────────────────────────── */

static char s_diag[1536];
static volatile bool s_diag_busy;

static void diag(const char *fmt, ...)
{
    size_t n = strlen(s_diag);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_diag + n, sizeof(s_diag) - n, fmt, ap);
    va_end(ap);
}

static void ping_end(esp_ping_handle_t h, void *arg)
{
    xSemaphoreGive((SemaphoreHandle_t)arg);
}

static void diag_ping(const char *label, uint32_t addr)
{
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = addr;
    cfg.count = 5;
    cfg.interval_ms = 300;
    cfg.timeout_ms = 1000;
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    esp_ping_callbacks_t cb = { .cb_args = done, .on_ping_end = ping_end };
    esp_ping_handle_t h = NULL;
    if (!done || esp_ping_new_session(&cfg, &cb, &h) != ESP_OK) {
        diag("ping %s: could not start\n", label);
    } else {
        esp_ping_start(h);
        xSemaphoreTake(done, pdMS_TO_TICKS(10000));
        esp_ping_stop(h);
        uint32_t sent = 0, got = 0, ms = 0;
        esp_ping_get_profile(h, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
        esp_ping_get_profile(h, ESP_PING_PROF_REPLY, &got, sizeof(got));
        esp_ping_get_profile(h, ESP_PING_PROF_DURATION, &ms, sizeof(ms));
        diag("ping %s: %lu of %lu replies, %lu ms total\n", label, (unsigned long)got, (unsigned long)sent, (unsigned long)ms);
        esp_ping_delete_session(h);
    }
    if (done) vSemaphoreDelete(done);
}

/* Connects; with a request, also waits for the first reply bytes */
static void diag_tcp(const char *label, uint32_t addr, int port, const char *request)
{
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = addr };
    struct timeval tv = { .tv_sec = 6 };
    int s = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int64_t t0 = esp_timer_get_time();
    if (connect(s, (struct sockaddr *)&to, sizeof(to)) != 0) {
        diag("tcp %s:%d: connect failed, errno %d, %d ms\n", label, port, errno, (int)((esp_timer_get_time() - t0) / 1000));
    } else {
        diag("tcp %s:%d: connected, %d ms", label, port, (int)((esp_timer_get_time() - t0) / 1000));
        if (request) {
            char buf[32];
            send(s, request, strlen(request), 0);
            int n = recv(s, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = 0;
                buf[strcspn(buf, "\r\n")] = 0;
                diag(", reply \"%s\"", buf);
            } else {
                diag(", no reply (%d, errno %d)", n, errno);
            }
        }
        diag("\n");
    }
    close(s);
}

static void diag_task(void *arg)
{
    s_diag[0] = 0;
    if (!s_started || s_paused) {       /* Wi-Fi calls on a stopped radio crash */
        diag("state %d: radio off (no saved network and no scan yet, or an app is running)\ndone\n", (int)s_state);
        s_diag_busy = false;
        vTaskDelete(NULL);
    }
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    wifi_ap_record_t ap = { 0 };
    esp_netif_ip_info_t ip = { 0 };
    esp_netif_dns_info_t d1 = { 0 }, d2 = { 0 };
    diag("state %d, ssid %s, ip %s\n", (int)s_state, s_ssid, s_ip);
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        diag("ap %s " MACSTR ", channel %d, rssi %d, auth %d, drops %d, last reason %d\n", (char *)ap.ssid, MAC2STR(ap.bssid),
             ap.primary, ap.rssi, ap.authmode, s_drops, s_last_reason);
    if (sta) {
        esp_netif_get_ip_info(sta, &ip);
        esp_netif_get_dns_info(sta, ESP_NETIF_DNS_MAIN, &d1);
        esp_netif_get_dns_info(sta, ESP_NETIF_DNS_BACKUP, &d2);
        diag("mask " IPSTR ", gateway " IPSTR ", dns " IPSTR " + " IPSTR "\n", IP2STR(&ip.netmask), IP2STR(&ip.gw),
             IP2STR(&d1.ip.u_addr.ip4), IP2STR(&d2.ip.u_addr.ip4));
    }
    int8_t power = 0;
    wifi_bandwidth_t bw = 0;
    uint8_t proto = 0;
    esp_wifi_get_max_tx_power(&power);
    esp_wifi_get_bandwidth(WIFI_IF_STA, &bw);
    esp_wifi_get_protocol(WIFI_IF_STA, &proto);
    diag("tx power %.2f dBm, bandwidth %d, protocol 0x%x, ap phy b%d g%d n%d ax%d\n", power / 4.0, (int)bw, proto,
         ap.phy_11b, ap.phy_11g, ap.phy_11n, ap.phy_11ax);
    uint32_t one = ESP_IP4TOADDR(1, 1, 1, 1);
    diag_ping("gateway", ip.gw.addr);
    if ((int)arg != 1) {
        if ((int)arg == 3) {
            uint16_t n = 12;
            wifi_ap_record_t *rec = calloc(n, sizeof(*rec));
            if (rec && esp_wifi_scan_start(NULL, true) == ESP_OK) {
                esp_wifi_scan_get_ap_records(&n, rec);
                for (int i = 0; i < n && rec[i].ssid[0]; i++) diag("scan %s ch %d rssi %d\n", (char *)rec[i].ssid, rec[i].primary, rec[i].rssi);
            }
            free(rec);
        }
        diag("done\n");
        s_diag_busy = false;
        vTaskDelete(NULL);
    }
    diag_ping("1.1.1.1", one);
    diag_tcp("gateway", ip.gw.addr, 80, "GET / HTTP/1.0\r\n\r\n");
    diag_tcp("1.1.1.1", one, 443, NULL);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    int64_t t0 = esp_timer_get_time();
    int e = getaddrinfo("github.com", NULL, &hints, &res);
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (e == 0 && res) {
        struct sockaddr_in *a = (struct sockaddr_in *)res->ai_addr;
        diag("dns github.com: %s, %d ms\n", inet_ntoa(a->sin_addr), ms);
        diag_tcp("github.com", a->sin_addr.s_addr, 443, NULL);
        freeaddrinfo(res);
    } else {
        diag("dns github.com: failed (%d), %d ms\n", e, ms);
    }
    diag("done\n");
    s_diag_busy = false;
    vTaskDelete(NULL);
}

void lx_net_diag_start(int mode)
{
    if (s_diag_busy) return;
    s_diag_busy = true;
    s_diag[0] = 0;
    if (xTaskCreate(diag_task, "lx_netdiag", 6144, (void *)mode, 3, NULL) != pdPASS) s_diag_busy = false;
}

bool lx_net_diag_busy(void)     { return s_diag_busy; }
const char *lx_net_diag_text(void) { return s_diag; }
