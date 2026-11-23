/* Between the Bluetooth core (lx_bt.c), the USB dongle transport (lx_bt_usb.c) and the speaker
 * (lx_bt_speaker.c). Bluedroid boards only. */
#pragma once

#include "lx_bt.h"
#include "esp_bluedroid_hci.h"

/* lx_bt.c: jobs on the "lx_bt" task (internal stack) */
int  lx_bt_run(int (*fn)(void *), void *arg);       /* waits for the result */
void lx_bt_post(void (*fn)(void *), void *arg);     /* does not wait */
void lx_bt_transport_lost(void);                    /* the dongle went away under the stack */

/* lx_bt_usb.c */
void lx_bt_usb_init(void);                          /* registers the USB host client */
bool lx_bt_usb_present(void);
const char *lx_bt_usb_name(void);
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
