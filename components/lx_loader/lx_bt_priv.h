/* Between the Bluetooth core (lx_bt.c), the USB dongle transport (lx_bt_usb.c) and the speaker
 * (lx_bt_speaker.c). Bluedroid boards only. */
#pragma once

#include "lx_bt.h"
#include "esp_bluedroid_hci.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "lx_input.h"

/* lx_bt.c: jobs on the "lx_bt" task (internal stack) */
int  lx_bt_run(int (*fn)(void *), void *arg);       /* waits for the result */
void lx_bt_post(void (*fn)(void *), void *arg);     /* does not wait */
void lx_bt_transport_lost(void);                    /* the dongle went away under the stack */
bool lx_bt_app_holds(void);                         /* an app has the stack (its BLE interface) */

/* BLE GAP and GATT client events: Bluedroid takes one callback of each, lx_bt.c hands every event
 * to each of these (registered once, they stay across stack restarts). GATT client handlers check
 * gattc_if: each registers its own app. */
enum { LX_BT_BLE_APP, LX_BT_BLE_PADS, LX_BT_BLE_SCAN, LX_BT_BLE_TEST, LX_BT_BLE_N };
void lx_bt_ble_handlers(int slot, esp_gap_ble_cb_t gap, esp_gattc_cb_t gattc);

/* lx_bt_usb.c */
void lx_bt_usb_init(void);                          /* registers the USB host client */
bool lx_bt_usb_present(void);
const char *lx_bt_usb_name(void);
int  lx_bt_usb_unacked(char *buf, int size);     /* ACL packets the dongle has not completed, per link (text) */
const esp_bluedroid_hci_driver_operations_t *lx_bt_usb_ops(void);
bool lx_bt_usb_open(void);                          /* before Bluedroid starts on it: IN transfers go */
void lx_bt_usb_close(void);                         /* after Bluedroid stopped */
const char *lx_bt_usb_why(void);                    /* why the last open failed, or "" */

/* lx_svc_ble_bd.c: on the lx_bt task, before Bluedroid stops */
void lx_bt_ble_stack_down(void);

/* lx_bt_speaker.c */
void lx_bt_speaker_init(void);
void lx_bt_speaker_stack_up(void);                  /* on the lx_bt task, Bluedroid just started on a dongle */
void lx_bt_speaker_stack_down(void);                /* on the lx_bt task, before Bluedroid stops */
void lx_bt_speaker_dongle(bool present);            /* from the USB client task */
void lx_bt_speaker_pause(bool on);                  /* nested: no reconnect attempts while > 0 */
void lx_bt_speaker_wait_page(void);                 /* lx_bt task: until a connect attempt in progress ends */
bool lx_bt_speaker_paging(void);                    /* a call to the speaker pages now (no LE connection requests then) */
bool lx_bt_speaker_scanning(void);                  /* the pairing scan (inquiry + BLE) runs */

/* lx_bt_testpad.c (build with -DLX_BT_TESTPAD=1): this board poses as a BLE gamepad */
void lx_bt_testpad_init(void);

/* lx_bt_pads.c */
void lx_bt_pads_init(void);
bool lx_bt_pads_owns(const uint8_t *bda);           /* a pad's link (BLE): not the app's */
void lx_bt_pads_app(bool active);                   /* lx_bt task: an app takes / lets go of BLE */
void lx_bt_pads_cancel_open(void);                  /* lx_bt task: a pending BLE pad request makes way (scan) */
uint32_t lx_bt_speaker_taken(void);             /* audio frames Bluedroid took for the speaker (status) */
uint32_t lx_bt_usb_sent(void);                  /* ACL packets sent to the dongle (status) */
uint32_t lx_bt_pads_still_ms(void);                /* since any connected pad's last input change (UINT32_MAX: none) */
bool lx_bt_pads_stats(const uint8_t *addr, lx_hidpad_stats_t *out);   /* a connected pad's report timing */
void lx_bt_pads_stack_up(void);                     /* on the lx_bt task, Bluedroid just started on a dongle */
void lx_bt_pads_stack_down(void);                   /* on the lx_bt task, before Bluedroid stops */
void lx_bt_pads_dongle(bool present);               /* from the USB client task */
