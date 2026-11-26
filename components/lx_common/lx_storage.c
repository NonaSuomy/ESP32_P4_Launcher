/* Shared guards for SD-card operations that span multiple FatFs calls. */
#include "lx_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_random.h"

static const char *TAG = "lx_storage";
static SemaphoreHandle_t s_sd_write_mutex;

bool lx_common_init(void)
{
    if (s_sd_write_mutex) return true;
    s_sd_write_mutex = xSemaphoreCreateMutex();
    return s_sd_write_mutex != NULL;
}

bool lx_sd_write_try_lock(void)
{
    return s_sd_write_mutex && xSemaphoreTake(s_sd_write_mutex, 0) == pdTRUE;
}

void lx_sd_write_lock(void)
{
    if (s_sd_write_mutex) xSemaphoreTake(s_sd_write_mutex, portMAX_DELAY);
}

void lx_sd_write_unlock(void)
{
    if (s_sd_write_mutex) xSemaphoreGive(s_sd_write_mutex);
}

int lx_sd_replace_file(const char *staging_path, const char *destination_path)
{
    struct stat st;
    if (stat(destination_path, &st) != 0) {
        if (errno != ENOENT) return -1;
        return rename(staging_path, destination_path);
    }
    if (S_ISDIR(st.st_mode)) {
        errno = EISDIR;
        return -1;
    }

    char backup[256];
    bool found_name = false;
    for (unsigned attempt = 0; attempt < 4; attempt++) {
        int n = snprintf(backup, sizeof(backup), "%s.bak-%08lx", destination_path,
                         (unsigned long)esp_random());
        if (n < 0 || (size_t)n >= sizeof(backup)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (stat(backup, &st) != 0 && errno == ENOENT) {
            found_name = true;
            break;
        }
    }
    if (!found_name) {
        errno = EEXIST;
        return -1;
    }
    if (rename(destination_path, backup) != 0) return -1;
    if (rename(staging_path, destination_path) != 0) {
        int commit_errno = errno;
        if (rename(backup, destination_path) != 0)
            ESP_LOGE(TAG, "replace failed; original remains at %s", backup);
        errno = commit_errno;
        return -1;
    }
    if (remove(backup) != 0)
        ESP_LOGW(TAG, "new file installed; old version remains at %s (errno %d)", backup, errno);
    return 0;
}
