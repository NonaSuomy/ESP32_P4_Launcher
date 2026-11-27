/*
 * DevLink wire protocol. Mirrored in tools/papp_dev.py — keep in sync.
 *
 * Frame:  0xA5 0x44 | type u8 | flags u8 | seq u16 | len u32 | payload[len] | crc32 u32
 * crc32 (IEEE) covers type..payload. Little-endian. Response type = request | 0x80,
 * same seq; DL_F_ERROR set => payload is an error message. Async frames use seq 0.
 */
#pragma once

#include <stdint.h>

#define DL_SOF0            0xA5
#define DL_SOF1            0x44
#define DL_HDR_SIZE        10
#define DL_MAX_PAYLOAD     (32 * 1024 + 256)
#define DL_CHUNK           (16 * 1024)

#define DL_F_ERROR         0x01
#define DL_F_APP           0x02     /* LOG frame from the app (vs. launcher) */

enum {
    DL_PING        = 0x01,
    DL_INFO        = 0x02,
    DL_TIME        = 0x03,   /* i64 local wall-clock seconds (the host's)        */
    DL_AUTH        = 0x04,   /* TCP: six-digit PIN; serial with empty payload reads the PIN */
    DL_LS          = 0x10,   /* path                         -> JSON list        */
    DL_STAT        = 0x11,   /* path                         -> JSON             */
    DL_MKDIR       = 0x12,   /* path (mkdir -p)                                  */
    DL_RM          = 0x13,   /* path (file or empty dir)                         */
    DL_RENAME      = 0x14,   /* old \0 new                                       */
    DL_PUT_BEGIN   = 0x20,   /* u32 size, u32 crc32, path                        */
    DL_PUT_DATA    = 0x21,   /* bytes                        -> u32 total        */
    DL_PUT_END     = 0x22,   /*                               verify + rename    */
    DL_GET         = 0x28,   /* u32 offset, u32 max, path    -> u32 size + bytes */
    DL_RUN         = 0x30,   /* path \0 arg1 \0 ...                              */
    DL_STOP        = 0x31,   /* [u32 grace_ms]                                   */
    DL_STATUS      = 0x32,   /*                               -> JSON            */
    DL_REBOOT      = 0x40,
    DL_CRASH       = 0x41,   /*                               -> JSON            */
    DL_CRASH_CLEAR = 0x42,
    DL_BOOTLOG     = 0x43,   /*                               -> text logged during boot */
    DL_SHOT        = 0x50,   /* u32 offset, u32 max          -> dl_shot_hdr_t + bytes */
    DL_NET         = 0x52,   /* [mode: start] -> busy flag + text of the network test */
    DL_DISPLAY     = 0x53,   /* u8 on: panel scan-out + backlight (bsp_display_scanout) */
    DL_BT          = 0x54,   /* text command (lx_bt_devlink)  -> text / JSON     */
    DL_INPUT       = 0x51,   /* dl_input_* request; STATE      -> JSON            */
    DL_LOG         = 0x70,   /* async: text                                      */
    DL_EVENT       = 0x71,   /* async: JSON                                      */
};

/* SHOT: offset 0 snapshots the visible framebuffer; later offsets page through
 * the same snapshot. Pixels are RLE16-compressed: u16 n; n & 0x8000 -> one pixel
 * repeated (n & 0x7FFF) times, else n literal pixels follow. */
#define DL_SHOT_FMT_RGB565_RLE16  1

/* INPUT: first byte selects the request */
enum { DL_INPUT_PAD = 1, DL_INPUT_KEY = 2, DL_INPUT_STATE = 3, DL_INPUT_UNMAP = 4 };   /* UNMAP: u8 player */

typedef struct __attribute__((packed)) {
    uint8_t  kind;            /* DL_INPUT_PAD                 */
    uint8_t  player;          /* 0..3                         */
    uint32_t buttons;         /* PAPP_BTN_* held for hold_ms  */
    uint32_t hold_ms;
} dl_input_pad_t;

typedef struct __attribute__((packed)) {
    uint8_t  kind;            /* DL_INPUT_KEY                 */
    uint16_t usage;           /* HID keyboard usage           */
    uint16_t mods;            /* PAPP_MOD_*                   */
    uint32_t hold_ms;
} dl_input_key_t;

typedef struct __attribute__((packed)) {
    uint16_t width, height;   /* panel, native orientation   */
    uint16_t format;          /* DL_SHOT_FMT_*               */
    uint16_t rotation;        /* bsp_info_t.display_rot      */
    uint32_t total;           /* compressed size             */
} dl_shot_hdr_t;
