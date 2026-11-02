/*
 * Board Support Package contract.
 *
 * The launcher core only ever talks to the hardware through this header. Each
 * board implements it in boards/<board_id>/board/ (component name "board"),
 * selected at configure time with -DLX_BOARD=<board_id>.
 * See docs/ARCHITECTURE.md §7.1. Grows phase by phase (display, touch, audio, buttons).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Board capability flags */
#define BSP_CAP_SDCARD    (1u << 0)
#define BSP_CAP_DISPLAY   (1u << 1)
#define BSP_CAP_TOUCH     (1u << 2)
#define BSP_CAP_AUDIO     (1u << 3)
#define BSP_CAP_GPIO_PAD  (1u << 4)
#define BSP_CAP_PADDLE    (1u << 5)
#define BSP_CAP_USB_HOST  (1u << 6)
#define BSP_CAP_BATTERY   (1u << 7)
#define BSP_CAP_RTC       (1u << 8)

typedef struct {
    const char *id;            /* "guition_p4_43"                       */
    const char *name;          /* human readable                        */
    uint32_t    caps;          /* BSP_CAP_*                             */
    uint16_t    display_w;     /* panel size in its native orientation  */
    uint16_t    display_h;
    uint16_t    display_rot;   /* how landscape content maps onto the panel:
                                  0 = as is, 90 / 270 = panel is portrait; a landscape
                                  pixel (x, y) lands at panel (y, W-1-x) for 90 and
                                  (H-1-y, x) for 270, W x H = landscape size */
} bsp_info_t;

/* Early board init (power rails, shared buses). Call once at boot. */
esp_err_t bsp_init(void);

const bsp_info_t *bsp_info(void);

/* Mount the SD card FAT filesystem at mount_point (e.g. "/sd"). */
esp_err_t bsp_sdcard_mount(const char *mount_point);
bool      bsp_sdcard_mounted(void);

/* Optional wired network; call after esp_netif/event-loop initialization.
 * Returns NOT_SUPPORTED on boards without it. The IP string is empty until DHCP. */
esp_err_t bsp_ethernet_init(void);
const char *bsp_ethernet_ip(void);

/* ── Display ───────────────────────────────────────────────────────────────
 * The panel scans out one of bsp_display_fb_count() RGB565 framebuffers
 * (display_w x display_h, native orientation, DMA-capable PSRAM).
 * Draw into a hidden one, then bsp_display_show() it: the switch happens at
 * the next refresh, so there is no tearing. */
esp_err_t bsp_display_init(void);
int       bsp_display_fb_count(void);
uint16_t *bsp_display_fb(int index);
esp_err_t bsp_display_show(int index);          /* writes back cache, flips    */
int       bsp_display_visible(void);            /* index currently on screen  */
bool      bsp_display_wait_vsync(uint32_t timeout_ms);
/* Wait until the buffer that was on screen before the last show() is no
 * longer scanned out (returns at once if it already is). Call before drawing
 * into the hidden buffer. */
bool      bsp_display_wait_flip(uint32_t timeout_ms);
void      bsp_display_backlight(int percent);   /* 0 = off                    */

/* ── Touch ─────────────────────────────────────────────────────────────────
 * Points are reported in landscape coordinates (the same frame lx_canvas and
 * the compositor draw in: display_rot already applied), 0..W-1 x 0..H-1. */
#define BSP_TOUCH_MAX_POINTS 5

typedef struct {
    uint16_t x, y;
    uint16_t size;              /* contact size, 0 if unknown */
    uint8_t  id;                /* track id, stable while the finger is down */
} bsp_touch_point_t;

esp_err_t bsp_touch_init(void);
int       bsp_touch_read(bsp_touch_point_t *pts, int max);   /* -> points down, <0 on error */

/* ── Audio ─────────────────────────────────────────────────────────────────
 * The codec runs at one fixed rate (bsp_audio_rate()), 16-bit stereo, set up
 * once at boot. The launcher's mixer resamples app streams to it (§7.4). */
esp_err_t bsp_audio_init(void);
int       bsp_audio_rate(void);
/* Blocks until all frames are queued to the I2S DMA or timeout_ms passes;
 * returns frames written. */
size_t    bsp_audio_write(const int16_t *stereo, size_t frames, uint32_t timeout_ms);
void      bsp_audio_volume(int percent);          /* 0 = mute                    */

/* ── Buttons and analog inputs ─────────────────────────────────────────────
 * Buttons on the board (or an add-on found at init), decoded into the
 * standard gamepad layout: the same bits as PAPP_BTN_* in papp_abi.h.
 * bsp_buttons_init sets BSP_CAP_GPIO_PAD / BSP_CAP_PADDLE for what it finds
 * and returns ESP_ERR_NOT_FOUND if there is nothing. Reads are cheap enough
 * to poll every few ms from a task (not from an ISR). */
#define BSP_BTN_A        (1u << 0)
#define BSP_BTN_B        (1u << 1)
#define BSP_BTN_X        (1u << 2)
#define BSP_BTN_Y        (1u << 3)
#define BSP_BTN_L1       (1u << 4)
#define BSP_BTN_R1       (1u << 5)
#define BSP_BTN_L2       (1u << 6)
#define BSP_BTN_R2       (1u << 7)
#define BSP_BTN_SELECT   (1u << 8)
#define BSP_BTN_START    (1u << 9)
#define BSP_BTN_HOME     (1u << 12)
#define BSP_BTN_UP       (1u << 16)
#define BSP_BTN_DOWN     (1u << 17)
#define BSP_BTN_LEFT     (1u << 18)
#define BSP_BTN_RIGHT    (1u << 19)

#define BSP_ANALOG_PADDLE 0

esp_err_t bsp_buttons_init(void);
uint32_t  bsp_buttons_read(void);                 /* BSP_BTN_* currently down     */
int       bsp_analog_read(int ch);                /* 0..65535 (calibrated), -1 if absent */

/* ── Real-time clock ───────────────────────────────────────────────────────
 * Keeps UTC (seconds since 1970) across power-off. bsp_rtc_init sets BSP_CAP_RTC
 * when a clock answers; get returns ESP_ERR_INVALID_STATE when it lost the time
 * (battery out) or was never set. Boards without one return ESP_ERR_NOT_SUPPORTED. */
esp_err_t bsp_rtc_init(void);
esp_err_t bsp_rtc_get(int64_t *utc);
esp_err_t bsp_rtc_set(int64_t utc);

/* Battery voltage in mV at its divider, -1 if the board can't measure it. Without a
 * cell fitted the value is meaningless: callers check it is in the range of a Li-ion. */
int       bsp_battery_mv(void);

#ifdef __cplusplus
}
#endif
