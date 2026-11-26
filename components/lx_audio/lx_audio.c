/*
 * Audio engine — see lx_audio.h.
 *
 * Each stream's ring holds S16 stereo frames at the stream's own rate (the
 * writer converts U8/mono on the way in). Single producer (the app) and single
 * consumer (the mixer task): head/tail are free-running counters, so the ring
 * itself needs no lock. s_lock only keeps a stream from being closed or
 * cleared while the mixer renders it.
 */
#include "lx_audio.h"
#include "bsp.h"

#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "lx_audio";

#define BLOCK_FRAMES   240          /* 5 ms at 48 kHz per mixer pass */
#define MIN_LATENCY_MS 20
#define DEF_LATENCY_MS 100
#define MIXER_PRIO     20
#define MIXER_CORE     1

struct lx_audio_stream {
    bool     used, paused, playing;
    int      rate, channels;
    lx_audio_fmt_t fmt;
    int32_t  gain;                  /* 0..256 */
    int16_t *ring;                  /* stereo frames */
    uint32_t cap;
    volatile uint32_t head, tail;   /* free running; head written by app, tail by mixer */
    uint32_t step, frac;            /* 16.16 source frames per output frame */
    int16_t  prev[2], cur[2];       /* interpolation endpoints */
    uint32_t underruns;
    uint64_t played;
    SemaphoreHandle_t space;        /* given after the mixer consumed frames */
};

static lx_audio_stream_t s_streams[LX_AUDIO_MAX_STREAMS];
static SemaphoreHandle_t s_lock, s_wake;
static int s_rate, s_master = 60;
static volatile bool s_hold;        /* every stream silent: launcher overlay */
static volatile lx_audio_tap_t s_tap;   /* output to a Bluetooth speaker instead */
static int32_t s_mix[BLOCK_FRAMES * 2];
static int16_t s_out[BLOCK_FRAMES * 2];

/* Resample + mix up to n output frames of one stream into s_mix. */
static void render(lx_audio_stream_t *s, int n)
{
    for (int i = 0; i < n; i++) {
        while (s->frac >= 0x10000) {
            uint32_t tail = s->tail;
            if (tail == __atomic_load_n(&s->head, __ATOMIC_ACQUIRE)) {
                if (s->playing) { s->underruns++; s->playing = false; }
                return;                         /* rest of the block stays silent */
            }
            const int16_t *f = s->ring + (tail % s->cap) * 2;
            s->prev[0] = s->cur[0]; s->prev[1] = s->cur[1];
            s->cur[0] = f[0];       s->cur[1] = f[1];
            __atomic_store_n(&s->tail, tail + 1, __ATOMIC_RELEASE);
            s->played++;
            s->playing = true;
            s->frac -= 0x10000;
        }
        int32_t t = s->frac >> 1;               /* 15 bits: diff * t fits in int32 */
        int32_t l = s->prev[0] + (((s->cur[0] - s->prev[0]) * t) >> 15);
        int32_t r = s->prev[1] + (((s->cur[1] - s->prev[1]) * t) >> 15);
        s_mix[i * 2]     += (l * s->gain) >> 8;
        s_mix[i * 2 + 1] += (r * s->gain) >> 8;
        s->frac += s->step;
    }
}

static void mixer_task(void *arg)
{
    for (;;) {
        bool any = false;
        memset(s_mix, 0, sizeof(s_mix));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < LX_AUDIO_MAX_STREAMS; i++) {
            lx_audio_stream_t *s = &s_streams[i];
            if (!s->used) continue;
            any = true;
            if (s->paused || s_hold) continue;
            uint32_t before = s->tail;
            render(s, BLOCK_FRAMES);
            if (s->tail != before) xSemaphoreGive(s->space);
        }
        xSemaphoreGive(s_lock);

        if (!any) {                             /* idle: nothing to play */
            xSemaphoreTake(s_wake, pdMS_TO_TICKS(200));
            continue;
        }
        for (int i = 0; i < BLOCK_FRAMES * 2; i++) {
            int32_t v = s_mix[i];
            s_out[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
        }
        lx_audio_tap_t tap = s_tap;
        if (tap) {
            tap(s_out, BLOCK_FRAMES);
            memset(s_out, 0, sizeof(s_out));
        }
        bsp_audio_write(s_out, BLOCK_FRAMES, 200);   /* paces the loop */
    }
}

esp_err_t lx_audio_init(void)
{
    if (s_lock) return ESP_OK;
    s_rate = bsp_audio_rate();
    if (s_rate <= 0) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    if (xTaskCreatePinnedToCore(mixer_task, "lx_mixer", 3072, NULL, MIXER_PRIO, NULL, MIXER_CORE) != pdPASS)
        return ESP_ERR_NO_MEM;
    bsp_audio_volume(s_master);
    ESP_LOGI(TAG, "mixer up: %d Hz, %d-frame blocks, %d streams", s_rate, BLOCK_FRAMES, LX_AUDIO_MAX_STREAMS);
    return ESP_OK;
}

int lx_audio_output_rate(void)
{
    return s_rate;
}

void lx_audio_master_volume(int percent)
{
    s_master = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    bsp_audio_volume(s_master);
}

void lx_audio_hold(bool hold)
{
    s_hold = hold;
}

void lx_audio_set_tap(lx_audio_tap_t tap)
{
    s_tap = tap;
    if (s_wake) xSemaphoreGive(s_wake);
}

int lx_audio_master_volume_get(void)
{
    return s_master;
}

lx_audio_stream_t *lx_audio_open(int rate, int channels, lx_audio_fmt_t fmt, int latency_ms)
{
    if (!s_lock || rate < 4000 || rate > 192000 || (channels != 1 && channels != 2) ||
        (fmt != LX_AUDIO_S16 && fmt != LX_AUDIO_U8))
        return NULL;
    if (latency_ms <= 0) latency_ms = DEF_LATENCY_MS;
    if (latency_ms < MIN_LATENCY_MS) latency_ms = MIN_LATENCY_MS;
    if (latency_ms > 2000) latency_ms = 2000;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    lx_audio_stream_t *s = NULL;
    for (int i = 0; i < LX_AUDIO_MAX_STREAMS; i++)
        if (!s_streams[i].used) { s = &s_streams[i]; break; }
    if (!s) goto out;

    uint32_t cap = (uint32_t)((int64_t)rate * latency_ms / 1000);
    int16_t *ring = heap_caps_malloc(cap * 4, MALLOC_CAP_SPIRAM);
    SemaphoreHandle_t space = xSemaphoreCreateBinary();
    if (!ring || !space) {
        heap_caps_free(ring);
        if (space) vSemaphoreDelete(space);
        s = NULL;
        goto out;
    }
    *s = (lx_audio_stream_t){
        .used = true, .rate = rate, .channels = channels, .fmt = fmt, .gain = 256,
        .ring = ring, .cap = cap, .space = space,
        .step = (uint32_t)(((uint64_t)rate << 16) / s_rate),
        .frac = 0x10000,                /* first output frame pulls a source frame */
    };
out:
    xSemaphoreGive(s_lock);
    if (s) {
        xSemaphoreGive(s_wake);
        ESP_LOGI(TAG, "open %d Hz %s %s, ring %lu frames", rate, channels == 1 ? "mono" : "stereo",
                 fmt == LX_AUDIO_U8 ? "U8" : "S16", (unsigned long)s->cap);
    }
    return s;
}

static void close_locked(lx_audio_stream_t *s)
{
    if (!s->used) return;
    ESP_LOGI(TAG, "close %d Hz: %llu frames played, %lu underruns", s->rate,
             (unsigned long long)s->played, (unsigned long)s->underruns);
    heap_caps_free(s->ring);
    vSemaphoreDelete(s->space);
    memset(s, 0, sizeof(*s));
}

static bool valid(lx_audio_stream_t *s)
{
    return s >= s_streams && s < s_streams + LX_AUDIO_MAX_STREAMS && s->used;
}

void lx_audio_close(lx_audio_stream_t *s)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (valid(s)) close_locked(s);
    xSemaphoreGive(s_lock);
}

void lx_audio_close_all(void)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < LX_AUDIO_MAX_STREAMS; i++) close_locked(&s_streams[i]);
    xSemaphoreGive(s_lock);
}

size_t lx_audio_write(lx_audio_stream_t *s, const void *data, size_t frames, uint32_t timeout_ms,
                      volatile bool *abort)
{
    if (!valid(s) || !data) return 0;
    const int bpf = s->channels * (s->fmt == LX_AUDIO_U8 ? 1 : 2);
    const uint8_t *src = data;
    TickType_t start = xTaskGetTickCount(), limit = pdMS_TO_TICKS(timeout_ms);
    size_t done = 0;

    while (done < frames) {
        uint32_t head = s->head;
        uint32_t room = s->cap - (head - __atomic_load_n(&s->tail, __ATOMIC_ACQUIRE));
        if (room == 0) {
            TickType_t waited = xTaskGetTickCount() - start;
            if (waited >= limit || (abort && *abort)) break;
            TickType_t slice = limit - waited < pdMS_TO_TICKS(20) ? limit - waited : pdMS_TO_TICKS(20);
            xSemaphoreTake(s->space, slice);
            continue;
        }
        uint32_t n = frames - done < room ? frames - done : room;
        for (uint32_t i = 0; i < n; i++, src += bpf) {
            int16_t *d = s->ring + ((head + i) % s->cap) * 2;
            int16_t l, r;
            if (s->fmt == LX_AUDIO_U8) {
                l = (int16_t)((src[0] - 128) << 8);
                r = s->channels == 2 ? (int16_t)((src[1] - 128) << 8) : l;
            } else {
                const int16_t *p = (const int16_t *)src;
                l = p[0];
                r = s->channels == 2 ? p[1] : l;
            }
            d[0] = l; d[1] = r;
        }
        __atomic_store_n(&s->head, head + n, __ATOMIC_RELEASE);
        done += n;
    }
    return done;
}

void lx_audio_pause(lx_audio_stream_t *s, bool pause)
{
    if (valid(s)) s->paused = pause;
}

void lx_audio_set_gain(lx_audio_stream_t *s, int percent)
{
    if (!valid(s)) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    s->gain = percent * 256 / 100;
}

void lx_audio_clear(lx_audio_stream_t *s)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (valid(s)) {
        __atomic_store_n(&s->tail, s->head, __ATOMIC_RELEASE);
        s->playing = false;
        xSemaphoreGive(s->space);
    }
    xSemaphoreGive(s_lock);
}

void lx_audio_stats(lx_audio_stream_t *s, lx_audio_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!valid(s)) return;
    out->queued_frames = s->head - s->tail;
    out->capacity_frames = s->cap;
    out->underruns = s->underruns;
    out->played_frames = s->played;
}
