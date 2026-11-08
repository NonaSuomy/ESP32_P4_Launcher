/*
 * DevLink — remote development service over USB-Serial-JTAG.
 *
 * One task parses request frames and answers them. ESP_LOG output and app
 * logs are forwarded as LOG frames, but only while a host has talked to us in
 * the last few seconds (host tools send PING as keep-alive). With no host
 * listening nothing is written, so the device never stalls on a full USB FIFO.
 */
#include "lx_devlink.h"
#include "devlink_proto.h"
#include "lx_app.h"
#include "lx_common.h"
#include "bsp.h"
#include "lx_input.h"
#include "lx_net.h"
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
#include "esp_core_dump.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "devlink";

#define HOST_ALIVE_US      (10 * 1000 * 1000)
#define RX_TIMEOUT_MS      2000

static SemaphoreHandle_t s_tx_lock;
static vprintf_like_t    s_orig_vprintf;
static volatile int64_t  s_last_host_us = -HOST_ALIVE_US;
static uint8_t          *s_rx;        /* request payload  */
static char             *s_tx;        /* response scratch */

/* PUT state */
static FILE    *s_put_f;
static char     s_put_path[160];
static uint32_t s_put_size, s_put_crc, s_put_done, s_put_crc_run;

/* ── Framing ────────────────────────────────────────────────────────────── */

static bool host_alive(void)
{
    return usb_serial_jtag_is_connected() &&
           esp_timer_get_time() - s_last_host_us < HOST_ALIVE_US;
}

static bool write_all(const void *p, size_t len, TickType_t to)
{
    const uint8_t *b = p;
    while (len) {
        /* The driver queues into a 16 KB ring buffer: an item larger than that never fits */
        int n = usb_serial_jtag_write_bytes(b, len > 4096 ? 4096 : len, to);
        if (n <= 0) return false;
        b += n; len -= n;
    }
    return true;
}

static bool dl_send(uint8_t type, uint8_t flags, uint16_t seq,
                    const void *payload, uint32_t len, TickType_t lock_to)
{
    uint8_t hdr[DL_HDR_SIZE] = {
        DL_SOF0, DL_SOF1, type, flags, seq & 0xFF, seq >> 8,
        len & 0xFF, (len >> 8) & 0xFF, (len >> 16) & 0xFF, len >> 24,
    };
    uint32_t crc = lx_crc32(0, hdr + 2, DL_HDR_SIZE - 2);
    crc = lx_crc32(crc, payload, len);

    if (xSemaphoreTake(s_tx_lock, lock_to) != pdTRUE) return false;
    TickType_t to = pdMS_TO_TICKS(1000);
    bool ok = write_all(hdr, sizeof(hdr), to) &&
              (!len || write_all(payload, len, to)) &&
              write_all(&crc, 4, to);
    xSemaphoreGive(s_tx_lock);
    return ok;
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
    int r = s_orig_vprintf(fmt, ap);     /* keep the UART console */
    if (host_alive() && xSemaphoreGetMutexHolder(s_tx_lock) != xTaskGetCurrentTaskHandle()) {
        char buf[256];
        int n = vsnprintf(buf, sizeof(buf), fmt, ap2);
        if (n > 0) dl_send(DL_LOG, 0, 0, buf, n < (int)sizeof(buf) ? n : sizeof(buf) - 1, pdMS_TO_TICKS(20));
    }
    va_end(ap2);
    return r;
}

void lx_devlink_app_log(const char *text, size_t len)
{
    if (host_alive()) dl_send(DL_LOG, DL_F_APP, 0, text, len, pdMS_TO_TICKS(50));
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
    jcat(&j, ",\"caps\":%lu,\"display\":[%u,%u],\"sd\":%s,\"uptime_ms\":%lld,"
             "\"free_internal\":%u,\"free_psram\":%u,\"largest_psram\":%u,\"app_state\":\"%s\",\"app\":",
         (unsigned long)b->caps, b->display_w, b->display_h, bsp_sdcard_mounted() ? "true" : "false",
         esp_timer_get_time() / 1000,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
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

static void dispatch(uint8_t type, uint16_t seq, uint8_t *p, uint32_t len)
{
    p[len] = 0;   /* string payloads are NUL-terminated for free */
    const char *str = (const char *)p;

    switch (type) {
    case DL_PING:   reply_str(type, seq, "pong"); break;
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
        reply_str(type, seq, "ok");
        break;
    case DL_RM:
        if (remove(str) == 0 || rmdir(str) == 0) reply_str(type, seq, "ok");
        else reply_err(type, seq, "cannot remove %s (errno %d)", str, errno);
        break;
    case DL_RENAME: {
        const char *to = str + strlen(str) + 1;
        if ((const uint8_t *)to < p + len && rename(str, to) == 0) reply_str(type, seq, "ok");
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
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(200));
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
        break;
    case DL_CRASH: h_crash(seq); break;
    case DL_BOOTLOG: reply(type, seq, s_boot, s_boot ? s_boot_len : 0); break;
    case DL_SHOT: h_shot(seq, p, len); break;
    case DL_INPUT: h_input(seq, p, len); break;
    case DL_NET: {
        if (len && p[0]) lx_net_diag_start(p[0]);
        static char text[1600];
        text[0] = lx_net_diag_busy() ? '1' : '0';
        snprintf(text + 1, sizeof(text) - 1, "%s", lx_net_diag_text());
        reply_str(type, seq, text);
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

/* ── RX task ────────────────────────────────────────────────────────────── */

static bool read_exact(uint8_t *buf, size_t len, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (len) {
        int n = usb_serial_jtag_read_bytes(buf, len, pdMS_TO_TICKS(50));
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
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1 || c != DL_SOF0) continue;
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
        if (calc != crc) {
            reply_err(type, seq, "frame CRC mismatch");
            continue;
        }
        s_last_host_us = esp_timer_get_time();
        dispatch(type, seq, s_rx, len);
    }
}

/* ── App exit event ─────────────────────────────────────────────────────── */

void lx_devlink_app_exited(const char *path, int code, bool forced,
                           uint32_t leaked_allocs, uint32_t leaked_files)
{
    if (!host_alive()) return;
    char buf[300];
    jbuf_t j = { .buf = buf, .cap = sizeof(buf) };
    jcat(&j, "{\"ev\":\"app_exit\",\"path\":");
    jstr(&j, path);
    jcat(&j, ",\"code\":%d,\"forced\":%s,\"leaked_allocs\":%lu,\"leaked_files\":%lu}", code,
         forced ? "true" : "false", (unsigned long)leaked_allocs, (unsigned long)leaked_files);
    dl_send(DL_EVENT, 0, 0, j.buf, j.len, pdMS_TO_TICKS(200));
}

/* ── Start ──────────────────────────────────────────────────────────────── */

esp_err_t lx_devlink_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = 40 * 1024,
        .tx_buffer_size = 16 * 1024,
    };
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB-Serial-JTAG driver install failed: %s", esp_err_to_name(err));
        return err;
    }
    s_rx = heap_caps_malloc(DL_MAX_PAYLOAD + 1, MALLOC_CAP_SPIRAM);
    s_tx = heap_caps_malloc(DL_MAX_PAYLOAD, MALLOC_CAP_SPIRAM);
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_rx || !s_tx || !s_tx_lock) return ESP_ERR_NO_MEM;

    s_orig_vprintf = esp_log_set_vprintf(dl_vprintf);
    if (xTaskCreatePinnedToCore(devlink_task, "devlink", 8192, NULL, 4, NULL, 1) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "DevLink ready on USB-Serial-JTAG");
    return ESP_OK;
}
