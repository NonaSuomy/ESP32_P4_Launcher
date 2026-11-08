/*
 * The ESP32-C6 companion's own firmware (esp_hosted "network_adapter"): its version, and an update
 * from a file on the SD card through esp_hosted's OTA calls. Each board pins its own esp_hosted
 * version (lx_net's CMakeLists passes it as LX_HOSTED_VERSION); the C6 should run the same one.
 * Called on the lx_net task only (internal stack, serialized with the Wi-Fi calls).
 */
#include "lx_c6.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if LX_HOSTED_MAJOR >= 2                         /* 1.4 (Tab5, Waveshare) has neither call */
#include "esp_hosted.h"
#define HAVE_VERSION 1
#if LX_HOSTED_MAJOR > 2 || LX_HOSTED_MINOR >= 6  /* 2.6 and later: public OTA calls */
#include "esp_hosted_ota.h"
#define OTA_BEGIN()        esp_hosted_slave_ota_begin()
#define OTA_WRITE(p, n)    esp_hosted_slave_ota_write(p, n)
#define OTA_END()          esp_hosted_slave_ota_end()
#define HAVE_ACTIVATE 1
#else                                            /* 2.0: the RPC wrappers behind esp_hosted_slave_ota() */
int rpc_ota_begin(void);
int rpc_ota_write(uint8_t *ota_data, uint32_t ota_data_len);
int rpc_ota_end(void);
#define OTA_BEGIN()        rpc_ota_begin()
#define OTA_WRITE(p, n)    rpc_ota_write(p, n)
#define OTA_END()          rpc_ota_end()
#endif
#endif

static const char *TAG = "lx_c6";

#define CHUNK       1400                /* what esp_hosted's own OTA sends per RPC */
#define MAX_IMAGE   (1536 * 1024)       /* one app slot in the C6's partition table */
#define CHIP_C6     13                  /* esp_chip_id_t ESP_CHIP_ID_ESP32C6 */

static char s_version[16];              /* "" = not asked yet, "?" = it didn't answer */
static int s_major = -1, s_minor = -1;
static volatile int s_percent = -1;
static char s_error[96];

const char *lx_c6_version(void)        { return s_version; }
const char *lx_c6_host_version(void)   { return LX_HOSTED_VERSION; }
int lx_c6_update_percent(void)         { return s_percent; }
const char *lx_c6_update_error(void)   { return s_error; }

bool lx_c6_update_supported(void)
{
#ifdef OTA_BEGIN
    return true;
#else
    return false;
#endif
}

void lx_c6_query(void)
{
#ifdef HAVE_VERSION
    esp_hosted_coprocessor_fwver_t v = { 0 };
    if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK) {
        snprintf(s_version, sizeof(s_version), "%u.%u.%u", (unsigned)v.major1, (unsigned)v.minor1, (unsigned)v.patch1);
        s_major = (int)v.major1;
        s_minor = (int)v.minor1;
        ESP_LOGI(TAG, "C6 firmware %s, launcher speaks esp_hosted %s", s_version, LX_HOSTED_VERSION);
        return;
    }
#endif
    snprintf(s_version, sizeof(s_version), "?");
    ESP_LOGW(TAG, "C6 firmware version unknown (older firmware, or not answering)");
}

/* major.minor of a "2.12.13" string */
static bool major_minor(const char *s, int *major, int *minor)
{
    return sscanf(s, "%d.%d", major, minor) == 2;
}

bool lx_c6_check_image(const char *path, char *version, size_t vlen, char *why, size_t wlen)
{
    version[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(why, wlen, "No file %s", path);
        return false;
    }
    uint8_t h[24 + 8 + 256];            /* image header, first segment header, esp_app_desc_t */
    size_t n = fread(h, 1, sizeof(h), f);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    const uint8_t *d = h + 32;
    uint32_t magic = d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24;
    if (n < sizeof(h) || h[0] != 0xE9 || magic != 0xABCD5432) {
        snprintf(why, wlen, "Not an ESP firmware image");
        return false;
    }
    if ((h[12] | h[13] << 8) != CHIP_C6) {
        snprintf(why, wlen, "The image is not for an ESP32-C6");
        return false;
    }
    char project[33];
    snprintf(version, vlen, "%.32s", (const char *)d + 16);
    snprintf(project, sizeof(project), "%.32s", (const char *)d + 48);
    if (strcmp(project, "network_adapter") != 0) {
        snprintf(why, wlen, "Not esp_hosted firmware (%s)", project);
        return false;
    }
    int a, b, c, e;
    if (!major_minor(version, &a, &b) || !major_minor(LX_HOSTED_VERSION, &c, &e) || a != c || b != e) {
        snprintf(why, wlen, "Firmware %s doesn't match this launcher (%s)", version, LX_HOSTED_VERSION);
        return false;
    }
    if (size <= 0 || size > MAX_IMAGE) {
        snprintf(why, wlen, "The image is too big (%ld KB)", size / 1024);
        return false;
    }
    why[0] = 0;
    return true;
}

static void fail(const char *fmt, esp_err_t e)
{
    snprintf(s_error, sizeof(s_error), fmt, esp_err_to_name(e));
    ESP_LOGE(TAG, "%s", s_error);
    s_percent = -1;
}

void lx_c6_update(const char *path)
{
    s_error[0] = 0;
#ifndef OTA_BEGIN
    snprintf(s_error, sizeof(s_error), "This board can't update its Wi-Fi chip");
    (void)path;
    return;
#else
    char version[33];
    if (!lx_c6_check_image(path, version, sizeof(version), s_error, sizeof(s_error))) return;
    if (!s_version[0] || s_version[0] == '?') lx_c6_query();
    int old_major = s_major, old_minor = s_minor;
    FILE *f = fopen(path, "rb");
    uint8_t *buf = malloc(CHUNK);
    if (!f || !buf) {
        if (f) fclose(f);
        free(buf);
        snprintf(s_error, sizeof(s_error), "Could not read %s", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f), done = 0;
    fseek(f, 0, SEEK_SET);
    ESP_LOGW(TAG, "updating the C6 from %s to %s (%ld bytes)", s_version, version, size);
    s_percent = 0;
    esp_err_t e = OTA_BEGIN();
    if (e != ESP_OK) {
        fclose(f);
        free(buf);
        fail("The Wi-Fi chip refused the update (%s)", e);
        return;
    }
    size_t n;
    while ((n = fread(buf, 1, CHUNK, f)) > 0) {
        e = OTA_WRITE(buf, n);
        if (e != ESP_OK) break;
        done += (long)n;
        s_percent = (int)(done * 99 / size);
    }
    fclose(f);
    free(buf);
    if (e != ESP_OK || done != size) {
        OTA_END();                      /* closes the session; the C6 keeps its old firmware */
        fail("Sending the firmware failed (%s)", e != ESP_OK ? e : ESP_FAIL);
        return;
    }
    e = OTA_END();                      /* the C6 checks the image here */
    if (e != ESP_OK) {
        fail("The Wi-Fi chip rejected the firmware (%s)", e);
        return;
    }
#ifdef HAVE_ACTIVATE
    /* 2.6 and later firmware waits for activate; older firmware restarts itself after OTA end */
    if (old_major > 2 || (old_major == 2 && old_minor > 5)) {
        e = esp_hosted_slave_ota_activate();
        if (e != ESP_OK) ESP_LOGW(TAG, "activate: %s", esp_err_to_name(e));
    }
#else
    (void)old_major;
    (void)old_minor;
#endif
    s_percent = 100;
    ESP_LOGW(TAG, "C6 firmware %s installed, restarting", version);
    vTaskDelay(pdMS_TO_TICKS(6000));    /* the C6 restarts into it; the board restarts to reconnect */
    esp_restart();
#endif
}
