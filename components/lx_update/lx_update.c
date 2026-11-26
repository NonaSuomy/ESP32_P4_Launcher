#include "lx_update.h"
#include "lx_common.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs.h"

static const char *TAG = "lx_update";

#define CHUNK 4096

static bool s_rolled_back;

void lx_update_confirm(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "%s confirmed", esp_app_get_description()->version);
    }
    /* The invalid slot stays marked, so report each failed version only once */
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (!bad || esp_ota_get_partition_description(bad, &desc) != ESP_OK) return;
    nvs_handle_t h;
    if (nvs_open("launcher", NVS_READWRITE, &h) != ESP_OK) return;
    char seen[32] = "";
    size_t len = sizeof(seen);
    nvs_get_str(h, "upd_bad", seen, &len);
    if (strcmp(seen, desc.version) != 0) {
        s_rolled_back = true;
        nvs_set_str(h, "upd_bad", desc.version);
        nvs_commit(h);
    }
    nvs_close(h);
}

bool lx_update_rolled_back(void)
{
    return s_rolled_back;
}

/* A launcher image for this chip and this board: the project name carries the board */
static bool image_ok(esp_app_desc_t *desc)
{
    FILE *f = fopen(LX_UPDATE_FILE, "rb");
    if (!f) return false;
    esp_image_header_t img;
    esp_image_segment_header_t seg;
    bool ok = fread(&img, 1, sizeof(img), f) == sizeof(img) &&
              fread(&seg, 1, sizeof(seg), f) == sizeof(seg) &&
              fread(desc, 1, sizeof(*desc), f) == sizeof(*desc);
    fclose(f);
    if (!ok || img.magic != ESP_IMAGE_HEADER_MAGIC || img.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID ||
        desc->magic_word != ESP_APP_DESC_MAGIC_WORD ||
        strcmp(desc->project_name, esp_app_get_description()->project_name) != 0) {
        ESP_LOGW(TAG, "%s is not a launcher image for this chip and board", LX_UPDATE_FILE);
        return false;
    }
    return true;
}

bool lx_update_available(char *version, size_t len)
{
    esp_app_desc_t desc;
    if (!image_ok(&desc)) return false;
    if (strncmp(desc.version, esp_app_get_description()->version, sizeof(desc.version)) == 0)
        return false;
    snprintf(version, len, "%s", desc.version);
    return true;
}

esp_err_t lx_update_install(void (*progress)(int percent))
{
    struct stat st;
    esp_app_desc_t desc;
    if (stat(LX_UPDATE_FILE, &st) != 0) return ESP_ERR_NOT_FOUND;
    if (!image_ok(&desc)) return ESP_ERR_INVALID_VERSION;          /* another board's image */
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || (size_t)st.st_size > part->size) return ESP_ERR_INVALID_SIZE;
    FILE *f = fopen(LX_UPDATE_FILE, "rb");
    uint8_t *buf = malloc(CHUNK);
    esp_ota_handle_t h = 0;
    esp_err_t err = f && buf ? esp_ota_begin(part, st.st_size, &h) : ESP_ERR_NO_MEM;
    size_t done = 0, n;
    int shown = -1;
    while (err == ESP_OK && (n = fread(buf, 1, CHUNK, f)) > 0) {
        err = esp_ota_write(h, buf, n);
        done += n;
        int pct = (int)(done * 100 / st.st_size);
        if (progress && pct / 5 != shown) { shown = pct / 5; progress(pct); }
    }
    if (err == ESP_OK && done != (size_t)st.st_size) err = ESP_ERR_INVALID_SIZE;
    if (f) fclose(f);
    free(buf);
    if (h) {
        if (err == ESP_OK) err = esp_ota_end(h);       /* checks the image and its hash */
        else esp_ota_abort(h);
    }
    if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
    if (err == ESP_OK) {
        if (lx_sd_write_try_lock()) {
            remove(LX_UPDATE_DONE);
            if (rename(LX_UPDATE_FILE, LX_UPDATE_DONE) != 0)
                ESP_LOGW(TAG, "could not archive update file (errno %d)", errno);
            lx_sd_write_unlock();
        } else {
            ESP_LOGW(TAG, "SD card busy; leaving the installed update file in place");
        }
        ESP_LOGI(TAG, "installed to %s", part->label);
    } else {
        ESP_LOGE(TAG, "install failed: %s", esp_err_to_name(err));
    }
    return err;
}
