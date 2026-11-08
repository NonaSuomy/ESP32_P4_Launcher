/*
 * HTTPS helpers shared by the launcher update and the app catalogue.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lx_http.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lx_http";

#define CHUNK 4096

esp_http_client_handle_t lx_http_new(const char *url)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = CHUNK,
        .buffer_size_tx = CHUNK,                /* release downloads redirect to long URLs */
        .timeout_ms = 8000,                     /* a lost reply is retried sooner */
        .user_agent = "P4-Launcher",
    };
    return esp_http_client_init(&cfg);
}

int lx_http_open(esp_http_client_handle_t c, char *why, size_t why_len)
{
    for (int hops = 0; hops < 5; hops++) {
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "connect failed: %s", esp_err_to_name(err));
            snprintf(why, why_len, "%s", esp_err_to_name(err));
            return -1;
        }
        if (esp_http_client_fetch_headers(c) < 0) {
            ESP_LOGW(TAG, "no HTTP response");
            snprintf(why, why_len, "no HTTP response");
            return -1;
        }
        int status = esp_http_client_get_status_code(c);
        if (status < 300 || status >= 400) return status;
        esp_http_client_set_redirection(c);
        esp_http_client_close(c);
    }
    snprintf(why, why_len, "too many redirects");
    return -1;
}

int lx_http_get_text(const char *url, size_t max, char **out, char *why, size_t why_len)
{
    *out = NULL;
    char *body = heap_caps_calloc(1, max, MALLOC_CAP_SPIRAM);
    if (!body) {
        snprintf(why, why_len, "out of memory");
        return -1;
    }
    int status = -1;
    for (int tries = 0; status < 0 && tries < 4; tries++) {        /* a weak signal drops some */
        if (tries) vTaskDelay(pdMS_TO_TICKS(1000));
        esp_http_client_handle_t c = lx_http_new(url);
        if (!c) break;
        status = lx_http_open(c, why, why_len);
        size_t got = 0;
        if (status == 200) {
            int n;
            while (got < max - 1 && (n = esp_http_client_read(c, body + got, max - 1 - got)) > 0)
                got += n;
            body[got] = 0;
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    if (status == 200) *out = body;
    else free(body);
    return status;
}

void lx_http_url(char *url, size_t len, const char *override_file, const char *def)
{
    snprintf(url, len, "%s", def);
    FILE *f = fopen(override_file, "r");
    if (!f) return;
    if (fgets(url, len, f)) url[strcspn(url, "\r\n")] = 0;
    fclose(f);
    if (!url[0]) snprintf(url, len, "%s", def);
}

bool lx_version_newer(const char *a, const char *b)
{
    int x[3] = { 0 }, y[3] = { 0 };
    sscanf(a, "%d.%d.%d", &x[0], &x[1], &x[2]);
    sscanf(b, "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; i++)
        if (x[i] != y[i]) return x[i] > y[i];
    return false;
}
