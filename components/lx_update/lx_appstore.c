/*
 * Online app catalogue: reads apps.json from the "apps" release and installs an
 * app (its .papp, checked against the SHA-256 in the catalogue, and its icon) on
 * the SD card. The new file is written beside the old one and only replaces it
 * once complete, so a failed download leaves the installed app as it was.
 *
 * apps.json: {"format":1, "folders":{"Emulators":{"icon":url}, ...},
 *             "apps":[{"id","name","version","folder","dir","file","url","size","sha256","icon"}],
 *             "gamepads":{"url","size","sha256"}, "gamenames":{"url","size","sha256"},
 *             "codecs":{"h264":{"url","size","sha256","version"}, ...},
 *             "community":[{the app fields, "author","description","license","source"}]}
 * gamepads is the controller mapping database (SDcard/config/gamepads.txt in the
 * repository): fetching the catalogue also brings the card's copy up to date.
 * gamenames is the Neo Geo game name list (SDcard/roms/neogeo/gamenames.txt), the
 * same way, on cards that have a /sd/roms/neogeo folder.
 * community lists apps shared without their source (community/ in the repository),
 * not reviewed: they come after the project's apps and always go to the Community folder.
 * codecs are the CODC modules (/sd/launcher/codecs/<name>.papp): an app's "needs":["h264"]
 * installs them with it, and reading the catalogue updates the ones already on the card.
 * category and description (launcher 0.26+) are what the Store shows. The apps' icons
 * are kept in /sd/launcher/store/<id>.png for the Store's list.
 */
#include "lx_appstore.h"
#include "esp_attr.h"     /* EXT_RAM_BSS_ATTR */
#include "lx_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static const char *TAG = "lx_appstore";

#define CATALOGUE_URL "https://github.com/giltal/ESP32_P4_Launcher/releases/download/apps/apps.json"
#define URL_OVERRIDE  "/sd/update/apps_url.txt"
#define CATALOGUE_MAX (64 * 1024)
#define CHUNK         8192
#define MAX_FOLDERS   8
#define GAMEPADS_FILE "/sd/config/gamepads.txt"
#define NEOGEO_DIR    "/sd/roms/neogeo"
#define GAMENAMES_FILE NEOGEO_DIR "/gamenames.txt"
#define ICON_DIR      "/sd/launcher/store"
#define MAX_CODECS    4

static volatile lx_appstore_state_t s_state;
static volatile int s_percent;
static char s_error[96], s_why[40];
static lx_appstore_app_t *s_apps;
static int s_count;
static EXT_RAM_BSS_ATTR struct { char name[24]; char icon[192]; } s_folders[MAX_FOLDERS];
static int s_nfolders;
static volatile bool s_pads_updated;
static EXT_RAM_BSS_ATTR struct { char name[16], url[192], sha256[65]; long size; } s_codecs[MAX_CODECS];
static int s_ncodecs;

static struct {
    int index;
    char papp[160], icon[160], folder_icon[160];
} s_job;

lx_appstore_state_t lx_appstore_state(void) { return s_state; }
void lx_appstore_reset(void)                 { s_state = LX_APPSTORE_IDLE; }
int lx_appstore_percent(void)                { return s_percent; }
const char *lx_appstore_error(void)          { return s_error; }
int lx_appstore_count(void)                  { return s_count; }
bool lx_appstore_newer(const char *a, const char *b) { return lx_version_newer(a, b); }
bool lx_appstore_gamepads_updated(void)      { return s_pads_updated; }

const lx_appstore_app_t *lx_appstore_app(int i)
{
    return i >= 0 && i < s_count ? &s_apps[i] : NULL;
}

static void fail(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text);
    ESP_LOGW(TAG, "%s", text);
    s_state = LX_APPSTORE_FAILED;
}

static void copy_str(char *dst, size_t len, cJSON *o, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(o, key);
    snprintf(dst, len, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static bool download(const char *url, const char *path, const char *sha, long size, bool progress);

/* SHA-256 (hex) of a file, "" if there is none */
static void file_sha(const char *path, char hex[65])
{
    hex[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    uint8_t *buf = heap_caps_aligned_alloc(64, CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), hash[32];
    if (!buf) {                         /* "" = download it again */
        fclose(f);
        return;
    }
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    size_t n;
    while ((n = fread(buf, 1, CHUNK, f)) > 0) mbedtls_sha256_update(&ctx, buf, n);   /* aligned: no sector-at-a-time reads */
    fclose(f);
    free(buf);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
}

/* A data file on the card (catalogue entry {"url","size","sha256"}), replaced when
 * the catalogue has another one; true if it was replaced */
static bool data_sync(cJSON *g, const char *path, const char *what)
{
    char url[192], sha[65], have[65];
    copy_str(url, sizeof(url), g, "url");
    copy_str(sha, sizeof(sha), g, "sha256");
    cJSON *size = cJSON_GetObjectItem(g, "size");
    if (!url[0] || strlen(sha) != 64 || !cJSON_IsNumber(size)) return false;
    file_sha(path, have);
    if (strcasecmp(have, sha) == 0) return false;
    bool ok = download(url, path, sha, (long)size->valuedouble, false);
    ESP_LOGI(TAG, "%s %s", what, ok ? "updated" : "did not download");
    return ok;
}

static bool codec_name_ok(const char *n)
{
    if (!n[0] || strlen(n) >= sizeof(s_codecs[0].name)) return false;
    for (; *n; n++)
        if (!((*n >= 'a' && *n <= 'z') || (*n >= '0' && *n <= '9') || *n == '_')) return false;
    return true;
}

/* The catalogue's codec modules into s_codecs */
static void parse_codecs(cJSON *j)
{
    s_ncodecs = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(j, "codecs")) {
        if (s_ncodecs >= MAX_CODECS || !m->string || !codec_name_ok(m->string)) continue;
        cJSON *size = cJSON_GetObjectItem(m, "size");
        snprintf(s_codecs[s_ncodecs].name, sizeof(s_codecs[0].name), "%s", m->string);
        copy_str(s_codecs[s_ncodecs].url, sizeof(s_codecs[0].url), m, "url");
        copy_str(s_codecs[s_ncodecs].sha256, sizeof(s_codecs[0].sha256), m, "sha256");
        s_codecs[s_ncodecs].size = cJSON_IsNumber(size) ? (long)size->valuedouble : 0;
        if (s_codecs[s_ncodecs].url[0] && strlen(s_codecs[s_ncodecs].sha256) == 64 && s_codecs[s_ncodecs].size > 0)
            s_ncodecs++;
    }
}

/* Codec module i onto the card if it isn't there or differs; only_update: leave a missing one out.
 * True when the card's copy is the catalogue's */
static bool codec_sync(int i, bool only_update, bool progress)
{
    char path[80], have[65];
    struct stat st;
    snprintf(path, sizeof(path), LX_APPSTORE_CODEC_DIR "/%s.papp", s_codecs[i].name);
    bool there = stat(path, &st) == 0;
    if (only_update && !there) return true;
    if (there && st.st_size == s_codecs[i].size) {
        file_sha(path, have);
        if (strcasecmp(have, s_codecs[i].sha256) == 0) return true;
    }
    bool ok = download(s_codecs[i].url, path, s_codecs[i].sha256, s_codecs[i].size, progress);
    ESP_LOGI(TAG, "codec %s %s", s_codecs[i].name, ok ? "installed" : "did not download");
    return ok;
}

long lx_appstore_codec_bytes(const lx_appstore_app_t *a)
{
    long bytes = 0;
    char needs[sizeof(a->needs)], path[80];
    struct stat st;
    snprintf(needs, sizeof(needs), "%s", a ? a->needs : "");
    for (char *save, *n = strtok_r(needs, ",", &save); n; n = strtok_r(NULL, ",", &save))
        for (int i = 0; i < s_ncodecs; i++) {
            if (strcmp(s_codecs[i].name, n) != 0) continue;
            snprintf(path, sizeof(path), LX_APPSTORE_CODEC_DIR "/%s.papp", n);
            if (stat(path, &st) != 0 || st.st_size != s_codecs[i].size) bytes += s_codecs[i].size;
        }
    return bytes;
}

/* The controller database, and the Neo Geo names where there is a Neo Geo folder; codec
 * modules already on the card are brought up to date */
static void data_files_sync(cJSON *j)
{
    parse_codecs(j);
    for (int i = 0; i < s_ncodecs; i++) codec_sync(i, true, false);
    s_pads_updated = data_sync(cJSON_GetObjectItem(j, "gamepads"), GAMEPADS_FILE, "gamepad database");
    struct stat st;
    if (stat(NEOGEO_DIR, &st) == 0 && S_ISDIR(st.st_mode))
        data_sync(cJSON_GetObjectItem(j, "gamenames"), GAMENAMES_FILE, "Neo Geo game names");
}

/* One catalogue entry into s_apps; false if it is incomplete or names a path outside /sd/apps */
static bool parse_app(cJSON *a, bool community)
{
    if (s_count >= LX_APPSTORE_MAX) return false;
    lx_appstore_app_t *e = &s_apps[s_count];
    memset(e, 0, sizeof(*e));
    copy_str(e->id, sizeof(e->id), a, "id");
    copy_str(e->name, sizeof(e->name), a, "name");
    copy_str(e->version, sizeof(e->version), a, "version");
    copy_str(e->folder, sizeof(e->folder), a, "folder");
    copy_str(e->dir, sizeof(e->dir), a, "dir");
    copy_str(e->file, sizeof(e->file), a, "file");
    copy_str(e->url, sizeof(e->url), a, "url");
    copy_str(e->icon, sizeof(e->icon), a, "icon");
    copy_str(e->sha256, sizeof(e->sha256), a, "sha256");
    copy_str(e->description, sizeof(e->description), a, "description");
    copy_str(e->category, sizeof(e->category), a, "category");
    cJSON *need;
    cJSON_ArrayForEach(need, cJSON_GetObjectItem(a, "needs")) {
        if (!cJSON_IsString(need) || !codec_name_ok(need->valuestring) ||
            strlen(e->needs) + strlen(need->valuestring) + 2 > sizeof(e->needs)) continue;
        if (e->needs[0]) strcat(e->needs, ",");
        strcat(e->needs, need->valuestring);
    }
    cJSON *size = cJSON_GetObjectItem(a, "size");
    e->size = cJSON_IsNumber(size) ? (long)size->valuedouble : 0;
    if (community) {
        e->community = true;
        snprintf(e->folder, sizeof(e->folder), "%s", LX_APPSTORE_COMMUNITY);
        copy_str(e->author, sizeof(e->author), a, "author");
    }
    for (int i = 0; i < s_count; i++)           /* the project's app wins over a community one */
        if (strcmp(s_apps[i].id, e->id) == 0) return false;
    bool bad = strstr(e->dir, "..") || strchr(e->dir, '/') || strstr(e->file, "..") ||
               strchr(e->file, '/') || strstr(e->folder, "..") || strchr(e->folder, '/');
    if (!e->id[0] || !e->dir[0] || !e->file[0] || !e->url[0] || strlen(e->sha256) != 64 || e->size <= 0 || bad)
        return false;
    s_count++;
    return true;
}

const char *lx_appstore_icon_path(const lx_appstore_app_t *a, char *buf, int len)
{
    if (!a || !a->icon[0] || strchr(a->id, '/') || strstr(a->id, "..")) snprintf(buf, len, "%s", "");
    else snprintf(buf, len, ICON_DIR "/%s.png", a->id);
    return buf;
}

/* Icons not on the card yet; a failed one is tried again next time */
static void icons_sync(void)
{
    int want = 0, done = 0;
    char path[96];
    struct stat st;
    for (int i = 0; i < s_count; i++)
        want += lx_appstore_icon_path(&s_apps[i], path, sizeof(path))[0] && stat(path, &st) != 0;
    for (int i = 0; i < s_count && want; i++) {
        if (!lx_appstore_icon_path(&s_apps[i], path, sizeof(path))[0] || stat(path, &st) == 0) continue;
        download(s_apps[i].icon, path, NULL, 0, false);
        s_percent = ++done * 100 / want;
    }
    if (want) ESP_LOGI(TAG, "%d of %d icons saved", done, want);
}

static void fetch_task(void *arg)
{
    char url[256];
    lx_http_url(url, sizeof(url), URL_OVERRIDE, CATALOGUE_URL);
    char *body = NULL;
    int status = lx_http_get_text(url, CATALOGUE_MAX, &body, s_why, sizeof(s_why));
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    cJSON *apps = cJSON_GetObjectItem(j, "apps");
    if (status == 404) {
        fail("No apps have been published yet.");
    } else if (status < 0) {
        char text[sizeof(s_error)];
        snprintf(text, sizeof(text), "Could not reach the app server (%s).", s_why);
        fail(text);
    } else if (!cJSON_IsArray(apps)) {
        fail("The app list is not valid.");
    } else {
        s_count = 0;
        cJSON *a;
        cJSON_ArrayForEach(a, apps) parse_app(a, false);
        int own = s_count;
        cJSON_ArrayForEach(a, cJSON_GetObjectItem(j, "community")) parse_app(a, true);
        s_nfolders = 0;
        cJSON *f;
        cJSON_ArrayForEach(f, cJSON_GetObjectItem(j, "folders")) {
            if (s_nfolders >= MAX_FOLDERS) break;
            snprintf(s_folders[s_nfolders].name, sizeof(s_folders[0].name), "%s", f->string);
            copy_str(s_folders[s_nfolders].icon, sizeof(s_folders[0].icon), f, "icon");
            s_nfolders++;
        }
        ESP_LOGI(TAG, "catalogue: %d apps, %d community", own, s_count - own);
        data_files_sync(j);
        s_percent = 0;
        icons_sync();
        s_state = LX_APPSTORE_DONE;
    }
    cJSON_Delete(j);
    vTaskDelete(NULL);
}

void lx_appstore_fetch(void)
{
    if (!s_apps) s_apps = heap_caps_calloc(LX_APPSTORE_MAX, sizeof(lx_appstore_app_t), MALLOC_CAP_SPIRAM);
    if (!s_apps) {
        fail("Out of memory.");
        return;
    }
    s_state = LX_APPSTORE_BUSY;
    if (xTaskCreate(fetch_task, "lx_appstore", 10240, NULL, 3, NULL) != pdPASS) fail("Out of memory.");
}

/* Creates the directories above path */
static void make_dirs(const char *path)
{
    char p[160];
    snprintf(p, sizeof(p), "%s", path);
    for (char *s = strchr(p + 1, '/'); s; s = strchr(s + 1, '/')) {
        *s = 0;
        mkdir(p, 0775);
        *s = '/';
    }
}

/* Downloads url to path through path.tmp; sha (hex) and size are checked when given */
static bool download(const char *url, const char *path, const char *sha, long size, bool progress)
{
    char tmp[168];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    make_dirs(path);
    uint8_t *buf = heap_caps_aligned_alloc(64, CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    FILE *f = fopen(tmp, "wb");
    esp_http_client_handle_t c = lx_http_new(url);
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    long got = 0;
    int status = buf && f && c ? lx_http_open(c, s_why, sizeof(s_why)) : -1;
    bool ok = status == 200;
    if (!ok && status > 0) snprintf(s_why, sizeof(s_why), "HTTP %d", status);
    while (ok) {
        int n = esp_http_client_read(c, (char *)buf, CHUNK);
        if (n < 0) {
            ok = false;
            snprintf(s_why, sizeof(s_why), "connection lost");
        }
        if (n <= 0) break;
        mbedtls_sha256_update(&ctx, buf, n);
        if (fwrite(buf, 1, n, f) != (size_t)n) {
            ok = false;
            snprintf(s_why, sizeof(s_why), "SD card write failed");
        }
        got += n;
        if (progress && size) s_percent = (int)(got * 100 / size);
    }
    uint8_t hash[32];
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);
    if (f) fclose(f);
    free(buf);
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
    if (ok && size && got != size) {
        ok = false;
        snprintf(s_why, sizeof(s_why), "incomplete");
    } else if (ok && sha && strcasecmp(hex, sha) != 0) {
        ok = false;
        snprintf(s_why, sizeof(s_why), "damaged (SHA-256)");
    }
    if (!ok) {
        remove(tmp);
        return false;
    }
    remove(path);                       /* FAT rename doesn't replace */
    return rename(tmp, path) == 0;
}

static const char *folder_icon_url(const char *folder)
{
    for (int i = 0; i < s_nfolders; i++)
        if (strcmp(s_folders[i].name, folder) == 0 && s_folders[i].icon[0]) return s_folders[i].icon;
    return NULL;
}

static void install_task(void *arg)
{
    const lx_appstore_app_t *e = &s_apps[s_job.index];
    ESP_LOGI(TAG, "installing %s %s to %s", e->id, e->version, s_job.papp);
    if (!download(e->url, s_job.papp, e->sha256, e->size, true)) {
        char text[sizeof(s_error)];
        snprintf(text, sizeof(text), "%s did not download (%s).", e->name, s_why);
        fail(text);
        vTaskDelete(NULL);
        return;
    }
    if (e->icon[0] && s_job.icon[0]) download(e->icon, s_job.icon, NULL, 0, false);   /* optional */
    struct stat st;
    const char *fi = folder_icon_url(e->folder);
    if (fi && s_job.folder_icon[0] && stat(s_job.folder_icon, &st) != 0)
        download(fi, s_job.folder_icon, NULL, 0, false);
    char needs[sizeof(e->needs)];
    snprintf(needs, sizeof(needs), "%s", e->needs);
    for (char *save, *n = strtok_r(needs, ",", &save); n; n = strtok_r(NULL, ",", &save)) {
        int i = 0;
        while (i < s_ncodecs && strcmp(s_codecs[i].name, n) != 0) i++;
        s_percent = 0;
        if (i == s_ncodecs || !codec_sync(i, false, true)) {
            char text[160];                 /* fail() keeps what fits */
            snprintf(text, sizeof(text), "%s is installed, but its %s codec did not download (%s).", e->name, n,
                     i == s_ncodecs ? "not in the catalogue" : s_why);
            fail(text);
            vTaskDelete(NULL);
            return;
        }
    }
    s_state = LX_APPSTORE_DONE;
    vTaskDelete(NULL);
}

void lx_appstore_install(int i, const char *papp_path, const char *icon_path, const char *folder_icon_path)
{
    if (i < 0 || i >= s_count) {
        fail("No such app.");
        return;
    }
    s_job.index = i;
    snprintf(s_job.papp, sizeof(s_job.papp), "%s", papp_path);
    snprintf(s_job.icon, sizeof(s_job.icon), "%s", icon_path ? icon_path : "");
    snprintf(s_job.folder_icon, sizeof(s_job.folder_icon), "%s", folder_icon_path ? folder_icon_path : "");
    s_percent = 0;
    s_state = LX_APPSTORE_BUSY;
    if (xTaskCreate(install_task, "lx_appstore", 10240, NULL, 3, NULL) != pdPASS) fail("Out of memory.");
}
