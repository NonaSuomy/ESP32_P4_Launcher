/*
 * PAPP SDK — public API for apps (preview).
 *
 * Apps include only this header. It wraps the launcher interfaces imported at
 * load time (see papp_abi.h); the plumbing lives in the SDK runtime.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "papp_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PAPP_SDK_VERSION  0x00000100u   /* 0.1.0 */

/* ── Manifest ───────────────────────────────────────────────────────────── */
/* PAPP_MANIFEST(.id = "demo.hello", .name = "Hello", .version = PAPP_APP_VERSION, .main_stack = 16384);
 * PAPP_APP_VERSION comes from the version.txt beside the app's sources ("1.2.0"),
 * which papp_build.py passes in; the launcher compares it with the online one. */
#ifndef PAPP_APP_VERSION
#define PAPP_APP_VERSION "1.0.0"
#endif
#define PAPP_MANIFEST(...)                                                       \
    __attribute__((section(".papp_manifest"), used))                             \
    const papp_manifest_t __papp_manifest = {                                    \
        .magic = PAPP_MANIFEST_MAGIC, .sdk_version = PAPP_SDK_VERSION, __VA_ARGS__ }

/* ── Placement ──────────────────────────────────────────────────────────────
 * Everything runs from PSRAM by default (through the CPU caches). Tag hot
 * functions and the data they hammer to put them in internal SRAM instead:
 *   PAPP_FAST void cpu_step(void) { ... }
 *   PAPP_FAST_DATA static uint8_t ram[2048] = { ... };
 *   PAPP_FAST_BSS  static uint16_t line[320];
 * Internal SRAM is small (the launcher caps it per app): use it for inner
 * loops, not whole programs. Calls between FAST and normal code are fine.
 * On a launcher without executable SRAM the tagged parts load into PSRAM. */
#define PAPP_FAST       __attribute__((section(".fast.text"), noinline))
#define PAPP_FAST_DATA  __attribute__((section(".fast.data")))
#define PAPP_FAST_BSS   __attribute__((section(".fast.bss")))

/* ── System ─────────────────────────────────────────────────────────────── */
void        papp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int64_t     papp_time_us(void);
void        papp_delay_ms(uint32_t ms);
bool        papp_exit_requested(void);          /* poll in your main loop */
void        papp_exit(int code) __attribute__((noreturn));
const char *papp_board_id(void);
uint32_t    papp_launcher_abi(void);
bool        papp_local_time(papp_datetime_t *out);  /* false: clock not set, or older launcher */
bool        papp_utc_time(int64_t *seconds);        /* Unix time (UTC); false: not set, or launcher < 0.27 */

/* ── Memory ─────────────────────────────────────────────────────────────── */
void  *papp_alloc(size_t size, uint32_t caps, size_t align);   /* PAPP_MEM_* caps */
void  *papp_realloc(void *ptr, size_t size);
void   papp_free(void *ptr);
size_t papp_mem_free_size(uint32_t caps);

/* ── Files (also reachable through stdio when linking libc) ─────────────── */
const papp_if_fs_t *papp_fs(void);   /* NULL if the launcher has no FS */

/* ── Display ────────────────────────────────────────────────────────────────
 * Draw into a surface of any size; the launcher scales it to the display
 * (cfg.scale) and rotates it for the panel. Only one surface for now.
 *   papp_surface_cfg_t cfg = { .width = 320, .height = 240,
 *       .format = PAPP_PIXFMT_RGB565, .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
 *   papp_surface_t *s = papp_disp_surface_create(&cfg);
 *   for (;;) { uint16_t *px = papp_disp_lock(s, &stride); draw(px); papp_disp_present(s); } */
#define PAPP_RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

bool            papp_disp_info(papp_disp_info_t *out);       /* false: no display */
papp_surface_t *papp_disp_surface_create(const papp_surface_cfg_t *cfg);
void            papp_disp_surface_destroy(papp_surface_t *s);
void           *papp_disp_lock(papp_surface_t *s, int *stride_px);
int             papp_disp_present(papp_surface_t *s);        /* waits for vsync */
/* Zero copy: show the app's own width x height RGB565 buffer (stride = width), no lock needed. */
int             papp_disp_present_ext(papp_surface_t *s, const void *pixels);
int             papp_disp_touch(papp_surface_t *s, papp_touch_point_t *pts, int max);

/* ── Audio ──────────────────────────────────────────────────────────────────
 * Open a stream at the rate/format your engine produces; the launcher
 * resamples and mixes it. write() blocks while the stream's ring is full, so
 * a loop of write() calls is paced by playback. Use stats().queued_frames for
 * emulator-style rate control.
 *   papp_audio_cfg_t cfg = { .rate = 22050, .channels = 1, .format = PAPP_AUDIO_S16 };
 *   papp_audio_stream_t *a = papp_audio_open(&cfg);
 *   papp_audio_write(a, samples, n, 100); */
uint32_t             papp_audio_output_rate(void);          /* 0: no audio */
papp_audio_stream_t *papp_audio_open(const papp_audio_cfg_t *cfg);
void                 papp_audio_close(papp_audio_stream_t *s);
size_t               papp_audio_write(papp_audio_stream_t *s, const void *data, size_t frames,
                                      uint32_t timeout_ms);
void                 papp_audio_pause(papp_audio_stream_t *s, bool pause);
void                 papp_audio_set_gain(papp_audio_stream_t *s, int percent);
void                 papp_audio_clear(papp_audio_stream_t *s);
void                 papp_audio_stats(papp_audio_stream_t *s, papp_audio_stats_t *out);

/* ── Input ──────────────────────────────────────────────────────────────────
 * Poll state (papp_input_gamepad: standard layout, PAPP_BTN_*) or drain
 * events (key/button down/up, device added/removed). HOME belongs to the
 * launcher: pressing it (or holding Select+Start 1 s) asks the app to exit.
 * Buttons built into the board (e.g. the GPIO pad add-on) merge into player 1. */
bool     papp_input_gamepad(int player, papp_gamepad_t *out);   /* false: not connected */
bool     papp_input_key_down(uint16_t usage);                   /* HID usage id */
uint16_t papp_input_modifiers(void);                            /* PAPP_MOD_*   */
bool     papp_input_poll_event(papp_input_event_t *ev);         /* false: none  */
void     papp_input_flush(void);
/* Board analog inputs (PAPP_ANALOG_PADDLE): 0..65535, -1 if absent or the
 * launcher predates INPT v2. */
int      papp_input_analog(int ch);

/* ── Threads ────────────────────────────────────────────────────────────────
 * NULL / false when the launcher has no THRD. cfg may be NULL (8 KB internal
 * stack, normal priority, either core). Join every thread you create. Worker
 * threads should poll papp_exit_requested() like the main loop; whatever is
 * still running when the app ends is stopped by the launcher.
 *   int worker(void *arg) { ...; return 0; }
 *   papp_thread_t *t = papp_thread_create(worker, ctx, NULL);
 *   papp_thread_join(t, PAPP_WAIT_FOREVER, &code);
 * With --libc, stdio/errno are per thread and the C library is thread-safe. */
papp_thread_t *papp_thread_create(papp_thread_fn_t fn, void *arg, const papp_thread_cfg_t *cfg);
bool           papp_thread_join(papp_thread_t *t, uint32_t timeout_ms, int *exit_code);  /* false: timeout */
void           papp_thread_exit(int code) __attribute__((noreturn));   /* main thread: = papp_exit */
papp_thread_t *papp_thread_self(void);                                 /* NULL on the main thread  */
void           papp_yield(void);

papp_mutex_t *papp_mutex_create(bool recursive);
void          papp_mutex_destroy(papp_mutex_t *m);
void          papp_mutex_lock(papp_mutex_t *m);
bool          papp_mutex_trylock(papp_mutex_t *m, uint32_t timeout_ms);
void          papp_mutex_unlock(papp_mutex_t *m);

papp_sem_t *papp_sem_create(uint32_t initial, uint32_t max);
void        papp_sem_destroy(papp_sem_t *s);
bool        papp_sem_take(papp_sem_t *s, uint32_t timeout_ms);
void        papp_sem_give(papp_sem_t *s);

papp_queue_t *papp_queue_create(uint32_t length, uint32_t item_size);   /* items are copied */
void          papp_queue_destroy(papp_queue_t *q);
bool          papp_queue_send(papp_queue_t *q, const void *item, uint32_t timeout_ms);
bool          papp_queue_recv(papp_queue_t *q, void *item, uint32_t timeout_ms);
uint32_t      papp_queue_count(papp_queue_t *q);

/* ── Launcher screens ───────────────────────────────────────────────────────
 * The content browser every emulator needs: folders, Favorites / Recent tabs,
 * search, and Run / Resume / Restart / Delete save per ROM. It takes over the
 * screen and input until the user picks (your surface returns on its next
 * present). Returns CANCEL when the user backs out or the app must exit.
 *   papp_pick_cfg_t cfg = { .title = "NES", .root = "/sd/roms/nes", .extensions = ".nes",
 *                           .save_dir = "/sd/data/nes/saves", .save_ext = ".sav" };
 *   papp_pick_result_t r;
 *   if (papp_ui_pick_file(&cfg, &r) == PAPP_PICK_RESUME) load_state(r.save_path); */
int  papp_ui_pick_file(const papp_pick_cfg_t *cfg, papp_pick_result_t *out);
void papp_ui_message(const char *title, const char *text);
/* A menu over your (dimmed) picture: returns the item index, or -1 when the
 * user backs out or the app must exit. At most 7 items.
 *   static const char *const items[] = { "Resume", "Save", "Exit" };
 *   switch (papp_ui_menu("Game menu", items, 3, 0)) { ... } */
int  papp_ui_menu(const char *title, const char *const *items, int n, int initial);
/* The launcher's volume overlay (what Y does in the emulators). Returns the
 * new level 0..100, or -1 on a launcher without it. */
int  papp_ui_volume(void);
/* The launcher's Wi-Fi picker: the user chooses a network and types its
 * password; it becomes the board's saved network. Returns 1 connected, 0
 * cancelled, < 0 no Wi-Fi or a launcher before 0.29 (PAPP_E_UNSUPPORTED).
 *   if (papp_net()->up() < 0 && papp_ui_wifi() == 1) ... connected */
int  papp_ui_wifi(void);

/* ── Network ────────────────────────────────────────────────────────────── */
/* Wi-Fi, sockets, TLS and HTTP(S): the NET interface (papp_abi.h has the calls). NULL on a
 * launcher before 0.29. The first call keeps Wi-Fi on for the run; what the app opens is
 * closed when it ends.
 *   const papp_if_net_t *net = papp_net();
 *   if (net && net->up(15000) == PAPP_E_OK) {
 *       int status;
 *       papp_http_req_t req = { .url = "https://example.com/data.json" };
 *       papp_http_t *h = net->http_open(&req, &status);
 *       if (h) { char buf[512]; int n; while ((n = net->http_read(h, buf, sizeof(buf))) > 0) use(buf, n);
 *                net->http_close(h); }
 *   } */
const papp_if_net_t *papp_net(void);

/* ── Bluetooth LE ───────────────────────────────────────────────────────── */
/* The BLE interface (papp_abi.h has the calls). NULL on a launcher before 0.29 and on boards
 * without BLE; available() starts the host (false: the C6 firmware has no BLE). A peripheral:
 *   static uint16_t h_val;
 *   static const papp_ble_chr_t chrs[] = { { PAPP_BLE_UUID16(0x2A19), PAPP_BLE_READ | PAPP_BLE_NOTIFY, 1, &h_val } };
 *   static const papp_ble_svc_t svc = { PAPP_BLE_UUID16(0x180F), chrs, 1 };
 *   ble->gatt_register(&svc, 1);              (first: the table goes in when the host starts)
 *   uint8_t adv[31]; int n = 0;
 *   papp_ble_ad_flags(adv, &n); papp_ble_ad_name(adv, &n, "my-app");
 *   papp_ble_adv_cfg_t a = { .adv = adv, .adv_len = n, .connectable = 1 };
 *   ble->adv_start(&a);    then wait_event() for CONNECTED / WRITE / SUBSCRIBE, notify() values */
const papp_if_ble_t *papp_ble(void);

#define PAPP_BLE_UUID16(v) { 2, { (uint8_t)((v) & 0xFF), (uint8_t)((v) >> 8) } }

/* Advertising data: appends one AD structure to buf (31 bytes at most); false if it doesn't fit */
static inline bool papp_ble_ad_add(uint8_t *buf, int *len, uint8_t type, const void *data, int n)
{
    if (*len + 2 + n > 31) return false;
    buf[(*len)++] = (uint8_t)(n + 1);
    buf[(*len)++] = type;
    for (int i = 0; i < n; i++) buf[(*len)++] = ((const uint8_t *)data)[i];
    return true;
}
static inline bool papp_ble_ad_flags(uint8_t *buf, int *len)        /* general discoverable, LE only */
{
    uint8_t f = 0x06;
    return papp_ble_ad_add(buf, len, 0x01, &f, 1);
}
static inline bool papp_ble_ad_name(uint8_t *buf, int *len, const char *name)
{
    int n = 0;
    while (name[n]) n++;
    return papp_ble_ad_add(buf, len, 0x09, name, n);
}
static inline bool papp_ble_ad_uuid(uint8_t *buf, int *len, const papp_ble_uuid_t *u)  /* complete list of one */
{
    return papp_ble_ad_add(buf, len, u->len == 2 ? 0x03 : u->len == 4 ? 0x05 : 0x07, u->u, u->len);
}
/* Finds an AD structure in received data (an ADV event): its payload and length, or NULL */
static inline const uint8_t *papp_ble_ad_find(const uint8_t *data, int len, uint8_t type, int *out_len)
{
    for (int i = 0; i + 1 < len && data[i];) {
        int n = data[i];
        if (i + 1 + n > len) break;
        if (data[i + 1] == type) { *out_len = n - 1; return &data[i + 2]; }
        i += n + 1;
    }
    return 0;
}
/* "6e400001-b5a3-f393-e0a9-e50e24dcca9e" -> a 128-bit UUID (stored little endian, as on air) */
static inline bool papp_ble_uuid_parse(const char *s, papp_ble_uuid_t *out)
{
    int k = 15, half = -1;
    out->len = 16;
    for (; *s && k >= 0; s++) {
        int v = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10 :
                *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (v < 0) { if (*s == '-') continue; return false; }
        if (half < 0) half = v;
        else { out->u[k--] = (uint8_t)(half << 4 | v); half = -1; }
    }
    return k < 0 && !*s;
}

/* ── Raw interfaces ─────────────────────────────────────────────────────── */
extern const papp_if_sys_t *papp_if_sys(void);
extern const papp_if_mem_t *papp_if_mem(void);
extern const papp_if_disp_t *papp_if_disp(void);   /* NULL if absent */
extern const papp_if_audi_t *papp_if_audi(void);   /* NULL if absent */
extern const papp_if_inpt_t *papp_if_inpt(void);   /* NULL if absent */
extern const papp_if_thrd_t *papp_if_thrd(void);   /* NULL if absent */
extern const papp_if_ui_t   *papp_if_ui(void);     /* NULL if absent */

#ifdef __cplusplus
}
#endif
