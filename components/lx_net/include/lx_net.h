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
} lx_net_ap_t;

esp_err_t lx_net_init(void);                /* task, time zone; joins a saved network */

lx_net_state_t lx_net_state(void);
const char *lx_net_ssid(void);              /* connected or saved network, "" if none */
const char *lx_net_ip(void);

void lx_net_scan(void);                     /* result: lx_net_scan_results */
int  lx_net_scan_results(lx_net_ap_t *out, int max);    /* -1 while scanning */
void lx_net_connect(const char *ssid, const char *password);    /* saved once connected */
void lx_net_forget(void);

/* The C6's firmware (lx_c6.h has the version and progress): read again, or install a file.
 * The update stops Wi-Fi, sends the image and restarts the board once the C6 took it. */
void lx_net_c6_refresh(void);
void lx_net_c6_update(const char *path);

/* While an app runs the radio is stopped, so it can't cost the app anything */
void lx_net_pause(bool pause);

/* Network self-test for DevLink: route, ping, TCP, DNS; text grows while busy */
void lx_net_diag_start(int mode);           /* 1 all, 2 link and gateway ping, 3 also scan */
bool lx_net_diag_busy(void);
const char *lx_net_diag_text(void);

int         lx_net_tz_count(void);
const char *lx_net_tz_name(int index);
int         lx_net_tz_get(void);            /* 0 = not set: clock shown as DevLink set it */
void        lx_net_tz_set(int index);
void        lx_net_time_sync(void);   /* NTP now (once connected); the result also goes to the RTC */
