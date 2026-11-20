/*
 * Input manager core — logical devices, event queue, system chord, injection.
 * USB HID lives in lx_input_usb.c and only calls the source functions below.
 */
#include "lx_input.h"
#include "lx_input_priv.h"
#include "bsp.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "lx_input";

#define QUEUE_LEN      64
#define CHORD_HOLD_US  1000000

typedef struct {
    bool     attached;              /* a real device owns the slot */
    papp_gamepad_t st;              /* device state (buttons without injection) */
    uint32_t injected;
    uint32_t effective;             /* st.buttons | injected, as last reported */
    int64_t  chord_since;           /* Select+Start both down since (0 = no) */
    bool     chord_fired;
    esp_timer_handle_t release;
    uint16_t last_vid, last_pid;    /* survive a disconnect: the pad gets its slot back */
    bool     has_map;
    uint8_t  map[LX_INPUT_MAP_BITS];    /* raw button bit -> PAPP_BTN_ bit, 0xFF = none */
} pad_t;

static SemaphoreHandle_t s_lock;
static lx_input_sys_cb_t s_sys_cb;
static pad_t s_pads[PAPP_MAX_PLAYERS];
static bool s_kbd_attached;
static uint8_t s_kbd_mods, s_kbd_keys[6];
static uint8_t s_keymap[32];                /* bitmap of HID usages down */
static uint16_t s_injected_key;
static esp_timer_handle_t s_key_release, s_chord_tick;
static papp_input_event_t s_queue[QUEUE_LEN];
static uint32_t s_q_head, s_q_tail;
static bool s_board;                        /* board buttons merge into player 1 */
static uint32_t s_board_buttons;
static int s_paddle = -1;

#define LOCK()   xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

/* ── Events ─────────────────────────────────────────────────────────────── */

static void push_event(uint8_t type, uint8_t device, uint16_t code, uint16_t text)
{
    if (s_q_head - s_q_tail == QUEUE_LEN) s_q_tail++;      /* full: drop the oldest */
    s_queue[s_q_head++ % QUEUE_LEN] = (papp_input_event_t){
        .type = type, .device = device, .code = code, .mods = s_kbd_mods, .text = text,
        .time_ms = (uint32_t)(esp_timer_get_time() / 1000),
    };
}

bool lx_input_poll_event(papp_input_event_t *out)
{
    LOCK();
    bool any = s_q_tail != s_q_head;
    if (any) *out = s_queue[s_q_tail++ % QUEUE_LEN];
    UNLOCK();
    return any;
}

void lx_input_flush(void)
{
    LOCK();
    s_q_tail = s_q_head;
    UNLOCK();
}

/* ── Gamepads ───────────────────────────────────────────────────────────── */

static uint32_t pad_mapped(const pad_t *p, uint32_t raw)
{
    if (!p->has_map) return raw;
    uint32_t out = 0;
    for (int i = 0; i < LX_INPUT_MAP_BITS; i++)
        if ((raw & (1u << i)) && p->map[i] < 32) out |= 1u << p->map[i];
    return out;
}

/* Emit button events for a change of a pad's effective state; detect chords.
 * Called with the lock held. Returns true if the system chord fired. */
static bool pad_refresh(int player)
{
    pad_t *p = &s_pads[player];
    uint32_t now = pad_mapped(p, p->st.buttons | p->injected) | (player == 0 ? s_board_buttons : 0);
    uint32_t diff = now ^ p->effective;
    for (int b = 0; diff; b++, diff >>= 1) {
        if (diff & 1)
            push_event(now & (1u << b) ? PAPP_EV_BUTTON_DOWN : PAPP_EV_BUTTON_UP, player, b, 0);
    }
    bool fire = (now & PAPP_BTN_HOME) && !(p->effective & PAPP_BTN_HOME);
    p->effective = now;

    const uint32_t chord = PAPP_BTN_SELECT | PAPP_BTN_START;
    if ((now & chord) == chord) {
        int64_t t = esp_timer_get_time();
        if (!p->chord_since) p->chord_since = t;
        else if (!p->chord_fired && t - p->chord_since >= CHORD_HOLD_US) {
            p->chord_fired = true;
            fire = true;
        }
    } else {
        p->chord_since = 0;
        p->chord_fired = false;
    }
    return fire;
}

static void fire_sys(bool fire)
{
    if (fire && s_sys_cb) s_sys_cb(LX_INPUT_SYS_HOME);
}

bool lx_input_gamepad(int player, papp_gamepad_t *out)
{
    if (player < 0 || player >= PAPP_MAX_PLAYERS) return false;
    LOCK();
    pad_t *p = &s_pads[player];
    bool board = player == 0 && s_board;
    *out = p->st;
    out->buttons = p->effective;
    out->connected = p->attached || p->injected || board;
    if (!p->attached && board) snprintf(out->name, sizeof(out->name), "Board pad");
    else if (!p->attached && p->injected) snprintf(out->name, sizeof(out->name), "DevLink");
    bool c = out->connected;
    UNLOCK();
    return c;
}

/* Mappings live on the SD card (lx_padmap.c). Launchers before 0.23 kept them in
 * NVS: such a mapping still applies and is copied to the card for sharing. */
static void map_key(char *key, uint16_t vid, uint16_t pid)
{
    snprintf(key, 16, "p%04x%04x", vid, pid);
}

static bool map_load_nvs(uint16_t vid, uint16_t pid, uint8_t *map)
{
    nvs_handle_t h;
    if (nvs_open("padmap", NVS_READONLY, &h) != ESP_OK) return false;
    char key[16];
    map_key(key, vid, pid);
    size_t len = LX_INPUT_MAP_BITS;
    bool ok = nvs_get_blob(h, key, map, &len) == ESP_OK && len == LX_INPUT_MAP_BITS;
    nvs_close(h);
    return ok;
}

static const char *map_load(const char *name, uint16_t vid, uint16_t pid, uint8_t *map)
{
    switch (lx_padmap_load(vid, pid, map)) {
    case LX_PADMAP_MINE: return "your mapping";
    case LX_PADMAP_DB:   return "mapping from the database";
    }
    if (!map_load_nvs(vid, pid, map)) return NULL;
    lx_padmap_save(vid, pid, name, map);
    return "saved mapping";
}

int lx_input_pad_attach(const char *name, uint16_t vid, uint16_t pid)
{
    uint8_t map[LX_INPUT_MAP_BITS];
    const char *from = map_load(name, vid, pid, map);
    bool has_map = from != NULL;
    LOCK();
    int player = -1;
    for (int i = 0; i < PAPP_MAX_PLAYERS && player < 0; i++)    /* back to its own slot */
        if (!s_pads[i].attached && s_pads[i].last_vid == vid && s_pads[i].last_pid == pid) player = i;
    for (int i = 0; i < PAPP_MAX_PLAYERS && player < 0; i++)    /* else a slot nobody left */
        if (!s_pads[i].attached && !s_pads[i].last_vid) player = i;
    for (int i = 0; i < PAPP_MAX_PLAYERS && player < 0; i++)
        if (!s_pads[i].attached) player = i;
    if (player >= 0) {
        pad_t *p = &s_pads[player];
        p->attached = true;
        p->last_vid = vid;
        p->last_pid = pid;
        p->has_map = has_map;
        if (has_map) memcpy(p->map, map, sizeof(map));
        p->st = (papp_gamepad_t){ .connected = 1, .vid = vid, .pid = pid };
        snprintf(p->st.name, sizeof(p->st.name), "%s", name);
        push_event(PAPP_EV_DEVICE_ADDED, player, 0, 0);
    }
    UNLOCK();
    if (player >= 0) ESP_LOGI(TAG, "player %d: %s (%04X:%04X)%s%s", player + 1, name, vid, pid,
                              has_map ? ", " : "", has_map ? from : "");
    return player;
}

void lx_input_pad_update(int player, uint32_t buttons, int16_t lx, int16_t ly, int16_t rx, int16_t ry,
                         uint16_t lt, uint16_t rt)
{
    if (player < 0 || player >= PAPP_MAX_PLAYERS) return;
    LOCK();
    pad_t *p = &s_pads[player];
    p->st.buttons = buttons;
    p->st.lx = lx; p->st.ly = ly; p->st.rx = rx; p->st.ry = ry;
    p->st.lt = lt; p->st.rt = rt;
    bool fire = pad_refresh(player);
    UNLOCK();
    fire_sys(fire);
}

uint32_t lx_input_pad_raw(int player)
{
    if (player < 0 || player >= PAPP_MAX_PLAYERS) return 0;
    LOCK();
    uint32_t raw = s_pads[player].st.buttons | s_pads[player].injected;
    UNLOCK();
    return raw;
}

int lx_input_pad_first(void)
{
    LOCK();
    int player = -1;
    for (int i = 0; i < PAPP_MAX_PLAYERS && player < 0; i++)
        if (s_pads[i].attached) player = i;
    UNLOCK();
    return player;
}

esp_err_t lx_input_pad_set_map(int player, const uint8_t *map)
{
    if (player < 0 || player >= PAPP_MAX_PLAYERS || !s_pads[player].attached) return ESP_ERR_INVALID_ARG;
    pad_t *p = &s_pads[player];
    esp_err_t e = lx_padmap_save(p->st.vid, p->st.pid, p->st.name, map);
    char key[16];                           /* an NVS copy from before 0.23 must not come back */
    map_key(key, p->st.vid, p->st.pid);
    nvs_handle_t h;
    if (nvs_open("padmap", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_erase_key(h, key) == ESP_OK) nvs_commit(h);
        nvs_close(h);
    }
    uint8_t db[LX_INPUT_MAP_BITS];          /* without one of your own: the database's */
    bool from_db = !map && lx_padmap_load(p->st.vid, p->st.pid, db) == LX_PADMAP_DB;
    LOCK();
    p->has_map = map || from_db;
    if (map) memcpy(p->map, map, LX_INPUT_MAP_BITS);
    else if (from_db) memcpy(p->map, db, LX_INPUT_MAP_BITS);
    bool fire = pad_refresh(player);
    UNLOCK();
    fire_sys(fire);
    return e;
}

void lx_input_pad_reload_maps(void)
{
    for (int i = 0; i < PAPP_MAX_PLAYERS; i++) {
        LOCK();
        pad_t *p = &s_pads[i];
        bool attached = p->attached;
        uint16_t vid = p->st.vid, pid = p->st.pid;
        UNLOCK();
        if (!attached) continue;
        uint8_t map[LX_INPUT_MAP_BITS];
        bool has_map = lx_padmap_load(vid, pid, map) != LX_PADMAP_NONE;
        LOCK();
        if (p->attached && p->st.vid == vid && p->st.pid == pid) {
            p->has_map = has_map;
            if (has_map) memcpy(p->map, map, LX_INPUT_MAP_BITS);
        }
        bool fire = pad_refresh(i);
        UNLOCK();
        fire_sys(fire);
    }
}

void lx_input_pad_detach(int player)
{
    if (player < 0 || player >= PAPP_MAX_PLAYERS) return;
    LOCK();
    pad_t *p = &s_pads[player];
    p->st.buttons = 0;
    pad_refresh(player);                    /* release whatever was held */
    p->attached = false;
    memset(&p->st, 0, sizeof(p->st));
    push_event(PAPP_EV_DEVICE_REMOVED, player, 0, 0);
    UNLOCK();
    ESP_LOGI(TAG, "player %d disconnected", player + 1);
}

/* ── Keyboard ───────────────────────────────────────────────────────────── */

/* US layout: usage 0x04.. -> unshifted / shifted character */
static uint16_t key_text(uint8_t usage, uint8_t mods)
{
    static const char plain[]   = "abcdefghijklmnopqrstuvwxyz1234567890\r\x1b\b\t -=[]\\#;'`,./";
    static const char shifted[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\r\x1b\b\t _+{}|~:\"~<>?";
    bool shift = mods & (PAPP_MOD_LSHIFT | PAPP_MOD_RSHIFT);
    if (usage >= 0x04 && usage < 0x04 + sizeof(plain) - 1)
        return (uint8_t)(shift ? shifted : plain)[usage - 0x04];
    return 0;
}

static void key_set(uint8_t usage, bool down)
{
    if (!usage) return;
    bool was = s_keymap[usage >> 3] & (1u << (usage & 7));
    if (was == down) return;
    if (down) s_keymap[usage >> 3] |= 1u << (usage & 7);
    else      s_keymap[usage >> 3] &= ~(1u << (usage & 7));
    push_event(down ? PAPP_EV_KEY_DOWN : PAPP_EV_KEY_UP, 0xFF, usage, down ? key_text(usage, s_kbd_mods) : 0);
}

static bool has_key(const uint8_t keys[6], uint8_t k)
{
    for (int i = 0; i < 6; i++) if (keys[i] == k) return true;
    return false;
}

void lx_input_kbd_attach(void)
{
    LOCK();
    s_kbd_attached = true;
    push_event(PAPP_EV_DEVICE_ADDED, 0xFF, 0, 0);
    UNLOCK();
    ESP_LOGI(TAG, "keyboard connected");
}

void lx_input_kbd_report(uint8_t mods, const uint8_t keys[6])
{
    if (keys[0] == 0x01) return;            /* phantom state (too many keys): ignore */
    LOCK();
    s_kbd_mods = mods;
    for (int i = 0; i < 6; i++)
        if (s_kbd_keys[i] && !has_key(keys, s_kbd_keys[i])) key_set(s_kbd_keys[i], false);
    for (int i = 0; i < 6; i++)
        if (keys[i] && !has_key(s_kbd_keys, keys[i])) key_set(keys[i], true);
    memcpy(s_kbd_keys, keys, 6);
    UNLOCK();
}

void lx_input_kbd_detach(void)
{
    static const uint8_t none[6];
    lx_input_kbd_report(0, none);
    LOCK();
    s_kbd_attached = false;
    push_event(PAPP_EV_DEVICE_REMOVED, 0xFF, 0, 0);
    UNLOCK();
    ESP_LOGI(TAG, "keyboard disconnected");
}

bool lx_input_key_down(uint16_t usage)
{
    if (usage > 255) return false;
    return s_keymap[usage >> 3] & (1u << (usage & 7));
}

uint16_t lx_input_modifiers(void)
{
    return s_kbd_mods;
}

/* ── Injection ──────────────────────────────────────────────────────────── */

static void pad_release_cb(void *arg)
{
    int player = (int)(intptr_t)arg;
    LOCK();
    s_pads[player].injected = 0;
    pad_refresh(player);
    UNLOCK();
}

void lx_input_inject_buttons(int player, uint32_t buttons, uint32_t hold_ms)
{
    if (player < 0 || player >= PAPP_MAX_PLAYERS) return;
    pad_t *p = &s_pads[player];
    esp_timer_stop(p->release);
    LOCK();
    p->injected = buttons;
    bool fire = pad_refresh(player);
    UNLOCK();
    fire_sys(fire);
    esp_timer_start_once(p->release, (uint64_t)(hold_ms ? hold_ms : 100) * 1000);
}

static void key_release_cb(void *arg)
{
    LOCK();
    if (s_injected_key) key_set(s_injected_key, false);
    s_injected_key = 0;
    s_kbd_mods = 0;
    UNLOCK();
}

void lx_input_inject_key(uint16_t usage, uint16_t mods, uint32_t hold_ms)
{
    if (!usage || usage > 255) return;
    esp_timer_stop(s_key_release);
    LOCK();
    if (s_injected_key) key_set(s_injected_key, false);
    s_kbd_mods = mods;
    s_injected_key = usage;
    key_set(usage, true);
    UNLOCK();
    esp_timer_start_once(s_key_release, (uint64_t)(hold_ms ? hold_ms : 50) * 1000);
}

/* Pads that only report on change would never complete a held chord: re-check
 * every 100 ms while one is being held. */
static void chord_tick_cb(void *arg)
{
    bool fire = false;
    LOCK();
    for (int i = 0; i < PAPP_MAX_PLAYERS; i++)
        if (s_pads[i].chord_since && !s_pads[i].chord_fired) fire |= pad_refresh(i);
    UNLOCK();
    fire_sys(fire);
}

/* ── Board buttons ──────────────────────────────────────────────────────── */

_Static_assert(BSP_BTN_A == PAPP_BTN_A && BSP_BTN_L1 == PAPP_BTN_L1 && BSP_BTN_SELECT == PAPP_BTN_SELECT &&
               BSP_BTN_HOME == PAPP_BTN_HOME && BSP_BTN_UP == PAPP_BTN_UP && BSP_BTN_RIGHT == PAPP_BTN_RIGHT,
               "BSP button bits must match PAPP_BTN_*");

#define BOARD_POLL_MS 5

/* Polls the board's buttons (merged into player 1, so a built-in pad and a
 * USB pad both drive single-player apps) and the paddle. A change is taken
 * once two polls in a row agree: the shared ADC pins pass through the second
 * level while settling, which would otherwise flash B before A. */
static void board_task(void *arg)
{
    bool pad = bsp_info()->caps & BSP_CAP_GPIO_PAD;
    bool paddle = bsp_info()->caps & BSP_CAP_PADDLE;
    uint32_t last = 0;
    for (;;) {
        if (paddle) s_paddle = bsp_analog_read(BSP_ANALOG_PADDLE);
        if (pad) {
            uint32_t b = bsp_buttons_read();
            if (b == last && b != s_board_buttons) {
                LOCK();
                s_board_buttons = b;
                bool fire = pad_refresh(0);
                UNLOCK();
                fire_sys(fire);
            }
            last = b;
        }
        vTaskDelay(pdMS_TO_TICKS(BOARD_POLL_MS));
    }
}

static void board_start(void)
{
    if (bsp_buttons_init() != ESP_OK) return;
    uint32_t caps = bsp_info()->caps;
    if (caps & BSP_CAP_GPIO_PAD) {
        LOCK();
        s_board = true;
        push_event(PAPP_EV_DEVICE_ADDED, 0, 0, 0);
        UNLOCK();
    }
    ESP_LOGI(TAG, "board input:%s%s", caps & BSP_CAP_GPIO_PAD ? " GPIO pad (P1)" : "",
             caps & BSP_CAP_PADDLE ? " paddle" : "");
    xTaskCreatePinnedToCore(board_task, "lx_btn", 3072, NULL, 6, NULL, 0);
}

int lx_input_analog(int ch)
{
    if (ch >= PAPP_ANALOG_ACCEL_X && ch <= PAPP_ANALOG_ACCEL_Z) return bsp_analog_read(ch);   /* read when asked, not polled */
    return ch == PAPP_ANALOG_PADDLE ? s_paddle : -1;
}

/* ── Init / summary ─────────────────────────────────────────────────────── */

void lx_input_describe(char *buf, size_t len)
{
    size_t n = 0;
    buf[0] = 0;
    LOCK();
    for (int i = 0; i < PAPP_MAX_PLAYERS && n < len; i++) {
        if (s_pads[i].attached)
            n += snprintf(buf + n, len - n, "%sP%d %s", n ? ", " : "", i + 1, s_pads[i].st.name);
    }
    if (s_board && n < len) n += snprintf(buf + n, len - n, "%sboard pad (P1)", n ? ", " : "");
    if (s_kbd_attached && n < len) n += snprintf(buf + n, len - n, "%skeyboard", n ? ", " : "");
    UNLOCK();
}

esp_err_t lx_input_init(lx_input_sys_cb_t sys_cb)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    s_sys_cb = sys_cb;
    for (int i = 0; i < PAPP_MAX_PLAYERS; i++) {
        esp_timer_create_args_t a = { .callback = pad_release_cb, .arg = (void *)(intptr_t)i, .name = "inj_pad" };
        esp_timer_create(&a, &s_pads[i].release);
    }
    esp_timer_create_args_t k = { .callback = key_release_cb, .name = "inj_key" };
    esp_timer_create(&k, &s_key_release);
    esp_timer_create_args_t c = { .callback = chord_tick_cb, .name = "chord" };
    esp_timer_create(&c, &s_chord_tick);
    esp_timer_start_periodic(s_chord_tick, 100 * 1000);

    board_start();

    if (bsp_info()->caps & BSP_CAP_USB_HOST) return lx_input_usb_start();
    ESP_LOGI(TAG, "no USB host on this board");
    return ESP_OK;
}
