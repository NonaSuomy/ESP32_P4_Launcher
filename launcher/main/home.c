/*
 * Launcher home screen (design "Option 2", landscape): header with logo, clock,
 * SD card and settings; the installed apps, the last one run first; a system bar.
 * Plus a settings screen (volume, brightness, system info).
 *
 * LVGL 9 renders the whole 800x480 screen into one PSRAM buffer (direct mode);
 * after each update the compositor rotates it onto the panel. All LVGL calls
 * happen on the home task. While an app runs the task only waits: the app
 * owns the display and input, and the home screen is redrawn when it exits.
 *
 * Apps: /sd/apps/<dir>/<file>.papp, named by their manifest, with an optional
 * /sd/apps/<dir>/icon.png (any size; shown at 88x88). Gamepad / keyboard move
 * the focus to the nearest item in the direction pressed; A / Enter starts.
 *
 * Folders: a directory in /sd/apps with no .papp in it, holding app directories
 * (/sd/apps/<folder>/<dir>/<file>.papp), one level only. X / Y, or holding a tile,
 * opens its menu: move an app, rename or delete a folder.
 *
 * The Store (store.c), the first tile of the main page, finds, installs, updates
 * and removes apps; its tile shows how many updates are waiting.
 */
#include "home.h"
#include "home_ui.h"
#include "store.h"
#include "bsp.h"
#include "lx_app.h"
#include "lx_audio.h"
#include "lx_comp.h"
#include "lx_common.h"
#include "lx_input.h"
#include "lx_update.h"
#include "lx_appstore.h"
#include "lx_net.h"
#include "lx_c6.h"
#include "lx_web.h"
#include "esp_system.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/temperature_sensor.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "home";

/* Paths are capped on purpose; a longer one is cut, not overflowed */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define LAST_APP_FILE "/sd/launcher/last_app.txt"
#define MAX_APPS      64           /* apps + folders */
#define MAX_NAME      24           /* folder name */
#define ICON          88

static const uint32_t TILE_COLORS[] = { 0x2F7BF6, 0x8E44EC, 0x22B35E, 0xF39C12, 0x12A7A7, 0xE8457C, 0x5B4BDB, 0x3B4A63 };

typedef struct {
    char path[160];             /* .papp, or the directory of a folder */
    char icon[176];             /* "S:/sd/apps/x/icon.png" or "" */
    char name[32];
    char id[32];
    char version[16];
    char parent[MAX_NAME + 8];  /* folder it is in, "" = main page */
    bool hidden;                /* PAPP_MF_HIDDEN */
    bool is_folder;
} app_entry_t;

static app_entry_t *s_apps;     /* MAX_APPS, in PSRAM: internal RAM is short at boot (7" board) */
static int s_napps;             /* entries in s_apps */
static int s_ninstalled;        /* of them, apps */
static char s_folder[MAX_NAME + 8];     /* folder on screen, "" = main page */
static char s_files_app[160];   /* the Files app: opened by the SD card panel, not a tile */
static char s_last[160];        /* last launched .papp */

static lv_display_t *s_disp;
static uint16_t *s_frame;
static lv_obj_t *s_home, *s_settings;
static lv_obj_t *s_clk_area;            /* home clock: opens the clock setup (RTC boards) */
static void on_clk_open(lv_event_t *ev);
static lv_obj_t *s_clock, *s_date, *s_sd_free, *s_sd_apps, *s_grid, *s_count, *s_title;
static lv_obj_t *s_sys_cpu, *s_sys_sram, *s_sys_psram, *s_sys_temp, *s_sys_pads, *s_sys_state;
static lv_obj_t *s_vol_slider, *s_bri_slider, *s_vol_val, *s_bri_val, *s_info;
static temperature_sensor_handle_t s_tsens;
static char s_launch[160];      /* set by a click; launched by the task loop */
static int s_volume = 60, s_brightness = 100;
static bool s_show_dev;          /* show PAPP_MF_HIDDEN apps */
static lv_obj_t *s_dev_switch;

/* ── Focus (gamepad / keyboard): nearest item in the pressed direction ──── */

#define MAX_FOCUS 160            /* the Store: apps + categories + buttons */
static lv_obj_t *s_focusable[MAX_FOCUS];
static int s_nfocus;
static lv_obj_t *s_focused;

static void focus_clear_list(void)
{
    s_nfocus = 0;
    s_focused = NULL;
}

static void focus_add(lv_obj_t *o)
{
    if (s_nfocus < MAX_FOCUS) s_focusable[s_nfocus++] = o;
}

static void focus_set(lv_obj_t *o)
{
    if (s_focused == o) return;
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);
    s_focused = o;
    if (o) {
        lv_obj_add_state(o, LV_STATE_FOCUSED);
        if (!lv_obj_get_style_outline_width(o, LV_PART_MAIN)) {    /* message box buttons have no ring of their own */
            lv_obj_set_style_outline_width(o, 3, LV_STATE_FOCUSED);
            lv_obj_set_style_outline_color(o, C_ACCENT, LV_STATE_FOCUSED);
            lv_obj_set_style_outline_pad(o, 3, LV_STATE_FOCUSED);
        }
        lv_obj_scroll_to_view_recursive(o, LV_ANIM_ON);
    }
}

static void focus_move(int dx, int dy)
{
    if (!s_focused) {
        if (s_nfocus) focus_set(s_focusable[0]);
        return;
    }
    lv_area_t a;
    lv_obj_get_coords(s_focused, &a);
    int cx = (a.x1 + a.x2) / 2, cy = (a.y1 + a.y2) / 2;
    lv_obj_t *best = NULL;
    int best_score = 1 << 30;
    for (int i = 0; i < s_nfocus; i++) {
        lv_obj_t *o = s_focusable[i];
        if (o == s_focused) continue;
        lv_area_t b;
        lv_obj_get_coords(o, &b);
        int ox = (b.x1 + b.x2) / 2 - cx, oy = (b.y1 + b.y2) / 2 - cy;
        int along = ox * dx + oy * dy, across = abs(dx ? oy : ox);
        if (along <= 0) continue;
        /* Something in the same row (or column) wins over a nearer one beside it */
        bool inline_ = dx ? b.y1 <= a.y2 && b.y2 >= a.y1 : b.x1 <= a.x2 && b.x2 >= a.x1;
        int score = inline_ ? along : 100000 + along + 3 * across;
        if (score < best_score) { best_score = score; best = o; }
    }
    if (best) focus_set(best);
}

/* ── Apps ──────────────────────────────────────────────────────────────── */

static int app_cmp(const void *a, const void *b)
{
    const app_entry_t *x = a, *y = b;
    if (x->is_folder != y->is_folder) return y->is_folder - x->is_folder;
    return strcasecmp(x->name, y->name);
}

/* Adds the app in dir; false if there is no .papp in it */
static bool scan_app(const char *dir, const char *parent)
{
    bool found = false;
    DIR *sd = opendir(dir);
    if (!sd) return true;
    struct dirent *fe;
    while (s_napps < MAX_APPS && (fe = readdir(sd))) {
        size_t n = strlen(fe->d_name);
        if (n < 6 || strcasecmp(fe->d_name + n - 5, ".papp") != 0) continue;
        app_entry_t *e = &s_apps[s_napps];
        memset(e, 0, sizeof(*e));
        snprintf(e->parent, sizeof(e->parent), "%s", parent);
        snprintf(e->path, sizeof(e->path), "%s/%s", dir, fe->d_name);
        papp_manifest_t m;
        if (lx_app_read_manifest(e->path, &m) == ESP_OK) {
            snprintf(e->name, sizeof(e->name), "%s", m.name);
            snprintf(e->id, sizeof(e->id), "%s", m.id);
            snprintf(e->version, sizeof(e->version), "%s", m.version);
            e->hidden = m.flags & PAPP_MF_HIDDEN;
            if (strcmp(m.id, "tool.files") == 0) {
                snprintf(s_files_app, sizeof(s_files_app), "%s", e->path);
                found = true;
                break;
            }
        } else {
            snprintf(e->name, sizeof(e->name), "%.*s", (int)(n - 5), fe->d_name);
        }
        char icon[160];
        struct stat st;
        snprintf(icon, sizeof(icon), "%s/icon.png", dir);
        if (stat(icon, &st) == 0) snprintf(e->icon, sizeof(e->icon), "S:%s", icon);
        if (!e->hidden || s_show_dev) {
            s_napps++;
            s_ninstalled++;
        }
        found = true;
        break;                                      /* one app per directory */
    }
    closedir(sd);
    return found;
}

static void scan_dir(const char *base, const char *parent)
{
    DIR *d = opendir(base);
    if (!d) return;
    struct dirent *de;
    while (s_napps < MAX_APPS && (de = readdir(d))) {
        if (de->d_name[0] == '.' || de->d_type != DT_DIR) continue;
        char dir[128];
        snprintf(dir, sizeof(dir), "%s/%s", base, de->d_name);
        if (scan_app(dir, parent) || parent[0] || s_napps >= MAX_APPS) continue;
        app_entry_t *e = &s_apps[s_napps++];        /* no app in it: a folder */
        memset(e, 0, sizeof(*e));
        e->is_folder = true;
        snprintf(e->path, sizeof(e->path), "%s", dir);
        snprintf(e->name, sizeof(e->name), "%s", de->d_name);
        char icon[160];                             /* a folder can have its own icon too */
        struct stat st;
        snprintf(icon, sizeof(icon), "%s/icon.png", dir);
        if (stat(icon, &st) == 0) snprintf(e->icon, sizeof(e->icon), "S:%s", icon);
        scan_dir(dir, de->d_name);
    }
    closedir(d);
}

/* The Community folder is always on the home screen, so users know where shared apps
 * go (the Store installs them there). Made once per boot when the card has none. Its
 * icon and the Store's are built into the launcher and written to the card for LVGL. */
extern const uint8_t folder_community_png_start[] asm("_binary_folder_community_png_start");
extern const uint8_t folder_community_png_end[] asm("_binary_folder_community_png_end");
extern const uint8_t store_png_start[] asm("_binary_store_png_start");
extern const uint8_t store_png_end[] asm("_binary_store_png_end");
#define STORE_ICON "/sd/launcher/store.png"

/* Writes a built-in file unless the card has one (of the same size: keep_any = any size) */
static void embedded_file(const char *path, const uint8_t *start, const uint8_t *end, bool keep_any)
{
    struct stat st;
    if (stat(path, &st) == 0 && (keep_any || st.st_size == end - start)) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(start, 1, end - start, f);
    fclose(f);
}

static void community_folder(void)
{
    static bool done;
    struct stat st;
    if (done) return;
    if (stat(APPS_DIR, &st) != 0) return;                   /* no card (yet) */
    done = true;
    mkdir("/sd/launcher", 0775);
    embedded_file(STORE_ICON, store_png_start, store_png_end, false);
    const char *dir = APPS_DIR "/" LX_APPSTORE_COMMUNITY;
    if (stat(dir, &st) != 0 && mkdir(dir, 0775) != 0) return;
    embedded_file(APPS_DIR "/" LX_APPSTORE_COMMUNITY "/icon.png", folder_community_png_start,
                  folder_community_png_end, true);
}

static void scan_apps(void)
{
    community_folder();
    s_napps = s_ninstalled = 0;
    s_files_app[0] = 0;
    if (!s_apps) s_apps = heap_caps_calloc(MAX_APPS, sizeof(app_entry_t), MALLOC_CAP_SPIRAM);
    if (!s_apps) return;
    scan_dir(APPS_DIR, "");
    bool here = false;
    for (int i = 0; i < s_napps; i++) here |= s_apps[i].is_folder && strcmp(s_apps[i].name, s_folder) == 0;
    if (!here) s_folder[0] = 0;
    qsort(s_apps, s_napps, sizeof(app_entry_t), app_cmp);

    s_last[0] = 0;
    FILE *f = fopen(LAST_APP_FILE, "r");
    if (f) {
        if (fgets(s_last, sizeof(s_last), f)) s_last[strcspn(s_last, "\r\n")] = 0;
        fclose(f);
    }
    /* The last app run comes first (and gets the focus) */
    for (int i = 1; i < s_napps; i++) {
        if (strcmp(s_apps[i].path, s_last) != 0) continue;
        app_entry_t last = s_apps[i];
        memmove(&s_apps[1], &s_apps[0], i * sizeof(app_entry_t));
        s_apps[0] = last;
        break;
    }
}

static void remember_last(const char *path)
{
    mkdir("/sd/launcher", 0775);
    FILE *f = fopen(LAST_APP_FILE, "w");
    if (f) {
        fprintf(f, "%s\n", path);
        fclose(f);
    }
    snprintf(s_last, sizeof(s_last), "%s", path);
}

/* An icon: the PNG src scaled to size, or a coloured tile with the initial of name
 * (key picks the colour; a folder gets the folder symbol) */
static lv_obj_t *icon_box(lv_obj_t *parent, const char *src, const char *key, const char *name, bool folder, int size)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_style_radius(box, size / 4, 0);
    lv_obj_set_style_clip_corner(box, true, 0);

    if (src && src[0]) {
        lv_image_header_t h;
        if (lv_image_decoder_get_info(src, &h) == LV_RESULT_OK && h.w && h.h) {
            lv_obj_t *img = lv_image_create(box);
            lv_image_set_src(img, src);
            int big = h.w > h.h ? h.w : h.h;
            lv_image_set_scale(img, (uint32_t)(256 * size / big));
            lv_obj_center(img);
            return box;
        }
    }
    uint32_t hash = 5381;
    for (const char *p = key && key[0] ? key : "?"; *p; p++) hash = hash * 33 + (uint8_t)*p;
    uint32_t c = folder ? 0xD9921A : TILE_COLORS[hash % (sizeof(TILE_COLORS) / sizeof(TILE_COLORS[0]))];
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(box, lv_color_lighten(lv_color_hex(c), 40), 0);
    lv_obj_set_style_bg_grad_color(box, lv_color_hex(c), 0);
    lv_obj_set_style_bg_grad_dir(box, LV_GRAD_DIR_VER, 0);
    lv_obj_t *l = lv_label_create(box);
    char initial[2] = { name && name[0] ? (char)toupper((unsigned char)name[0]) : '?', 0 };
    lv_label_set_text(l, folder ? LV_SYMBOL_DIRECTORY : initial);
    lv_obj_set_style_text_font(l, size >= 80 ? &lv_font_montserrat_48 : &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, C_TEXT, 0);
    lv_obj_center(l);
    return box;
}

static lv_obj_t *make_icon(lv_obj_t *parent, const app_entry_t *e, int size)
{
    return icon_box(parent, e->icon, e->id[0] ? e->id : e->name, e->name, e->is_folder, size);
}

/* The Store's tile: first on the main page, not an app on the card */
static app_entry_t s_store_tile = { .path = "store", .icon = "S:" STORE_ICON, .name = "Store", .id = "launcher.store" };
static lv_obj_t *s_store_badge;         /* updates waiting, on the tile */
static int s_badge_shown;

/* ── Clicks ────────────────────────────────────────────────────────────── */

static bool s_long_press;        /* the click ending a long press is not a click */
static void folder_enter(const char *name);
static void tile_menu(const app_entry_t *e);

static void on_tile_click(lv_event_t *ev)
{
    const app_entry_t *e = lv_event_get_user_data(ev);
    if (s_long_press) s_long_press = false;
    else if (e == &s_store_tile) store_open(NULL);
    else if (e->is_folder) folder_enter(e->name);
    else snprintf(s_launch, sizeof(s_launch), "%s", e->path);
}

static void on_tile_long(lv_event_t *ev)
{
    s_long_press = true;
    tile_menu(lv_event_get_user_data(ev));
}

static void show_settings(void);

static void on_gear_click(lv_event_t *ev)   { show_settings(); }
static void on_back_click(lv_event_t *ev)   { show_home(); }

/* ── Home screen ───────────────────────────────────────────────────────── */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

/* The layout is drawn for 800x480; on a bigger screen right-hand items move by
 * s_dx, bottom ones by s_dy, and full-width ones grow by them */
static int s_dx, s_dy;

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(p, radius, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(p, C_PANEL, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, C_BORDER, 0);
    return p;
}

/* Focus look for clickable things: a bright outline */
static void focus_style(lv_obj_t *o, int radius)
{
    lv_obj_set_style_outline_width(o, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(o, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(o, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_radius(o, radius, 0);
}

static void badge_update(void)
{
    s_badge_shown = store_updates();
    if (!s_store_badge) return;
    lv_label_set_text_fmt(s_store_badge, "%d", s_badge_shown);
    if (s_badge_shown) lv_obj_remove_flag(s_store_badge, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_store_badge, LV_OBJ_FLAG_HIDDEN);
}

static void build_grid(void)
{
    lv_obj_clean(s_grid);
    s_store_badge = NULL;
    int shown = 0;
    for (int i = -1; i < s_napps; i++) {
        app_entry_t *e = i < 0 ? &s_store_tile : &s_apps[i];
        if (i < 0 ? s_folder[0] != 0 : strcmp(e->parent, s_folder) != 0) continue;
        shown += i >= 0;
        lv_obj_t *t = lv_obj_create(s_grid);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, 118, 128);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        focus_style(t, 18);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(t, C_FOCUS, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(t, C_FOCUS, LV_STATE_PRESSED);
        lv_obj_t *ic = make_icon(t, e, ICON);
        lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_t *l = label(t, &lv_font_montserrat_16, C_TEXT, e->name);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, 112);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_obj_set_user_data(t, e);
        lv_obj_add_event_cb(t, on_tile_click, LV_EVENT_CLICKED, e);
        lv_obj_add_event_cb(t, on_tile_long, LV_EVENT_LONG_PRESSED, e);
        if (i < 0) {
            s_store_badge = label(t, &lv_font_montserrat_16, C_TEXT, "");
            lv_obj_set_style_bg_opa(s_store_badge, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(s_store_badge, C_RED, 0);
            lv_obj_set_style_radius(s_store_badge, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_min_width(s_store_badge, 26, 0);
            lv_obj_set_style_pad_ver(s_store_badge, 3, 0);
            lv_obj_set_style_pad_hor(s_store_badge, 6, 0);
            lv_obj_set_style_text_align(s_store_badge, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(s_store_badge, LV_ALIGN_TOP_RIGHT, -6, 0);
            badge_update();
        }
    }
    if (!shown) {
        lv_obj_t *l = label(s_grid, &lv_font_montserrat_16, C_DIM, s_folder[0] ?
                            "This folder is empty. To add an app, hold it or press X on the main page." :
                            "No apps on the SD card yet: open the Store to get some.");
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(l, s_folder[0] ? 700 : 560);
        lv_obj_set_style_pad_top(l, 40, 0);
    }
    if (s_folder[0]) {
        char buf[48];
        snprintf(buf, sizeof(buf), LV_SYMBOL_DIRECTORY "  %s", s_folder);
        lv_label_set_text(s_title, buf);
        lv_label_set_text(s_count, LV_SYMBOL_LEFT " Back");
    } else {
        lv_label_set_text(s_title, "Applications");
        lv_label_set_text(s_count, LV_SYMBOL_PLUS " New folder");
    }
}

static lv_obj_t *s_gear, *s_sd;

static void home_focus_list(void)
{
    lv_obj_t *keep = s_focused;
    focus_clear_list();
    lv_obj_remove_state(s_gear, LV_STATE_FOCUSED);      /* s_focused may have been reset */
    lv_obj_remove_state(s_count, LV_STATE_FOCUSED);
    lv_obj_remove_state(s_sd, LV_STATE_FOCUSED);
    if (s_clk_area) lv_obj_remove_state(s_clk_area, LV_STATE_FOCUSED);
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_grid); i++) {
        lv_obj_t *c = lv_obj_get_child(s_grid, i);
        if (lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE)) focus_add(c);
    }
    focus_add(s_gear);
    focus_add(s_sd);
    focus_add(s_count);
    if (s_clk_area) focus_add(s_clk_area);
    bool still_there = false;
    for (int i = 0; i < s_nfocus; i++) still_there |= s_focusable[i] == keep;
    s_focused = NULL;
    lv_obj_t *first = s_nfocus ? s_focusable[0] : NULL;
    if (first && lv_obj_get_user_data(first) == &s_store_tile && s_nfocus > 1 &&
        lv_obj_get_parent(s_focusable[1]) == s_grid)
        first = s_focusable[1];                         /* the last app run, ready to start again */
    focus_set(still_there ? keep : first);
    if (!still_there) lv_obj_scroll_to_y(s_grid, 0, LV_ANIM_OFF);
}

/* Redraws the grid and puts the focus on the entry with this path */
static void home_show(const char *focus)
{
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);
    s_focused = NULL;
    build_grid();
    for (uint32_t i = 0; focus && i < lv_obj_get_child_count(s_grid); i++) {
        lv_obj_t *c = lv_obj_get_child(s_grid, i);
        const app_entry_t *e = lv_obj_get_user_data(c);
        if (e && strcmp(e->path, focus) == 0) s_focused = c;
    }
    home_focus_list();
}

static void home_view(const char *focus)
{
    scan_apps();
    home_show(focus);
}

static void on_count_click(lv_event_t *ev);
static void on_sd_click(lv_event_t *ev);

static void build_home(void)
{
    s_home = lv_obj_create(NULL);
    lv_obj_remove_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_home, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_home, C_BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(s_home, LV_GRAD_DIR_VER, 0);

    /* Header: logo, clock, SD card, settings */
    lv_obj_t *p4 = label(s_home, &lv_font_montserrat_48, C_ACCENT, "P4");
    lv_obj_set_pos(p4, 18, 6);
    lv_obj_t *name = label(s_home, &lv_font_montserrat_48, C_TEXT, "Launcher");
    lv_obj_set_pos(name, 92, 6);
    lv_obj_t *sub = label(s_home, &lv_font_montserrat_16, C_DIM, "ESP32-P4  Application Hub");
    lv_obj_set_pos(sub, 22, 62);

    if (bsp_info()->caps & BSP_CAP_RTC) {               /* the clock opens its setup */
        s_clk_area = lv_obj_create(s_home);
        lv_obj_remove_style_all(s_clk_area);
        lv_obj_set_pos(s_clk_area, 340 + s_dx / 2, 4);
        lv_obj_set_size(s_clk_area, 270, 82);
        lv_obj_add_flag(s_clk_area, LV_OBJ_FLAG_CLICKABLE);
        focus_style(s_clk_area, 14);
        lv_obj_set_style_bg_color(s_clk_area, C_FOCUS, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(s_clk_area, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(s_clk_area, on_clk_open, LV_EVENT_CLICKED, NULL);
    }
    s_clock = label(s_home, &lv_font_montserrat_48, C_TEXT, "--:--");
    lv_obj_set_pos(s_clock, 352 + s_dx / 2, 6);
    s_date = label(s_home, &lv_font_montserrat_16, C_DIM, "");
    lv_obj_set_pos(s_date, 356 + s_dx / 2, 62);

    lv_obj_t *sd = s_sd = panel(s_home, 506 + s_dx, 10, 214, 76, 14);
    lv_obj_add_flag(sd, LV_OBJ_FLAG_CLICKABLE);
    focus_style(sd, 14);
    lv_obj_set_style_bg_color(sd, C_FOCUS, LV_STATE_PRESSED);
    lv_obj_add_event_cb(sd, on_sd_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sdi = label(sd, &lv_font_montserrat_28, C_GREEN, LV_SYMBOL_SD_CARD);
    lv_obj_set_pos(sdi, 14, 20);
    lv_obj_t *sdt = label(sd, &lv_font_montserrat_16, C_TEXT, "SD Card");
    lv_obj_set_pos(sdt, 52, 6);
    s_sd_free = label(sd, &lv_font_montserrat_16, C_GREEN, "");
    lv_obj_set_pos(s_sd_free, 52, 28);
    s_sd_apps = label(sd, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_set_pos(s_sd_apps, 52, 50);

    s_gear = lv_obj_create(s_home);
    lv_obj_remove_style_all(s_gear);
    lv_obj_set_pos(s_gear, 732 + s_dx, 20);
    lv_obj_set_size(s_gear, 56, 56);
    lv_obj_add_flag(s_gear, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_gear, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_gear, lv_color_hex(0x16243D), 0);
    lv_obj_set_style_border_width(s_gear, 1, 0);
    lv_obj_set_style_border_color(s_gear, C_BORDER, 0);
    focus_style(s_gear, 14);
    lv_obj_t *gi = label(s_gear, &lv_font_montserrat_28, C_TEXT, LV_SYMBOL_SETTINGS);
    lv_obj_center(gi);
    lv_obj_add_event_cb(s_gear, on_gear_click, LV_EVENT_CLICKED, NULL);

    /* Applications */
    s_title = label(s_home, &lv_font_montserrat_20, C_TEXT, "Applications");
    lv_obj_set_pos(s_title, 20, 100);
    s_count = label(s_home, &lv_font_montserrat_16, lv_color_hex(0x6F8FC0), "");
    lv_obj_align(s_count, LV_ALIGN_TOP_RIGHT, -22, 103);
    lv_obj_add_flag(s_count, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_count, 12);
    focus_style(s_count, 6);
    lv_obj_add_event_cb(s_count, on_count_click, LV_EVENT_CLICKED, NULL);

    s_grid = lv_obj_create(s_home);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_pos(s_grid, 10, 128);
    lv_obj_set_size(s_grid, 780 + s_dx, 292 + s_dy);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(s_grid, 6, 0);
    lv_obj_set_style_pad_column(s_grid, 8, 0);
    lv_obj_set_style_pad_row(s_grid, 6, 0);
    lv_obj_set_scroll_dir(s_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_ACTIVE);

    /* System bar */
    lv_obj_t *bar = panel(s_home, 16, 426 + s_dy, 768 + s_dx, 46, 12);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_sys_cpu = label(bar, &lv_font_montserrat_14, C_TEXT, "");
    s_sys_sram = label(bar, &lv_font_montserrat_14, C_TEXT, "");
    s_sys_psram = label(bar, &lv_font_montserrat_14, C_TEXT, "");
    s_sys_temp = label(bar, &lv_font_montserrat_14, C_TEXT, "");
    s_sys_pads = label(bar, &lv_font_montserrat_14, C_TEXT, "");
    s_sys_state = label(bar, &lv_font_montserrat_14, C_GREEN, LV_SYMBOL_OK " System ready");
}

static void refresh_home(void)
{
    scan_apps();
    build_grid();
    home_focus_list();
}

/* ── Settings screen ───────────────────────────────────────────────────── */

static void save_settings(void)
{
    nvs_handle_t h;
    if (nvs_open("launcher", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "volume", (uint8_t)s_volume);
    nvs_set_u8(h, "bright", (uint8_t)s_brightness);
    nvs_commit(h);
    nvs_close(h);
}

static void apply_settings(void)
{
    lx_audio_master_volume(s_volume);
    bsp_display_backlight(s_brightness);
    char buf[16];
    if (s_vol_val) {
        snprintf(buf, sizeof(buf), "%d%%", s_volume);
        lv_label_set_text(s_vol_val, buf);
        snprintf(buf, sizeof(buf), "%d%%", s_brightness);
        lv_label_set_text(s_bri_val, buf);
    }
}

static void on_slider(lv_event_t *ev)
{
    lv_obj_t *s = lv_event_get_target(ev);
    if (s == s_vol_slider) s_volume = lv_slider_get_value(s);
    else s_brightness = lv_slider_get_value(s);
    apply_settings();
    if (lv_event_get_code(ev) == LV_EVENT_RELEASED) save_settings();
}

static lv_obj_t *settings_row(lv_obj_t *parent, int y, const char *title, const char *symbol,
                              int min, int max, int value, lv_obj_t **val_out)
{
    lv_obj_t *row = panel(parent, 24, y, 752 + s_dx, 64, 14);
    lv_obj_t *ic = label(row, &lv_font_montserrat_24, C_ACCENT, symbol);
    lv_obj_set_pos(ic, 18, 18);
    lv_obj_t *t = label(row, &lv_font_montserrat_20, C_TEXT, title);
    lv_obj_set_pos(t, 60, 20);
    lv_obj_t *s = lv_slider_create(row);
    lv_obj_set_size(s, 420 + s_dx, 12);
    lv_obj_set_pos(s, 220, 26);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, C_BLUE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, C_TEXT, LV_PART_KNOB);
    lv_obj_set_style_outline_width(s, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(s, 8, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(s, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, on_slider, LV_EVENT_RELEASED, NULL);
    *val_out = label(row, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_set_pos(*val_out, 668 + s_dx, 20);
    return s;
}

static lv_obj_t *s_back;
static void refresh_info(void);
static void upd_message(const char *title, const char *text);
static void show_settings(void);

/* ── Controller mapping ──────────────────────────────────────── */

#define MAP_STEP_US 6000000             /* nothing pressed for this long: skip the step */

static const struct { const char *name; uint8_t bit; } k_map_steps[] = {
    { "D-pad UP", 16 }, { "D-pad DOWN", 17 }, { "D-pad LEFT", 18 }, { "D-pad RIGHT", 19 },
    { "A", 0 }, { "B", 1 }, { "X", 2 }, { "Y", 3 }, { "L1", 4 }, { "R1", 5 }, { "L2", 6 },
    { "R2", 7 }, { "SELECT", 8 }, { "START", 9 }, { "L3", 10 }, { "R3", 11 },
};
#define MAP_STEPS ((int)(sizeof(k_map_steps) / sizeof(k_map_steps[0])))

static lv_obj_t *s_map_btn, *s_map_scr, *s_map_prompt, *s_map_hint, *s_map_bar;
static int s_map_step = -1, s_map_player, s_map_done;
static uint8_t s_map[LX_INPUT_MAP_BITS];
static bool s_map_armed;                /* all released since the last step */
static int64_t s_map_since;

static void map_show_step(void)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "Press %s", k_map_steps[s_map_step].name);
    lv_label_set_text(s_map_prompt, buf);
    snprintf(buf, sizeof(buf), "Step %d of %d. No such button? Wait a moment.", s_map_step + 1, MAP_STEPS);
    lv_label_set_text(s_map_hint, buf);
    lv_bar_set_value(s_map_bar, s_map_step, LV_ANIM_OFF);
    s_map_since = esp_timer_get_time();
}

static void map_finish(void)
{
    s_map_step = -1;
    if (s_map_done >= 4) {
        /* Buttons not assigned keep their default, unless that function moved */
        for (int i = 0; i < LX_INPUT_MAP_BITS; i++) {
            if (s_map[i] != 0xFF) continue;
            bool taken = false;
            for (int j = 0; j < LX_INPUT_MAP_BITS; j++) taken |= s_map[j] == i;
            if (!taken) s_map[i] = (uint8_t)i;
        }
    }
    esp_err_t e = s_map_done >= 4 ? lx_input_pad_set_map(s_map_player, s_map) : ESP_OK;
    lx_input_flush();
    show_settings();
    if (s_map_done < 4) upd_message("Controller", "Fewer than 4 buttons pressed, nothing was changed.");
    else if (e != ESP_OK) upd_message("Controller", "The mapping could not be saved on the SD card.");
    else upd_message("Controller", "Mapping saved for this controller, in /sd/config/gamepads_mine.txt.");
}

static void map_tick(void)
{
    uint32_t raw = lx_input_pad_raw(s_map_player);
    lx_input_flush();
    if (!raw) s_map_armed = true;
    int64_t now = esp_timer_get_time();
    bool next = now - s_map_since > MAP_STEP_US;
    if (s_map_armed && raw) {
        int bit = __builtin_ctz(raw);
        if (bit < LX_INPUT_MAP_BITS && s_map[bit] == 0xFF) {
            s_map[bit] = k_map_steps[s_map_step].bit;
            s_map_done++;
            next = true;
        }
        s_map_armed = false;
    }
    if (!next) return;
    if (++s_map_step >= MAP_STEPS) map_finish();
    else map_show_step();
}

static void on_map_start(lv_event_t *ev)
{
    s_map_player = lx_input_pad_first();
    if (s_map_player < 0) {
        upd_message("Controller", "Connect a USB controller first.");
        return;
    }
    memset(s_map, 0xFF, sizeof(s_map));
    s_map_done = 0;
    s_map_armed = false;
    s_map_step = 0;
    focus_clear_list();
    lv_screen_load(s_map_scr);
    map_show_step();
}

static void on_map_cancel(lv_event_t *ev)
{
    s_map_step = -1;
    lx_input_flush();
    show_settings();
}

static void on_map_reset(lv_event_t *ev)
{
    s_map_step = -1;
    lx_input_pad_set_map(s_map_player, NULL);
    lx_input_flush();
    show_settings();
    upd_message("Controller", "Your mapping is removed. This controller uses the shared database's, "
                "or its default buttons.");
}

static void build_map_screen(void)
{
    s_map_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_map_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_map_scr, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_map_scr, C_BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(s_map_scr, LV_GRAD_DIR_VER, 0);
    lv_obj_t *t = label(s_map_scr, &lv_font_montserrat_28, C_TEXT, "Map controller");
    lv_obj_set_pos(t, 32, 26);
    s_map_prompt = label(s_map_scr, &lv_font_montserrat_48, C_ACCENT, "");
    lv_obj_align(s_map_prompt, LV_ALIGN_CENTER, 0, -40);
    s_map_hint = label(s_map_scr, &lv_font_montserrat_20, C_DIM, "");
    lv_obj_align(s_map_hint, LV_ALIGN_CENTER, 0, 20);
    s_map_bar = lv_bar_create(s_map_scr);
    lv_obj_set_size(s_map_bar, 600, 12);
    lv_obj_align(s_map_bar, LV_ALIGN_CENTER, 0, 70);
    lv_bar_set_range(s_map_bar, 0, MAP_STEPS);
    lv_obj_set_style_bg_color(s_map_bar, C_BLUE, LV_PART_INDICATOR);
    static const struct { const char *text; int x; lv_event_cb_t cb; } b[] = {
        { "Cancel", 200, on_map_cancel }, { "Reset to default", 420, on_map_reset },
    };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *btn = lv_button_create(s_map_scr);
        lv_obj_set_pos(btn, b[i].x + s_dx / 2, 390 + s_dy);
        lv_obj_set_style_bg_color(btn, C_BLUE, 0);
        lv_obj_center(label(btn, &lv_font_montserrat_20, C_TEXT, b[i].text));
        lv_obj_add_event_cb(btn, b[i].cb, LV_EVENT_CLICKED, NULL);
    }
}

static lv_obj_t *s_crash_box, *s_crash_ok;

static void on_crash_ok(lv_event_t *ev)
{
    lv_msgbox_close(s_crash_box);
    s_crash_box = s_crash_ok = NULL;
    s_focused = NULL;
    if (lv_screen_active() == s_settings) show_settings();
    else if (!store_focus()) home_focus_list();
}

static void on_dev_switch(lv_event_t *ev)
{
    s_show_dev = lv_obj_has_state(s_dev_switch, LV_STATE_CHECKED);
    nvs_handle_t h;
    if (nvs_open("launcher", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "devapps", s_show_dev);
        nvs_commit(h);
        nvs_close(h);
    }
    scan_apps();                /* the grid is rebuilt when Settings closes */
    refresh_info();
}

/* ── Wi-Fi screens ───────────────────────────────────────────── */

static lv_obj_t *s_wifi_btn, *s_wifi_scr, *s_wifi_status, *s_wifi_list, *s_wifi_tz, *s_wifi_chip;
static lv_obj_t *s_wifi_scan, *s_wifi_forget, *s_wifi_back, *s_pw_scr, *s_pw_title, *s_pw_text;
static lv_obj_t *s_web_btn, *s_web_info;
static bool s_wifi_scanning;
enum { PW_WIFI, PW_NEW_FOLDER, PW_RENAME };     /* s_pw_scr also asks for folder names */
static int s_pw_mode;
static void folder_name_done(const char *name);
static lx_net_ap_t s_wifi_aps[LX_NET_MAX_APS];
static int s_wifi_sel;

static void wifi_focus(void)
{
    /* Keep the focus on a button across a scan; list rows are rebuilt by it */
    lv_obj_t *keep = s_focused;
    bool fixed = keep == s_wifi_back || keep == s_wifi_scan || keep == s_wifi_forget || keep == s_web_btn ||
                 keep == s_wifi_tz;
    if (fixed) lv_obj_remove_state(keep, LV_STATE_FOCUSED);
    focus_clear_list();
    focus_add(s_wifi_back);
    focus_add(s_wifi_scan);
    focus_add(s_wifi_forget);
    focus_add(s_web_btn);
    focus_add(s_wifi_tz);
    for (uint32_t i = 0; i < lv_obj_get_child_count(s_wifi_list); i++)
        focus_add(lv_obj_get_child(s_wifi_list, i));
    s_focused = NULL;
    focus_set(fixed && lv_screen_active() == s_wifi_scr ? keep : s_wifi_scan);
}

static void on_wifi_scan(lv_event_t *ev)
{
    s_wifi_scanning = true;
    lv_obj_clean(s_wifi_list);
    lx_net_scan();
}

static void web_refresh(void);

static void on_wifi_open(lv_event_t *ev)
{
    lv_screen_load(s_wifi_scr);
    wifi_focus();
    web_refresh();
    on_wifi_scan(NULL);
}

static void on_wifi_back(lv_event_t *ev) { show_settings(); }
static void on_wifi_forget(lv_event_t *ev);

/* The file manager runs on the launcher's network connection, on request */

static void web_refresh(void)
{
    char buf[80] = "";
    if (lx_web_running()) snprintf(buf, sizeof(buf), "http://%s   PIN %s", lx_net_ip(), lx_web_pin());
    lv_label_set_text(s_web_info, buf);
    lv_label_set_text(lv_obj_get_child(s_web_btn, 0), lx_web_running() ? "Stop file manager" : "File manager");
}

static void on_web_toggle(lv_event_t *ev)
{
    bool start = !lx_web_running();
    if (!start) lx_web_stop();
    else if (lx_net_ip()[0]) lx_web_start();        /* connected; the state may say scanning */
    web_refresh();
    if (start && !lx_web_running()) lv_label_set_text(s_web_info, "Connect to Wi-Fi first");
}

static void on_wifi_tz(lv_event_t *ev)
{
    lx_net_tz_set(lv_dropdown_get_selected(s_wifi_tz));
}

static void on_wifi_pick(lv_event_t *ev)
{
    s_wifi_sel = (int)(intptr_t)lv_event_get_user_data(ev);
    if (!s_wifi_aps[s_wifi_sel].secure) {
        lx_net_connect(s_wifi_aps[s_wifi_sel].ssid, "");
        return;
    }
    char buf[64];
    snprintf(buf, sizeof(buf), "Password for %s", s_wifi_aps[s_wifi_sel].ssid);
    s_pw_mode = PW_WIFI;
    lv_label_set_text(s_pw_title, buf);
    lv_textarea_set_password_mode(s_pw_text, true);
    lv_textarea_set_max_length(s_pw_text, 63);
    lv_textarea_set_text(s_pw_text, "");
    lv_screen_load(s_pw_scr);
    focus_clear_list();
}

static void pw_close(void)
{
    lv_textarea_set_text(s_pw_text, "");
    if (s_pw_mode == PW_WIFI) {
        lv_screen_load(s_wifi_scr);
        wifi_focus();
    } else {
        lv_screen_load(s_home);
        s_focused = NULL;
        home_focus_list();
    }
}

static void pw_accept(void)
{
    char text[64];
    snprintf(text, sizeof(text), "%s", lv_textarea_get_text(s_pw_text));
    if (s_pw_mode == PW_WIFI) lx_net_connect(s_wifi_aps[s_wifi_sel].ssid, text);
    pw_close();
    if (s_pw_mode != PW_WIFI) folder_name_done(text);
}

static void on_pw_key(lv_event_t *ev)
{
    if (lv_event_get_code(ev) == LV_EVENT_READY) pw_accept();
    else pw_close();
}

static lv_obj_t *wifi_button(lv_obj_t *parent, int x, int y, int w, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, 44);
    lv_obj_set_style_bg_color(b, C_BLUE, 0);
    lv_obj_set_style_outline_width(b, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(b, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(b, 3, LV_STATE_FOCUSED);
    lv_obj_center(label(b, &lv_font_montserrat_20, C_TEXT, text));
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void build_wifi_screens(void)
{
    s_wifi_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_wifi_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_wifi_scr, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_wifi_scr, C_BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(s_wifi_scr, LV_GRAD_DIR_VER, 0);
    s_wifi_back = wifi_button(s_wifi_scr, 16, 14, 56, LV_SYMBOL_LEFT, on_wifi_back);
    lv_obj_t *t = label(s_wifi_scr, &lv_font_montserrat_28, C_TEXT, "Wi-Fi and time");
    lv_obj_set_pos(t, 92, 20);
    s_wifi_status = label(s_wifi_scr, &lv_font_montserrat_16, C_DIM, "");
    lv_obj_set_pos(s_wifi_status, 24, 72);
    s_wifi_scan = wifi_button(s_wifi_scr, 24, 104, 120, "Scan", on_wifi_scan);
    s_wifi_forget = wifi_button(s_wifi_scr, 160, 104, 200, "Forget network", on_wifi_forget);
    s_web_btn = wifi_button(s_wifi_scr, 500 + s_dx, 14, 276, "File manager", on_web_toggle);
    s_web_info = label(s_wifi_scr, &lv_font_montserrat_16, C_ACCENT, "");
    lv_obj_set_pos(s_web_info, 500 + s_dx, 72);
    lv_obj_t *z = label(s_wifi_scr, &lv_font_montserrat_16, C_TEXT, "Time zone");
    lv_obj_set_pos(z, 390 + s_dx, 116);
    s_wifi_tz = lv_dropdown_create(s_wifi_scr);
    lv_obj_set_pos(s_wifi_tz, 484 + s_dx, 104);
    lv_obj_set_width(s_wifi_tz, 292);
    lv_dropdown_clear_options(s_wifi_tz);
    for (int i = 0; i < lx_net_tz_count(); i++)
        lv_dropdown_add_option(s_wifi_tz, lx_net_tz_name(i), LV_DROPDOWN_POS_LAST);
    lv_dropdown_set_selected(s_wifi_tz, lx_net_tz_get());
    lv_obj_set_style_outline_width(s_wifi_tz, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s_wifi_tz, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(s_wifi_tz, on_wifi_tz, LV_EVENT_VALUE_CHANGED, NULL);
    s_wifi_list = lv_list_create(s_wifi_scr);
    lv_obj_set_pos(s_wifi_list, 24, 160);
    lv_obj_set_size(s_wifi_list, 752 + s_dx, 282 + s_dy);
    s_wifi_chip = label(s_wifi_scr, &lv_font_montserrat_16, C_DIM, "");
    lv_obj_set_pos(s_wifi_chip, 24, 450 + s_dy);
    lv_obj_set_style_bg_color(s_wifi_list, C_PANEL, 0);
    lv_obj_set_style_border_color(s_wifi_list, C_BORDER, 0);

    s_pw_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_pw_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_pw_scr, C_BG_TOP, 0);
    s_pw_title = label(s_pw_scr, &lv_font_montserrat_20, C_TEXT, "");
    lv_obj_set_pos(s_pw_title, 24, 16);
    s_pw_text = lv_textarea_create(s_pw_scr);
    lv_obj_set_pos(s_pw_text, 24, 52);
    lv_obj_set_size(s_pw_text, 752 + s_dx, 52);
    lv_textarea_set_one_line(s_pw_text, true);
    lv_textarea_set_password_mode(s_pw_text, true);
    lv_textarea_set_max_length(s_pw_text, 63);
    lv_obj_t *kb = lv_keyboard_create(s_pw_scr);
    lv_obj_set_size(kb, 800 + s_dx, 360);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_28, LV_PART_ITEMS);    /* the default is tiny on keys this big */
    lv_obj_set_style_text_font(s_pw_text, &lv_font_montserrat_24, 0);
    lv_keyboard_set_textarea(kb, s_pw_text);
    lv_obj_add_event_cb(kb, on_pw_key, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, on_pw_key, LV_EVENT_CANCEL, NULL);
}

/* ── Clock setup (boards with an RTC) ───────────────────────────────────
 * Opened from the clock on the home screen. The time is entered as local time
 * in the chosen zone; the RTC keeps the matching UTC. */

enum { CF_DAY, CF_MONTH, CF_YEAR, CF_HOUR, CF_MIN, CF_N };
static const struct { const char *name; int w; } k_clk_fields[CF_N] = {
    { "Day", 110 }, { "Month", 110 }, { "Year", 150 }, { "Hour", 110 }, { "Minute", 110 },
};
static lv_obj_t *s_clk_scr, *s_clk_back, *s_clk_tz, *s_clk_sync, *s_clk_cancel, *s_clk_save;
static lv_obj_t *s_clk_box[CF_N], *s_clk_val[CF_N], *s_clk_note;
static int s_clk[CF_N];

static int clk_days(int month, int year)
{
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return d[month - 1] + (month == 2 && leap);
}

static void clk_show(void)
{
    int dmax = clk_days(s_clk[CF_MONTH], s_clk[CF_YEAR]);
    if (s_clk[CF_DAY] > dmax) s_clk[CF_DAY] = dmax;
    for (int i = 0; i < CF_N; i++)
        lv_label_set_text_fmt(s_clk_val[i], i == CF_YEAR ? "%d" : "%02d", s_clk[i]);
}

static void clk_load(void)
{
    time_t now = time(NULL);
    struct tm tm;
    if (now < 1704067200) now = 1767268800;         /* not set: 2026-01-01 12:00 */
    localtime_r(&now, &tm);
    s_clk[CF_DAY] = tm.tm_mday;
    s_clk[CF_MONTH] = tm.tm_mon + 1;
    s_clk[CF_YEAR] = tm.tm_year + 1900;
    s_clk[CF_HOUR] = tm.tm_hour;
    s_clk[CF_MIN] = tm.tm_min;
    clk_show();
}

/* +1 / -1 on a field, wrapping around its range */
static void clk_step(int f, int d)
{
    static const int lo[CF_N] = { 1, 1, 2024, 0, 0 }, hi_fixed[CF_N] = { 0, 12, 2099, 23, 59 };
    int hi = f == CF_DAY ? clk_days(s_clk[CF_MONTH], s_clk[CF_YEAR]) : hi_fixed[f];
    int v = s_clk[f] + d;
    s_clk[f] = v > hi ? lo[f] : v < lo[f] ? hi : v;
    clk_show();
    lv_label_set_text(s_clk_note, "");
}

static int clk_field_of(lv_obj_t *o)
{
    for (int i = 0; i < CF_N; i++) if (o && s_clk_box[i] == o) return i;
    return -1;
}

/* Touch: the upper half of a field counts up, the lower half down */
static void on_clk_box(lv_event_t *ev)
{
    lv_obj_t *box = lv_event_get_target(ev);
    lv_indev_t *in = lv_indev_active();
    if (!in || lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    lv_area_t a;
    lv_obj_get_coords(box, &a);
    clk_step(clk_field_of(box), p.y < (a.y1 + a.y2) / 2 ? 1 : -1);
    focus_set(box);
}

static void clk_focus(void)
{
    focus_clear_list();
    focus_add(s_clk_back);
    focus_add(s_clk_tz);
    for (int i = 0; i < CF_N; i++) focus_add(s_clk_box[i]);
    focus_add(s_clk_sync);
    focus_add(s_clk_cancel);
    focus_add(s_clk_save);
    s_focused = NULL;
    focus_set(s_clk_box[CF_HOUR]);
}

static void on_clk_open(lv_event_t *ev)
{
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);
    lv_dropdown_set_selected(s_clk_tz, lx_net_tz_get());
    clk_load();
    lv_label_set_text(s_clk_note, lx_net_tz_get() ? "" : "Choose your time zone first, then set the local time.");
    lv_screen_load(s_clk_scr);
    clk_focus();
}

static void on_clk_close(lv_event_t *ev)
{
    show_home();
}

static void on_clk_tz(lv_event_t *ev)
{
    lx_net_tz_set(lv_dropdown_get_selected(s_clk_tz));
    lv_dropdown_set_selected(s_wifi_tz, lv_dropdown_get_selected(s_clk_tz));
    clk_load();                                     /* the same moment in the new zone */
    lv_label_set_text(s_clk_note, "");
}

static void clk_after_sync(lv_timer_t *t)
{
    clk_load();
    lv_label_set_text(s_clk_note, "Time taken from the internet and saved.");
}

static void on_clk_sync(lv_event_t *ev)
{
    if (lx_net_state() != LX_NET_CONNECTED) {
        lv_label_set_text(s_clk_note, "Connect to Wi-Fi first (Settings, Wi-Fi).");
        return;
    }
    lx_net_time_sync();
    lv_label_set_text(s_clk_note, "Asking the internet for the time...");
    lv_timer_t *t = lv_timer_create(clk_after_sync, 3000, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void update_status(lv_timer_t *t);

static void on_clk_save(lv_event_t *ev)
{
    struct tm tm = {
        .tm_year = s_clk[CF_YEAR] - 1900, .tm_mon = s_clk[CF_MONTH] - 1, .tm_mday = s_clk[CF_DAY],
        .tm_hour = s_clk[CF_HOUR], .tm_min = s_clk[CF_MIN], .tm_isdst = -1,    /* the zone decides summer time */
    };
    time_t t = mktime(&tm);
    struct timeval tv = { .tv_sec = t };
    settimeofday(&tv, NULL);
    esp_err_t err = bsp_rtc_set(t);
    ESP_LOGI(TAG, "clock set by hand, RTC %s", esp_err_to_name(err));
    update_status(NULL);
    if (err == ESP_OK) show_home();
    else lv_label_set_text(s_clk_note, "The clock is set, but the RTC did not take it.");
}

static void build_clock_screen(void)
{
    s_clk_scr = lv_obj_create(NULL);
    lv_obj_remove_flag(s_clk_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_clk_scr, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_clk_scr, C_BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(s_clk_scr, LV_GRAD_DIR_VER, 0);
    s_clk_back = wifi_button(s_clk_scr, 16, 14, 56, LV_SYMBOL_LEFT, on_clk_close);
    lv_obj_set_pos(label(s_clk_scr, &lv_font_montserrat_28, C_TEXT, "Date and time"), 92, 20);

    lv_obj_set_pos(label(s_clk_scr, &lv_font_montserrat_20, C_TEXT, "Time zone"), 24, 96);
    s_clk_tz = lv_dropdown_create(s_clk_scr);
    lv_obj_set_pos(s_clk_tz, 150, 84);
    lv_obj_set_width(s_clk_tz, 292);
    lv_dropdown_clear_options(s_clk_tz);
    for (int i = 0; i < lx_net_tz_count(); i++)
        lv_dropdown_add_option(s_clk_tz, lx_net_tz_name(i), LV_DROPDOWN_POS_LAST);
    lv_obj_set_style_outline_width(s_clk_tz, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s_clk_tz, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_add_event_cb(s_clk_tz, on_clk_tz, LV_EVENT_VALUE_CHANGED, NULL);

    int total = -20, gap = 20;
    for (int i = 0; i < CF_N; i++) total += k_clk_fields[i].w + gap;
    int x = (800 + s_dx - total) / 2, y = 180 + s_dy / 3;
    for (int i = 0; i < CF_N; i++) {
        int w = k_clk_fields[i].w;
        lv_obj_t *n = label(s_clk_scr, &lv_font_montserrat_16, C_DIM, k_clk_fields[i].name);
        lv_obj_set_width(n, w);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(n, x, y - 26);
        lv_obj_t *b = s_clk_box[i] = panel(s_clk_scr, x, y, w, 120, 14);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        focus_style(b, 14);
        lv_obj_add_event_cb(b, on_clk_box, LV_EVENT_CLICKED, NULL);
        lv_obj_align(label(b, &lv_font_montserrat_16, C_DIM, LV_SYMBOL_UP), LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_align(label(b, &lv_font_montserrat_16, C_DIM, LV_SYMBOL_DOWN), LV_ALIGN_BOTTOM_MID, 0, -6);
        s_clk_val[i] = label(b, &lv_font_montserrat_48, C_TEXT, "");
        lv_obj_center(s_clk_val[i]);
        x += w + gap;
    }
    lv_obj_t *hint = label(s_clk_scr, &lv_font_montserrat_16, C_DIM,
                           "Left / right picks a field, up / down changes it, A goes to Save. Touch: tap the top or bottom of a field.");
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, y + 136);
    s_clk_note = label(s_clk_scr, &lv_font_montserrat_16, C_ACCENT, "");
    lv_obj_align(s_clk_note, LV_ALIGN_TOP_MID, 0, y + 166);

    int by = 400 + s_dy;
    s_clk_sync = wifi_button(s_clk_scr, 24, by, 260, LV_SYMBOL_REFRESH " Sync from internet", on_clk_sync);
    s_clk_cancel = wifi_button(s_clk_scr, 504 + s_dx, by, 128, "Cancel", on_clk_close);
    s_clk_save = wifi_button(s_clk_scr, 648 + s_dx, by, 128, "Save", on_clk_save);
}

static void wifi_tick(void)
{
    static int64_t last;
    int64_t now = esp_timer_get_time();
    if (lv_screen_active() != s_wifi_scr || now - last < 300000) return;
    last = now;
    char buf[96];
    switch (lx_net_state()) {
    case LX_NET_CONNECTED:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI " Connected to %s, %s", lx_net_ssid(), lx_net_ip());
        break;
    case LX_NET_CONNECTING:
        snprintf(buf, sizeof(buf), "Connecting to %s...", lx_net_ssid());
        break;
    case LX_NET_SCANNING: snprintf(buf, sizeof(buf), "Scanning..."); break;
    case LX_NET_FAILED:
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " Could not join %s. Check the password.", lx_net_ssid());
        break;
    case LX_NET_NO_RADIO: snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " The Wi-Fi chip did not answer."); break;
    case LX_NET_IDLE: snprintf(buf, sizeof(buf), "Not connected"); break;
    default: snprintf(buf, sizeof(buf), s_wifi_scanning ? "Starting the radio..." : "Radio off"); break;
    }
    lv_label_set_text(s_wifi_status, buf);
    const char *c6 = lx_c6_version();
    snprintf(buf, sizeof(buf), "Wi-Fi chip firmware %s (launcher: esp_hosted %s)",
             !c6[0] ? "-" : c6[0] == '?' ? "unknown" : c6, lx_c6_host_version());
    lv_label_set_text(s_wifi_chip, buf);
    if (!s_wifi_scanning) return;
    int n = lx_net_scan_results(s_wifi_aps, LX_NET_MAX_APS);
    if (n < 0) return;
    s_wifi_scanning = false;
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), "%s  (%d dBm%s)", s_wifi_aps[i].ssid, s_wifi_aps[i].rssi,
                 s_wifi_aps[i].secure ? ", password" : ", open");
        lv_obj_t *b = lv_list_add_button(s_wifi_list, LV_SYMBOL_WIFI, buf);
        lv_obj_set_style_outline_width(b, 2, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_color(b, C_ACCENT, LV_STATE_FOCUSED);
        lv_obj_add_event_cb(b, on_wifi_pick, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (!n) lv_list_add_text(s_wifi_list, "No networks found");
    wifi_focus();
}

static lv_obj_t *s_update_btn;
static void on_upd_check(lv_event_t *ev);
enum { ACT_CLOSE, ACT_OPEN, ACT_RENAME, ACT_DELETE, ACT_DELETE_YES, ACT_PICK, ACT_OUT, ACT_NEW_MOVE, ACT_FORGET,
       ACT_STORE_OPEN, ACT_TO_FOLDER };
static lv_obj_t *s_menu;
static void menu_open(const char *title, const char *text, int items);
static void menu_add(const char *text, int act);

static void build_settings(void)
{
    s_settings = lv_obj_create(NULL);
    lv_obj_remove_flag(s_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_settings, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(s_settings, C_BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(s_settings, LV_GRAD_DIR_VER, 0);

    s_back = lv_obj_create(s_settings);
    lv_obj_remove_style_all(s_back);
    lv_obj_set_pos(s_back, 16, 14);
    lv_obj_set_size(s_back, 56, 56);
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s_back, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_back, lv_color_hex(0x16243D), 0);
    focus_style(s_back, 14);
    lv_obj_t *bi = label(s_back, &lv_font_montserrat_28, C_TEXT, LV_SYMBOL_LEFT);
    lv_obj_center(bi);
    lv_obj_add_event_cb(s_back, on_back_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = label(s_settings, &lv_font_montserrat_28, C_TEXT, "Settings");
    lv_obj_set_pos(t, 92, 26);

    s_vol_slider = settings_row(s_settings, 80, "Volume", LV_SYMBOL_VOLUME_MAX, 0, 100, s_volume, &s_vol_val);
    s_bri_slider = settings_row(s_settings, 150, "Brightness", LV_SYMBOL_IMAGE, 10, 100, s_brightness, &s_bri_val);

    lv_obj_t *dev = panel(s_settings, 24, 220, 752 + s_dx, 52, 14);
    lv_obj_t *di = label(dev, &lv_font_montserrat_24, C_ACCENT, LV_SYMBOL_LIST);
    lv_obj_set_pos(di, 18, 13);
    lv_obj_t *dt = label(dev, &lv_font_montserrat_20, C_TEXT, "Show developer apps");
    lv_obj_set_pos(dt, 60, 14);
    s_dev_switch = lv_switch_create(dev);
    lv_obj_set_pos(s_dev_switch, 664 + s_dx, 10);
    lv_obj_set_style_bg_color(s_dev_switch, C_BLUE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_outline_width(s_dev_switch, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s_dev_switch, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(s_dev_switch, 6, LV_STATE_FOCUSED);
    if (s_show_dev) lv_obj_add_state(s_dev_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(s_dev_switch, on_dev_switch, LV_EVENT_VALUE_CHANGED, NULL);
    int bw = (752 + s_dx - 2 * 10) / 3;        /* three buttons share the row */
    s_map_btn = lv_button_create(s_settings);
    lv_obj_set_pos(s_map_btn, 24, 278);
    lv_obj_set_size(s_map_btn, bw, 52);
    lv_obj_set_style_bg_color(s_map_btn, C_BLUE, 0);
    lv_obj_set_style_outline_width(s_map_btn, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s_map_btn, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(s_map_btn, 3, LV_STATE_FOCUSED);
    lv_obj_center(label(s_map_btn, &lv_font_montserrat_20, C_TEXT, "Map controller"));
    lv_obj_add_event_cb(s_map_btn, on_map_start, LV_EVENT_CLICKED, NULL);
    build_map_screen();
    s_wifi_btn = lv_button_create(s_settings);
    lv_obj_set_pos(s_wifi_btn, 24 + bw + 10, 278);
    lv_obj_set_size(s_wifi_btn, bw, 52);
    lv_obj_set_style_bg_color(s_wifi_btn, C_BLUE, 0);
    lv_obj_set_style_outline_width(s_wifi_btn, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s_wifi_btn, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(s_wifi_btn, 3, LV_STATE_FOCUSED);
    lv_obj_center(label(s_wifi_btn, &lv_font_montserrat_20, C_TEXT, LV_SYMBOL_WIFI " Wi-Fi"));
    lv_obj_add_event_cb(s_wifi_btn, on_wifi_open, LV_EVENT_CLICKED, NULL);
    build_wifi_screens();
    s_update_btn = lv_button_create(s_settings);
    lv_obj_set_pos(s_update_btn, 24 + 2 * (bw + 10), 278);
    lv_obj_set_size(s_update_btn, bw, 52);
    lv_obj_set_style_bg_color(s_update_btn, C_BLUE, 0);
    lv_obj_set_style_outline_width(s_update_btn, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(s_update_btn, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_pad(s_update_btn, 3, LV_STATE_FOCUSED);
    lv_obj_center(label(s_update_btn, &lv_font_montserrat_20, C_TEXT, LV_SYMBOL_DOWNLOAD " Update"));
    lv_obj_add_event_cb(s_update_btn, on_upd_check, LV_EVENT_CLICKED, NULL);

    lv_obj_t *info = panel(s_settings, 24, 336, 752 + s_dx, 136 + s_dy, 14);
    s_info = label(info, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_set_pos(s_info, 18, 10);
    lv_obj_set_style_text_line_space(s_info, 6, 0);
    apply_settings();
}

static void refresh_info(void)
{
    const bsp_info_t *b = bsp_info();
    uint64_t total = 0, free_b = 0;
    esp_vfs_fat_info("/sd", &total, &free_b);
    char buf[512], batt[32] = "";
    int mv = bsp_battery_mv();
    if (mv >= 0) snprintf(batt, sizeof(batt), "   Battery pin %.2f V", mv / 1000.0);   /* -1: none */
    snprintf(buf, sizeof(buf),
             "Board         %s\n"
             "Launcher      v%s   (app ABI %s, ESP-IDF %s)\n"
             "Memory        %u KB internal free,  %u KB PSRAM free\n"
             "SD card       %.1f GB free of %.1f GB,  %d apps%s\n"
             "Input         %s",
             b->name, LX_LAUNCHER_VERSION, LX_ABI_STRING, esp_get_idf_version(),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
             free_b / 1e9, total / 1e9, s_ninstalled, batt, "");
    size_t n = strlen(buf);
    lx_input_describe(buf + n, sizeof(buf) - n);
    if (!buf[n]) snprintf(buf + n, sizeof(buf) - n, "no gamepad or keyboard");
    lv_label_set_text(s_info, buf);
}

static void show_settings(void)
{
    refresh_info();
    lv_screen_load(s_settings);
    focus_clear_list();
    focus_add(s_back);
    focus_add(s_vol_slider);
    focus_add(s_bri_slider);
    focus_add(s_dev_switch);
    focus_add(s_map_btn);
    focus_add(s_wifi_btn);
    focus_add(s_update_btn);
    for (int i = 0; i < s_nfocus; i++)      /* back from Wi-Fi or a dialog: no ring left on the old one */
        lv_obj_remove_state(s_focusable[i], LV_STATE_FOCUSED);
    focus_set(s_vol_slider);
}

static uint32_t s_web_seen;     /* file manager changes already on the grid */

void show_home(void)
{
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);    /* else it stays lit on Settings */
    lv_screen_load(s_home);
    if (store_take_changed() | (lx_web_changes() != s_web_seen)) {
        s_web_seen = lx_web_changes();
        scan_apps();
    }
    build_grid();
    s_focused = NULL;
    home_focus_list();
}

/* ── Folders ───────────────────────────────────────────────────────────── */

static lv_obj_t *s_menu_return;
static char s_target[160], s_target_name[32], s_target_id[32];   /* what the menu is about */
static bool s_name_move;                        /* move s_target into the folder being named */

static void menu_close(void)
{
    lv_msgbox_close(s_menu);
    s_menu = NULL;
    s_long_press = false;
    s_focused = s_menu_return;
    if (lv_screen_active() == s_wifi_scr) wifi_focus();
    else home_focus_list();
}

static void menu_open(const char *title, const char *text, int items)
{
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);
    s_menu_return = s_focused;
    focus_clear_list();
    s_menu = lv_msgbox_create(NULL);
    lv_msgbox_add_title(s_menu, title);
    lv_obj_set_width(s_menu, 460);
    if (text) lv_msgbox_add_text(s_menu, text);
    if (items > 5) lv_obj_set_height(lv_msgbox_get_content(s_menu), 5 * 54);
}

static void on_menu_item(lv_event_t *ev);

static void menu_add(const char *text, int act)
{
    lv_obj_t *b = lv_button_create(lv_msgbox_get_content(s_menu));
    lv_obj_set_size(b, lv_pct(100), 44);
    lv_obj_set_style_bg_color(b, C_BLUE, 0);
    lv_obj_set_style_outline_width(b, 3, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_color(b, C_ACCENT, LV_STATE_FOCUSED);
    lv_obj_center(label(b, &lv_font_montserrat_20, C_TEXT, text));
    lv_obj_add_event_cb(b, on_menu_item, LV_EVENT_CLICKED, (void *)(intptr_t)act);
    focus_add(b);
    if (s_nfocus == 1) focus_set(b);
}

/* The password can't be read back, so forgetting a network has to be asked */
static void on_wifi_forget(lv_event_t *ev)
{
    char title[64];
    snprintf(title, sizeof(title), "Forget %s?", lx_net_ssid()[0] ? lx_net_ssid() : "this network");
    menu_open(title, "You will need its password to join again.", 2);
    menu_add("Cancel", ACT_CLOSE);
    menu_add("Forget", ACT_FORGET);
}

static void folder_message(const char *title, const char *text)
{
    menu_open(title, text, 1);
    menu_add("OK", ACT_CLOSE);
}

static void folder_enter(const char *name)
{
    snprintf(s_folder, sizeof(s_folder), "%s", name);
    home_show(NULL);
}

static void folder_leave(void)
{
    char path[64];
    snprintf(path, sizeof(path), APPS_DIR "/%s", s_folder);
    s_folder[0] = 0;
    home_show(path);
}

static void name_open(const char *title, const char *text, int mode)
{
    s_pw_mode = mode;
    lv_label_set_text(s_pw_title, title);
    lv_textarea_set_password_mode(s_pw_text, false);
    lv_textarea_set_max_length(s_pw_text, MAX_NAME);
    lv_textarea_set_text(s_pw_text, text);
    lv_screen_load(s_pw_scr);
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);
    focus_clear_list();
}

static void on_sd_click(lv_event_t *ev)
{
    if (s_files_app[0]) snprintf(s_launch, sizeof(s_launch), "%s", s_files_app);
    else folder_message("Files is not installed", "Copy the Files app to /sd/apps/files/files.papp.");
}

static void on_count_click(lv_event_t *ev)
{
    if (s_folder[0]) {
        folder_leave();
        return;
    }
    s_name_move = false;
    name_open("Name of the new folder", "New folder", PW_NEW_FOLDER);
}

/* Moves the app's directory into a folder ("" = the main page) */
static void move_app(const char *path, const char *folder)
{
    char dir[160], dst[160];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    *slash = 0;
    const char *base = strrchr(dir, '/') + 1;
    if (folder[0]) snprintf(dst, sizeof(dst), APPS_DIR "/%s/%s", folder, base);
    else snprintf(dst, sizeof(dst), APPS_DIR "/%s", base);
    struct stat st;
    bool ok = stat(dst, &st) != 0 && rename(dir, dst) == 0;
    home_view(NULL);
    if (!ok) folder_message("Cannot move the app", "An app with the same directory name is already there.");
}

/* Only the empty folder goes: its apps return to the main page, and anything
 * else in it is kept (the folder stays) */
static void folder_delete(const char *name)
{
    char dir[64], src[128], dst[128];
    snprintf(dir, sizeof(dir), APPS_DIR "/%s", name);
    for (;;) {
        DIR *d = opendir(dir);
        if (!d) break;
        struct dirent *de;
        src[0] = 0;
        while ((de = readdir(d))) {
            if (de->d_name[0] == '.' || de->d_type != DT_DIR) continue;
            snprintf(src, sizeof(src), "%s/%s", dir, de->d_name);
            snprintf(dst, sizeof(dst), APPS_DIR "/%s", de->d_name);
            break;
        }
        closedir(d);                            /* never rename during readdir */
        if (!src[0]) break;
        struct stat st;
        if (stat(dst, &st) == 0 || rename(src, dst) != 0) {
            home_view(dir);
            folder_message("Cannot delete the folder", "An app inside has the same directory name as one on the main page.");
            return;
        }
    }
    snprintf(src, sizeof(src), "%s/icon.png", dir);   /* the folder's own icon goes with it */
    remove(src);
    bool ok = rmdir(dir) == 0;
    home_view(NULL);
    if (!ok) folder_message("Folder kept", "Its apps are back on the main page, but it still holds other files. Use the Files app to see them.");
}

static void folder_name_done(const char *text)
{
    char name[MAX_NAME + 1], path[64];
    while (*text == ' ') text++;
    snprintf(name, sizeof(name), "%s", text);
    for (size_t n = strlen(name); n && name[n - 1] == ' '; n--) name[n - 1] = 0;
    size_t n = strlen(name);
    if (!n || name[0] == '.' || name[n - 1] == '.' || strpbrk(name, "/\\:*?\"<>|")) {
        folder_message("Cannot use this name", "A name can't be empty, start or end with a dot, or use / \\ : * ? \" < > |");
        return;
    }
    snprintf(path, sizeof(path), APPS_DIR "/%s", name);
    bool same = s_pw_mode == PW_RENAME && strcasecmp(name, s_target_name) == 0;
    struct stat st;
    if (!same && stat(path, &st) == 0) {
        folder_message("Cannot use this name", "A folder or app directory already has this name.");
        return;
    }
    if (s_pw_mode == PW_RENAME) {
        if (rename(s_target, path) != 0) folder_message("Cannot rename", "The SD card refused the new name.");
        else home_view(path);
    } else if (mkdir(path, 0775) != 0) {
        folder_message("Cannot create the folder", "The SD card refused it.");
    } else if (s_name_move) {
        move_app(s_target, name);
    } else {
        home_view(path);
    }
}

static void tile_menu(const app_entry_t *e)
{
    if (!e || e == &s_store_tile) return;
    snprintf(s_target, sizeof(s_target), "%s", e->path);
    snprintf(s_target_name, sizeof(s_target_name), "%s", e->name);
    snprintf(s_target_id, sizeof(s_target_id), "%s", e->id);
    if (e->is_folder) {
        menu_open(e->name, NULL, 4);
        menu_add("Open", ACT_OPEN);
        menu_add("Rename", ACT_RENAME);
        menu_add("Delete", ACT_DELETE);
    } else {
        char ver[32];
        snprintf(ver, sizeof(ver), "Version %s", e->version[0] ? e->version : "unknown");
        menu_open(e->name, ver, 4);
        menu_add(LV_SYMBOL_DOWNLOAD " Open in Store", ACT_STORE_OPEN);
        menu_add("Move to folder", ACT_PICK);
        if (s_folder[0]) menu_add("Move to main page", ACT_OUT);
    }
    menu_add("Cancel", ACT_CLOSE);
}

static void on_menu_item(lv_event_t *ev)
{
    int act = (int)(intptr_t)lv_event_get_user_data(ev);
    menu_close();
    switch (act) {
    case ACT_CLOSE: break;
    case ACT_OPEN: folder_enter(s_target_name); break;
    case ACT_RENAME: name_open("New name of the folder", s_target_name, PW_RENAME); break;
    case ACT_DELETE:
        menu_open("Delete this folder?", "Its apps go back to the main page. No app is deleted.", 2);
        menu_add("Cancel", ACT_CLOSE);
        menu_add("Delete", ACT_DELETE_YES);
        break;
    case ACT_DELETE_YES: folder_delete(s_target_name); break;
    case ACT_PICK: {
        int n = 2;
        for (int i = 0; i < s_napps; i++) n += s_apps[i].is_folder;
        menu_open("Move to folder", NULL, n);
        for (int i = 0; i < s_napps; i++)
            if (s_apps[i].is_folder && strcmp(s_apps[i].name, s_folder) != 0)
                menu_add(s_apps[i].name, ACT_TO_FOLDER + i);
        menu_add(LV_SYMBOL_PLUS " New folder", ACT_NEW_MOVE);
        menu_add("Cancel", ACT_CLOSE);
        break;
    }
    case ACT_OUT: move_app(s_target, ""); break;
    case ACT_FORGET: lx_net_forget(); break;
    case ACT_STORE_OPEN: store_open(s_target_id[0] ? s_target_id : s_target_name); break;
    case ACT_NEW_MOVE:
        s_name_move = true;
        name_open("Name of the new folder", "New folder", PW_NEW_FOLDER);
        break;
    default: move_app(s_target, s_apps[act - ACT_TO_FOLDER].name); break;
    }
}

/* ── Periodic updates: clock, SD, system bar ───────────────────────────── */

#define BAT_MV_EMPTY 3300               /* the reference's curve */
#define BAT_MV_FULL  3900
#define BAT_MV_MIN   2800               /* outside this: no cell fitted */
#define BAT_MV_MAX   4500

static void update_status(lv_timer_t *t)
{
    static int tick;
    char buf[64];
    time_t now = time(NULL);
    if (now > 1700000000) {                     /* set (by DevLink, later RTC / NTP) */
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(buf, sizeof(buf), "%H:%M", &tm);
        lv_label_set_text(s_clock, buf);
        strftime(buf, sizeof(buf), "%a, %d %b %Y", &tm);
        lv_label_set_text(s_date, buf);
    } else {
        lv_label_set_text(s_clock, "--:--");
        lv_label_set_text(s_date, "Clock not set");
    }

    if (tick++ % 10 == 0) {
        uint64_t total = 0, free_b = 0;
        static bool had_card = true;
        bool card = bsp_sdcard_mounted();
        if (card && !had_card && lv_screen_active() == s_home) refresh_home();    /* inserted after boot */
        had_card = card;
        if (esp_vfs_fat_info("/sd", &total, &free_b) == ESP_OK) {
            snprintf(buf, sizeof(buf), "%.1f GB free", free_b / 1e9);
            lv_label_set_text(s_sd_free, buf);
        } else {
            lv_label_set_text(s_sd_free, "not mounted");
        }
        snprintf(buf, sizeof(buf), "%d app%s installed", s_ninstalled, s_ninstalled == 1 ? "" : "s");
        lv_label_set_text(s_sd_apps, buf);
    }

    snprintf(buf, sizeof(buf), "%d MHz x2", CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ);
    lv_label_set_text(s_sys_cpu, buf);
    snprintf(buf, sizeof(buf), "SRAM %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
    lv_label_set_text(s_sys_sram, buf);
    snprintf(buf, sizeof(buf), "PSRAM %.1f MB", heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1048576.0);
    lv_label_set_text(s_sys_psram, buf);
    float c;
    if (s_tsens && temperature_sensor_get_celsius(s_tsens, &c) == ESP_OK) {
        snprintf(buf, sizeof(buf), "Chip %.0f C", c);
        lv_label_set_text(s_sys_temp, buf);
    }
    int pads = 0;
    for (int i = 0; i < PAPP_MAX_PLAYERS; i++) {
        papp_gamepad_t g;
        pads += lx_input_gamepad(i, &g);
    }
    int bat = (bsp_info()->caps & BSP_CAP_BATTERY) ? bsp_battery_mv() : -1;
    if (bat >= BAT_MV_MIN && bat <= BAT_MV_MAX) {           /* a cell is fitted */
        int pct = (bat - BAT_MV_EMPTY) * 100 / (BAT_MV_FULL - BAT_MV_EMPTY);
        pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
        const char *sym = bat > BAT_MV_FULL ? LV_SYMBOL_CHARGE : pct > 75 ? LV_SYMBOL_BATTERY_FULL :
                          pct > 50 ? LV_SYMBOL_BATTERY_3 : pct > 25 ? LV_SYMBOL_BATTERY_2 :
                          pct > 8 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(buf, sizeof(buf), LV_SYMBOL_USB " %d pad%s   %s %d%%", pads, pads == 1 ? "" : "s", sym, pct);
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_USB " %d pad%s", pads, pads == 1 ? "" : "s");
    }
    if (lx_net_state() == LX_NET_CONNECTED) {
        size_t n = strlen(buf);
        snprintf(buf + n, sizeof(buf) - n, "   " LV_SYMBOL_WIFI);
    }
    lv_label_set_text(s_sys_pads, buf);
    if (lv_screen_active() == s_settings && tick % 2 == 0) refresh_info();
}

/* After a crash reboot: say which app crashed (details: papp_dev.py crash) */
static void show_crash_notice(void)
{
    lx_app_crash_ctx_t c;
    if (!lx_app_last_crash(&c)) return;
    papp_manifest_t m;
    const char *name = lx_app_read_manifest(c.path, &m) == ESP_OK ? m.name : c.path;
    char text[256];
    snprintf(text, sizeof(text), "%s stopped with an error and the board restarted.\n"
             "Details for developers: papp_dev.py crash", name);
    lv_obj_t *mb = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mb, LV_SYMBOL_WARNING "  App crashed");
    lv_msgbox_add_text(mb, text);
    lv_obj_t *ok = lv_msgbox_add_footer_button(mb, "OK");
    lv_obj_add_event_cb(ok, on_crash_ok, LV_EVENT_CLICKED, mb);
    lv_obj_set_width(mb, 520);
    s_crash_box = mb;
    s_crash_ok = ok;
    focus_set(ok);
}

/* ── Launcher update from the SD card ─────────────────────── */

#define CONFIRM_AFTER_US 10000000       /* a new version must survive this long */

static lv_obj_t *s_upd_box, *s_upd_text;
static volatile bool s_upd_go;

static void upd_close(void)
{
    lv_msgbox_close(s_upd_box);
    s_upd_box = s_upd_text = NULL;
    s_focused = NULL;
    if (lv_screen_active() == s_settings) show_settings();
    else home_focus_list();
}

static bool s_upd_online;       /* following an online update */

static void on_upd_later(lv_event_t *ev)
{
    s_upd_online = false;
    upd_close();
}

static void upd_box(const char *text, bool ask);

static void on_upd_install(lv_event_t *ev)
{
    if (s_upd_online) {
        upd_close();                            /* no buttons while it downloads */
        upd_box("Downloading...", false);
        lx_update_online_download();
    } else {
        s_upd_go = true;
    }
}

static void upd_progress(int percent)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "Installing... %d%%", percent);
    lv_label_set_text(s_upd_text, buf);
    lv_refr_now(s_disp);
}

static void upd_message(const char *title, const char *text)
{
    lv_obj_t *mb = lv_msgbox_create(NULL);
    lv_msgbox_add_title(mb, title);
    lv_msgbox_add_text(mb, text);
    lv_obj_t *ok = lv_msgbox_add_footer_button(mb, "OK");
    lv_obj_add_event_cb(ok, on_crash_ok, LV_EVENT_CLICKED, mb);
    lv_obj_set_width(mb, 520);
    s_crash_box = mb;
    s_crash_ok = ok;
    focus_set(ok);
}

static void upd_box(const char *text, bool ask)
{
    s_upd_box = lv_msgbox_create(NULL);
    lv_msgbox_add_title(s_upd_box, LV_SYMBOL_DOWNLOAD "  Launcher update");
    s_upd_text = lv_msgbox_add_text(s_upd_box, text);
    lv_obj_set_width(s_upd_box, 520);
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);
    focus_clear_list();
    if (!ask) return;
    lv_obj_t *yes = lv_msgbox_add_footer_button(s_upd_box, "Install");
    lv_obj_t *no = lv_msgbox_add_footer_button(s_upd_box, "Later");
    lv_obj_add_event_cb(yes, on_upd_install, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(no, on_upd_later, LV_EVENT_CLICKED, NULL);
    focus_add(yes);
    focus_add(no);
    focus_set(yes);
}

/* Wi-Fi chip (C6) firmware from the card: /sd/update/c6.bin, offered first by Settings > Update */
#define C6_FILE "/sd/update/c6.bin"
static bool s_c6_updating;

static void on_c6_install(lv_event_t *ev)
{
    lv_msgbox_close(s_upd_box);
    s_upd_box = lv_msgbox_create(NULL);         /* no buttons: it ends in a restart or an error */
    lv_msgbox_add_title(s_upd_box, LV_SYMBOL_WIFI "  Wi-Fi chip update");
    s_upd_text = lv_msgbox_add_text(s_upd_box, "Updating the Wi-Fi chip... 0%\nDon't turn the board off.");
    lv_obj_set_width(s_upd_box, 560);
    focus_clear_list();
    s_focused = NULL;
    s_c6_updating = true;
    lx_net_c6_update(C6_FILE);
}

static void c6_tick(void)
{
    static int last = -2;
    int p = lx_c6_update_percent();
    if (lx_c6_update_error()[0]) {
        s_c6_updating = false;
        last = -2;
        upd_close();
        upd_message(LV_SYMBOL_WARNING "  Wi-Fi chip update", lx_c6_update_error());
        return;
    }
    if (p == last || !s_upd_text) return;
    last = p;
    char buf[80];
    if (p >= 100) snprintf(buf, sizeof(buf), "Done. The board restarts in a few seconds.");
    else snprintf(buf, sizeof(buf), "Updating the Wi-Fi chip... %d%%\nDon't turn the board off.", p < 0 ? 0 : p);
    lv_label_set_text(s_upd_text, buf);
}

/* Install / Later for C6 firmware ver; Later is the default */
static void c6_ask(const char *ver, lv_event_cb_t on_yes)
{
    char text[200];
    const char *cur = lx_c6_version();
    snprintf(text, sizeof(text), "Update the Wi-Fi chip to firmware %s?\nNow: %s\nTakes about a minute, then the board restarts.",
             ver, cur[0] && cur[0] != '?' ? cur : "unknown");
    s_upd_box = lv_msgbox_create(NULL);
    lv_msgbox_add_title(s_upd_box, LV_SYMBOL_WIFI "  Wi-Fi chip update");
    s_upd_text = lv_msgbox_add_text(s_upd_box, text);
    lv_obj_t *yes = lv_msgbox_add_footer_button(s_upd_box, "Install");
    lv_obj_t *no = lv_msgbox_add_footer_button(s_upd_box, "Later");
    lv_obj_add_event_cb(yes, on_yes, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(no, on_upd_later, LV_EVENT_CLICKED, NULL);
    lv_obj_set_width(s_upd_box, 560);
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);
    focus_clear_list();
    focus_add(yes);
    focus_add(no);
    focus_set(no);                      /* a chip update is never the default */
}

/* true when it showed something about the card's C6 image */
static bool c6_offer(void)
{
    struct stat st;
    if (stat(C6_FILE, &st) != 0) return false;
    char ver[33], why[96], text[200];
    if (!lx_c6_update_supported()) {
        upd_message("Wi-Fi chip update", "This board can't update its Wi-Fi chip.");
        return true;
    }
    if (!lx_c6_check_image(C6_FILE, ver, sizeof(ver), why, sizeof(why))) {
        snprintf(text, sizeof(text), "%s is not usable:\n%s", C6_FILE, why);
        upd_message(LV_SYMBOL_WARNING "  Wi-Fi chip update", text);
        return true;
    }
    if (strcmp(lx_c6_version(), ver) == 0) return false;   /* already on it: carry on with the launcher */
    c6_ask(ver, on_c6_install);
    return true;
}

/* The release's C6 firmware, when it is another version than the chip runs and fits this launcher */
static bool c6_online_wanted(void)
{
    const char *v = lx_update_online_c6_version(), *host = lx_c6_host_version();
    int a, b, c, d;
    return v[0] && lx_c6_update_supported() && strcmp(v, lx_c6_version()) != 0 &&
           sscanf(v, "%d.%d", &a, &b) == 2 && sscanf(host, "%d.%d", &c, &d) == 2 && a == c && b == d;
}

static void on_c6_download(lv_event_t *ev)
{
    upd_close();
    upd_box("Downloading the Wi-Fi chip firmware...", false);
    s_upd_online = true;
    lx_update_online_download_c6();
}

static void on_upd_check(lv_event_t *ev)
{
    if (c6_offer()) return;
    if (lx_net_state() != LX_NET_CONNECTED) {
        upd_message("Launcher update", "Connect to Wi-Fi first.");
        return;
    }
    s_upd_online = true;
    upd_box("Checking for updates...", false);
    lx_update_online_check();
}

static void upd_online_tick(void)
{
    char buf[96];
    switch (lx_update_online_state()) {
    case LX_ONLINE_BUSY:
        if (lx_update_online_percent() > 0 && s_upd_text) {
            snprintf(buf, sizeof(buf), "Downloading... %d%%", lx_update_online_percent());
            lv_label_set_text(s_upd_text, buf);
        }
        break;
    case LX_ONLINE_AVAILABLE:
        upd_close();
        snprintf(buf, sizeof(buf), "Install launcher %s?\nThis version: %s",
                 lx_update_online_version(), LX_LAUNCHER_VERSION);
        upd_box(buf, true);
        lx_update_online_reset();
        break;
    case LX_ONLINE_LATEST:
        s_upd_online = false;
        upd_close();
        lx_update_online_reset();
        if (c6_online_wanted()) c6_ask(lx_update_online_c6_version(), on_c6_download);
        else upd_message("Launcher update", "This is the latest version.");
        break;
    case LX_ONLINE_C6_READY:
        s_upd_online = false;
        lx_update_online_reset();
        on_c6_install(NULL);            /* checked by its SHA-256; lx_c6 checks the image again */
        break;
    case LX_ONLINE_FAILED:
        s_upd_online = false;
        upd_close();
        snprintf(buf, sizeof(buf), "%s", lx_update_online_error());
        lx_update_online_reset();
        upd_message("Launcher update", buf);
        break;
    case LX_ONLINE_READY:
        s_upd_online = false;
        lx_update_online_reset();
        s_upd_go = true;
        break;
    default: break;
    }
}

static void upd_offer(void)
{
    char ver[32], text[128];
    if (lx_update_rolled_back()) {
        upd_message(LV_SYMBOL_WARNING "  Update failed",
                    "The new launcher did not start, so the previous version was restored.");
        return;
    }
    if (!lx_update_available(ver, sizeof(ver))) return;
    snprintf(text, sizeof(text), "Install launcher %s from the SD card?\nThis version: %s",
             ver, LX_LAUNCHER_VERSION);
    s_upd_box = lv_msgbox_create(NULL);
    lv_msgbox_add_title(s_upd_box, LV_SYMBOL_DOWNLOAD "  Launcher update");
    s_upd_text = lv_msgbox_add_text(s_upd_box, text);
    lv_obj_t *yes = lv_msgbox_add_footer_button(s_upd_box, "Install");
    lv_obj_t *no = lv_msgbox_add_footer_button(s_upd_box, "Later");
    lv_obj_add_event_cb(yes, on_upd_install, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(no, on_upd_later, LV_EVENT_CLICKED, NULL);
    lv_obj_set_width(s_upd_box, 520);
    focus_clear_list();
    focus_add(yes);
    focus_add(no);
    focus_set(yes);
}

static void upd_install(void)
{
    s_upd_go = false;
    esp_err_t err = lx_update_install(upd_progress);
    if (err == ESP_OK) {
        lv_label_set_text(s_upd_text, "Done. Restarting...");
        lv_refr_now(s_disp);
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
    }
    char buf[80];
    snprintf(buf, sizeof(buf), "Install failed: %s", esp_err_to_name(err));
    lv_label_set_text(s_upd_text, buf);
}

/* ── For the other launcher screens (home_ui.h) ──────────────────────── */

int ui_dx(void)                 { return s_dx; }
int ui_dy(void)                 { return s_dy; }
void ui_focus_clear(void)       { focus_clear_list(); }
void ui_focus_add(lv_obj_t *o)  { focus_add(o); }
void ui_focus_set(lv_obj_t *o)  { focus_set(o); }
lv_obj_t *ui_focus_get(void)    { return s_focused; }
void ui_focus_style(lv_obj_t *o, int radius)                    { focus_style(o, radius); }
void ui_message(const char *title, const char *text)            { upd_message(title, text); }
bool home_show_dev(void)        { return s_show_dev; }

void ui_focus_drop(void)
{
    if (s_focused) lv_obj_remove_state(s_focused, LV_STATE_FOCUSED);
    s_focused = NULL;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    return label(parent, font, color, text);
}

lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h, int radius)
{
    return panel(parent, x, y, w, h, radius);
}

lv_obj_t *ui_button(lv_obj_t *parent, int x, int y, int w, const char *text, lv_event_cb_t cb)
{
    return wifi_button(parent, x, y, w, text, cb);
}

lv_obj_t *ui_icon(lv_obj_t *parent, const char *src, const char *key, const char *name, int size)
{
    return icon_box(parent, src, key, name, false, size);
}

lv_obj_t *ui_screen(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(scr, C_BG_TOP, 0);
    lv_obj_set_style_bg_grad_color(scr, C_BG_BOTTOM, 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);
    return scr;
}

void home_launch(const char *path)
{
    snprintf(s_launch, sizeof(s_launch), "%s", path);
}

/* ── LVGL glue ─────────────────────────────────────────────────────────── */

static uint32_t tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void flush_cb(lv_display_t *d, const lv_area_t *area, uint8_t *px)
{
    if (lv_display_flush_is_last(d) && lx_app_state() == LX_APP_IDLE)
        lx_comp_launcher_present(s_frame);
    lv_display_flush_ready(d);
}

#define DIM_AFTER_US   180000000LL      /* home screen left alone: dim */
#define DIM_PERCENT    15
static int64_t s_last_input;
static bool s_dimmed;

static void dim_tick(void)
{
    bool idle = esp_timer_get_time() - s_last_input > DIM_AFTER_US;
    if (idle == s_dimmed) return;
    s_dimmed = idle;
    bsp_display_backlight(idle && s_brightness > DIM_PERCENT ? DIM_PERCENT : s_brightness);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    bsp_touch_point_t p[1];
    if ((bsp_info()->caps & BSP_CAP_TOUCH) && bsp_touch_read(p, 1) > 0) {
        data->point.x = p[0].x;
        data->point.y = p[0].y;
        data->state = LV_INDEV_STATE_PRESSED;
        s_last_input = esp_timer_get_time();
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/* Gamepad and keyboard: directions move the focus; on a slider left/right
 * change its value; A / Start / Enter click; B / Esc go back. */
static void poll_keys(void)
{
    papp_input_event_t ev;
    while (lx_input_poll_event(&ev)) {
        s_last_input = esp_timer_get_time();
        int dx = 0, dy = 0, ok = 0, back = 0, menu = 0;
        if (ev.type == PAPP_EV_BUTTON_DOWN) {
            switch (ev.code) {
            case 16: dy = -1; break;
            case 17: dy = 1; break;
            case 18: dx = -1; break;
            case 19: dx = 1; break;
            case 0: case 9: ok = 1; break;          /* A, START */
            case 1: back = 1; break;                /* B */
            case 2: case 3: menu = 1; break;        /* X, Y */
            default: break;
            }
        } else if (ev.type == PAPP_EV_KEY_DOWN) {
            if (lv_screen_active() == s_pw_scr) {   /* a USB keyboard types the password */
                if (ev.code == 0x28 || ev.code == 0x58) {
                    pw_accept();
                } else if (ev.code == 0x2A) {
                    lv_textarea_delete_char(s_pw_text);
                } else if (ev.code == 0x29) {
                    pw_close();
                } else if (ev.text >= 32 && ev.text < 127) {
                    lv_textarea_add_char(s_pw_text, ev.text);
                }
                continue;
            }
            switch (ev.code) {
            case 0x52: dy = -1; break;
            case 0x51: dy = 1; break;
            case 0x50: dx = -1; break;
            case 0x4F: dx = 1; break;
            case 0x28: case 0x58: case 0x2C: ok = 1; break;   /* Enter, keypad Enter, Space */
            case 0x29: case 0x2A: back = 1; break;            /* Esc, Backspace */
            case 0x2B: case 0x65: menu = 1; break;            /* Tab, Menu */
            default: break;
            }
        } else {
            continue;
        }
        if (s_crash_box) {
            if (ok || back) lv_obj_send_event(s_crash_ok, LV_EVENT_CLICKED, NULL);
            continue;
        }
        if (dy && s_clk_scr && clk_field_of(s_focused) >= 0) {
            clk_step(clk_field_of(s_focused), -dy);
        } else if (ok && s_clk_scr && clk_field_of(s_focused) >= 0) {
            focus_set(s_clk_save);                  /* up / down change the field: A goes on to Save */
        } else if (dx && s_clk_tz && s_focused == s_clk_tz) {
            int n = lv_dropdown_get_option_count(s_clk_tz);
            lv_dropdown_set_selected(s_clk_tz, (lv_dropdown_get_selected(s_clk_tz) + n + dx) % n);
            on_clk_tz(NULL);
        } else if (dx && s_focused == s_wifi_tz) {
            int n = lv_dropdown_get_option_count(s_wifi_tz);
            lv_dropdown_set_selected(s_wifi_tz, (lv_dropdown_get_selected(s_wifi_tz) + n + dx) % n);
            on_wifi_tz(NULL);
        } else if (dx && s_focused && lv_obj_check_type(s_focused, &lv_slider_class)) {
            lv_slider_set_value(s_focused, lv_slider_get_value(s_focused) + dx * 5, LV_ANIM_OFF);
            lv_obj_send_event(s_focused, LV_EVENT_VALUE_CHANGED, NULL);
            save_settings();
        } else if (dx || dy) {
            focus_move(dx, dy);
        } else if (ok && lv_screen_active() == s_pw_scr && s_pw_mode != PW_WIFI) {
            pw_accept();
        } else if (ok && s_focused) {
            lv_obj_send_event(s_focused, LV_EVENT_CLICKED, NULL);
        } else if (menu && s_focused && !s_menu && lv_obj_get_parent(s_focused) == s_grid) {
            tile_menu(lv_obj_get_user_data(s_focused));
        } else if (back && s_menu) {
            menu_close();
        } else if (back && lv_screen_active() == s_home && s_folder[0]) {
            folder_leave();
        } else if (back && lv_screen_active() == s_settings) {
            show_home();
        } else if (back && s_clk_scr && lv_screen_active() == s_clk_scr) {
            show_home();
        } else if (back && lv_screen_active() == s_wifi_scr) {
            show_settings();
        } else if (back && store_back()) {
            /* a Store screen took it */
        } else if (back && lv_screen_active() == s_pw_scr) {
            pw_close();
        }
    }
}

/* The input event queue only reports digital buttons. Sample the left stick
 * separately while the launcher is active so it can navigate like a D-pad. */
static void poll_stick_navigation(void)
{
    enum { STICK_DEADZONE = 12000 };
    static int last_dir;
    static int64_t repeat_at;
    papp_gamepad_t g = { 0 };
    if (!lx_input_gamepad(0, &g)) {
        last_dir = 0;
        return;
    }

    int dir = 0;
    int ax = g.lx < 0 ? -g.lx : g.lx;
    int ay = g.ly < 0 ? -g.ly : g.ly;
    if (ax >= STICK_DEADZONE || ay >= STICK_DEADZONE) {
        if (ax > ay) dir = g.lx < 0 ? 1 : 2;       /* left, right */
        else         dir = g.ly < 0 ? 3 : 4;       /* up, down */
    }

    int64_t now = esp_timer_get_time();
    if (!dir) {
        last_dir = 0;
        return;
    }
    if (dir != last_dir) {
        last_dir = dir;
        repeat_at = now + 350000;
    } else if (now < repeat_at) {
        return;
    } else {
        repeat_at = now + 150000;
    }

    s_last_input = now;
    switch (dir) {
    case 1: focus_move(-1, 0); break;
    case 2: focus_move(1, 0); break;
    case 3: focus_move(0, -1); break;
    case 4: focus_move(0, 1); break;
    }
}

/* Select + Y anywhere: the visible picture, landscape, as a BMP on the card */
static void screenshot(void)
{
    lx_comp_screenshot(NULL, 0);
}

static void screenshot_tick(void)
{
    static bool held;
    papp_gamepad_t g = { 0 };
    lx_input_gamepad(0, &g);
    bool now = (g.buttons & (PAPP_BTN_SELECT | PAPP_BTN_Y)) == (PAPP_BTN_SELECT | PAPP_BTN_Y);
    if (now && !held) screenshot();
    held = now;
}

static void launch(const char *path)
{
    char err[160];
    if (store_installing()) {           /* the download writes to the card */
        upd_message("Please wait", "An app is being downloaded. Start it again when it is done.");
        return;
    }
    remember_last(path);
    ESP_LOGI(TAG, "starting %s", path);
    if (lx_app_launch(path, 0, NULL, err, sizeof(err)) != ESP_OK) {
        lv_obj_t *mb = lv_msgbox_create(NULL);
        lv_msgbox_add_title(mb, "Cannot start the app");
        lv_msgbox_add_text(mb, err);
        lv_msgbox_add_close_button(mb);
    }
}

static void home_task(void *arg)
{
    bool was_running = false, confirmed = false, offered = false;
    for (;;) {
        if (!confirmed && esp_timer_get_time() > CONFIRM_AFTER_US) {
            lx_update_confirm();
            confirmed = true;
        }
        if (lx_app_state() != LX_APP_IDLE) {
            s_last_input = esp_timer_get_time();
            if (s_dimmed) dim_tick();
            screenshot_tick();
            if (!was_running) {
                lx_net_pause(true);
                char p[128];                              /* also apps started over DevLink */
                snprintf(p, sizeof(p), "%s", lx_app_path());
                if (p[0] && strcmp(p, s_last) != 0) remember_last(p);
            }
            was_running = true;
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (was_running) {                      /* back from an app: redraw everything */
            was_running = false;
            lx_net_pause(false);
            lx_input_flush();
            s_volume = lx_audio_master_volume_get();
            lv_slider_set_value(s_vol_slider, s_volume, LV_ANIM_OFF);
            apply_settings();
            if (lv_screen_active() != s_home) lv_screen_load(s_home);
            s_focused = NULL;
            refresh_home();
            lv_obj_invalidate(lv_screen_active());
        }
        if (confirmed && !offered && !s_crash_box) {
            offered = true;
            upd_offer();
        }
        dim_tick();
        screenshot_tick();
        if (s_map_step >= 0) map_tick();
        else {
            poll_keys();
            poll_stick_navigation();
        }
        wifi_tick();
        store_tick();
        if (store_updates() != s_badge_shown) badge_update();
        if (lx_web_changes() != s_web_seen && lv_screen_active() == s_home &&
            lv_obj_get_child_count(lv_layer_top()) == 0) {            /* not under a menu or message */
            s_web_seen = lx_web_changes();
            const app_entry_t *e = s_focused ? lv_obj_get_user_data(s_focused) : NULL;
            char keep[160] = "";
            if (e) snprintf(keep, sizeof(keep), "%s", e->path);
            home_view(keep[0] ? keep : NULL);
        }
        uint32_t wait = lv_timer_handler();
        if (s_upd_online) upd_online_tick();
        if (s_c6_updating) c6_tick();
        if (s_upd_go) upd_install();
        if (s_launch[0]) {
            char path[160];
            snprintf(path, sizeof(path), "%s", s_launch);
            s_launch[0] = 0;
            launch(path);
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(wait < 5 ? 5 : wait > 20 ? 20 : wait));
    }
}

esp_err_t home_start(void)
{
    nvs_handle_t h;
    if (nvs_open("launcher", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v;
        if (nvs_get_u8(h, "volume", &v) == ESP_OK) s_volume = v;
        if (nvs_get_u8(h, "bright", &v) == ESP_OK) s_brightness = v;
        if (nvs_get_u8(h, "devapps", &v) == ESP_OK) s_show_dev = v;
        nvs_close(h);
    }

    temperature_sensor_config_t tc = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&tc, &s_tsens) == ESP_OK) temperature_sensor_enable(s_tsens);
    else s_tsens = NULL;

    int w, h_px;
    lx_comp_display_size(&w, &h_px);
    size_t bytes = (size_t)w * h_px * 2;
    s_frame = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM);
    if (!s_frame) return ESP_ERR_NO_MEM;

    lv_init();
    lv_tick_set_cb(tick_ms);
    s_disp = lv_display_create(w, h_px);
    lv_display_set_color_format(s_disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(s_disp, s_frame, NULL, bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(s_disp, flush_cb);
    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read);

    s_dx = w - 800;
    s_dy = h_px - 480;
    build_home();
    build_settings();
    store_build();
    if (s_clk_area) build_clock_screen();
    refresh_home();
    lv_screen_load(s_home);
    lv_timer_create(update_status, 1000, NULL);
    update_status(NULL);
    show_crash_notice();

    xTaskCreatePinnedToCore(home_task, "home", 8192, NULL, 4, NULL, 0);
    ESP_LOGI(TAG, "home screen up, %d apps", s_ninstalled);
    return ESP_OK;
}
