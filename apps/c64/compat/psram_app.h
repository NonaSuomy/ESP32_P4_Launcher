/*
 * PSRAM App Loader — Shared ABI Header
 *
 * This header defines the interface between the launcher and PSRAM-loaded apps.
 * It is used by both:
 *   - The launcher (to populate the service table and load/run apps)
 *   - PSRAM apps (to access launcher services via function pointers)
 *
 * ABI version must match between launcher and apps.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── .pax Binary Format ─────────────────────────────────────────────── */

#define PAX_MAGIC       0x50415050   /* legacy wire magic; ABI 1 remains unchanged */
#define PAX_ABI_VERSION 1
#define PAX_HEADER_SIZE 32
/* Zeroed pointer slots a loader leaves after app_services_t (see the
 * APPEND-ONLY ZONE), so services appended later read NULL. */
#define PAX_SERVICES_SPARE_SLOTS 64

/* A callback that a PAX asks the loader to run on the ESPHome/LVGL task.
 * The callback must be short and must not wait for the PAX worker.  It is
 * useful for LVGL APIs, which are single-threaded and cannot be called from
 * the PAX worker task. */
typedef int (*pax_ui_call_fn_t)(void *arg);
typedef void (*pax_file_progress_fn_t)(void *context, uint64_t bytes_done,
                                        uint64_t bytes_total);

/* Pages displayed inside the loader's shared in-game menu. The loader copies
 * each page and its optional 64x56 RGB565 preview before set_page returns. */
#define PAX_GAME_MENU_MAX_ITEMS 8
#define PAX_GAME_MENU_PREVIEW_WIDTH 64
#define PAX_GAME_MENU_PREVIEW_HEIGHT 56
enum {
    PAX_GAME_MENU_ACTION_OPEN = 0,
    PAX_GAME_MENU_ITEM_ADJUSTABLE = 1,
    PAX_GAME_MENU_ITEM_DISABLED = 2
};
typedef struct {
    uint32_t action;
    uint32_t flags;
    char label[48];
} pax_game_menu_item_t;
typedef struct {
    char title[32];
    char status[64];
    uint32_t back_action;
    uint32_t count;
    uint32_t selected;
    pax_game_menu_item_t items[PAX_GAME_MENU_MAX_ITEMS];
    const uint16_t *preview;
} pax_game_menu_page_t;
typedef void (*pax_game_menu_action_fn_t)(void *context, uint32_t action, int32_t direction);

typedef struct __attribute__((packed)) {
    uint32_t magic;        /* Must be PAX_MAGIC                        */
    uint32_t version;      /* ABI version (PAX_ABI_VERSION)            */
    uint32_t entry_off;    /* Byte offset to entry fn from text start   */
    uint32_t text_size;    /* Size of .text + .rodata segment           */
    uint32_t data_size;    /* Size of .data segment (initialized)       */
    uint32_t bss_size;     /* Size of .bss segment (zero-filled)        */
    uint32_t flags;        /* Reserved, must be 0                       */
    uint32_t reserved;     /* Padding to 32 bytes                       */
} pax_header_t;

#ifdef __cplusplus
static_assert(sizeof(pax_header_t) == PAX_HEADER_SIZE, "pax_header_t must be 32 bytes");
#else
_Static_assert(sizeof(pax_header_t) == PAX_HEADER_SIZE, "pax_header_t must be 32 bytes");
#endif

/* ── Gamepad State (matches odroid_gamepad_state layout) ─────────────── */

enum {
    PAX_INPUT_UP = 0,
    PAX_INPUT_RIGHT,
    PAX_INPUT_DOWN,
    PAX_INPUT_LEFT,
    PAX_INPUT_SELECT,
    PAX_INPUT_START,
    PAX_INPUT_A,
    PAX_INPUT_B,
    PAX_INPUT_X,
    PAX_INPUT_Y,
    PAX_INPUT_L,
    PAX_INPUT_R,
    PAX_INPUT_MENU,
    PAX_INPUT_VOLUME,
    PAX_INPUT_MAX
};

typedef struct {
    int values[PAX_INPUT_MAX];
} pax_gamepad_state_t;

/* A queued keyboard event from the launcher's USB HID text path.  PAX apps
 * receive Quake-style key numbers (ASCII for printable keys, or the named
 * constants defined by the app) and a press/release state. */
typedef struct {
    int key;
    int down;
} pax_keyboard_event_t;

/* Named USB keyboard values used by input_keyboard_read(). Printable keys
 * use ASCII; these values keep the four HID arrows distinct from 'P', 'Q',
 * 'R', and 'S'. */
#define PAX_KEY_ENTER    13
#define PAX_KEY_UP       153
#define PAX_KEY_RIGHT    154
#define PAX_KEY_DOWN     155
#define PAX_KEY_LEFT     156

/* One finger on the touch panel (touch_read_points), in canvas coordinates
 * like touch_read. id stays the same while that finger stays down. */
typedef struct {
    int16_t x;
    int16_t y;
    uint8_t id;
} pax_touch_point_t;

/* What file_stat reports about a file or folder. */
typedef struct {
    uint64_t size;      /* bytes; 0 for a folder                          */
    int64_t  mtime;     /* last change, seconds since 1970; 0 if unknown  */
    uint32_t is_dir;    /* 1 for a folder, 0 for a file                   */
    uint32_t reserved;  /* 0                                              */
} pax_file_stat_t;

#ifdef __cplusplus
static_assert(sizeof(pax_file_stat_t) == 24, "pax_file_stat_t must be 24 bytes");
#else
_Static_assert(sizeof(pax_file_stat_t) == 24, "pax_file_stat_t must be 24 bytes");
#endif

/* Snapshot returned by the launcher's verified PAX installer.  state:
 * 0 idle, 1 transferring, 2 completed, 3 failed. */
typedef struct {
    uint32_t state;
    uint32_t done;
    uint32_t total;
    int32_t result;
    char status[96];
} pax_install_progress_t;

/* Snapshot for launcher status UI. battery_percent is -1 when this board has
 * no configured battery monitor. */
enum {
    PAX_NETWORK_NONE = 0,
    PAX_NETWORK_WIFI = 1,
    PAX_NETWORK_ETHERNET = 2
};
typedef struct {
    char local_time[16];
    int32_t network_type;
    int32_t battery_percent;
} pax_device_status_t;

/* Runtime IP configuration. IPv4 addresses are host-order 0xAABBCCDD values.
 * A zero DNS address leaves the interface's current/default DNS unchanged. */
typedef struct {
    int32_t network_type;
    int32_t connected;
    int32_t dhcp_enabled;
    uint32_t ipv4;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
} pax_network_config_t;

enum {
    PAX_STORAGE_SD = 0,
    PAX_STORAGE_USB = 1,
    PAX_STORAGE_COUNT = 2
};
typedef struct {
    int32_t available;
    int32_t mounted;
    uint64_t total_bytes;
    uint64_t free_bytes;
    char mount_path[16];
} pax_storage_status_t;

/* IDs for the loader's procedural UI sound generator. */
enum {
    PAX_UI_SOUND_CLICK = 0,
    PAX_UI_SOUND_CONFIRM,
    PAX_UI_SOUND_CHIME,
    PAX_UI_SOUND_BACK,
    PAX_UI_SOUND_VOLUME,
};

/* ── Memory Capability Flags (matches ESP-IDF MALLOC_CAP_*) ─────────── */

#define PAX_MEM_CAP_SPIRAM   (1 << 10)  /* MALLOC_CAP_SPIRAM */
#define PAX_MEM_CAP_INTERNAL (1 << 11)  /* MALLOC_CAP_INTERNAL */
#define PAX_MEM_CAP_DMA      (1 << 2)   /* MALLOC_CAP_DMA */

/* ── App Services Table ──────────────────────────────────────────────── */
/*
 * Function pointer table populated by the launcher.
 * Passed to the loaded app's entry function.
 * The PSRAM app must NOT call any function outside this table.
 */
typedef struct {
    uint32_t abi_version;   /* Must equal PAX_ABI_VERSION */

    /* ── Display ─────────────────────────────────────────────────────── */
    uint16_t *(*display_get_framebuffer)(void);
    uint16_t *(*display_get_emu_buffer)(void);
    void      (*display_flush)(void);
    void      (*display_emu_flush)(void);
    void      (*display_clear)(uint16_t color);
    void      (*display_set_scale)(float sx, float sy);
    void      (*display_write_frame_rgb565)(const uint16_t *buffer);
    void      (*display_write_frame_custom)(const uint16_t *buffer,
                  uint16_t in_w, uint16_t in_h, float scale,
                  bool byte_swap);
    void      (*display_write_rect)(int x, int y, int w, int h,
                  const uint16_t *data);
    void      (*display_lock)(void);
    void      (*display_unlock)(void);

    /* ── Audio ───────────────────────────────────────────────────────── */
    void (*audio_init)(int sample_rate);
    void (*audio_submit)(short *stereo_buf, int frame_count);

    /* ── Input ───────────────────────────────────────────────────────── */
    void (*input_gamepad_read)(pax_gamepad_state_t *state);

    /* ── File I/O (standard C wrappers) ──────────────────────────────── */
    void  *(*file_open)(const char *path, const char *mode);
    int    (*file_close)(void *stream);
    size_t (*file_read)(void *ptr, size_t size, size_t nmemb, void *stream);
    size_t (*file_write)(const void *ptr, size_t size, size_t nmemb,
                         void *stream);
    int    (*file_seek)(void *stream, long offset, int whence);
    long   (*file_tell)(void *stream);

    /* ── Memory ──────────────────────────────────────────────────────── */
    /* mem_alloc, mem_calloc and mem_realloc give PSRAM on the ESPHome
     * loader; internal RAM only when PSRAM is full, and never its last
     * 64 KB, which the speaker, SD card and network drivers need (its
     * pax_memory.h). mem_caps_alloc gives the heap the caps name
     * (PAX_MEM_CAP_INTERNAL for the few buffers that must be internal);
     * without SPIRAM, INTERNAL or DMA it is like mem_alloc. mem_free frees
     * a block from any of them. */
    void *(*mem_alloc)(size_t size);
    void *(*mem_calloc)(size_t n, size_t size);
    void *(*mem_realloc)(void *ptr, size_t size);
    void  (*mem_free)(void *ptr);
    void *(*mem_caps_alloc)(size_t size, uint32_t caps);

    /* ── System ──────────────────────────────────────────────────────── */
    int     (*log_printf)(const char *fmt, ...);
    int     (*log_vprintf)(const char *fmt, va_list args);
    void    (*delay_ms)(int ms);
    int64_t (*get_time_us)(void);

    /* ── Settings (NVS) ──────────────────────────────────────────────── */
    char   *(*settings_rom_path_get)(void);
    void    (*settings_rom_path_set)(const char *path);
    int32_t (*settings_volume_get)(void);
    void    (*settings_volume_set)(int32_t level);
    int32_t (*settings_brightness_get)(void);
    void    (*settings_brightness_set)(int32_t level);

    /* ── FreeRTOS Tasks ──────────────────────────────────────────────── */
    int  (*task_create)(void (*fn)(void *), const char *name,
                        uint32_t stack_depth, void *arg,
                        int priority, void *out_handle, int core);
    void (*task_delete)(void *handle);

    /* ── PNG Loading ─────────────────────────────────────────────────── */
    uint16_t *(*png_load_rgb565)(const char *path,
                                  uint16_t *out_w, uint16_t *out_h);

    /* ── PPA Sprite Blit (hardware color-keyed blend) ────────────────── */
    int (*sprite_blit)(uint16_t *framebuf, uint32_t fb_w, uint32_t fb_h,
                       uint32_t x, uint32_t y,
                       const uint16_t *sprite, uint32_t sp_w, uint32_t sp_h,
                       uint16_t colorkey);

    /* ── PPA Framebuffer Copy (hardware DMA copy via PPA SRM) ────────── */
    int (*fb_copy)(const uint16_t *src, uint16_t *dst,
                   uint32_t w, uint32_t h);

    /* ────────────────────────────────────────────────────────────────
     * APPEND-ONLY ZONE — added after ABI v1 shipped.
     *
     * New services are appended here, never inserted above, so existing
     * .pax binaries keep their field offsets and run unchanged without
     * a version bump. A new field may be NULL if an OLDER launcher loads
     * a newer app, so touch-aware apps MUST null-check before calling:
     *     if (svc->touch_read && svc->touch_read(&x, &y)) { ... }
     * Loaders since display_get_size/display_set_canvas leave
     * PAX_SERVICES_SPARE_SLOTS zeroed pointers after the table, so a field
     * appended later reads NULL on them. Loaders built before those two
     * fields end the table at net_resolve and whatever follows it is NOT
     * zero: an app that calls a field added after net_resolve needs a
     * loader that has it.
     * ──────────────────────────────────────────────────────────────── */

    /* ── Touch (GT911) ───────────────────────────────────────────────── */
    /* Read the capacitive touch panel. Coordinates are reported in the
     * LANDSCAPE native-framebuffer space after the panel's 180-degree display
     * transform, inside the app's canvas: x in [0, width-1], y in
     * [0, height-1] — [0,799] x [0,479] unless the app chose another canvas
     * with display_set_canvas. Touches outside the canvas (and on the
     * loader's close control beside it) are not reported.
     * Returns 1 if currently touched (and fills *x,*y), 0 if not. Either
     * pointer may be NULL. */
    int (*touch_read)(int *x, int *y);

    /* ── Analog paddle / wheel (ADC2_CH2, GPIO 51) ───────────────────── */
    /* Raw 12-bit reading (0..4095) from the physical analog wheel — the
     * same potentiometer the Atari paddle games use. Returns -1 when it is
     * unavailable: on the HDMI build (no paddle is wired) or if the ADC
     * could not be initialised. The ADC is brought up lazily on first call.
     *
     * The reading is refreshed by the launcher inside input_gamepad_read(),
     * so poll that every frame to keep values fresh.
     *
     * Appended after touch_read — null-check before calling:
     *     if (svc->paddle_read) { int raw = svc->paddle_read(); ... } */
    int (*paddle_read)(void);

    /* ── Dedicated PAX close control (L3) ───────────────────────────── */
    /* Returns 1 while the configured physical L3 button is held. This is
     * appended after paddle_read so existing PAX binaries remain ABI-safe. */
    int (*input_l3_read)(void);

    /* ── USB mouse events ────────────────────────────────────────────── */
    /* Returns 1 when a mouse report has been accumulated since the last
     * call. dx/dy are relative motion in the PAX canvas/presentation space:
     * positive dx moves the visible cursor right and positive dy moves it
     * down. The loader applies the inverse of the display's 180-degree
     * presentation rotation exactly once; apps must not rotate these values.
     * buttons uses HID bits 1/2/4 for left/right/middle. Appended so existing
     * ABI-v1 binaries remain safe. */
    int (*input_mouse_read)(int *dx, int *dy, int *buttons);

    /* ── USB keyboard text/events ────────────────────────────────────── */
    /* Pop one queued keyboard event. Returns 1 when an event was returned,
     * 0 when the queue is empty. Printable keys use ASCII values; special
     * keys use the Quake key constants in the app. */
    int (*input_keyboard_read)(pax_keyboard_event_t *event);

    /* ── UDP networking (lwIP) ───────────────────────────────────────── */
    /* Minimal UDP for LAN multiplayer. IPv4 addresses and ports are in HOST
     * byte order (0xC0A80A05 is 192.168.10.5).
     *   net_udp_open   Bind a non-blocking UDP socket to `port` on every
     *                  interface (0: any free port); SO_BROADCAST when
     *                  `broadcast` is non-zero. Returns a handle >= 0, or -1.
     *   net_udp_send   Send `len` bytes to ip:port. Returns the bytes sent,
     *                  0 when the stack would block, -1 on error.
     *   net_udp_recv   Take one waiting datagram without blocking. Returns its
     *                  length (at most `len`), 0 when none is waiting, -1 on
     *                  error; `ip`/`port` (may be NULL) receive the sender.
     *   net_udp_close  Close a handle. The loader closes any the app leaves
     *                  open when it exits.
     *   net_ipv4       The device's IPv4 address and netmask (either may be
     *                  NULL). Returns 1 when the network is up, else 0.
     * Appended after input_keyboard_read — null-check before calling. */
    int  (*net_udp_open)(uint16_t port, int broadcast);
    int  (*net_udp_send)(int handle, const void *buf, int len, uint32_t ip, uint16_t port);
    int  (*net_udp_recv)(int handle, void *buf, int len, uint32_t *ip, uint16_t *port);
    void (*net_udp_close)(int handle);
    int  (*net_ipv4)(uint32_t *ip, uint32_t *netmask);

    /* ── TCP networking (lwIP) ───────────────────────────────────────── */
    /* Same byte-order rules as UDP. TCP handles share the UDP handle table;
     * close them with net_udp_close. Nothing here blocks except net_resolve.
     *   net_tcp_connect  Start connecting to ip:port. Returns a handle >= 0
     *                    (net_poll reports it writable once connected, or
     *                    failed), or -1.
     *   net_tcp_listen   Accept connections on `port` on every interface.
     *                    Returns a handle >= 0, or -1.
     *   net_tcp_accept   Take one waiting connection from a listening handle.
     *                    Returns its handle, -2 when none is waiting, -1 on
     *                    error; `ip`/`port` (may be NULL) receive the peer.
     *   net_tcp_send     Send up to `len` bytes. Returns the bytes taken,
     *                    0 when the stack would block, -1 on a failed link.
     *   net_tcp_recv     Read up to `len` bytes. Returns the bytes read, 0 when
     *                    the peer closed, -2 when nothing is waiting, -1 on
     *                    error.
     *   net_poll         Readiness of any handle: bit 0 readable (data, a
     *                    waiting connection or a close), bit 1 writable (e.g.
     *                    a finished connect), bit 2 failed (e.g. refused).
     *                    Returns 0 when nothing is ready, -1 for a bad handle.
     *   net_resolve      IPv4 address of `host` (a name or a dotted quad),
     *                    in host byte order. May block while DNS answers.
     *                    Returns 1 and sets *ip, or 0.
     * Appended after net_ipv4 — null-check before calling. */
    int  (*net_tcp_connect)(uint32_t ip, uint16_t port);
    int  (*net_tcp_listen)(uint16_t port);
    int  (*net_tcp_accept)(int handle, uint32_t *ip, uint16_t *port);
    int  (*net_tcp_send)(int handle, const void *buf, int len);
    int  (*net_tcp_recv)(int handle, void *buf, int len);
    int  (*net_poll)(int handle);
    int  (*net_resolve)(const char *host, uint32_t *ip);

    /* ── Display canvas size ─────────────────────────────────────────── */
    /* The canvas is the part of the panel an app draws. Every app starts
     * with the ABI v1 canvas of 800x480, centred on the panel, so apps that
     * never call these two services behave exactly as before. The canvas
     * size applies to everything on the display side: display_get_framebuffer
     * (width x height RGB565, stride = width), display_flush, display_clear,
     * display_write_frame_rgb565 (one full canvas), display_write_rect
     * (clipped to the canvas), display_write_frame_custom / display_emu_flush
     * (the scaled frame is centred in the canvas and at most its size),
     * touch_read (canvas coordinates) and the loader's screenshots.
     *
     *   display_get_size    The canvas this app should use: the user's
     *                       per-app Screen setting from the store, else the
     *                       size its store listing recommends (canvas
     *                       "recommended", when it fits), else the
     *                       device's default canvas (YAML canvas_width /
     *                       canvas_height, normally the whole panel, e.g.
     *                       1024x600). Once the app has switched with
     *                       display_set_canvas it reports the canvas in use.
     *                       Either pointer may be NULL.
     *   display_set_canvas  Switch to a width x height canvas. Both must be
     *                       even, at least 320x240 and at most the panel
     *                       (display_get_size's answer always qualifies).
     *                       Returns 0 on success; -1 refuses the size and
     *                       leaves the canvas as it was. A change clears the
     *                       framebuffer and the whole panel to black; asking
     *                       for the current size changes nothing. Call it
     *                       before drawing, from the task that draws, and
     *                       read display_get_framebuffer afterwards. On a
     *                       canvas too wide for the close control beside it,
     *                       the control is not drawn: taps in the canvas's
     *                       top-right corner reach the app and only a 2 s
     *                       hold there closes it, so offer your own exit.
     *
     * Typical use:
     *     int w = 800, h = 480;
     *     if (svc->display_get_size && svc->display_set_canvas) {
     *         svc->display_get_size(&w, &h);
     *         if (svc->display_set_canvas(w, h) != 0) { w = 800; h = 480; }
     *     }
     *     uint16_t *fb = svc->display_get_framebuffer();   // w x h
     * An app with fixed sizes picks the largest of its own that fits in
     * display_get_size's answer and passes that to display_set_canvas.
     * Appended after net_resolve — null-check before calling (an older
     * launcher leaves them NULL and the canvas is always 800x480). */
    void (*display_get_size)(int *width, int *height);
    int  (*display_set_canvas)(int width, int height);

    /* ── MIDI (a USB-MIDI cable or keyboard, usb_midi) ─────────────────
     *   midi_read   Copies up to len received MIDI bytes into buf and
     *               returns how many (0: nothing new). Plain MIDI, whole
     *               messages, status byte first: 90 3C 64 is note on,
     *               middle C. Bytes that came before the app started are
     *               dropped.
     *   midi_write  Sends whole MIDI messages (status byte first; SysEx
     *               F0 ... F7 included; no running status). Returns 0 when
     *               queued, -1 when no device is plugged in, the loader has
     *               no usb_midi or the queue is full.
     * Appended after display_set_canvas: NULL on older loaders. */
    int  (*midi_read)(uint8_t *buf, int len);
    int  (*midi_write)(const uint8_t *data, int len);

    /* ── Multi-touch ────────────────────────────────────────────────────
     *   touch_read_points  Fills up to max points with the fingers on the
     *                      panel now (the first finger first) and returns
     *                      how many; 0: none. Same canvas coordinates as
     *                      touch_read; fingers outside the canvas are left
     *                      out. The GT911 reports up to 5.
     * Appended after midi_write: NULL on older loaders. */
    int  (*touch_read_points)(pax_touch_point_t *points, int max);

    /* ── TLS (HTTPS) connections (ESP-IDF esp-tls, mbedTLS) ─────────────
     * A TLS client connection made and verified by the loader, so an app
     * speaks HTTPS without its own mbedTLS. The server's certificate is
     * checked against the firmware's CA bundle and must name `host`, which
     * is also sent as SNI; a connection that fails either check fails.
     * Nothing here blocks the calling task.
     *   net_tls_connect  Start a connection to host:port (a name or a dotted
     *                    quad). Name lookup, the TCP connect and the TLS
     *                    handshake run on a loader task in the background
     *                    (up to 15 s each). Returns a handle >= 0 (a TLS
     *                    handle, not a UDP/TCP one), or -1 when the loader
     *                    has no CA bundle or all sessions are in use.
     *   net_tls_status   1 ready, 0 still connecting, -1 failed (name lookup,
     *                    network, certificate or handshake; the loader logs
     *                    which). After net_tls_recv returns -1 it tells a
     *                    close (1) from an error (-1). -1 for a bad handle.
     *   net_tls_send     Send up to `len` bytes. Returns the bytes taken, 0
     *                    when the socket would block (and while connecting),
     *                    -1 on error or a closed connection. After 0, call
     *                    again with the same bytes: mbedTLS may already hold
     *                    them in its output record.
     *   net_tls_recv     Read up to `len` decrypted bytes. Returns the bytes
     *                    read, 0 when nothing is waiting (and while
     *                    connecting), -1 when the peer closed the connection
     *                    or it failed (see net_tls_status).
     *   net_tls_close    Close a handle, including one still connecting. The
     *                    loader closes any the app leaves open when it exits.
     * net_poll also takes a TLS handle: bit 0 when decrypted bytes wait or
     * the socket has input (possibly part of a record, so net_tls_recv may
     * still return 0), bit 1 when the session is ready and the socket can
     * take data, bit 2 when it failed; 0 while connecting.
     * At most 4 sessions are open at once (connecting ones included). Each
     * takes about 25 KB of the loader's internal RAM while open (mbedTLS's
     * 16 KB input and 4 KB output record buffers plus its context), plus up
     * to about 20 KB more and an 8 KB task stack during the handshake. Use a
     * handle from one task at a time.
     * Appended after touch_read_points: NULL on older loaders. */
    int  (*net_tls_connect)(const char *host, uint16_t port);
    int  (*net_tls_status)(int handle);
    int  (*net_tls_send)(int handle, const void *buf, int len);
    int  (*net_tls_recv)(int handle, void *buf, int len);
    void (*net_tls_close)(int handle);

    /* ── App hand-off (one app opening another) ─────────────────────────
     * An app can hand the device to another app and, if it likes, get it
     * back afterwards: NetSurf opens a video link in psram_video, which
     * returns to the page when it quits.
     *   app_open            Open `target` with the argument `arg` (NULL or ""
     *                       for none; at most PAX_APP_ARG_MAX - 1 bytes).
     *                       `target` is an app's name such as "psram_video",
     *                       found the way the store and library find it (the
     *                       installed copy in the install folder, then each
     *                       library source in order: its folder, or its HTTP
     *                       catalog page), the http(s):// URL of a .pax, or
     *                       the /sd/... path of one. Returns 0 when accepted,
     *                       -1 when the app is unknown, the argument is too
     *                       long, a close or another hand-off is under way,
     *                       or return_after would make more than two apps
     *                       wait. May block for a few seconds while an HTTP
     *                       catalog is searched. Once accepted, the loader
     *                       asks the calling app to quit exactly as its close
     *                       control does (the app may also just return from
     *                       app_entry at once), then starts the target. With
     *                       return_after non-zero, the loader starts the
     *                       caller again after the target quits (or fails to
     *                       load), with the caller's resume argument.
     *   app_get_arg         The argument this app was launched with: copied
     *                       into buf (NUL-terminated, cut to len - 1 bytes).
     *                       Returns its full length, 0 when there is none
     *                       (launches from the store, the library, a button
     *                       or remotely). buf may be NULL when len is 0.
     *   app_set_resume_arg  The argument this app gets when the loader starts
     *                       it again after an app it opened with return_after
     *                       (the page to show again, say). It starts out as
     *                       the app's own launch argument. Returns 0, or -1
     *                       when longer than PAX_APP_ARG_MAX - 1 bytes.
     * An app opened this way runs like one started from the store. At most two
     * apps wait to be returned to, and a launch from the menu forgets them.
     * Appended after net_tls_close: NULL on older loaders. */
    int  (*app_open)(const char *target, const char *arg, int return_after);
    int  (*app_get_arg)(char *buf, int len);
    int  (*app_set_resume_arg)(const char *arg);

    /* ── Folder listing ─────────────────────────────────────────────────
     *   file_list_dir  The entries of the folder `path` (a /sd/... path, as
     *                  for file_open): each name followed by a NUL, a
     *                  folder's with '/' added before the NUL, one after
     *                  another in buf. Returns how many entries it wrote, or
     *                  -1 when the folder cannot be opened. Entries that do
     *                  not fit in len bytes are left out; "." and ".." are
     *                  never listed. The order is the file system's.
     * Appended after app_set_resume_arg: NULL on older loaders. */
    int  (*file_list_dir)(const char *path, char *buf, int len);

    /* ── File management (folders, delete, rename, stat) ────────────────
     * For apps that manage files on the card, such as an archive
     * extractor. Paths are absolute, as for file_open: /sd/... (the card)
     * or a folder under one of the loader's data roots (/usb0/...). A
     * path anywhere else, or with a "." or ".." component, a backslash or
     * a colon, is refused. A trailing '/' is ignored. Unlike file_open's
     * read fallback, no other root is tried: each call acts on exactly the
     * path given. All return 0 on success and -1 on an error or a refused
     * path.
     *   file_mkdir   Create the folder `path` and any missing parents. 0
     *                also when it already exists; -1 when something in the
     *                way is a file.
     *   file_remove  Delete the file `path`, or the folder `path` if it is
     *                empty. A root itself (/sd) is never removed.
     *   file_rename  Rename or move `from` to `to` (both checked). A file
     *                already at `to` is not replaced: -1. Moving between
     *                two roots (/sd to /usb0) is not possible: copy it.
     *   file_stat    Fill *out for the file or folder `path`: its size, 1 in
     *                is_dir for a folder, and its last change time where the
     *                file system has one (FAT keeps it to 2 s, in the time
     *                zone the card was written in). -1 when it does not
     *                exist; *out is then zeroed.
     * Appended after file_list_dir: NULL on older loaders. */
    int  (*file_mkdir)(const char *path);
    int  (*file_remove)(const char *path);
    int  (*file_rename)(const char *from, const char *to);
    int  (*file_stat)(const char *path, pax_file_stat_t *out);

    /* ── ZIP-backed ROM helper (append-only) ──────────────────────────
     * Find the first regular ZIP entry whose extension is present in the
     * pipe-separated list, extract it to cache_dir/cache_stem.<ext>, and
     * return the portable path in out_path. The archive is not modified. */
    int  (*file_zip_extract_first)(const char *archive,
                                   const char *extensions,
                                   const char *cache_dir,
                                   const char *cache_stem,
                                   char *out_path, size_t out_size);

    /* Run callback on the loader's ESPHome loop task and wait for it to
     * finish.  timeout_ms=0 means wait indefinitely.  Appended after all
     * existing services so older PAXs keep the original ABI layout. */
    int  (*ui_call_sync)(pax_ui_call_fn_t callback, void *arg,
                         uint32_t timeout_ms);

    /* Store-only install bridge. The loader validates the target metadata,
     * downloads to a temporary file, verifies size/SHA-256, syncs declared
     * app data, and atomically commits the installed PAX. It is available
     * only while launcher_ui.pax is running. Poll progress for state/bytes
     * and a short user-facing status string. */
    int (*store_install_start)(const char *url, const char *name,
                               const char *title, const char *version,
                               uint32_t size, const char *sha256);
    int (*store_install_status)(pax_install_progress_t *out);

    /* Append-only launcher controls/status; NULL on older loaders. */
    int (*device_status_get)(pax_device_status_t *out);
    int32_t (*settings_auto_brightness_get)(void);
    void (*settings_auto_brightness_set)(int32_t enabled);
    /* Archive/file download bridge. URL is fetched to a validated /sd path
     * using the loader's staged .part + sync + rename writer; size and hash
     * are intentionally unknown for Archive.org ZIP members. */
    int (*archive_download_start)(const char *url, const char *path);

    /* Procedurally generated launcher sounds. Optional on older loaders. */
    void (*ui_sound)(uint8_t sound, uint8_t param);

    /* Launcher settings/status extensions. Appended so older apps retain all
     * existing offsets; older loaders leave these fields NULL via spare slots. */
    int (*network_config_get)(pax_network_config_t *out);
    int (*network_config_set)(const pax_network_config_t *config);
    int (*storage_status_get)(int32_t storage, pax_storage_status_t *out);
    int (*storage_set_mounted)(int32_t storage, int32_t mounted);

    /* Archive.org account session. Only the signed-in session cookies are
     * retained by the loader in RAM; the PAX must never log or persist them.
     * get returns 1 and a Cookie header when signed in, 0 when not signed in. */
    int (*archive_auth_set)(const char *logged_in_user, const char *logged_in_sig);
    int (*archive_auth_get)(char *cookie_header, size_t capacity);

    /* Optional HID wheel input, appended to preserve all earlier ABI offsets. */
    int (*input_mouse_scroll_read)(int *wheel);

    /* Decode an image with transparency composited over 0x00BBGGRR. */
    uint16_t *(*png_load_rgb565_blended)(const char *path,
                                         uint16_t *out_w, uint16_t *out_h,
                                         uint32_t background_bgr888);

    /* Keep the spare-slot order aligned with newer app-side ABI headers. */
    int (*file_zip_extract_named)(const char *archive, const char *entry,
                                  const char *destination, uint32_t max_bytes,
                                  uint32_t *written_bytes);
    void *(*dir_open)(const char *path);
    int (*dir_read)(void *dir, char *name, size_t name_size, int *is_dir);
    int (*dir_close)(void *dir);

    /* Extract the first matching ZIP entry while reporting cumulative
     * uncompressed output bytes to a short PAX callback. */
    int (*file_zip_extract_first_progress)(const char *archive,
                                           const char *extensions,
                                           const char *cache_dir,
                                           const char *cache_stem,
                                           char *out_path, size_t out_size,
                                           pax_file_progress_fn_t progress,
                                           void *context);

    /* Append-only shared menu bridge. Register from the emulator thread.
     * Actions run on that same thread while the shared menu pauses gameplay;
     * they never run on the ESPHome UI task. direction is -1/+1 for left/right,
     * or 0 for A/Start. OPEN requests the initial page. NULL callback unregisters.
     * set_page(NULL) returns to GAME MENU; close resumes gameplay. Older loaders
     * leave these pointers NULL in their zeroed spare service slots. */
    int (*game_menu_register)(const char *label, pax_game_menu_action_fn_t callback, void *context);
    int (*game_menu_set_page)(const pax_game_menu_page_t *page);
    void (*game_menu_close)(void);

    /* Cryptographically strong random bytes from the device RNG. Appended so
     * older PAX binaries continue to read the preceding ABI fields unchanged. */
    int (*random_bytes)(void *buffer, size_t length);

    /* True once the launcher has asked this app to stop. Appended to preserve
     * every ABI-v1 field offset; NULL on older launchers. */
    bool (*exit_requested)(void);

    /* Optional executable PSRAM allocation and code-cache synchronization
     * for P4 JIT apps. Release allocations with jit_free before app_entry
     * returns. These fields are NULL on older loader builds. */
    void *(*jit_alloc)(size_t size);
    void (*jit_free)(void *exec_address);
    int (*jit_sync)(void *address, size_t size);


} app_services_t;

/* Relative HID movement is already in the visible direction. The loader's
 * PPA rotation and the panel's 180-degree MADCTL rotation cancel each other,
 * so the PAX source-canvas cursor should receive the deltas unchanged. */
static inline void pax_mouse_delta_to_canvas(int *dx, int *dy)
{
    (void)dx;
    (void)dy;
}

/* Size of the largest app_open / app_set_resume_arg argument, NUL included. */
#define PAX_APP_ARG_MAX 2048

/* ── Entry Point Signature ───────────────────────────────────────────── */
/*
 * Every PSRAM app must export this function at the entry_off specified
 * in its .pax header. It receives the service table and returns 0 on
 * success or a negative error code.
 */
typedef int (*pax_entry_fn_t)(const app_services_t *svc);

/* ── Loader API (launcher-side only) ─────────────────────────────────── */
#ifndef PAX_APP_SIDE  /* Excluded when building PSRAM apps */

#include "esp_err.h"

/* Opaque handle for a loaded PSRAM app */
typedef struct psram_app *psram_app_handle_t;

/**
 * Load a .pax binary from the filesystem into PSRAM.
 * Does NOT execute it yet.
 *
 * @param path   Absolute path, e.g. "/sd/roms/pax/opentyrian.pax"
 * @param handle Receives the loaded app handle
 * @return ESP_OK on success
 */
esp_err_t psram_app_load(const char *path, psram_app_handle_t *handle);

/**
 * Load a .pax binary over HTTP(S) into PSRAM without writing it to the SD
 * card. The binary is streamed into its final PSRAM allocation and is fully
 * validated before it can execute.
 *
 * @param url    HTTP(S) URL serving one complete .pax binary
 * @param handle Receives the loaded app handle
 * @return ESP_OK on success
 */
esp_err_t psram_app_load_url(const char *url, psram_app_handle_t *handle);

/**
 * Execute a previously loaded PSRAM app.
 * Maps code as executable, calls the entry point, waits for return,
 * then unmaps. Blocks until the app returns.
 *
 * @param handle  Loaded app handle
 * @return The app's return code (0 = success)
 */
int psram_app_run(psram_app_handle_t handle);

/**
 * Unload a PSRAM app and free all resources.
 *
 * @param handle  Loaded app handle (NULL-safe)
 */
void psram_app_unload(psram_app_handle_t handle);

/**
 * Self-test: verify PSRAM XIP works by loading and executing a tiny
 * built-in function from PSRAM. Logs results.
 *
 * @return ESP_OK if PSRAM code execution works
 */
esp_err_t psram_app_selftest(void);

#endif /* !PAX_APP_SIDE */

#ifdef __cplusplus
}
#endif
