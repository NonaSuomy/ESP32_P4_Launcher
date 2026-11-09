#include "lx_net.h"
#include "lx_c6.h"
#include "bsp.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
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

typedef enum { CMD_SCAN, CMD_CONNECT, CMD_FORGET, CMD_PAUSE, CMD_RESUME, CMD_RETRY, CMD_JOIN, CMD_C6_INFO, CMD_C6_UPDATE, CMD_RADIO,
               CMD_APP_HOLD, CMD_APP_RELEASE, CMD_APP_JOIN, CMD_AP_START, CMD_AP_STOP, CMD_RESTORE } cmd_t;

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
static char s_c6_path[96];
static esp_timer_handle_t s_retry_timer;

/* An app using NET holds the radio (no pause), may join another network for its run (s_temp:
   the saved one is kept in s_saved_*) and may run a SoftAP beside the station. */
static bool s_app, s_temp, s_saved_want, s_ap_on;
static char s_saved_ssid[33], s_saved_pass[65];
static char s_app_ssid[33], s_app_pass[65];
static struct { char ssid[33], pass[65]; uint8_t channel, max; } s_ap_req;
static esp_netif_t *s_ap_netif;
static volatile int s_ap_clients;
static QueueHandle_t s_app_ev;
static SemaphoreHandle_t s_call_lock, s_call_done;
static esp_err_t s_call_err;

static void app_event(uint8_t type, uint16_t code)
{
    if (!s_app || !s_app_ev) return;
    lx_net_app_event_t e = { .type = type, .code = code, .time_ms = (uint32_t)(esp_timer_get_time() / 1000) };
    if (xQueueSend(s_app_ev, &e, 0) != pdTRUE) {      /* full: the oldest goes */
        lx_net_app_event_t old;
        xQueueReceive(s_app_ev, &old, 0);
        xQueueSend(s_app_ev, &e, 0);
    }
}

static void post(cmd_t c)
{
    if (s_q) xQueueSend(s_q, &c, 0);
}

/* ── Saved network ───────────────────────────────────────────── */

/* The network lives on the SD card, so a card moved to another board takes it along:
 *   ssid=<name>
 *   password=<password>
 * Written when a network picked in Settings first connects, removed by Forget. Hand-written
 * files work too. Older launchers kept it in NVS ("net": ssid, pass): read once and moved to the card.
 * The time zone stays in NVS. */
#define WIFI_DIR   "/sd/config"
#define WIFI_FILE  WIFI_DIR "/wifi.txt"

static void copy_field(char *dst, size_t size, const char *src)    /* cut to the field's size */
{
    size_t n = strnlen(src, size - 1);
    memcpy(dst, src, n);
    dst[n] = 0;
}

static bool file_load(void)
{
    FILE *f = fopen(WIFI_FILE, "r");
    if (!f) return false;
    char line[128];
    s_ssid[0] = s_pass[0] = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;         /* only the line end: names may have spaces */
        if (!strncmp(line, "ssid=", 5))          copy_field(s_ssid, sizeof(s_ssid), line + 5);
        else if (!strncmp(line, "password=", 9)) copy_field(s_pass, sizeof(s_pass), line + 9);
    }
    fclose(f);
    ESP_LOGI(TAG, "network from %s: %s", WIFI_FILE, s_ssid[0] ? s_ssid : "(none)");
    return true;
}

static bool file_save(void)
{
    mkdir(WIFI_DIR, 0775);
    FILE *f = fopen(WIFI_FILE ".tmp", "w");
    if (!f) return false;
    fprintf(f, "# Wi-Fi network of the launcher (Settings -> Wi-Fi). Copy this file to another card to\n"
               "# set up a board, or delete it to forget the network.\n"
               "ssid=%s\npassword=%s\n", s_ssid, s_pass);
    bool ok = fclose(f) == 0;
    if (ok) {
        remove(WIFI_FILE);                      /* FAT rename doesn't replace */
        ok = rename(WIFI_FILE ".tmp", WIFI_FILE) == 0;
    }
    if (!ok) ESP_LOGW(TAG, "could not write %s", WIFI_FILE);
    return ok;
}

static void nvs_creds_erase(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    bool had = nvs_erase_key(h, "ssid") == ESP_OK;
    had |= nvs_erase_key(h, "pass") == ESP_OK;
    if (had) nvs_commit(h);
    nvs_close(h);
}

static void creds_load(void)
{
    bool from_file = file_load();
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return;
    if (!from_file) {                           /* saved by an older launcher */
        size_t a = sizeof(s_ssid), b = sizeof(s_pass);
        if (nvs_get_str(h, "ssid", s_ssid, &a) != ESP_OK) s_ssid[0] = 0;
        if (nvs_get_str(h, "pass", s_pass, &b) != ESP_OK) s_pass[0] = 0;
    }
    uint8_t tz;
    if (nvs_get_u8(h, "tz", &tz) == ESP_OK && tz < ZONES) s_tz = tz;
    nvs_close(h);
    if (!from_file && s_ssid[0] && file_save()) {
        ESP_LOGI(TAG, "network %s moved from NVS to %s", s_ssid, WIFI_FILE);
        nvs_creds_erase();
    }
}

static void creds_save(bool keep)
{
    if (keep) {
        if (file_save()) nvs_creds_erase();
    } else {
        remove(WIFI_FILE);
        nvs_creds_erase();
    }
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
        app_event(LX_NET_EV_DISCONNECTED, dis->reason);
        if (s_paused || !s_want) return;
        if (++s_retries > MAX_RETRIES && (s_save || s_temp)) {   /* a new or app network: give up */
            s_want = false;
            s_state = LX_NET_FAILED;
            ESP_LOGW(TAG, "could not join %s", s_ssid);
            return;
        }
        s_state = LX_NET_CONNECTING;
        post(CMD_RETRY);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        s_ap_clients++;
        app_event(LX_NET_EV_AP_JOINED, (uint16_t)s_ap_clients);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) s_ap_clients--;
        app_event(LX_NET_EV_AP_LEFT, (uint16_t)s_ap_clients);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        wifi_ap_record_t cur;           /* late, from the network an app's join just left */
        if (s_temp && esp_wifi_sta_get_ap_info(&cur) == ESP_OK && strcmp((char *)cur.ssid, s_ssid)) {
            ESP_LOGW(TAG, "address on %s while joining %s: ignored", (char *)cur.ssid, s_ssid);
            return;
        }
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retries = 0;
        s_state = LX_NET_CONNECTED;
        wifi_ap_record_t ap;            /* the C6 may rejoin a network it saved itself (another firmware's) */
        if (!s_ssid[0] && esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
            snprintf(s_ssid, sizeof(s_ssid), "%s", (char *)ap.ssid);
        ESP_LOGI(TAG, "connected to %s, %s", s_ssid, s_ip);
        app_event(LX_NET_EV_CONNECTED, 0);
        esp_netif_dns_info_t dns = { 0 };
        esp_netif_get_dns_info(ev->esp_netif, ESP_NETIF_DNS_MAIN, &dns);
        ESP_LOGI(TAG, "DNS " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        esp_netif_dns_info_t backup = { .ip.type = ESP_IPADDR_TYPE_V4 };
        backup.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
        esp_netif_set_dns_info(ev->esp_netif, dns.ip.u_addr.ip4.addr ? ESP_NETIF_DNS_BACKUP : ESP_NETIF_DNS_MAIN, &backup);
        if (s_save) {
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

/* Some C6 firmwares report STA_START twice (seen on a Guition 4.3" from a newer batch). IDF's default
 * handler then starts the netif again and lwIP asserts "netif already added". Registered before the
 * defaults, so it runs first: a repeated start stops the netif, and the default handler starts it cleanly. */
static esp_netif_t *s_sta;
static bool s_sta_up;

static void on_sta_start_stop(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_STOP) {
        s_sta_up = false;
        return;
    }
    if (s_sta_up && s_sta) {
        ESP_LOGW(TAG, "Wi-Fi chip reported start twice, restarting the interface");
        esp_netif_action_stop(s_sta, base, WIFI_EVENT_STA_STOP, NULL);
    }
    s_sta_up = true;
}

static bool radio_start(void)
{
    if (s_started) return true;
    esp_netif_init();
    esp_event_loop_create_default();
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START, on_sta_start_stop, NULL);
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP, on_sta_start_stop, NULL);
    s_sta = esp_netif_create_default_wifi_sta();
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
    lx_c6_query();
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
        s_aps[out].channel = rec[i].primary;
        out++;
    }
    free(rec);
    s_state = s_state == LX_NET_SCANNING ? was : s_state;
    s_ap_count = out;
    app_event(LX_NET_EV_SCAN_DONE, (uint16_t)out);
}

static esp_err_t ap_start(void)
{
    if (!radio_start()) return ESP_ERR_INVALID_STATE;
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) return ESP_ERR_NO_MEM;
    wifi_config_t c = { 0 };
    size_t n = strnlen(s_ap_req.ssid, sizeof(c.ap.ssid));
    memcpy(c.ap.ssid, s_ap_req.ssid, n);
    c.ap.ssid_len = n;
    memcpy(c.ap.password, s_ap_req.pass, strnlen(s_ap_req.pass, sizeof(c.ap.password)));
    c.ap.authmode = s_ap_req.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    c.ap.max_connection = s_ap_req.max ? s_ap_req.max : 4;
    c.ap.channel = s_ap_req.channel;
    wifi_ap_record_t ap;                /* beside a station, the AP must use its channel */
    if (s_state == LX_NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) c.ap.channel = ap.primary;
    if (!c.ap.channel) c.ap.channel = 1;
    esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (e == ESP_OK) e = esp_wifi_set_config(WIFI_IF_AP, &c);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "SoftAP failed: %s", esp_err_to_name(e));
        esp_wifi_set_mode(WIFI_MODE_STA);
        return e;
    }
    s_ap_on = true;
    s_ap_clients = 0;
    ESP_LOGI(TAG, "SoftAP %s on channel %d", s_ap_req.ssid, c.ap.channel);
    return ESP_OK;
}

static void ap_stop(void)
{
    if (!s_ap_on) return;
    s_ap_on = false;
    s_ap_clients = 0;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "SoftAP stopped");
}

static void retry_fire(void *arg) { post(CMD_JOIN); }

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
            s_temp = false;                 /* a saved network replaces what an app joined for its run */
            s_want = true;
            s_retries = 0;
            s_state = LX_NET_CONNECTING;
            if (s_started && !s_paused) esp_wifi_disconnect();
            if (radio_start()) join();
            break;
        case CMD_RETRY:                     /* a timer: the task stays free for other requests */
            esp_timer_stop(s_retry_timer);
            esp_timer_start_once(s_retry_timer, (s_retries > MAX_RETRIES ? 15000 : 1500) * 1000ULL);
            break;
        case CMD_JOIN:                      /* a retry armed by a disconnect that a new join already mended */
            if (s_want && !s_paused && s_state != LX_NET_CONNECTED) join();
            break;
        case CMD_APP_HOLD:
            s_app = true;
            s_call_err = radio_start() ? ESP_OK : ESP_FAIL;     /* BLE needs the C6 link too */
            if (s_call_err == ESP_OK && s_paused) {
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
            xSemaphoreGive(s_call_done);
            break;
        case CMD_APP_JOIN:
            if (!s_temp) {
                s_temp = true;
                s_saved_want = s_want;
                memcpy(s_saved_ssid, s_ssid, sizeof(s_ssid));
                memcpy(s_saved_pass, s_pass, sizeof(s_pass));
            }
            memcpy(s_ssid, s_app_ssid, sizeof(s_ssid));
            memcpy(s_pass, s_app_pass, sizeof(s_pass));
            s_save = false;
            s_want = true;
            s_retries = 0;
            s_state = LX_NET_CONNECTING;
            if (s_started && !s_paused) esp_wifi_disconnect();
            if (radio_start()) join();
            xSemaphoreGive(s_call_done);
            break;
        case CMD_RADIO:                     /* BLE: the C6 link without holding Wi-Fi */
            s_call_err = radio_start() ? ESP_OK : ESP_FAIL;
            xSemaphoreGive(s_call_done);
            break;
        case CMD_AP_START:
            s_call_err = ap_start();
            xSemaphoreGive(s_call_done);
            break;
        case CMD_AP_STOP:
            ap_stop();
            xSemaphoreGive(s_call_done);
            break;
        case CMD_APP_RELEASE:               /* the app ended: its SoftAP and network go */
            ap_stop();
            if (s_temp) {
                s_temp = false;
                memcpy(s_ssid, s_saved_ssid, sizeof(s_ssid));
                memcpy(s_pass, s_saved_pass, sizeof(s_pass));
                s_want = s_saved_want && s_ssid[0];
                if (s_started && !s_paused) esp_wifi_disconnect();
                if (s_want) {
                    s_retries = 0;
                    s_state = LX_NET_CONNECTING;
                    if (!s_paused) join();
                } else {
                    s_state = s_started ? LX_NET_IDLE : LX_NET_OFF;
                }
            }
            s_app = false;
            xQueueReset(s_app_ev);
            xSemaphoreGive(s_call_done);
            break;
        case CMD_RESTORE:                   /* a new network failed or was given up: the saved one again */
            s_save = false;
            if (!file_load()) s_ssid[0] = s_pass[0] = 0;
            s_want = s_ssid[0] != 0;
            if (s_started && !s_paused) esp_wifi_disconnect();
            if (s_want) {
                s_retries = 0;
                s_state = LX_NET_CONNECTING;
                if (!s_paused && radio_start()) join();
            } else {
                s_state = s_started ? LX_NET_IDLE : LX_NET_OFF;
            }
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
        case CMD_C6_INFO:
            if (radio_start() && !s_paused) lx_c6_query();
            break;
        case CMD_C6_UPDATE:
            if (!radio_start()) break;
            if (s_paused) {
                s_paused = false;
                esp_wifi_start();
            }
            esp_wifi_disconnect();          /* the link carries only the update now */
            lx_c6_update(s_c6_path);        /* restarts the board when it worked */
            if (s_want) {
                s_retries = 0;
                s_state = LX_NET_CONNECTING;
                join();
            }
            break;
        case CMD_PAUSE:
            if (s_app) break;               /* the app uses the network */
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
    creds_load();
    if (k_zones[s_tz].posix) {
        setenv("TZ", k_zones[s_tz].posix, 1);
        tzset();
    }
    s_q = xQueueCreate(12, sizeof(cmd_t));
    s_app_ev = xQueueCreate(16, sizeof(lx_net_app_event_t));
    s_call_lock = xSemaphoreCreateMutex();
    s_call_done = xSemaphoreCreateBinary();
    const esp_timer_create_args_t ta = { .callback = retry_fire, .name = "lx_net_retry" };
    if (!s_q || !s_app_ev || !s_call_lock || !s_call_done || esp_timer_create(&ta, &s_retry_timer) != ESP_OK)
        return ESP_ERR_NO_MEM;
    if (xTaskCreatePinnedToCore(net_task, "lx_net", TASK_STACK, NULL, 4, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

lx_net_state_t lx_net_state(void) { return s_state; }
const char *lx_net_ssid(void)     { return s_ssid; }
const char *lx_net_ip(void)       { return s_ip; }

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
    s_state = LX_NET_CONNECTING;        /* now, so a caller polling the state doesn't see the old one */
    post(CMD_CONNECT);
}

void lx_net_restore(void) { post(CMD_RESTORE); }

void lx_net_forget(void)      { post(CMD_FORGET); }
void lx_net_c6_refresh(void)  { post(CMD_C6_INFO); }

void lx_net_c6_update(const char *path)
{
    snprintf(s_c6_path, sizeof(s_c6_path), "%s", path);
    post(CMD_C6_UPDATE);
}
void lx_net_pause(bool pause) { post(pause ? CMD_PAUSE : CMD_RESUME); }

/* For apps (NET, lx_loader) */

/* Runs a request on the lx_net task and waits for it; gives up when *abort is set */
static esp_err_t call(cmd_t c, volatile bool *abort)
{
    if (!s_q) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_call_lock, portMAX_DELAY);
    xSemaphoreTake(s_call_done, 0);
    s_call_err = ESP_OK;
    esp_err_t e = ESP_ERR_TIMEOUT;
    if (xQueueSend(s_q, &c, pdMS_TO_TICKS(1000)) == pdTRUE) {
        for (int i = 0; i < 300; i++) {                     /* 30 s at most (a scan in progress) */
            if (xSemaphoreTake(s_call_done, pdMS_TO_TICKS(100)) == pdTRUE) {
                e = s_call_err;
                break;
            }
            if (abort && *abort) break;
        }
    }
    xSemaphoreGive(s_call_lock);
    return e;
}

esp_err_t lx_net_app_hold(volatile bool *abort)
{
    return s_app ? ESP_OK : call(CMD_APP_HOLD, abort);
}

esp_err_t lx_net_radio_up(volatile bool *abort) { return s_started ? ESP_OK : call(CMD_RADIO, abort); }

void lx_net_app_release(void)
{
    if (s_app) call(CMD_APP_RELEASE, NULL);
}

bool lx_net_joining(void) { return s_want; }

esp_err_t lx_net_app_join(const char *ssid, const char *password, volatile bool *abort)
{
    copy_field(s_app_ssid, sizeof(s_app_ssid), ssid);
    copy_field(s_app_pass, sizeof(s_app_pass), password ? password : "");
    return call(CMD_APP_JOIN, abort);
}

bool lx_net_app_event(lx_net_app_event_t *out)
{
    return s_app_ev && xQueueReceive(s_app_ev, out, 0) == pdTRUE;
}

esp_err_t lx_net_ap_start(const char *ssid, const char *password, uint8_t channel, uint8_t max_clients,
                          volatile bool *abort)
{
    copy_field(s_ap_req.ssid, sizeof(s_ap_req.ssid), ssid);
    copy_field(s_ap_req.pass, sizeof(s_ap_req.pass), password ? password : "");
    s_ap_req.channel = channel;
    s_ap_req.max = max_clients;
    return call(CMD_AP_START, abort);
}

void lx_net_ap_stop(void) { call(CMD_AP_STOP, NULL); }

void lx_net_info(lx_net_info_t *o)
{
    memset(o, 0, sizeof(*o));
    o->state = s_state;
    memcpy(o->ssid, s_ssid, sizeof(o->ssid));
    o->ap_running = s_ap_on;
    o->ap_clients = (uint8_t)s_ap_clients;
    if (!s_started || s_paused) return;
    wifi_ap_record_t ap;
    if (s_state == LX_NET_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        o->rssi = ap.rssi;
        o->channel = ap.primary;
    }
    esp_wifi_get_mac(WIFI_IF_STA, o->mac);
    esp_netif_ip_info_t ip;
    if (s_sta && esp_netif_get_ip_info(s_sta, &ip) == ESP_OK) {
        o->ip = ip.ip.addr;
        o->mask = ip.netmask.addr;
        o->gw = ip.gw.addr;
        esp_netif_dns_info_t d;
        if (esp_netif_get_dns_info(s_sta, ESP_NETIF_DNS_MAIN, &d) == ESP_OK) o->dns = d.ip.u_addr.ip4.addr;
    }
    if (s_ap_on && s_ap_netif && esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) o->ap_ip = ip.ip.addr;
}

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
    wifi_ps_type_t ps = WIFI_PS_NONE;
#if LX_HOSTED_MAJOR >= 2
    esp_wifi_get_ps(&ps);
#endif
    diag("tx power %.2f dBm, bandwidth %d, protocol 0x%x, power save %d, ap phy b%d g%d n%d ax%d\n", power / 4.0,
         (int)bw, proto, (int)ps, ap.phy_11b, ap.phy_11g, ap.phy_11n, ap.phy_11ax);
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
