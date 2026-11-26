/*
 * A test codec module (papp_build --module): poses as h264.papp to check the launcher's module loading
 * without FFmpeg. Each packet gives one grey 64x32 frame whose Y value is the packet's first byte; the work
 * is done on a thread of its own (checks threads, newlib's snprintf/malloc and errno per thread in a module).
 *   python papp-sdk/tools/papp_build.py --name dummy --src apps/codecs/dummy/dummy_module.c --libc --module
 *       --out build/codecs/dummy, then put dummy.papp as /sd/launcher/codecs/h264.papp (codec_test checks it)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "papp/papp.h"

typedef struct {
    uint8_t *y, *u, *v;
    int pending;                    /* a packet's value waiting to become a frame, -1 = none */
    int64_t pts;
    bool eos;
    char name[32];
} dummy_t;

static int s_ctor = 0;
__attribute__((constructor)) static void ctor(void) { s_ctor = 42; }

static bool supports(int codec) { return codec == PAPP_CODEC_H264; }

static int worker(void *arg)
{
    dummy_t *d = arg;
    errno = 0;
    snprintf(d->name, sizeof(d->name), "dummy %d %.1f", s_ctor, 1.5);
    return strtol("123", NULL, 10);
}

static int open_(const papp_dec_cfg_t *cfg, void **out)
{
    dummy_t *d = calloc(1, sizeof(*d));
    if (!d) return PAPP_E_NOMEM;
    d->y = malloc(64 * 32);
    d->u = malloc(32 * 16);
    d->v = malloc(32 * 16);
    d->pending = -1;
    papp_thread_cfg_t tc = { .name = "dummy", .stack_size = 8192, .prio = cfg->prio, .core = -1 };
    papp_thread_t *t = papp_thread_create(worker, d, &tc);
    int code = 0;
    if (!t || !papp_if_thrd()->thread_join(t, 2000, &code) || code != 123 || strcmp(d->name, "dummy 42 1.5")) {
        papp_log("dummy module: thread check failed (%d, '%s')\n", code, d->name);
        return PAPP_E_IO;
    }
    memset(d->u, 128, 32 * 16);
    memset(d->v, 128, 32 * 16);
    *out = d;
    return PAPP_E_OK;
}

static int send_(void *ctx, const void *data, size_t len, int64_t pts)
{
    dummy_t *d = ctx;
    if (!data) { d->eos = true; return PAPP_E_OK; }
    if (d->pending >= 0) return PAPP_E_LIMIT;
    d->pending = len ? ((const uint8_t *)data)[0] : 0;
    d->pts = pts;
    return PAPP_E_OK;
}

static int receive_frame(void *ctx, papp_dec_frame_t *f)
{
    dummy_t *d = ctx;
    if (d->pending < 0) return d->eos ? PAPP_E_EOF : 0;
    memset(d->y, d->pending, 64 * 32);
    memset(f, 0, sizeof(*f));
    f->y = d->y; f->u = d->u; f->v = d->v;
    f->w = 64; f->h = 32; f->stride_y = 64; f->stride_uv = 32;
    f->pts = d->pts;
    f->keyframe = 1;
    d->pending = -1;
    return 1;
}

static int receive_audio(void *ctx, int16_t *pcm, size_t max, papp_dec_audio_t *info) { return PAPP_E_ARG; }
static void flush(void *ctx) { ((dummy_t *)ctx)->pending = -1; }
static void close_(void *ctx)
{
    dummy_t *d = ctx;
    free(d->y); free(d->u); free(d->v); free(d);
}

static const papp_codec_module_t s_table = {
    .magic = PAPP_CODEC_MODULE_MAGIC, .version = PAPP_CODEC_MODULE_VER, .max_cores = 1, .name = "dummy test module",
    .supports = supports, .open = open_, .send = send_, .receive_frame = receive_frame,
    .receive_audio = receive_audio, .flush = flush, .close = close_,
};

const void *papp_module_table(void) { return &s_table; }
