/*
 * Wi-Fi station and network time for the launcher, through the ESP32-C6
 * companion (esp_hosted). The radio stays off until a network is saved or a scan
 * is asked for. Everything runs on the lx_net task (internal stack), so callers
 * only post requests and read the state.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define LX_NET_MAX_APS 16

typedef enum {
    LX_NET_OFF,             /* radio not started */
    LX_NET_IDLE,            /* started, not connected */
    LX_NET_SCANNING,
    LX_NET_CONNECTING,
    LX_NET_CONNECTED,
    LX_NET_FAILED,          /* wrong password or network not found */
    LX_NET_NO_RADIO,        /* the companion didn't answer */
} lx_net_state_t;

typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secure;
    uint8_t channel;
} lx_net_ap_t;

esp_err_t lx_net_init(void);                /* task, time zone; joins a saved network */

lx_net_state_t lx_net_state(void);
const char *lx_net_ssid(void);              /* connected or saved network, "" if none */
const char *lx_net_ip(void);

void lx_net_scan(void);                     /* result: lx_net_scan_results */
int  lx_net_scan_results(lx_net_ap_t *out, int max);    /* -1 while scanning */
void lx_net_connect(const char *ssid, const char *password);    /* saved once connected */
void lx_net_restore(void);                  /* back to the saved network (a connect failed or was given up) */
void lx_net_forget(void);

/* The C6's firmware (lx_c6.h has the version and progress): read again, or install a file.
 * The update stops Wi-Fi, sends the image and restarts the board once the C6 took it. */
void lx_net_c6_refresh(void);
void lx_net_c6_update(const char *path);

/* While an app runs the radio is stopped, so it can't cost the app anything */
void lx_net_pause(bool pause);

/* For apps (the NET interface in lx_loader). The first NET call holds the radio for the run:
 * pause is ignored until lx_net_app_release, which the teardown calls; it also stops the
 * SoftAP and goes back to the saved network after lx_net_app_join. The blocking calls run on
 * the lx_net task and give up when *abort turns true (the app is asked to exit). */
enum { LX_NET_EV_CONNECTED = 1, LX_NET_EV_DISCONNECTED, LX_NET_EV_SCAN_DONE, LX_NET_EV_AP_JOINED, LX_NET_EV_AP_LEFT };
typedef struct {
    uint8_t  type;          /* LX_NET_EV_* */
    uint16_t code;
    uint32_t time_ms;
} lx_net_app_event_t;

typedef struct {
    lx_net_state_t state;
    int8_t   rssi;
    uint8_t  channel, ap_clients;
    bool     ap_running;
    char     ssid[33];
    uint8_t  mac[6];
    uint32_t ip, mask, gw, dns, ap_ip;     /* lwIP order (first octet in the low byte) */
} lx_net_info_t;

esp_err_t lx_net_app_hold(volatile bool *abort);
void      lx_net_app_release(void);
esp_err_t lx_net_radio_up(volatile bool *abort);   /* the C6 link up (BLE), Wi-Fi not held */
bool      lx_net_joining(void);           /* a network is wanted (saved or joined) */
esp_err_t lx_net_app_join(const char *ssid, const char *password, volatile bool *abort);
bool      lx_net_app_event(lx_net_app_event_t *out);
esp_err_t lx_net_ap_start(const char *ssid, const char *password, uint8_t channel, uint8_t max_clients,
                          volatile bool *abort);
void      lx_net_ap_stop(void);
void      lx_net_info(lx_net_info_t *out);

/* Network self-test for DevLink: route, ping, TCP, DNS; text grows while busy */
void lx_net_diag_start(int mode);           /* 1 all, 2 link and gateway ping, 3 also scan */
bool lx_net_diag_busy(void);
const char *lx_net_diag_text(void);

int         lx_net_tz_count(void);
const char *lx_net_tz_name(int index);
int         lx_net_tz_get(void);            /* 0 = not set: clock shown as DevLink set it */
void        lx_net_tz_set(int index);
void        lx_net_time_sync(void);   /* NTP now (once connected); the result also goes to the RTC */
