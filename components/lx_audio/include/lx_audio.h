/*
 * Audio engine (ARCHITECTURE §7.4, first cut).
 *
 * The codec runs at one fixed rate (bsp_audio_rate()). Apps open streams at
 * any rate, mono/stereo, U8/S16; each stream has a ring buffer that the mixer
 * task drains, resamples (linear), mixes, clips and writes to the BSP.
 * Underruns play silence, never stall. Push model only for now.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LX_AUDIO_MAX_STREAMS 8

typedef enum { LX_AUDIO_S16 = 1, LX_AUDIO_U8 = 2 } lx_audio_fmt_t;

typedef struct lx_audio_stream lx_audio_stream_t;

typedef struct {
    uint32_t queued_frames;     /* source frames waiting in the ring  */
    uint32_t capacity_frames;
    uint32_t underruns;         /* times the ring ran dry while playing */
    uint64_t played_frames;     /* source frames consumed by the mixer */
} lx_audio_stats_t;

esp_err_t lx_audio_init(void);          /* after bsp_audio_init() */
int       lx_audio_output_rate(void);
void      lx_audio_master_volume(int percent);
int       lx_audio_master_volume_get(void);
void      lx_audio_hold(bool hold);     /* silence every stream; their rings keep what is queued */

/* Output somewhere else (a Bluetooth speaker): the mixer hands each block (S16 stereo at
 * lx_audio_output_rate(), master volume not applied) to `tap` and plays silence on the board, which
 * still paces it. NULL: back to the board. tap runs on the mixer task and must not block. */
typedef void (*lx_audio_tap_t)(const int16_t *frames, int n);
void      lx_audio_set_tap(lx_audio_tap_t tap);

lx_audio_stream_t *lx_audio_open(int rate, int channels, lx_audio_fmt_t fmt, int latency_ms);
void   lx_audio_close(lx_audio_stream_t *s);
void   lx_audio_close_all(void);        /* app teardown */
/* Copies up to `frames` source frames into the ring; waits for space up to
 * timeout_ms. `abort` (may be NULL) is polled while waiting. */
size_t lx_audio_write(lx_audio_stream_t *s, const void *data, size_t frames, uint32_t timeout_ms,
                      volatile bool *abort);
void   lx_audio_pause(lx_audio_stream_t *s, bool pause);
void   lx_audio_set_gain(lx_audio_stream_t *s, int percent);   /* 0..100, per stream */
void   lx_audio_clear(lx_audio_stream_t *s);                   /* drop queued audio  */
void   lx_audio_stats(lx_audio_stream_t *s, lx_audio_stats_t *out);

#ifdef __cplusplus
}
#endif
