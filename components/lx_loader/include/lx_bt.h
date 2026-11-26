/*
 * Bluetooth core (Bluedroid boards): one Bluedroid stack shared by its users, the app's BLE
 * interface (lx_svc_ble_bd.c), the launcher's Bluetooth speaker (lx_bt_speaker.c) and Bluetooth
 * gamepads (lx_bt_pads.c).
 *
 * Controller: a USB Bluetooth dongle when one is plugged in (dual mode: BLE and Classic), else the
 * C6 over esp_hosted (BLE only). The choice is made when the stack starts; it stops when the last
 * user lets go, or when the dongle it runs on is pulled out. Start and stop run on the "lx_bt" task
 * (internal stack: Bluedroid and NVS must not run on a PSRAM stack).
 *
 * Boards without Bluedroid get stubs: no dongle, no speaker.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { LX_BT_USER_APP = 1, LX_BT_USER_SPEAKER = 2, LX_BT_USER_PADS = 4 };

void        lx_bt_init(void);                   /* boot, after the USB host (lx_input_init) */
bool        lx_bt_supported(void);              /* this build has Bluedroid */

/* Takes the stack for `user`, starting it if needed. need_classic: only on a dongle (the C6 is
 * BLE only). abort (may be NULL) is polled while the C6 link comes up. */
bool        lx_bt_acquire(int user, bool need_classic, volatile bool *abort);
void        lx_bt_release(int user);
bool        lx_bt_running(void);
bool        lx_bt_classic(void);                /* running on a dongle */
uint32_t    lx_bt_generation(void);             /* +1 at every start: callbacks to register again */

/* USB dongle */
bool        lx_bt_dongle_present(void);
const char *lx_bt_dongle_name(void);            /* "" when none */

/* Bluetooth speaker (A2DP source). The launcher's audio mixer plays on it while it is connected. */
typedef enum {
    LX_BT_SPK_OFF,              /* none saved, or turned off */
    LX_BT_SPK_NO_DONGLE,        /* saved and on, waiting for a dongle */
    LX_BT_SPK_CONNECTING,
    LX_BT_SPK_CONNECTED,        /* linked, audio not started yet */
    LX_BT_SPK_PLAYING,
    LX_BT_SPK_RETRY,            /* not found: tries again every few seconds */
} lx_bt_spk_state_t;

typedef struct {
    uint8_t addr[6];            /* most significant byte first, as shown */
    char    name[48];
    int8_t  rssi;
    bool    audio;              /* class of device says audio (speaker, headphones) */
    bool    hid;                /* class of device / BLE advert says HID (gamepad, keyboard, mouse) */
    bool    ble;                /* found by the BLE scan (HID over GATT); else Classic */
    uint8_t addr_type;          /* BLE: 0 public, 1 random */
} lx_bt_found_t;

#define LX_BT_MAX_FOUND 16

lx_bt_spk_state_t lx_bt_speaker_state(void);
const char *lx_bt_speaker_name(void);           /* saved speaker, "" when none */
bool        lx_bt_speaker_scan(void);           /* inquiry + BLE scan for ~10 s; false: no dongle */
int         lx_bt_speaker_results(lx_bt_found_t *out, int max);    /* -1 while scanning */
void        lx_bt_speaker_pick(const uint8_t addr[6], const char *name);   /* save, pair, connect */
void        lx_bt_speaker_enable(bool on);      /* connect to / let go of the saved speaker */
bool        lx_bt_speaker_enabled(void);
void        lx_bt_speaker_forget(void);
const char *lx_bt_speaker_error(void);          /* last failure, "" when none */

/* Bluetooth gamepads: Classic HID host (DualShock 4, DualSense, Switch Pro, ...) and HID over GATT
 * (BLE pads: Xbox, 8BitDo in BLE mode, ...), on a dongle. Paired pads are saved in
 * /sd/config/bt_pads.txt; they reconnect by themselves (press the pad's home / PS button) while a
 * dongle is plugged in, and become players like USB pads. Scan with lx_bt_speaker_scan. */
#define LX_BT_MAX_PADS 4
typedef struct {
    uint8_t addr[6];
    char    name[48];
    bool    connected;
    bool    ble;
} lx_bt_pad_t;

int         lx_bt_pads_list(lx_bt_pad_t *out, int max);    /* saved pads */
int         lx_bt_pads_linked(void);            /* pads connected now */
void        lx_bt_pads_pair(const lx_bt_found_t *f);
bool        lx_bt_pads_pair_saved(const uint8_t addr[6]);  /* pair a saved pad again (new keys); false: not saved */
bool        lx_bt_pads_pairing(void);           /* a pairing is under way */
void        lx_bt_pads_forget_all(void);
const char *lx_bt_pads_error(void);             /* last failure, "" when none */

/* DevLink "bt" (test aid): status | scan | results | pair <addr> | speaker <addr> [name] |
 * speaker on | speaker off | forget-speaker | forget-pads. Returns text (JSON for status). */
const char *lx_bt_devlink(const char *cmd);

#ifdef __cplusplus
}
#endif
