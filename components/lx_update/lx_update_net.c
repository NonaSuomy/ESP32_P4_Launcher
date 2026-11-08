/*
 * Online update: reads latest.json from the newest GitHub release, downloads
 * launcher.bin to the SD card and checks its SHA-256. Installing is then the same
 * as an update from the card, with the same rollback.
 *
 * latest.json: {"version":"0.19.0","url":"https://...","size":1347392,"sha256":"<hex>",
 *               "boards":{"<board id>":{"url":...,"size":...,"sha256":...,
 *                                       "c6":{"version":"2.12.13","url":...,"size":...,"sha256":...}}, ...}}
 * "c6" is the Wi-Fi chip firmware that matches the board's launcher (optional).
 * Each board takes its own entry. The top-level fields are the first board's
 * (guition_p4_43), where launchers from before per-board releases look.
 *
 * A /sd/update/ota_url.txt holding one URL replaces the release address, to test
 * against another server.
 */
#include "lx_update.h"
#include "lx_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "bsp.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static const char *TAG = "lx_update";

#define MANIFEST_URL  "https://github.com/giltal/ESP32_P4_Launcher/releases/latest/download/latest.json"
#define URL_OVERRIDE  "/sd/update/ota_url.txt"
#define TMP_FILE      "/sd/update/launcher.tmp"
#define MANIFEST_MAX  4096
#define CHUNK         4096

static volatile lx_online_state_t s_state;
static volatile int s_percent;
static char s_version[32], s_error[80], s_sha[65];
static char s_url[256];
static long s_size;
static char s_why[40];
static char s_c6_version[16], s_c6_url[256], s_c6_sha[65];
static long s_c6_size;
const char *lx_update_online_c6_version(void)  { return s_c6_version; }

lx_online_state_t lx_update_online_state(void) { return s_state; }
void lx_update_online_reset(void)              { s_state = LX_ONLINE_IDLE; }
int lx_update_online_percent(void)             { return s_percent; }
const char *lx_update_online_version(void)     { return s_version; }
const char *lx_update_online_error(void)       { return s_error; }

static void fail(const char *text)
{
    snprintf(s_error, sizeof(s_error), "%s", text);
    ESP_LOGW(TAG, "online update: %s", text);
    s_state = LX_ONLINE_FAILED;
}

static void check_task(void *arg)
{
    char url[256];
    lx_http_url(url, sizeof(url), URL_OVERRIDE, MANIFEST_URL);
    char *body = NULL;
    int status = lx_http_get_text(url, MANIFEST_MAX, &body, s_why, sizeof(s_why));
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    /* This board's entry; the top level only stands for the first board */
    const char *id = bsp_info()->id;
    cJSON *boards = cJSON_GetObjectItem(j, "boards"), *mine = cJSON_GetObjectItem(boards, id);
    bool legacy_ok = !boards && strcmp(id, "guition_p4_43") == 0;
    cJSON *e = mine ? mine : legacy_ok ? j : NULL;
    cJSON *v = cJSON_GetObjectItem(e, "version"), *u = cJSON_GetObjectItem(e, "url");
    cJSON *s = cJSON_GetObjectItem(e, "size"), *h = cJSON_GetObjectItem(e, "sha256");
    if (!v) v = cJSON_GetObjectItem(j, "version");
    s_c6_version[0] = 0;
    cJSON *c6 = cJSON_GetObjectItem(mine, "c6");
    cJSON *cv = cJSON_GetObjectItem(c6, "version"), *cu = cJSON_GetObjectItem(c6, "url");
    cJSON *cs = cJSON_GetObjectItem(c6, "size"), *ch = cJSON_GetObjectItem(c6, "sha256");
    if (cJSON_IsString(cv) && cJSON_IsString(cu) && cJSON_IsNumber(cs) && cJSON_IsString(ch) &&
        strlen(ch->valuestring) == 64) {
        snprintf(s_c6_version, sizeof(s_c6_version), "%s", cv->valuestring);
        snprintf(s_c6_url, sizeof(s_c6_url), "%s", cu->valuestring);
        snprintf(s_c6_sha, sizeof(s_c6_sha), "%s", ch->valuestring);
        s_c6_size = (long)cs->valuedouble;
    }
    if (status == 404) {
        fail("No release has been published yet.");
    } else if (j && !e) {
        fail("No release for this board yet.");
    } else if (status < 0) {
        char text[sizeof(s_error)];
        snprintf(text, sizeof(text), "Could not reach the update server (%s).", s_why);
        fail(text);
    } else if (!cJSON_IsString(v) || !cJSON_IsString(u) || !cJSON_IsNumber(s) || !cJSON_IsString(h) ||
               strlen(h->valuestring) != 64) {
        fail("The release information is not valid.");
    } else {
        snprintf(s_version, sizeof(s_version), "%s", v->valuestring);
        snprintf(s_url, sizeof(s_url), "%s", u->valuestring);
        snprintf(s_sha, sizeof(s_sha), "%s", h->valuestring);
        s_size = (long)s->valuedouble;
        s_state = lx_version_newer(s_version, esp_app_get_description()->version) ? LX_ONLINE_AVAILABLE
                                                                                  : LX_ONLINE_LATEST;
    }
    cJSON_Delete(j);
    vTaskDelete(NULL);
}

/* arg != NULL: the Wi-Fi chip firmware instead of the launcher */
static void download_task(void *arg)
{
    bool c6 = arg != NULL;
    const char *url = c6 ? s_c6_url : s_url, *want = c6 ? s_c6_sha : s_sha;
    const char *dest = c6 ? LX_UPDATE_C6_FILE : LX_UPDATE_FILE;
    long size = c6 ? s_c6_size : s_size;
    mkdir("/sd/update", 0775);
    uint8_t *buf = malloc(CHUNK);
    FILE *f = fopen(TMP_FILE, "wb");
    esp_http_client_handle_t c = lx_http_new(url);
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    long got = 0;
    bool ok = buf && f && c && lx_http_open(c, s_why, sizeof(s_why)) == 200;
    while (ok) {
        int n = esp_http_client_read(c, (char *)buf, CHUNK);
        if (n < 0) ok = false;
        if (n <= 0) break;
        mbedtls_sha256_update(&sha, buf, n);
        ok = fwrite(buf, 1, n, f) == (size_t)n;
        got += n;
        s_percent = size ? (int)(got * 100 / size) : 0;
    }
    uint8_t hash[32];
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);
    if (f) fclose(f);
    free(buf);
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    char hex[65];
    for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", hash[i]);
    if (!ok || got != size) {
        remove(TMP_FILE);
        fail("The download did not finish.");
    } else if (strcasecmp(hex, want) != 0) {
        remove(TMP_FILE);
        fail("The download is damaged (SHA-256 mismatch).");
    } else {
        remove(dest);
        rename(TMP_FILE, dest);
        s_state = c6 ? LX_ONLINE_C6_READY : LX_ONLINE_READY;
    }
    vTaskDelete(NULL);
}

void lx_update_online_check(void)
{
    s_state = LX_ONLINE_BUSY;
    if (xTaskCreate(check_task, "lx_update", 10240, NULL, 3, NULL) != pdPASS) fail("Out of memory.");
}

void lx_update_online_download(void)
{
    s_percent = 0;
    s_state = LX_ONLINE_BUSY;
    if (xTaskCreate(download_task, "lx_update", 10240, NULL, 3, NULL) != pdPASS) fail("Out of memory.");
}

void lx_update_online_download_c6(void)
{
    s_percent = 0;
    s_state = LX_ONLINE_BUSY;
    if (xTaskCreate(download_task, "lx_update", 10240, (void *)1, 3, NULL) != pdPASS) fail("Out of memory.");
}
