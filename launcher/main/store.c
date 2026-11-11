/*
 * The Store: every app the launcher knows about, from the online catalogue
 * (lx_appstore: apps.json on the "apps" release) and from the card.
 *
 * Front page: categories on the left, the apps of the chosen one on the right
 * (icon, name, one line, Get / Update / Installed). An app's page: description,
 * version, size, Install / Update / Reinstall, Open and Remove. Installed lists
 * the card's apps, also the ones the catalogue doesn't have (copied by hand).
 *
 * An installed app is updated where it is; a new one goes to the catalogue's
 * folder. Installs run one at a time from a queue, in the background, and the
 * home screen starts no app meanwhile. The catalogue is read once when Wi-Fi
 * first connects (for the update count on the Store tile) and again when the
 * Store is opened after a while. Without a network the Store opens on Installed.
 */
#include "store.h"
#include "home_ui.h"
#include "lx_app.h"
#include "lx_appstore.h"
#include "lx_input.h"
#include "lx_net.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "ff.h"

static const char *TAG = "store";

/* Paths are capped on purpose; a longer one is cut, not overflowed */
#pragma GCC diagnostic ignored "-Wformat-truncation"

#define MAX_ITEMS    (LX_APPSTORE_MAX + 48)

enum { CAT_ALL, CAT_EMU, CAT_GAMES, CAT_TOOLS, CAT_PROD, CAT_EDU, CAT_COMMUNITY, CAT_INSTALLED, CAT_N };
#define CAT_LAST_KEY CAT_EDU        /* categories an app can name in the catalogue */
static const char *const k_cat_name[CAT_N] = {
    "All apps", "Emulators", "Games", "Tools", "Productivity", "Education", "Community", "Installed",
};
static const char *const k_cat_symbol[CAT_N] = {
    LV_SYMBOL_LIST, LV_SYMBOL_VIDEO, LV_SYMBOL_PLAY, LV_SYMBOL_SETTINGS, LV_SYMBOL_EDIT, LV_SYMBOL_FILE,
    LV_SYMBOL_SHUFFLE, LV_SYMBOL_SD_CARD,
};
static const char *const k_cat_key[CAT_N] = { "", "emulators", "games", "tools", "productivity", "education" };

typedef struct {
    int cat;                    /* catalogue index, -1 = only on the card */
    uint8_t category;           /* CAT_EMU .. CAT_LAST_KEY; CAT_ALL = none given */
    bool community;
    char id[32];
    char name[32];
    char path[160];             /* the installed .papp, "" = not on the card */
    char version[16];           /* installed version */
    char icon[176];             /* "S:/sd/..." or "" */
} item_t;

static item_t *s_items;         /* MAX_ITEMS, in PSRAM */
static int s_nitems, s_updates;

/* Catalogue and installs. The Store keeps a copy of the catalogue, taken when a read
 * is done: lx_appstore's own list is rewritten while the next read runs. Its indices
 * are lx_appstore's, which install by index (no read runs during installs). */
static lx_appstore_app_t *s_cat;        /* LX_APPSTORE_MAX, in PSRAM */
static int s_ncat;
static bool s_cat_ok;                   /* the catalogue has been read */
static bool s_fetching, s_changed;
static int s_queue[LX_APPSTORE_MAX], s_nqueue;
static int s_doing = -1;                /* catalogue index being installed */
static char s_status_text[128];

/* Screens */
static lv_obj_t *s_scr, *s_back, *s_all, *s_status, *s_rail[CAT_N], *s_list;
static int s_tab = CAT_ALL;
static char (*s_row_id)[32];            /* MAX_ITEMS: the app of each row in s_list */
static lv_obj_t **s_row_state;          /* MAX_ITEMS: its Get / Update / Installed label */
static int s_nrows;

static lv_obj_t *s_dscr, *s_dback, *s_dbody, *s_dget, *s_dopen, *s_dremove, *s_dstate;
static char s_detail_id[32];            /* the app on the app page */
static lv_obj_t *s_confirm, *s_confirm_no, *s_confirm_yes;

static void list_show(void);
static void detail_show(void);

/* ── Items: the catalogue plus the card ────────────────────────────────── */

static uint8_t category_of(const char *key, const char *id)
{
    for (int c = CAT_EMU; c <= CAT_LAST_KEY; c++)
        if (strcmp(key, k_cat_key[c]) == 0) return c;
    if (strncmp(id, "emu.", 4) == 0) return CAT_EMU;            /* the project's id prefixes */
    if (strncmp(id, "game.", 5) == 0) return CAT_GAMES;
    if (strncmp(id, "tool.", 5) == 0) return CAT_TOOLS;
    return CAT_ALL;
}

/* By id; an app whose manifest can't be read goes by its name */
static item_t *item_by_id(const char *id)
{
    for (int i = 0; i < s_nitems; i++)
        if (strcmp(s_items[i].id[0] ? s_items[i].id : s_items[i].name, id) == 0) return &s_items[i];
    return NULL;
}

static item_t *item_by_cat(int cat)
{
    for (int i = 0; i < s_nitems; i++)
        if (s_items[i].cat == cat) return &s_items[i];
    return NULL;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* The app in dir (one per directory); false if there is no .papp in it */
static bool card_app(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *fe;
    bool found = false;
    while ((fe = readdir(d))) {
        size_t n = strlen(fe->d_name);
        if (n < 6 || strcasecmp(fe->d_name + n - 5, ".papp") != 0) continue;
        found = true;
        char path[160];
        snprintf(path, sizeof(path), "%s/%s", dir, fe->d_name);
        papp_manifest_t m;
        bool ok = lx_app_read_manifest(path, &m) == ESP_OK;
        item_t *it = ok ? item_by_id(m.id) : NULL;
        if (it && it->path[0]) it = NULL;                   /* a second copy: listed on its own */
        if (!it) {
            if (s_nitems >= MAX_ITEMS || (ok && (m.flags & PAPP_MF_HIDDEN) && !home_show_dev())) break;
            it = &s_items[s_nitems++];
            memset(it, 0, sizeof(*it));
            it->cat = -1;
            if (ok) {
                snprintf(it->id, sizeof(it->id), "%s", m.id);
                snprintf(it->name, sizeof(it->name), "%s", m.name);
            } else {
                snprintf(it->name, sizeof(it->name), "%.*s", (int)(n - 5), fe->d_name);
            }
            it->category = category_of("", it->id);
        }
        snprintf(it->path, sizeof(it->path), "%s", path);
        snprintf(it->version, sizeof(it->version), "%s", ok ? m.version : "?");
        if (!it->icon[0]) {
            char icon[160];
            snprintf(icon, sizeof(icon), "%s/icon.png", dir);
            if (file_exists(icon)) snprintf(it->icon, sizeof(it->icon), "S:%s", icon);
        }
        break;
    }
    closedir(d);
    return found;
}

/* Apps are in /sd/apps/<dir> or /sd/apps/<folder>/<dir> */
static void card_scan(void)
{
    DIR *d = opendir(APPS_DIR);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.' || de->d_type != DT_DIR) continue;
        char dir[128];
        snprintf(dir, sizeof(dir), APPS_DIR "/%s", de->d_name);
        if (card_app(dir)) continue;
        DIR *sub = opendir(dir);                            /* a folder of apps */
        if (!sub) continue;
        struct dirent *se;
        while ((se = readdir(sub))) {
            if (se->d_name[0] == '.' || se->d_type != DT_DIR) continue;
            char inner[160];
            snprintf(inner, sizeof(inner), "%s/%s", dir, se->d_name);
            card_app(inner);
        }
        closedir(sub);
    }
    closedir(d);
}

static int item_cmp(const void *a, const void *b)
{
    return strcasecmp(((const item_t *)a)->name, ((const item_t *)b)->name);
}

static const lx_appstore_app_t *cat_app(int i)
{
    return i >= 0 && i < s_ncat ? &s_cat[i] : NULL;
}

static bool has_update(const item_t *it)
{
    return it->cat >= 0 && it->path[0] && lx_appstore_newer(cat_app(it->cat)->version, it->version);
}

static void items_refresh(void)
{
    if (!s_items) {
        s_items = heap_caps_calloc(MAX_ITEMS, sizeof(item_t), MALLOC_CAP_SPIRAM);
        s_row_id = heap_caps_calloc(MAX_ITEMS, 32, MALLOC_CAP_SPIRAM);
        s_row_state = heap_caps_calloc(MAX_ITEMS, sizeof(lv_obj_t *), MALLOC_CAP_SPIRAM);
        if (!s_items || !s_row_id || !s_row_state) abort();
    }
    s_nitems = 0;
    for (int i = 0; i < s_ncat && s_nitems < MAX_ITEMS; i++) {
        const lx_appstore_app_t *a = &s_cat[i];
        item_t *it = &s_items[s_nitems++];
        memset(it, 0, sizeof(*it));
        it->cat = i;
        it->community = a->community;
        it->category = category_of(a->category, a->id);
        snprintf(it->id, sizeof(it->id), "%s", a->id);
        snprintf(it->name, sizeof(it->name), "%s", a->name);
        char icon[96];
        if (lx_appstore_icon_path(a, icon, sizeof(icon))[0] && file_exists(icon))
            snprintf(it->icon, sizeof(it->icon), "S:%s", icon);
    }
    card_scan();
    qsort(s_items, s_nitems, sizeof(item_t), item_cmp);
    s_updates = 0;
    for (int i = 0; i < s_nitems; i++) s_updates += has_update(&s_items[i]);
}

/* ── Install queue ─────────────────────────────────────────────────────── */

static bool queued(int cat)
{
    for (int q = 0; q < s_nqueue; q++)
        if (s_queue[q] == cat) return true;
    return false;
}

static void install_next(void)
{
    if (s_doing >= 0 || !s_nqueue) return;
    int i = s_queue[0];
    memmove(s_queue, s_queue + 1, --s_nqueue * sizeof(int));
    const lx_appstore_app_t *a = cat_app(i);
    const item_t *it = item_by_cat(i);
    char papp[160], icon[160], ficon[160] = "";
    if (it && it->path[0]) {                                /* update in place */
        snprintf(papp, sizeof(papp), "%s", it->path);
        snprintf(icon, sizeof(icon), "%s", papp);
        char *slash = strrchr(icon, '/');
        snprintf(slash, sizeof(icon) - (slash - icon), "/icon.png");
    } else if (a->folder[0]) {
        snprintf(papp, sizeof(papp), APPS_DIR "/%s/%s/%s", a->folder, a->dir, a->file);
        snprintf(icon, sizeof(icon), APPS_DIR "/%s/%s/icon.png", a->folder, a->dir);
        snprintf(ficon, sizeof(ficon), APPS_DIR "/%s/icon.png", a->folder);
    } else {
        snprintf(papp, sizeof(papp), APPS_DIR "/%s/%s", a->dir, a->file);
        snprintf(icon, sizeof(icon), APPS_DIR "/%s/icon.png", a->dir);
    }
    s_doing = i;
    snprintf(s_status_text, sizeof(s_status_text), "Installing %s...", a->name);
    lx_appstore_install(i, papp, icon, ficon[0] ? ficon : NULL);
}

static void install_add(int cat)
{
    if (cat < 0 || cat == s_doing || queued(cat) || s_nqueue >= LX_APPSTORE_MAX) return;
    s_queue[s_nqueue++] = cat;
    install_next();
}

static void fetch(void)
{
    s_fetching = true;
    snprintf(s_status_text, sizeof(s_status_text), "Reading the app list...");
    lx_appstore_fetch();
}

static bool idle(void)
{
    return !s_fetching && s_doing < 0 && !s_nqueue && lx_appstore_state() == LX_APPSTORE_IDLE;
}

/* ── Remove ────────────────────────────────────────────────────────────── */

/* The directory of an installed app, only when it is /sd/apps/<dir> or
 * /sd/apps/<folder>/<dir>: never the apps directory or a folder itself */
static bool app_dir(const char *papp, char *dir, int len)
{
    const char *root = APPS_DIR "/";
    size_t rn = strlen(root);
    if (strncmp(papp, root, rn) != 0 || strstr(papp, "..")) return false;
    snprintf(dir, len, "%s", papp);
    char *slash = strrchr(dir, '/');
    if (!slash || slash <= dir + rn) return false;
    *slash = 0;
    const char *rel = dir + rn;
    const char *inner = strchr(rel, '/');
    if (!rel[0] || rel[0] == '.' || (inner && (strchr(inner + 1, '/') || !inner[1] || inner[1] == '.'))) return false;
    return true;
}

/* What the POSIX calls can't do, through FatFS: readdir ends at an entry it can't
 * name (hiding the rest), and a read-only directory can't be deleted. The card is
 * the only FAT volume: drive 0. */
static bool ff_rm_dir(const char *path)
{
    char fp[200], c[300];
    snprintf(fp, sizeof(fp), "0:%s", path + strlen("/sd"));
    FILINFO fi;
    FF_DIR d;
    if (f_opendir(&d, fp) == FR_OK) {
        for (int n = 0; n < 64 && f_readdir(&d, &fi) == FR_OK && fi.fname[0]; n++) {
            ESP_LOGW(TAG, "left in %s: \"%s\" (attr 0x%02x, %lu bytes)", path, fi.fname, fi.fattrib,
                     (unsigned long)fi.fsize);
            snprintf(c, sizeof(c), "%s/%s", fp, fi.fname);
            f_chmod(c, 0, AM_RDO);
            if (!(fi.fattrib & AM_DIR)) f_unlink(c);
        }
        f_closedir(&d);
    }
    FRESULT r = f_stat(fp, &fi);
    if (r == FR_OK && (fi.fattrib & AM_RDO)) {
        ESP_LOGW(TAG, "%s is read-only (attr 0x%02x)", path, fi.fattrib);
        f_chmod(fp, 0, AM_RDO);
    }
    r = f_unlink(fp);
    if (r != FR_OK) ESP_LOGW(TAG, "%s: FatFS error %d", path, r);
    return r == FR_OK;
}

static bool rm_tree(const char *path, int depth)
{
    struct stat st;
    if (stat(path, &st) != 0) return true;
    if (!S_ISDIR(st.st_mode)) return remove(path) == 0;
    if (depth > 6) return false;
    for (;;) {
        DIR *d = opendir(path);
        if (!d) return false;
        char child[256] = "";
        struct dirent *de;
        while ((de = readdir(d))) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            break;
        }
        closedir(d);                        /* never delete during readdir */
        if (!child[0]) break;
        if (!rm_tree(child, depth + 1)) return false;
    }
    return rmdir(path) == 0 || ff_rm_dir(path);
}

/* ── Front page ────────────────────────────────────────────────────────── */

static bool in_tab(const item_t *it, int tab)
{
    switch (tab) {
    case CAT_ALL: return true;
    case CAT_COMMUNITY: return it->community;
    case CAT_INSTALLED: return it->path[0] != 0;
    default: return it->category == tab;
    }
}

/* The state of an app as its row and page show it; true = an action (drawn as a button) */
static bool item_state(const item_t *it, char *buf, int len)
{
    if (it->cat >= 0 && it->cat == s_doing) {
        snprintf(buf, len, "%d%%", lx_appstore_state() == LX_APPSTORE_BUSY ? lx_appstore_percent() : 0);
        return false;
    }
    if (it->cat >= 0 && queued(it->cat)) { snprintf(buf, len, "Waiting"); return false; }
    if (!it->path[0]) { snprintf(buf, len, "Get"); return true; }
    if (has_update(it)) { snprintf(buf, len, "Update"); return true; }
    snprintf(buf, len, it->cat >= 0 || !s_cat_ok ? "Installed" : "On the card");
    return false;
}

static void state_style(lv_obj_t *l, bool action)
{
    lv_obj_set_style_bg_opa(l, action ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(l, C_BLUE, 0);
    lv_obj_set_style_text_color(l, action ? C_TEXT : C_DIM, 0);
}

static void on_row(lv_event_t *ev)
{
    int k = (int)(intptr_t)lv_event_get_user_data(ev);
    if (k < 0 || k >= s_nrows) return;
    snprintf(s_detail_id, sizeof(s_detail_id), "%s", s_row_id[k]);
    detail_show();
}

static void on_tab(lv_event_t *ev)
{
    s_tab = (int)(intptr_t)lv_event_get_user_data(ev);
    ui_focus_set(s_rail[s_tab]);        /* a tap moves the focus too, or the old one keeps its frame */
    list_show();
}

static void on_back(lv_event_t *ev)
{
    show_home();
}

static void on_all(lv_event_t *ev)
{
    /* Updates when there are any, otherwise every project app not on the card (a new card) */
    int n = 0;
    for (int i = 0; i < s_nitems; i++) {
        const item_t *it = &s_items[i];
        bool want = s_updates ? has_update(it) : it->cat >= 0 && !it->path[0] && !it->community;
        if (want && it->cat != s_doing && !queued(it->cat)) {
            install_add(it->cat);
            n++;
        }
    }
    if (!n && s_doing < 0)
        snprintf(s_status_text, sizeof(s_status_text), s_cat_ok ? "All installed apps are up to date." :
                 "The app list is not loaded: connect to Wi-Fi in Settings.");
    list_show();
}

static lv_obj_t *note(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = ui_label(parent, &lv_font_montserrat_16, C_DIM, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(96));
    lv_obj_set_style_pad_all(l, 12, 0);
    return l;
}

/* Rebuilds the front page, keeping the focus on the same app or button */
static void list_show(void)
{
    char keep_id[32] = "";
    lv_obj_t *keep = NULL;
    int keep_row = -1;
    lv_obj_t *f = ui_focus_get();
    if (lv_screen_active() == s_scr && f && !s_confirm) {
        if (lv_obj_get_parent(f) == s_list) {
            keep_row = lv_obj_get_index(f);
            if (keep_row < s_nrows) snprintf(keep_id, sizeof(keep_id), "%s", s_row_id[keep_row]);
        } else {
            keep = f;
        }
    } else if (lv_screen_active() == s_dscr) {
        snprintf(keep_id, sizeof(keep_id), "%s", s_detail_id);     /* back from its page */
    }
    ui_focus_drop();
    if (lv_screen_active() != s_scr) lv_screen_load(s_scr);

    for (int c = 0; c < CAT_N; c++) {
        lv_obj_t *b = s_rail[c];
        lv_obj_set_style_bg_opa(b, c == s_tab ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        char buf[48];
        if (c == CAT_INSTALLED && s_updates)
            snprintf(buf, sizeof(buf), "%s  %s (%d)", k_cat_symbol[c], k_cat_name[c], s_updates);
        else
            snprintf(buf, sizeof(buf), "%s  %s", k_cat_symbol[c], k_cat_name[c]);
        lv_label_set_text(lv_obj_get_child(b, 0), buf);
    }

    lv_obj_clean(s_list);
    s_nrows = 0;
    int row_w = 582 + ui_dx() - 2 * 8 - 12;                 /* list padding, scrollbar */
    int missing = 0;
    for (int i = 0; i < s_nitems; i++) missing += s_items[i].cat >= 0 && !s_items[i].path[0] && !s_items[i].community;
    for (int k = 0; k < 2 * s_nitems; k++) {                /* apps with an update first */
        const item_t *it = &s_items[k % s_nitems];
        if (has_update(it) != (k < s_nitems) || !in_tab(it, s_tab) || s_nrows >= MAX_ITEMS) continue;
        lv_obj_t *b = lv_obj_create(s_list);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, row_w, 74);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        ui_focus_style(b, 12);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_color(b, C_FOCUS, LV_STATE_FOCUSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(b, C_FOCUS, LV_STATE_PRESSED);
        lv_obj_t *ic = ui_icon(b, it->icon, it->id[0] ? it->id : it->name, it->name, 56);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 8, 0);

        char state[24];
        bool action = item_state(it, state, sizeof(state));
        lv_obj_t *st = ui_label(b, &lv_font_montserrat_16, C_DIM, state);
        lv_obj_set_style_pad_hor(st, 16, 0);
        lv_obj_set_style_pad_ver(st, 7, 0);
        lv_obj_set_style_radius(st, 16, 0);
        state_style(st, action);
        lv_obj_align(st, LV_ALIGN_RIGHT_MID, -10, 0);

        int text_w = row_w - 80 - 140;
        lv_obj_t *n = ui_label(b, &lv_font_montserrat_20, C_TEXT, it->name);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_size(n, text_w, 26);                     /* one line, cut with ... */
        lv_obj_set_pos(n, 78, 11);
        char line[160];
        const lx_appstore_app_t *a = it->cat >= 0 ? cat_app(it->cat) : NULL;
        if (a && a->community)
            snprintf(line, sizeof(line), "By %s.  %s", a->author[0] ? a->author : "?", a->description);
        else if (a && a->description[0])
            snprintf(line, sizeof(line), "%s", a->description);
        else if (a)
            snprintf(line, sizeof(line), "Version %s", a->version);
        else
            snprintf(line, sizeof(line), s_cat_ok ? "Copied to the card, not in the Store. Version %s" :
                     "Version %s", it->version);       /* not known before the list is read */
        lv_obj_t *d = ui_label(b, &lv_font_montserrat_14, C_DIM, line);
        lv_label_set_long_mode(d, LV_LABEL_LONG_DOT);
        lv_obj_set_size(d, text_w, 18);
        lv_obj_set_pos(d, 78, 42);

        lv_obj_add_event_cb(b, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)s_nrows);
        snprintf(s_row_id[s_nrows], 32, "%s", it->id[0] ? it->id : it->name);
        s_row_state[s_nrows] = st;
        s_nrows++;
    }
    if (!s_cat_ok && s_tab != CAT_INSTALLED)
        note(s_list, s_fetching ? "Reading the app list..." :
             lx_net_state() == LX_NET_CONNECTED ? "The app list could not be read. Open the Store again to retry." :
             "Connect to Wi-Fi (Settings, Wi-Fi) to see the apps online. Installed shows the apps on the card.");
    else if (!s_nrows && (s_tab == CAT_PROD || s_tab == CAT_EDU))
        note(s_list, "No apps here yet.\n\nMade one? Share it: see \"Sharing your app\" in the launcher's README on GitHub.");
    else if (!s_nrows && s_tab == CAT_COMMUNITY)
        note(s_list, "No community apps yet.\n\nMade one? Share it: see \"Sharing your app\" in the launcher's README on GitHub.");
    else if (!s_nrows)
        note(s_list, s_tab == CAT_INSTALLED ? "No apps on the card yet." : "No apps here yet.");

    char buf[40];
    if (s_updates) snprintf(buf, sizeof(buf), LV_SYMBOL_REFRESH " Update all (%d)", s_updates);
    else if (s_cat_ok && missing) snprintf(buf, sizeof(buf), LV_SYMBOL_DOWNLOAD " Install all (%d)", missing);
    else snprintf(buf, sizeof(buf), LV_SYMBOL_OK " Up to date");
    lv_label_set_text(lv_obj_get_child(s_all, 0), buf);
    lv_label_set_text(s_status, s_status_text);

    ui_focus_clear();
    ui_focus_add(s_back);
    ui_focus_add(s_all);
    for (int c = 0; c < CAT_N; c++) ui_focus_add(s_rail[c]);
    for (int k = 0; k < s_nrows; k++) ui_focus_add(lv_obj_get_child(s_list, k));
    lv_obj_t *to = NULL;
    for (int k = 0; keep_id[0] && k < s_nrows; k++)
        if (strcmp(s_row_id[k], keep_id) == 0) to = lv_obj_get_child(s_list, k);
    if (!to && keep_row >= 0 && s_nrows) to = lv_obj_get_child(s_list, keep_row < s_nrows ? keep_row : s_nrows - 1);
    if (!to && keep) to = keep;
    if (!to) to = s_rail[s_tab];
    ui_focus_set(to);
}

static lv_obj_t *rail_button(lv_obj_t *parent, int y, int c)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_pos(b, 0, y);
    lv_obj_set_size(b, lv_pct(100), 42);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    ui_focus_style(b, 10);
    lv_obj_set_style_bg_color(b, C_BLUE, 0);
    lv_obj_set_style_bg_color(b, C_FOCUS, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_t *l = ui_label(b, &lv_font_montserrat_16, C_TEXT, k_cat_name[c]);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_add_event_cb(b, on_tab, LV_EVENT_CLICKED, (void *)(intptr_t)c);
    return b;
}

/* ── App page ──────────────────────────────────────────────────────────── */

static void on_detail_back(lv_event_t *ev)
{
    list_show();
}

static void on_get(lv_event_t *ev)
{
    item_t *it = item_by_id(s_detail_id);
    if (!it || it->cat < 0 || it->cat == s_doing || queued(it->cat)) return;
    install_add(it->cat);
    detail_show();
}

static void on_open(lv_event_t *ev)
{
    item_t *it = item_by_id(s_detail_id);
    if (it && it->path[0]) home_launch(it->path);
}

static void confirm_close(void)
{
    if (!s_confirm) return;
    lv_msgbox_close(s_confirm);
    s_confirm = s_confirm_no = s_confirm_yes = NULL;
}

static void on_remove_yes(lv_event_t *ev)
{
    confirm_close();
    item_t *it = item_by_id(s_detail_id);
    char dir[160];
    if (!it || !it->path[0] || !app_dir(it->path, dir, sizeof(dir)) || s_doing >= 0) {
        detail_show();
        return;
    }
    char name[32];
    snprintf(name, sizeof(name), "%s", it->name);
    bool ok = rm_tree(dir, 0);
    ESP_LOGI(TAG, "removed %s: %s", dir, ok ? "ok" : "failed");
    s_changed = true;
    snprintf(s_status_text, sizeof(s_status_text), ok ? "%s removed" : "%s: some files could not be removed", name);
    items_refresh();
    if (item_by_id(s_detail_id)) detail_show();     /* still in the catalogue */
    else list_show();
    if (!ok) ui_message("Remove", "Some of its files could not be removed. Use the Files app to see what is left.");
}

static void on_remove_no(lv_event_t *ev)
{
    confirm_close();
    detail_show();
}

static void on_remove(lv_event_t *ev)
{
    item_t *it = item_by_id(s_detail_id);
    char dir[160], text[300];
    if (!it || !it->path[0]) return;
    if (!app_dir(it->path, dir, sizeof(dir))) {
        ui_message("Remove", "This app is not in an app directory of its own, so the Store leaves it alone.");
        return;
    }
    if (s_doing >= 0) {
        ui_message("Remove", "An app is being installed. Try again when it is done.");
        return;
    }
    snprintf(text, sizeof(text), "This deletes %s and everything in it.\n"
             "ROMs and saves kept elsewhere on the card stay.", dir);
    ui_focus_drop();
    s_confirm = lv_msgbox_create(NULL);
    char title[48];
    snprintf(title, sizeof(title), "Remove %s?", it->name);
    lv_msgbox_add_title(s_confirm, title);
    lv_msgbox_add_text(s_confirm, text);
    lv_obj_set_width(s_confirm, 520);
    s_confirm_no = lv_msgbox_add_footer_button(s_confirm, "Cancel");
    s_confirm_yes = lv_msgbox_add_footer_button(s_confirm, "Remove");
    lv_obj_set_style_bg_color(s_confirm_yes, C_RED, 0);
    lv_obj_add_event_cb(s_confirm_no, on_remove_no, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_confirm_yes, on_remove_yes, LV_EVENT_CLICKED, NULL);
    ui_focus_clear();
    ui_focus_add(s_confirm_no);
    ui_focus_add(s_confirm_yes);
    ui_focus_set(s_confirm_no);
}

static void size_text(long size, char *buf, int len)
{
    if (size >= 1000000) snprintf(buf, len, "%.1f MB", size / 1e6);
    else snprintf(buf, len, "%ld KB", (size + 999) / 1000);
}

/* Fills the app page of s_detail_id; back to the front page when it is gone */
static void detail_show(void)
{
    item_t *it = item_by_id(s_detail_id);
    if (!it) {
        list_show();
        return;
    }
    lv_obj_t *keep = lv_screen_active() == s_dscr ? ui_focus_get() : NULL;
    ui_focus_drop();
    if (lv_screen_active() != s_dscr) lv_screen_load(s_dscr);
    lv_obj_clean(s_dbody);
    s_dget = s_dopen = s_dremove = NULL;
    const lx_appstore_app_t *a = it->cat >= 0 ? cat_app(it->cat) : NULL;
    int w = 752 + ui_dx();

    lv_obj_t *ic = ui_icon(s_dbody, it->icon, it->id[0] ? it->id : it->name, it->name, 96);
    lv_obj_set_pos(ic, 0, 0);
    lv_obj_t *n = ui_label(s_dbody, &lv_font_montserrat_28, C_TEXT, it->name);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_size(n, w - 120, 36);
    lv_obj_set_pos(n, 116, 4);

    char buf[320], size[16] = "";
    const char *cat = it->category != CAT_ALL ? k_cat_name[it->category] : it->community ? "Community" : "App";
    if (a) {
        size_text(a->size, size, sizeof(size));
        snprintf(buf, sizeof(buf), "%s%s   " LV_SYMBOL_RIGHT "  version %s,  %s", cat,
                 a->community && it->category != CAT_ALL ? ", community" : "", a->version, size);
    } else {
        snprintf(buf, sizeof(buf), "%s   " LV_SYMBOL_RIGHT "  version %s%s", cat, it->version,
                 s_cat_ok ? ",  not in the Store" : "");
    }
    lv_obj_t *m = ui_label(s_dbody, &lv_font_montserrat_16, C_ACCENT, buf);
    lv_obj_set_pos(m, 118, 46);
    int y = 74;
    if (a && a->community) {                    /* shared without source: say so before installing */
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING " By %s. A community app, not reviewed: like any app, "
                 "it can change anything on the card.", a->author[0] ? a->author : "?");
        lv_obj_t *c = ui_label(s_dbody, &lv_font_montserrat_14, C_ORANGE, buf);
        lv_label_set_long_mode(c, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(c, w - 120);
        lv_obj_set_pos(c, 118, y);
    }

    lv_obj_t *d = ui_label(s_dbody, &lv_font_montserrat_20, C_TEXT,
                           a && a->description[0] ? a->description :
                           a ? "No description." :
                           !s_cat_ok ? "The app list is not loaded yet (it needs Wi-Fi), so updates for this app are not known." :
                           "This app was copied to the card by hand: the Store has no "
                           "version of it, so it can only be opened or removed here.");
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(d, w);
    lv_obj_set_pos(d, 0, 124);

    if (it->path[0]) {
        char dir[160];
        snprintf(dir, sizeof(dir), "%s", it->path);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        snprintf(buf, sizeof(buf), LV_SYMBOL_SD_CARD "  Installed: version %s, in %s", it->version, dir);
    } else if (a) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_SD_CARD "  Not installed. It goes to %s.",
                 a->folder[0] ? a->folder : "the main page");
    }
    lv_obj_t *inst = ui_label(s_dbody, &lv_font_montserrat_16, C_DIM, buf);
    lv_label_set_long_mode(inst, LV_LABEL_LONG_DOT);
    lv_obj_set_size(inst, w, 22);
    lv_obj_set_pos(inst, 0, 250 + ui_dy() / 2);

    /* Buttons along the bottom of the page */
    int by = 334 + ui_dy();
    int bx = 0;
    char state[24];
    item_state(it, state, sizeof(state));
    bool busy = it->cat >= 0 && (it->cat == s_doing || queued(it->cat));
    if (a && !busy) {
        const char *verb = !it->path[0] ? LV_SYMBOL_DOWNLOAD " Install" :
                           has_update(it) ? LV_SYMBOL_REFRESH " Update" : LV_SYMBOL_REFRESH " Reinstall";
        if (has_update(it)) {
            snprintf(buf, sizeof(buf), "%s to %s", verb, a->version);
            verb = buf;
        }
        s_dget = ui_button(s_dbody, bx, by, 260, verb, on_get);
        bx += 276;
    }
    if (it->path[0] && !busy) {
        s_dopen = ui_button(s_dbody, bx, by, 150, LV_SYMBOL_PLAY " Open", on_open);
        bx += 166;
        s_dremove = ui_button(s_dbody, bx, by, 160, LV_SYMBOL_TRASH " Remove", on_remove);
        lv_obj_set_style_bg_color(s_dremove, C_RED, 0);
        bx += 176;
    }
    s_dstate = ui_label(s_dbody, &lv_font_montserrat_20, C_ACCENT, "");
    lv_obj_set_pos(s_dstate, bx + 6, by + 10);
    if (it->cat >= 0 && it->cat == s_doing) {
        snprintf(buf, sizeof(buf), "Installing... %s", state);
        lv_label_set_text(s_dstate, buf);
    } else if (it->cat >= 0 && queued(it->cat)) {
        lv_label_set_text(s_dstate, "Waiting to install");
    }

    ui_focus_clear();
    ui_focus_add(s_dback);
    if (s_dget) ui_focus_add(s_dget);
    if (s_dopen) ui_focus_add(s_dopen);
    if (s_dremove) ui_focus_add(s_dremove);
    /* The buttons are new objects: Back stays focused when it was chosen over them;
     * after an install (no buttons meanwhile) the focus goes to Open */
    static bool had_buttons;
    lv_obj_t *to = s_dget && (!it->path[0] || has_update(it)) ? s_dget : s_dopen ? s_dopen : s_dget ? s_dget : s_dback;
    if (keep == s_dback && had_buttons) to = s_dback;
    had_buttons = s_dget || s_dopen;
    ui_focus_set(to);
}

/* ── Building, ticking ─────────────────────────────────────────────────── */

void store_build(void)
{
    int dx = ui_dx(), dy = ui_dy();
    s_scr = ui_screen();
    s_back = ui_button(s_scr, 16, 14, 56, LV_SYMBOL_LEFT, on_back);
    lv_obj_t *t = ui_label(s_scr, &lv_font_montserrat_28, C_TEXT, "Store");
    lv_obj_set_pos(t, 92, 20);
    s_status = ui_label(s_scr, &lv_font_montserrat_16, C_DIM, "");
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_status, 290 + dx, 20);
    lv_obj_set_pos(s_status, 196, 28);
    s_all = ui_button(s_scr, 500 + dx, 14, 284, "", on_all);

    lv_obj_t *rail = ui_panel(s_scr, 16, 72, 176, 396 + dy, 14);
    lv_obj_set_style_pad_all(rail, 6, 0);
    int step = dy >= 40 ? 50 : 46;      /* 8 rows fit the 480-high screen too */
    for (int c = 0; c < CAT_N; c++)     /* a small gap above Community and above Installed */
        s_rail[c] = rail_button(rail, c * step + (c >= CAT_COMMUNITY) * 4 + (c >= CAT_INSTALLED) * 4, c);

    s_list = lv_obj_create(s_scr);
    lv_obj_set_pos(s_list, 202, 72);
    lv_obj_set_size(s_list, 582 + dx, 396 + dy);
    lv_obj_set_style_bg_color(s_list, C_PANEL, 0);
    lv_obj_set_style_border_color(s_list, C_BORDER, 0);
    lv_obj_set_style_radius(s_list, 14, 0);
    lv_obj_set_style_pad_all(s_list, 8, 0);
    lv_obj_set_style_pad_row(s_list, 4, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    s_dscr = ui_screen();
    s_dback = ui_button(s_dscr, 16, 14, 56, LV_SYMBOL_LEFT, on_detail_back);
    lv_obj_t *dt = ui_label(s_dscr, &lv_font_montserrat_28, C_TEXT, "Store");
    lv_obj_set_pos(dt, 92, 20);
    s_dbody = lv_obj_create(s_dscr);
    lv_obj_remove_style_all(s_dbody);
    lv_obj_set_pos(s_dbody, 24, 80);
    lv_obj_set_size(s_dbody, 752 + dx, 386 + dy);
    lv_obj_remove_flag(s_dbody, LV_OBJ_FLAG_SCROLLABLE);
}

void store_open(const char *id)
{
    bool online = lx_net_state() == LX_NET_CONNECTED;
    if (online && idle()) fetch();      /* every time: an app published a minute ago shows up */
    else if (!online && idle() && !s_cat_ok)
        snprintf(s_status_text, sizeof(s_status_text), LV_SYMBOL_WARNING " Offline: apps on the card only");
    items_refresh();
    if (!s_cat_ok && !s_fetching && !id) s_tab = CAT_INSTALLED;
    if (id && item_by_id(id)) {
        snprintf(s_detail_id, sizeof(s_detail_id), "%s", id);
        detail_show();
        return;
    }
    ui_focus_drop();                    /* the focus starts on the chosen category */
    lv_screen_load(s_scr);
    list_show();
}

bool store_back(void)
{
    if (s_confirm) {
        confirm_close();
        detail_show();
        return true;
    }
    if (lv_screen_active() == s_dscr) {
        list_show();
        return true;
    }
    if (lv_screen_active() == s_scr) {
        show_home();
        return true;
    }
    return false;
}

bool store_focus(void)
{
    if (lv_screen_active() == s_dscr) detail_show();
    else if (lv_screen_active() == s_scr) list_show();
    else return false;
    return true;
}

bool store_installing(void)     { return s_doing >= 0 || s_nqueue; }
int store_updates(void)         { return s_updates; }

bool store_take_changed(void)
{
    bool c = s_changed;
    s_changed = false;
    return c;
}

/* A visible Store screen shows the new state */
static void redraw(void)
{
    if (s_confirm) return;
    if (lv_screen_active() == s_dscr) detail_show();
    else if (lv_screen_active() == s_scr) list_show();
}

/* Progress without rebuilding the rows */
static void show_percent(int pct)
{
    char buf[48];
    const lx_appstore_app_t *a = cat_app(s_doing);
    if (!a) return;
    snprintf(s_status_text, sizeof(s_status_text), "Installing %s... %d%%", a->name, pct);
    if (lv_screen_active() == s_scr) {
        lv_label_set_text(s_status, s_status_text);
        for (int k = 0; k < s_nrows; k++) {
            if (strcmp(s_row_id[k], a->id) != 0) continue;
            snprintf(buf, sizeof(buf), "%d%%", pct);
            lv_label_set_text(s_row_state[k], buf);
        }
    } else if (lv_screen_active() == s_dscr && s_dstate && strcmp(s_detail_id, a->id) == 0) {
        snprintf(buf, sizeof(buf), "Installing... %d%%", pct);
        lv_label_set_text(s_dstate, buf);
    }
}

void store_tick(void)
{
    static int shown = -1;
    static bool tried;
    if (!tried && lx_net_state() == LX_NET_CONNECTED && idle()) {
        tried = true;                   /* once Wi-Fi is up: the update count for the tile */
        fetch();
    }
    lx_appstore_state_t st = lx_appstore_state();
    if (st == LX_APPSTORE_BUSY) {
        int pct = lx_appstore_percent();
        if (pct == shown) return;
        shown = pct;
        if (s_doing >= 0) {
            show_percent(pct);
        } else if (s_fetching && pct > 0 && lv_screen_active() == s_scr) {
            snprintf(s_status_text, sizeof(s_status_text), "Getting the app icons... %d%%", pct);
            lv_label_set_text(s_status, s_status_text);
        }
        return;
    }
    if (st == LX_APPSTORE_IDLE) return;
    char err[112];
    snprintf(err, sizeof(err), "%s", lx_appstore_error());
    lx_appstore_reset();
    shown = -1;
    if (s_fetching) {
        s_fetching = false;
        if (st == LX_APPSTORE_DONE) {
            if (!s_cat) s_cat = heap_caps_calloc(LX_APPSTORE_MAX, sizeof(lx_appstore_app_t), MALLOC_CAP_SPIRAM);
            if (!s_cat) abort();
            s_ncat = lx_appstore_count();
            for (int i = 0; i < s_ncat; i++) s_cat[i] = *lx_appstore_app(i);
            s_cat_ok = true;
            bool pads = lx_appstore_gamepads_updated();
            if (pads) lx_input_pad_reload_maps();
            items_refresh();
            snprintf(s_status_text, sizeof(s_status_text), "%d apps online%s", s_ncat,
                     pads ? ", controller database updated" : "");
        } else {
            snprintf(s_status_text, sizeof(s_status_text), LV_SYMBOL_WARNING " %s", err);
            items_refresh();
        }
        ESP_LOGI(TAG, "catalogue %s: %d apps, %d updates", st == LX_APPSTORE_DONE ? "read" : "failed",
                 s_ncat, s_updates);
        redraw();
        return;
    }
    if (s_doing < 0) return;
    const lx_appstore_app_t *a = cat_app(s_doing);
    s_doing = -1;
    if (st == LX_APPSTORE_DONE) {
        s_changed = true;
        snprintf(s_status_text, sizeof(s_status_text), "%s %s installed", a->name, a->version);
    } else {
        s_nqueue = 0;                   /* the rest would likely fail too */
        snprintf(s_status_text, sizeof(s_status_text), LV_SYMBOL_WARNING " %s", err);
    }
    items_refresh();
    install_next();
    redraw();
}
