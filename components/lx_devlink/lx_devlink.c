/*
 * DevLink — remote development service over USB-Serial-JTAG or board UART.
 *
 * One task parses request frames and answers them. ESP_LOG output and app
 * logs are forwarded as LOG frames, but only while a host has talked to us in
 * the last few seconds (host tools send PING as keep-alive). With no host
 * listening nothing is written, so the device never stalls on a full USB FIFO.
 */
#include "lx_devlink.h"
#include "esp_attr.h"     /* EXT_RAM_BSS_ATTR */
#include "hal/usb_serial_jtag_ll.h"
#include "devlink_proto.h"
#include "lx_app.h"
#include "lx_common.h"
#include "bsp.h"
#include "lx_input.h"
#include "lx_net.h"
#include "lx_bt.h"
#include "lx_c6.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "ff.h"
#include "diskio_sdmmc.h"
#include "esp_core_dump.h"
#include "nvs.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "devlink";

#define HOST_ALIVE_US      (10 * 1000 * 1000)
#define RX_TIMEOUT_MS      2000

typedef enum { DL_TRANSPORT_SERIAL, DL_TRANSPORT_TCP } dl_transport_t;
static SemaphoreHandle_t s_tx_lock;
static SemaphoreHandle_t s_dispatch_lock;
static vprintf_like_t    s_orig_vprintf;
static volatile int64_t  s_last_serial_host_us = -HOST_ALIVE_US;
#if CONFIG_LX_DEVLINK_TCP
static SemaphoreHandle_t s_tcp_tx_lock;
static volatile int64_t  s_last_tcp_host_us = -HOST_ALIVE_US;
static volatile int      s_tcp_fd = -1;
static volatile bool     s_tcp_authenticated;
static uint32_t          s_tcp_pin;
#endif
static volatile bool     s_paused;    /* an app has USB-C (lx_usbd): nothing goes to the serial/JTAG unit */
static volatile dl_transport_t s_reply_transport = DL_TRANSPORT_SERIAL;
static uint8_t          *s_rx;        /* request payload  */
static char             *s_tx;        /* response scratch */

#if CONFIG_LX_DEVLINK_UART
#define DEVLINK_UART UART_NUM_0
#endif
#if CONFIG_LX_DEVLINK_TCP
#define DEVLINK_TCP_PORT CONFIG_LX_DEVLINK_TCP_PORT
#define TCP_AUTH_MAX_FAILURES 5
#define TCP_AUTH_LOCKOUT_US (60 * 1000 * 1000LL)
static unsigned s_tcp_auth_failures;
static int64_t s_tcp_auth_block_until;
#endif

/* PUT state */
static FILE    *s_put_f;
static char     s_put_path[160];
static uint32_t s_put_size, s_put_crc, s_put_done, s_put_crc_run;

/* ── Framing ────────────────────────────────────────────────────────────── */

static bool serial_host_alive(void)
{
    int64_t now = esp_timer_get_time();
#if CONFIG_LX_DEVLINK_UART
    return !s_paused && now - s_last_serial_host_us < HOST_ALIVE_US;
#else
    return !s_paused && usb_serial_jtag_is_connected() &&
           now - s_last_serial_host_us < HOST_ALIVE_US;
#endif
}

static bool tcp_host_alive(void)
{
#if CONFIG_LX_DEVLINK_TCP
    return s_tcp_authenticated && s_tcp_fd >= 0 &&
           esp_timer_get_time() - s_last_tcp_host_us < HOST_ALIVE_US;
#else
    return false;
#endif
}

static bool serial_write_all(const void *p, size_t len, TickType_t to)
{
    const uint8_t *b = p;
    while (len) {
#if CONFIG_LX_DEVLINK_UART
        (void)to;
        int n = uart_write_bytes(DEVLINK_UART, b, len > 4096 ? 4096 : len);
#else
        int n = usb_serial_jtag_write_bytes(b, len > 4096 ? 4096 : len, to);
#endif
        if (n <= 0) return false;
        b += n;
        len -= n;
    }
    return true;
}

static bool tcp_write_all(int fd, const void *p, size_t len)
{
    const uint8_t *b = p;
    while (len) {
        int n = send(fd, b, len, 0);
        if (n <= 0) return false;
        b += n;
        len -= (size_t)n;
    }
    return true;
}

static bool dl_send_on(dl_transport_t transport, uint8_t type, uint8_t flags, uint16_t seq,
                       const void *payload, uint32_t len, TickType_t lock_to)
{
    uint8_t hdr[DL_HDR_SIZE] = {
        DL_SOF0, DL_SOF1, type, flags, seq & 0xFF, seq >> 8,
        len & 0xFF, (len >> 8) & 0xFF, (len >> 16) & 0xFF, len >> 24,
    };
    uint32_t crc = lx_crc32(0, hdr + 2, DL_HDR_SIZE - 2);
    crc = lx_crc32(crc, payload, len);

    if (transport == DL_TRANSPORT_SERIAL) {
        if (s_paused || xSemaphoreTake(s_tx_lock, lock_to) != pdTRUE) return false;
        TickType_t to = pdMS_TO_TICKS(1000);
        bool ok = serial_write_all(hdr, sizeof(hdr), to) &&
                  (!len || serial_write_all(payload, len, to)) &&
                  serial_write_all(&crc, 4, to);
        xSemaphoreGive(s_tx_lock);
        return ok;
    }
#if CONFIG_LX_DEVLINK_TCP
    if (xSemaphoreTake(s_tcp_tx_lock, lock_to) != pdTRUE) return false;
    int fd = s_tcp_fd;
    bool ok = fd >= 0 && tcp_write_all(fd, hdr, sizeof(hdr)) &&
              (!len || tcp_write_all(fd, payload, len)) &&
              tcp_write_all(fd, &crc, 4);
    xSemaphoreGive(s_tcp_tx_lock);
    return ok;
#else
    return false;
#endif
}

static bool dl_send(uint8_t type, uint8_t flags, uint16_t seq,
                    const void *payload, uint32_t len, TickType_t lock_to)
{
    return dl_send_on(s_reply_transport, type, flags, seq, payload, len, lock_to);
}

static void dl_send_async(uint8_t type, uint8_t flags, uint16_t seq,
                          const void *payload, uint32_t len, TickType_t lock_to)
{
    if (serial_host_alive()) dl_send_on(DL_TRANSPORT_SERIAL, type, flags, seq, payload, len, lock_to);
    if (tcp_host_alive()) dl_send_on(DL_TRANSPORT_TCP, type, flags, seq, payload, len, lock_to);
}

static void reply(uint8_t req, uint16_t seq, const void *p, uint32_t len)
{
    dl_send(req | 0x80, 0, seq, p, len, portMAX_DELAY);
}

static void reply_str(uint8_t req, uint16_t seq, const char *s)
{
    reply(req, seq, s, strlen(s));
}

static void reply_err(uint8_t req, uint16_t seq, const char *fmt, ...)
{
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    dl_send(req | 0x80, DL_F_ERROR, seq, msg, strlen(msg), portMAX_DELAY);
}

/* ── Boot log ───────────────────────────────────────────────────────────────
 * Messages from before DevLink is up (board bring-up) are otherwise lost on
 * boards whose UART console isn't wired out. The first BOOTLOG_SIZE bytes of
 * the log are kept and returned by DL_BOOTLOG. */

#define BOOTLOG_SIZE (24 * 1024)
static char *s_boot;
static size_t s_boot_len;
static vprintf_like_t s_boot_next;

static int boot_vprintf(const char *fmt, va_list ap)
{
    if (s_boot && s_boot_len < BOOTLOG_SIZE - 1) {
        va_list ap2;
        va_copy(ap2, ap);
        int n = vsnprintf(s_boot + s_boot_len, BOOTLOG_SIZE - s_boot_len, fmt, ap2);
        va_end(ap2);
        if (n > 0) s_boot_len = s_boot_len + n < BOOTLOG_SIZE - 1 ? s_boot_len + n : BOOTLOG_SIZE - 1;
    }
    return s_boot_next(fmt, ap);
}

void lx_devlink_early_init(void)
{
    s_boot = heap_caps_malloc(BOOTLOG_SIZE, MALLOC_CAP_SPIRAM);
    if (s_boot) s_boot_next = esp_log_set_vprintf(boot_vprintf);
}

/* ── Log forwarding ─────────────────────────────────────────────────────── */

static int dl_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    /* UART0 is shared with the AIO's CH340. Once DevLink is active, route logs
     * as frames so console text cannot corrupt the protocol stream. */
    bool active = serial_host_alive();
    int r;
#if CONFIG_LX_DEVLINK_UART
    if (active) {
        char buf[256];
        int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
        r = n;
        if (n > 0 && xSemaphoreGetMutexHolder(s_tx_lock) != xTaskGetCurrentTaskHandle())
            dl_send_on(DL_TRANSPORT_SERIAL, DL_LOG, 0, 0, buf, n < (int)sizeof(buf) ? n : sizeof(buf) - 1, pdMS_TO_TICKS(20));
    } else {
        r = s_orig_vprintf(fmt, ap);
    }
#else
    r = s_orig_vprintf(fmt, ap);
    if (active && xSemaphoreGetMutexHolder(s_tx_lock) != xTaskGetCurrentTaskHandle()) {
        char buf[256];
        int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
        if (n > 0) dl_send_on(DL_TRANSPORT_SERIAL, DL_LOG, 0, 0, buf, n < (int)sizeof(buf) ? n : sizeof(buf) - 1, pdMS_TO_TICKS(20));
    }
#endif
    if (tcp_host_alive() && xSemaphoreGetMutexHolder(s_tcp_tx_lock) != xTaskGetCurrentTaskHandle()) {
        va_list tcp_ap;
        va_copy(tcp_ap, ap);
        char buf[256];
        int n = vsnprintf(buf, sizeof(buf), fmt, tcp_ap);
        va_end(tcp_ap);
        if (n > 0) dl_send_on(DL_TRANSPORT_TCP, DL_LOG, 0, 0, buf,
                              n < (int)sizeof(buf) ? n : sizeof(buf) - 1, pdMS_TO_TICKS(20));
    }
    va_end(ap2);
    return r;
}

void lx_devlink_app_log(const char *text, size_t len)
{
    dl_send_async(DL_LOG, DL_F_APP, 0, text, len, pdMS_TO_TICKS(50));
}

/* ── JSON helpers ───────────────────────────────────────────────────────── */

typedef struct { char *buf; size_t cap, len; bool overflow; } jbuf_t;

static void jcat(jbuf_t *j, const char *fmt, ...)
{
    if (j->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->len, j->cap - j->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= j->cap - j->len) { j->overflow = true; return; }
    j->len += n;
}

static void jstr(jbuf_t *j, const char *s)
{
    jcat(j, "\"");
    for (; *s && !j->overflow; s++) {
        unsigned char c = *s;
        if (c == '"' || c == '\\') jcat(j, "\\%c", c);
        else if (c < 0x20) jcat(j, "\\u%04x", c);
        else jcat(j, "%c", c);
    }
    jcat(j, "\"");
}

static const char *app_state_name(void)
{
    switch (lx_app_state()) {
    case LX_APP_RUNNING:  return "running";
    case LX_APP_STOPPING: return "stopping";
    default:              return "idle";
    }
}

/* ── Handlers ───────────────────────────────────────────────────────────── */

static void mkdirs(const char *path, bool include_last)
{
    char tmp[160];
    strlcpy(tmp, path, sizeof(tmp));
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0775); *p = '/'; }
    }
    if (include_last) mkdir(tmp, 0775);
}

static void h_info(uint16_t seq)
{
    const bsp_info_t *b = bsp_info();
    jbuf_t j = { .buf = s_tx, .cap = DL_MAX_PAYLOAD };
    jcat(&j, "{\"launcher\":\"%s\",\"abi\":\"%s\",\"idf\":\"%s\",\"board\":",
         LX_LAUNCHER_VERSION, LX_ABI_STRING, esp_app_get_description()->idf_ver);
    jstr(&j, b->id);
    jcat(&j, ",\"board_name\":");
    jstr(&j, b->name);
    jcat(&j, ",\"caps\":%lu,\"display\":[%u,%u],\"sd\":%s,\"sd_bits\":%d,\"sd_1bit\":%s,\"uptime_ms\":%lld,"
             "\"free_internal\":%u,\"largest_fast\":%u,\"free_psram\":%u,\"largest_psram\":%u,\"app_state\":\"%s\",\"app\":",
         (unsigned long)b->caps, b->display_w, b->display_h, bsp_sdcard_mounted() ? "true" : "false",
         ff_sdmmc_bus_width(),                             /* data lines in use */
         ff_sdmmc_bus_fell_back() ? "true" : "false",     /* 4-bit failed, card moved to 1-bit */
         esp_timer_get_time() / 1000,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_EXEC | MALLOC_CAP_INTERNAL),   /* an app's FAST region */
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), app_state_name());
    jstr(&j, lx_app_path());
    jcat(&j, ",\"net_state\":%d,\"ip\":", (int)lx_net_state());
    jstr(&j, lx_net_ip());
    jcat(&j, ",\"c6_fw\":");                 /* "" until the radio first started, "?" = it didn't say */
    jstr(&j, lx_c6_version());
    jcat(&j, ",\"hosted\":");
    jstr(&j, lx_c6_host_version());
    int64_t rtc = -1;
    bsp_rtc_get(&rtc);                      /* -1: no RTC, or it lost the time */
    jcat(&j, ",\"time\":%lld,\"rtc\":%lld,\"reset_reason\":%d}", (long long)time(NULL), (long long)rtc,
         (int)esp_reset_reason());
    reply(DL_INFO, seq, j.buf, j.len);
}

static void h_ls(uint16_t seq, const char *path)
{
    DIR *d = opendir(path);
    if (!d) { reply_err(DL_LS, seq, "cannot open dir %s (errno %d)", path, errno); return; }
    jbuf_t j = { .buf = s_tx, .cap = DL_MAX_PAYLOAD - 64 };
    jcat(&j, "[");
    bool first = true;
    char full[300];
    struct dirent *e;
    while ((e = readdir(d)) && !j.overflow) {
        struct stat st = {0};
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        bool is_dir = e->d_type == DT_DIR;
        if (!is_dir) stat(full, &st);
        size_t mark = j.len;
        jcat(&j, "%s{\"n\":", first ? "" : ",");
        jstr(&j, e->d_name);
        jcat(&j, ",\"s\":%ld,\"d\":%d}", (long)st.st_size, is_dir);
        if (j.overflow) { j.len = mark; break; }
        first = false;
    }
    bool truncated = j.overflow;
    closedir(d);
    j.overflow = false;
    j.cap = DL_MAX_PAYLOAD;
    jcat(&j, "]%s", truncated ? "\n#truncated" : "");
    reply(DL_LS, seq, j.buf, j.len);
}

static void h_stat(uint16_t seq, const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) { reply_err(DL_STAT, seq, "not found: %s", path); return; }
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"s\":%ld,\"d\":%d,\"mtime\":%lld}",
             (long)st.st_size, S_ISDIR(st.st_mode) ? 1 : 0, (long long)st.st_mtime);
    reply_str(DL_STAT, seq, buf);
}

static void put_abort(void)
{
    if (s_put_f) {
        fclose(s_put_f);
        s_put_f = NULL;
        char part[170];
        snprintf(part, sizeof(part), "%s.part", s_put_path);
        remove(part);
    }
}

/* Changes to /sd/apps made over DevLink: the home screen rescans when this moves */
static volatile uint32_t s_app_changes;

static void note_change(const char *path)
{
    if (path && strncmp(path, "/sd/apps", 8) == 0) s_app_changes++;
}

uint32_t lx_devlink_app_changes(void)
{
    return s_app_changes;
}

static void h_put_begin(uint16_t seq, const uint8_t *p, uint32_t len)
{
    put_abort();
    if (len < 9) { reply_err(DL_PUT_BEGIN, seq, "bad request"); return; }
    memcpy(&s_put_size, p, 4);
    memcpy(&s_put_crc, p + 4, 4);
    strlcpy(s_put_path, (const char *)p + 8, sizeof(s_put_path));
    mkdirs(s_put_path, false);

    char part[170];
    snprintf(part, sizeof(part), "%s.part", s_put_path);
    s_put_f = fopen(part, "wb");
    if (!s_put_f) { reply_err(DL_PUT_BEGIN, seq, "cannot create %s (errno %d)", part, errno); return; }
    setvbuf(s_put_f, NULL, _IOFBF, 8192);
    s_put_done = 0;
    s_put_crc_run = 0;
    reply_str(DL_PUT_BEGIN, seq, "ready");
}

static void h_put_data(uint16_t seq, const uint8_t *p, uint32_t len)
{
    if (!s_put_f) { reply_err(DL_PUT_DATA, seq, "no transfer in progress"); return; }
    if (s_put_done + len > s_put_size) { put_abort(); reply_err(DL_PUT_DATA, seq, "too much data"); return; }
    if (fwrite(p, 1, len, s_put_f) != len) {
        put_abort();
        reply_err(DL_PUT_DATA, seq, "write failed (SD full?)");
        return;
    }
    s_put_crc_run = lx_crc32(s_put_crc_run, p, len);
    s_put_done += len;
    reply(DL_PUT_DATA, seq, &s_put_done, 4);
}

static void h_put_end(uint16_t seq)
{
    if (!s_put_f) { reply_err(DL_PUT_END, seq, "no transfer in progress"); return; }
    if (s_put_done != s_put_size || s_put_crc_run != s_put_crc) {
        put_abort();
        reply_err(DL_PUT_END, seq, "verify failed: got %lu bytes crc %08lx, expected %lu crc %08lx",
                  (unsigned long)s_put_done, (unsigned long)s_put_crc_run,
                  (unsigned long)s_put_size, (unsigned long)s_put_crc);
        return;
    }
    fclose(s_put_f);
    s_put_f = NULL;
    char part[170];
    snprintf(part, sizeof(part), "%s.part", s_put_path);
    remove(s_put_path);
    if (rename(part, s_put_path) != 0) {
        reply_err(DL_PUT_END, seq, "rename failed (errno %d)", errno);
        return;
    }
    ESP_LOGI(TAG, "received %s (%lu bytes)", s_put_path, (unsigned long)s_put_size);
    note_change(s_put_path);
    reply_str(DL_PUT_END, seq, "ok");
}

static void h_get(uint16_t seq, const uint8_t *p, uint32_t len)
{
    if (len < 9) { reply_err(DL_GET, seq, "bad request"); return; }
    uint32_t off, max;
    memcpy(&off, p, 4);
    memcpy(&max, p + 4, 4);
    const char *path = (const char *)p + 8;
    if (max > DL_CHUNK) max = DL_CHUNK;

    FILE *f = fopen(path, "rb");
    if (!f) { reply_err(DL_GET, seq, "cannot open %s", path); return; }
    fseek(f, 0, SEEK_END);
    uint32_t size = ftell(f);
    fseek(f, off, SEEK_SET);
    size_t n = off < size ? fread(s_tx + 4, 1, max, f) : 0;
    fclose(f);
    memcpy(s_tx, &size, 4);
    reply(DL_GET, seq, s_tx, 4 + n);
}

static void h_run(uint16_t seq, const uint8_t *p, uint32_t len)
{
    /* path \0 arg \0 arg ... */
    const char *args[16];
    int argc = 0;
    const char *path = (const char *)p;
    const char *s = path + strlen(path) + 1;
    const char *end = (const char *)p + len;
    while (s < end && *s && argc < 16) {
        args[argc++] = s;
        s += strlen(s) + 1;
    }
    char err[160] = "";
    if (lx_app_launch(path, argc, args, err, sizeof(err)) == ESP_OK)
        reply_str(DL_RUN, seq, "started");
    else
        reply_err(DL_RUN, seq, "%s", err);
}

static void h_status(uint16_t seq)
{
    jbuf_t j = { .buf = s_tx, .cap = DL_MAX_PAYLOAD };
    jcat(&j, "{\"state\":\"%s\",\"app\":", app_state_name());
    jstr(&j, lx_app_path());
    jcat(&j, ",\"exec_base\":%lu,\"free_internal\":%u,\"free_psram\":%u}",
         (unsigned long)lx_app_exec_base(),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    reply(DL_STATUS, seq, j.buf, j.len);
}

static void h_crash(uint16_t seq)
{
    jbuf_t j = { .buf = s_tx, .cap = DL_MAX_PAYLOAD };
    jcat(&j, "{\"reset_reason\":%d,\"app\":", (int)esp_reset_reason());
    lx_app_crash_ctx_t c;
    if (lx_app_last_crash(&c)) {
        jcat(&j, "{\"path\":");
        jstr(&j, c.path);
        jcat(&j, ",\"exec_base\":%lu,\"size\":%lu,\"fast_base\":%lu,\"fast_size\":%lu}",
             (unsigned long)c.exec_base, (unsigned long)c.image_size, (unsigned long)c.fast_base,
             (unsigned long)c.fast_size);
    } else {
        jcat(&j, "null");
    }
    jcat(&j, ",\"coredump\":");
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    esp_core_dump_summary_t *sum = malloc(sizeof(*sum));
    if (sum && esp_core_dump_image_check() == ESP_OK && esp_core_dump_get_summary(sum) == ESP_OK) {
        jcat(&j, "{\"task\":");
        jstr(&j, sum->exc_task);
        jcat(&j, ",\"pc\":%lu,\"ra\":%lu,\"sp\":%lu,\"mcause\":%lu,\"mtval\":%lu,\"stack\":[",
             (unsigned long)sum->exc_pc, (unsigned long)sum->ex_info.ra, (unsigned long)sum->ex_info.sp,
             (unsigned long)sum->ex_info.mcause, (unsigned long)sum->ex_info.mtval);
        /* Raw stack words: the host picks out return addresses that fall in an app/launcher image */
        uint32_t words = sum->exc_bt_info.dump_size / 4;
        for (uint32_t i = 0; i < words && i < 256; i++) {
            uint32_t w;
            memcpy(&w, sum->exc_bt_info.stackdump + i * 4, 4);
            jcat(&j, "%s%lu", i ? "," : "", (unsigned long)w);
        }
        jcat(&j, "]}");
    } else {
        jcat(&j, "null");
    }
    free(sum);
#else
    jcat(&j, "null");
#endif
    jcat(&j, "}");
    reply(DL_CRASH, seq, j.buf, j.len);
}

static uint8_t *s_shot;          /* compressed snapshot */
static uint32_t s_shot_len;

static uint32_t rle16(const uint16_t *px, uint32_t n, uint8_t *out)
{
    uint8_t *o = out;
    uint32_t i = 0;
    while (i < n) {
        uint32_t run = 1;
        while (i + run < n && run < 0x7FFF && px[i + run] == px[i]) run++;
        if (run >= 3) {
            uint16_t h = 0x8000 | run;
            memcpy(o, &h, 2); memcpy(o + 2, &px[i], 2); o += 4;
            i += run;
            continue;
        }
        /* literal stretch until the next run of 3+ */
        uint32_t start = i, lit = 0;
        while (i < n && lit < 0x7FFF) {
            if (i + 2 < n && px[i] == px[i + 1] && px[i] == px[i + 2]) break;
            i++; lit++;
        }
        uint16_t h = lit;
        memcpy(o, &h, 2);
        memcpy(o + 2, &px[start], lit * 2);
        o += 2 + lit * 2;
    }
    return o - out;
}

static void h_shot(uint16_t seq, const uint8_t *p, uint32_t len)
{
    const bsp_info_t *b = bsp_info();
    if (bsp_display_fb_count() == 0) { reply_err(DL_SHOT, seq, "no display"); return; }
    uint32_t off = 0, max = DL_CHUNK;
    if (len >= 8) { memcpy(&off, p, 4); memcpy(&max, p + 4, 4); }
    if (max > DL_CHUNK) max = DL_CHUNK;

    uint32_t pixels = (uint32_t)b->display_w * b->display_h;
    if (off == 0) {
        if (!s_shot) s_shot = heap_caps_malloc(pixels * 2 + (pixels / 0x7FFF + 1) * 2 + 64, MALLOC_CAP_SPIRAM);
        if (!s_shot) { reply_err(DL_SHOT, seq, "no memory for snapshot"); return; }
        s_shot_len = rle16(bsp_display_fb(bsp_display_visible()), pixels, s_shot);
    }
    if (!s_shot) { reply_err(DL_SHOT, seq, "no snapshot; request offset 0 first"); return; }

    dl_shot_hdr_t h = { b->display_w, b->display_h, DL_SHOT_FMT_RGB565_RLE16, b->display_rot, s_shot_len };
    uint32_t n = off < s_shot_len ? s_shot_len - off : 0;
    if (n > max) n = max;
    memcpy(s_tx, &h, sizeof(h));
    memcpy(s_tx + sizeof(h), s_shot + off, n);
    reply(DL_SHOT, seq, s_tx, sizeof(h) + n);
    if (off + n >= s_shot_len) {     /* last page sent: give the memory back to apps */
        heap_caps_free(s_shot);
        s_shot = NULL;
    }
}

static void h_input(uint16_t seq, const uint8_t *p, uint32_t len)
{
    if (len >= sizeof(dl_input_pad_t) && p[0] == DL_INPUT_PAD) {
        dl_input_pad_t r;
        memcpy(&r, p, sizeof(r));
        lx_input_inject_buttons(r.player, r.buttons, r.hold_ms);
        reply(DL_INPUT, seq, NULL, 0);
    } else if (len >= sizeof(dl_input_key_t) && p[0] == DL_INPUT_KEY) {
        dl_input_key_t r;
        memcpy(&r, p, sizeof(r));
        lx_input_inject_key(r.usage, r.mods, r.hold_ms);
        reply(DL_INPUT, seq, NULL, 0);
    } else if (len >= 2 && p[0] == DL_INPUT_UNMAP) {
        lx_input_pad_set_map(p[1], NULL);           /* back to the pad's own layout */
        reply(DL_INPUT, seq, NULL, 0);
    } else if (len >= 1 && p[0] == DL_INPUT_STATE) {
        jbuf_t j = { .buf = s_tx, .cap = DL_MAX_PAYLOAD };
        char desc[128];
        lx_input_describe(desc, sizeof(desc));
        jcat(&j, "{\"devices\":");
        jstr(&j, desc);
        jcat(&j, ",\"mods\":%u,\"paddle\":%d,\"players\":[", lx_input_modifiers(), lx_input_analog(PAPP_ANALOG_PADDLE));
        for (int i = 0; i < PAPP_MAX_PLAYERS; i++) {
            papp_gamepad_t g;
            bool c = lx_input_gamepad(i, &g);
            jcat(&j, "%s{\"connected\":%s,\"name\":", i ? "," : "", c ? "true" : "false");
            jstr(&j, g.name);
            jcat(&j, ",\"buttons\":%lu,\"lx\":%d,\"ly\":%d,\"rx\":%d,\"ry\":%d,\"lt\":%u,\"rt\":%u}",
                 (unsigned long)g.buttons, g.lx, g.ly, g.rx, g.ry, g.lt, g.rt);
        }
        jcat(&j, "]}");
        reply(DL_INPUT, seq, j.buf, j.len);
    } else {
        reply_err(DL_INPUT, seq, "bad INPUT request");
    }
}

/* Boards without it */
__attribute__((weak)) esp_err_t bsp_display_scanout(bool on) { return ESP_ERR_NOT_SUPPORTED; }

static void dispatch(uint8_t type, uint16_t seq, uint8_t *p, uint32_t len)
{
    p[len] = 0;   /* string payloads are NUL-terminated for free */
    const char *str = (const char *)p;

    switch (type) {
    case DL_PING:   reply_str(type, seq, "pong"); break;
    case DL_AUTH:
#if CONFIG_LX_DEVLINK_TCP
        if (s_reply_transport == DL_TRANSPORT_SERIAL && len == 0) {
            char pin[7];
            snprintf(pin, sizeof(pin), "%06lu", (unsigned long)s_tcp_pin);
            reply_str(type, seq, pin);
        } else {
            reply_err(type, seq, "AUTH is only available over serial for PIN recovery");
        }
#else
        reply_err(type, seq, "Wi-Fi DevLink is disabled");
#endif
        break;
    case DL_TIME:
        if (len >= 8) {
            /* Boards without RTC/network get the clock from the host: its local
             * wall-clock time, shown as is, then real UTC for a board with a zone. */
            int64_t secs;
            memcpy(&secs, p, sizeof(secs));
            if (len >= 16 && getenv("TZ")) memcpy(&secs, p + 8, sizeof(secs));
            struct timeval tv = { .tv_sec = (time_t)secs };
            int64_t was = time(NULL);
            settimeofday(&tv, NULL);
            /* The RTC keeps real UTC only: with no time zone set this is local time */
            if (getenv("TZ") && len >= 16 && (secs - was > 2 || was - secs > 2)) bsp_rtc_set(secs);
            reply_str(type, seq, "ok");
        } else {
            reply_err(type, seq, "bad TIME request");
        }
        break;
    case DL_INFO:   h_info(seq); break;
    case DL_LS:     h_ls(seq, len ? str : "/sd"); break;
    case DL_STAT:   h_stat(seq, str); break;
    case DL_MKDIR:
        mkdirs(str, true);
        note_change(str);
        reply_str(type, seq, "ok");
        break;
    case DL_RM:
        if (remove(str) == 0 || rmdir(str) == 0) { note_change(str); reply_str(type, seq, "ok"); }
        else reply_err(type, seq, "cannot remove %s (errno %d)", str, errno);
        break;
    case DL_RENAME: {
        const char *to = str + strlen(str) + 1;
        if ((const uint8_t *)to < p + len && rename(str, to) == 0) {
            note_change(str);
            note_change(to);
            reply_str(type, seq, "ok");
        }
        else reply_err(type, seq, "rename failed (errno %d)", errno);
        break;
    }
    case DL_PUT_BEGIN: h_put_begin(seq, p, len); break;
    case DL_PUT_DATA:  h_put_data(seq, p, len); break;
    case DL_PUT_END:   h_put_end(seq); break;
    case DL_GET:       h_get(seq, p, len); break;
    case DL_RUN:       h_run(seq, p, len); break;
    case DL_STOP: {
        uint32_t grace = 2000;
        if (len >= 4) memcpy(&grace, p, 4);
        if (lx_app_stop(grace) == ESP_OK) reply_str(type, seq, "stopping");
        else reply_err(type, seq, "no app running");
        break;
    }
    case DL_STATUS: h_status(seq); break;
    case DL_REBOOT:
        reply_str(type, seq, "rebooting");
#if CONFIG_LX_DEVLINK_UART
        if (s_reply_transport == DL_TRANSPORT_SERIAL) uart_wait_tx_done(DEVLINK_UART, pdMS_TO_TICKS(200));
#else
        if (s_reply_transport == DL_TRANSPORT_SERIAL) usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));
#endif
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
        break;
    case DL_CRASH: h_crash(seq); break;
    case DL_BOOTLOG: reply(type, seq, s_boot, s_boot ? s_boot_len : 0); break;
    case DL_SHOT: h_shot(seq, p, len); break;
    case DL_INPUT: h_input(seq, p, len); break;
    case DL_DISPLAY: {
        esp_err_t e = bsp_display_scanout(!len || p[0]);
        if (e == ESP_OK) reply_str(type, seq, !len || p[0] ? "display on" : "display off");
        else reply_err(type, seq, "display: %s", esp_err_to_name(e));
        break;
    }
    case DL_NET: {
        if (len && p[0]) lx_net_diag_start(p[0]);
        static EXT_RAM_BSS_ATTR char text[1600];
        text[0] = lx_net_diag_busy() ? '1' : '0';
        snprintf(text + 1, sizeof(text) - 1, "%s", lx_net_diag_text());
        reply_str(type, seq, text);
        break;
    }
    case DL_BT: {
        char cmd[96];
        size_t n = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
        memcpy(cmd, p, n);
        cmd[n] = 0;
        reply_str(type, seq, lx_bt_devlink(cmd));
        break;
    }
    case DL_CRASH_CLEAR:
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
        esp_core_dump_image_erase();
#endif
        reply_str(type, seq, "ok");
        break;
    default:
        reply_err(type, seq, "command 0x%02x not implemented", type);
        break;
    }
}

static void handle_frame(dl_transport_t transport, uint8_t type, uint16_t seq,
                         uint8_t *payload, uint32_t len, bool crc_ok)
{
    xSemaphoreTake(s_dispatch_lock, portMAX_DELAY);
    s_reply_transport = transport;
    if (!crc_ok) {
        reply_err(type, seq, "frame CRC mismatch");
    } else if (transport == DL_TRANSPORT_TCP) {
#if CONFIG_LX_DEVLINK_TCP
        if (!s_tcp_authenticated) {
            char expected[7];
            snprintf(expected, sizeof(expected), "%06lu", (unsigned long)s_tcp_pin);
            if (type == DL_AUTH && len == 6 && memcmp(payload, expected, 6) == 0) {
                s_tcp_authenticated = true;
                s_last_tcp_host_us = esp_timer_get_time();
                s_tcp_auth_failures = 0;
                s_tcp_auth_block_until = 0;
                reply_str(type, seq, "ok");
            } else {
                s_tcp_auth_failures++;
                reply_err(type, seq, type == DL_AUTH ? "invalid DevLink PIN" : "authenticate first");
                if (s_tcp_auth_failures >= TCP_AUTH_MAX_FAILURES) {
                    s_tcp_auth_block_until = esp_timer_get_time() + TCP_AUTH_LOCKOUT_US;
                    s_tcp_auth_failures = 0;
                }
            }
        } else if (type == DL_AUTH) {
            reply_err(type, seq, "TCP DevLink is already authenticated");
        } else {
            s_last_tcp_host_us = esp_timer_get_time();
            dispatch(type, seq, payload, len);
        }
#else
        reply_err(type, seq, "TCP DevLink is disabled");
#endif
    } else {
        s_last_serial_host_us = esp_timer_get_time();
        dispatch(type, seq, payload, len);
    }
    s_reply_transport = DL_TRANSPORT_SERIAL;
    xSemaphoreGive(s_dispatch_lock);
}

/* ── RX task ────────────────────────────────────────────────────────────── */

static bool read_exact(uint8_t *buf, size_t len, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (len) {
#if CONFIG_LX_DEVLINK_UART
        int n = uart_read_bytes(DEVLINK_UART, buf, len, pdMS_TO_TICKS(50));
#else
        int n = usb_serial_jtag_read_bytes(buf, len, pdMS_TO_TICKS(50));
#endif
        if (n > 0) { buf += n; len -= n; }
        else if (esp_timer_get_time() > deadline) return false;
    }
    return true;
}

static void devlink_task(void *arg)
{
    uint8_t hdr[DL_HDR_SIZE];
    for (;;) {
        /* Sync on the two start-of-frame bytes */
        uint8_t c;
#if CONFIG_LX_DEVLINK_UART
        if (uart_read_bytes(DEVLINK_UART, &c, 1, portMAX_DELAY) != 1 || c != DL_SOF0) continue;
#else
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1 || c != DL_SOF0) continue;
#endif
        if (!read_exact(&c, 1, RX_TIMEOUT_MS) || c != DL_SOF1) continue;
        hdr[0] = DL_SOF0; hdr[1] = DL_SOF1;
        if (!read_exact(hdr + 2, DL_HDR_SIZE - 2, RX_TIMEOUT_MS)) continue;

        uint8_t type = hdr[2];
        uint16_t seq = hdr[4] | (hdr[5] << 8);
        uint32_t len = hdr[6] | (hdr[7] << 8) | (hdr[8] << 16) | ((uint32_t)hdr[9] << 24);
        if (len > DL_MAX_PAYLOAD) continue;

        uint32_t crc;
        if (!read_exact(s_rx, len, RX_TIMEOUT_MS) || !read_exact((uint8_t *)&crc, 4, RX_TIMEOUT_MS)) continue;
        uint32_t calc = lx_crc32(lx_crc32(0, hdr + 2, DL_HDR_SIZE - 2), s_rx, len);
        handle_frame(DL_TRANSPORT_SERIAL, type, seq, s_rx, len, calc == crc);
    }
}

#if CONFIG_LX_DEVLINK_TCP
static bool tcp_read_exact(int fd, uint8_t *buf, size_t len)
{
    while (len) {
        int n = recv(fd, buf, len, 0);
        if (n <= 0) return false;
        buf += n;
        len -= (size_t)n;
    }
    return true;
}

static void tcp_disconnect(int fd)
{
    xSemaphoreTake(s_tcp_tx_lock, portMAX_DELAY);
    if (s_tcp_fd == fd) {
        s_tcp_fd = -1;
        s_tcp_authenticated = false;
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
    xSemaphoreGive(s_tcp_tx_lock);
    s_last_tcp_host_us = -HOST_ALIVE_US;
}

static void tcp_client_task(int fd, uint8_t *payload)
{
    uint8_t hdr[DL_HDR_SIZE];
    while (s_tcp_fd == fd) {
        uint8_t c;
        if (!tcp_read_exact(fd, &c, 1)) break;
        if (c != DL_SOF0 || !tcp_read_exact(fd, &c, 1) || c != DL_SOF1) continue;
        hdr[0] = DL_SOF0;
        hdr[1] = DL_SOF1;
        if (!tcp_read_exact(fd, hdr + 2, DL_HDR_SIZE - 2)) break;
        uint8_t type = hdr[2];
        uint16_t seq = hdr[4] | ((uint16_t)hdr[5] << 8);
        uint32_t len = hdr[6] | ((uint32_t)hdr[7] << 8) | ((uint32_t)hdr[8] << 16) |
                       ((uint32_t)hdr[9] << 24);
        if (len > DL_MAX_PAYLOAD) break;
        uint32_t crc;
        if (!tcp_read_exact(fd, payload, len) || !tcp_read_exact(fd, (uint8_t *)&crc, sizeof(crc))) break;
        uint32_t calc = lx_crc32(lx_crc32(0, hdr + 2, DL_HDR_SIZE - 2), payload, len);
        handle_frame(DL_TRANSPORT_TCP, type, seq, payload, len, calc == crc);
        if (s_tcp_auth_block_until > esp_timer_get_time()) break;
    }
    tcp_disconnect(fd);
}

static esp_err_t load_tcp_pin(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("devlink", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_get_u32(nvs, "wifi_pin", &s_tcp_pin);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_tcp_pin = 100000 + esp_random() % 900000;
        err = nvs_set_u32(nvs, "wifi_pin", s_tcp_pin);
        if (err == ESP_OK) err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK)
        ESP_LOGW(TAG, "Wi-Fi DevLink PIN: %06lu (shown on the local serial console)",
                 (unsigned long)s_tcp_pin);
    return err;
}

static void devlink_tcp_task(void *arg)
{
    uint8_t *payload = heap_caps_malloc(DL_MAX_PAYLOAD + 1, MALLOC_CAP_SPIRAM);
    if (!payload) {
        ESP_LOGE(TAG, "TCP DevLink has no PSRAM for its receive buffer");
        vTaskDelete(NULL);
        return;
    }
    int server = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (server < 0) {
        ESP_LOGE(TAG, "TCP DevLink socket failed: errno %d", errno);
        heap_caps_free(payload);
        vTaskDelete(NULL);
        return;
    }
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(DEVLINK_TCP_PORT),
                                .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(server, 1) != 0) {
        ESP_LOGE(TAG, "TCP DevLink bind/listen on %u failed: errno %d", DEVLINK_TCP_PORT, errno);
        close(server);
        heap_caps_free(payload);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "DevLink TCP listening on port %u", DEVLINK_TCP_PORT);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int fd = accept(server, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (s_tcp_auth_block_until > esp_timer_get_time()) {
            shutdown(fd, SHUT_RDWR);
            close(fd);
            continue;
        }
        struct timeval timeout = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        xSemaphoreTake(s_tcp_tx_lock, portMAX_DELAY);
        s_tcp_fd = fd;
        s_tcp_authenticated = false;
        xSemaphoreGive(s_tcp_tx_lock);
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntoa_r(peer.sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "TCP host connected from %s", ip);
        tcp_client_task(fd, payload);
    }
}
#endif

/* ── App exit event ─────────────────────────────────────────────────────── */

void lx_devlink_app_exited(const char *path, int code, bool forced,
                           uint32_t leaked_allocs, uint32_t leaked_files)
{
    char buf[300];
    jbuf_t j = { .buf = buf, .cap = sizeof(buf) };
    jcat(&j, "{\"ev\":\"app_exit\",\"path\":");
    jstr(&j, path);
    jcat(&j, ",\"code\":%d,\"forced\":%s,\"leaked_allocs\":%lu,\"leaked_files\":%lu}", code,
         forced ? "true" : "false", (unsigned long)leaked_allocs, (unsigned long)leaked_files);
    dl_send_async(DL_EVENT, 0, 0, j.buf, j.len, pdMS_TO_TICKS(200));
}

/* ── Pause ──────────────────────────────────────────────────────────────────
 * While an app has USB-C as a device of its own (lx_usbd) the serial/JTAG unit is off the pins: anything
 * queued for it can never leave, and its "TX FIFO empty" interrupt then fires without end (an interrupt
 * storm, then the interrupt watchdog). So DevLink stops sending and the unit's interrupts are masked until
 * the port comes back. */
void lx_devlink_pause(bool pause)
{
#if CONFIG_LX_DEVLINK_UART
    /* UART0 is independent of the USB-C device port claimed by apps. */
    (void)pause;
    return;
#else
    if (pause == s_paused) return;
    if (pause) {
        s_paused = true;
        if (s_tx_lock && xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {   /* no send mid-frame */
            usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(50));
            xSemaphoreGive(s_tx_lock);
        }
        usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY | USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
        usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY | USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
    } else {
        usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY | USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT);
        usb_serial_jtag_ll_ena_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_OUT_RECV_PKT | USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);
        s_last_serial_host_us = -HOST_ALIVE_US;   /* the host says hello again before anything is sent */
        s_paused = false;
    }
#endif
}

bool lx_devlink_network_enabled(void)
{
#if CONFIG_LX_DEVLINK_TCP
    return true;
#else
    return false;
#endif
}

/* ── Start ──────────────────────────────────────────────────────────────── */

esp_err_t lx_devlink_start(void)
{
#if CONFIG_LX_DEVLINK_UART
    /* The CH340 on the Elecrow AIO is wired to UART0: TX GPIO 37, RX GPIO 38. */
    esp_err_t err = uart_driver_install(DEVLINK_UART, 8 * 1024, 4 * 1024, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART0 DevLink driver install failed: %s", esp_err_to_name(err));
        return err;
    }
    const uart_config_t uart_cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_param_config(DEVLINK_UART, &uart_cfg);
    if (err == ESP_OK) err = uart_set_pin(DEVLINK_UART, 37, 38, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART0 DevLink configuration failed: %s", esp_err_to_name(err));
        return err;
    }
#else
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = 40 * 1024,
        .tx_buffer_size = 16 * 1024,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB-Serial-JTAG driver install failed: %s", esp_err_to_name(err));
        return err;
    }
#endif
    s_rx = heap_caps_malloc(DL_MAX_PAYLOAD + 1, MALLOC_CAP_SPIRAM);
    s_tx = heap_caps_malloc(DL_MAX_PAYLOAD, MALLOC_CAP_SPIRAM);
    s_tx_lock = xSemaphoreCreateMutex();
    s_dispatch_lock = xSemaphoreCreateMutex();
#if CONFIG_LX_DEVLINK_TCP
    s_tcp_tx_lock = xSemaphoreCreateMutex();
#endif
    if (!s_rx || !s_tx || !s_tx_lock || !s_dispatch_lock
#if CONFIG_LX_DEVLINK_TCP
        || !s_tcp_tx_lock
#endif
        ) return ESP_ERR_NO_MEM;

    s_orig_vprintf = esp_log_set_vprintf(dl_vprintf);
    if (xTaskCreatePinnedToCore(devlink_task, "devlink", 8192, NULL, 4, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
#if CONFIG_LX_DEVLINK_TCP
    esp_err_t pin_err = load_tcp_pin();
    if (pin_err != ESP_OK) {
        ESP_LOGE(TAG, "TCP DevLink disabled: cannot load pairing PIN from NVS: %s",
                 esp_err_to_name(pin_err));
    } else if (xTaskCreatePinnedToCore(devlink_tcp_task, "devlink_tcp", 6144, NULL, 4, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "TCP DevLink task creation failed");
    }
#endif
#if CONFIG_LX_DEVLINK_UART
    ESP_LOGI(TAG, "DevLink ready on UART0 (CH340)");
#else
    ESP_LOGI(TAG, "DevLink ready on USB-Serial-JTAG");
#endif
    return ESP_OK;
}
