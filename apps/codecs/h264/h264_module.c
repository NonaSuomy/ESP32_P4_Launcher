/*
 * h264.papp: the launcher's H.264 codec module (papp_abi.h, CODC): FFmpeg's decoder (LGPL 2.1+), loaded from
 * /sd/launcher/codecs/h264.papp on an app's first open of PAPP_CODEC_H264.
 *
 * Packets: with cfg.extra (an MP4's avcC) each packet is one access unit as the container has it; without,
 * the data is an Annex B byte stream in pieces of any size, which FFmpeg's parser cuts into access units.
 * Access units the decoder can't take yet wait in a queue, so send never has to refuse a piece it already
 * cut: it answers PAPP_E_LIMIT only while that queue is not empty.
 *
 * cores = 2: frame threading (two frames decoded at once, one on each core; one frame more delay).
 *
 * Every FFmpeg call runs on a thread of the decoder's own (the runner), never on the app's: FFmpeg needs
 * ~20 KB of stack in places (the Annex B parser, table set-up), more than an app's main thread has (16 KB).
 * With one core the runner decodes (32 KB internal stack); with two it only parses and hands frames to
 * FFmpeg's two workers (pthread_shim.c), so its stack goes to PSRAM.
 */
#include <string.h>
#include "papp/papp.h"
#include "pthread_shim.h"
#include "libavcodec/avcodec.h"
#include "libavutil/frame.h"
#include "libavutil/log.h"
#include "libavutil/pixfmt.h"

typedef struct {
    AVCodecContext *ctx;
    AVCodecParserContext *parser;   /* Annex B streams */
    AVFrame *frame;                 /* the picture receive_frame gave last */
    AVPacket **q;                   /* access units waiting for the decoder: q[q_head .. q_head + q_count) */
    int q_head, q_count, q_cap;
    bool eos, eos_sent;
    int bad;                        /* packets the decoder refused (damaged) */
    void *runner;                   /* runner_t */
} dec_t;

static bool h264_supports(int codec) { return codec == PAPP_CODEC_H264; }

/* -- The runner: one call at a time, the caller waits -- */

typedef struct job job_t;
struct job {
    int (*fn)(job_t *j);
    dec_t *d;
    const papp_dec_cfg_t *cfg;
    const void *data;
    size_t len;
    int64_t pts;
    papp_dec_frame_t *f;
    int ret;
};

typedef struct {
    papp_thread_t *t;
    papp_sem_t *go, *done;
    job_t *job;
    volatile bool quit;
} runner_t;

static int runner_main(void *arg)
{
    runner_t *r = arg;
    for (;;) {
        papp_if_thrd()->sem_take(r->go, PAPP_WAIT_FOREVER);
        if (r->quit) return 0;
        r->job->ret = r->job->fn(r->job);
        papp_if_thrd()->sem_give(r->done);
    }
}

static int run(runner_t *r, job_t *j)
{
    r->job = j;
    papp_if_thrd()->sem_give(r->go);
    papp_if_thrd()->sem_take(r->done, PAPP_WAIT_FOREVER);
    return j->ret;
}

static runner_t *runner_new(int cores, int prio)
{
    const papp_if_thrd_t *th = papp_if_thrd();
    runner_t *r = av_mallocz(sizeof(*r));
    if (!r) return NULL;
    r->go = th->sem_create(0, 1);
    r->done = th->sem_create(0, 1);
    papp_thread_cfg_t cfg = { .name = "h264", .stack_size = cores > 1 ? 48 * 1024 : 32 * 1024,
                              .prio = (uint8_t)prio, .core = -1,
                              .flags = cores > 1 ? PAPP_THREAD_STACK_PSRAM : 0 };
    if (r->go && r->done) r->t = papp_thread_create(runner_main, r, &cfg);
    if (!r->t) {
        if (r->go) th->sem_destroy(r->go);
        if (r->done) th->sem_destroy(r->done);
        av_free(r);
        return NULL;
    }
    return r;
}

static void runner_end(runner_t *r)
{
    const papp_if_thrd_t *th = papp_if_thrd();
    r->quit = true;
    th->sem_give(r->go);
    th->thread_join(r->t, PAPP_WAIT_FOREVER, NULL);
    th->sem_destroy(r->go);
    th->sem_destroy(r->done);
    av_free(r);
}

static void log_cb(void *avcl, int level, const char *fmt, va_list vl)
{
    if (level > AV_LOG_ERROR) return;
    char line[160];
    vsnprintf(line, sizeof(line), fmt, vl);
    papp_log("h264: %s", line);
}

static int do_open(const papp_dec_cfg_t *cfg, void **out)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) return PAPP_E_UNSUPPORTED;
    dec_t *d = av_mallocz(sizeof(*d));
    if (!d) return PAPP_E_NOMEM;
    d->ctx = avcodec_alloc_context3(codec);
    d->frame = av_frame_alloc();
    if (!d->ctx || !d->frame) goto nomem;
    if (cfg->extra && cfg->extra_len) {
        d->ctx->extradata = av_mallocz(cfg->extra_len + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!d->ctx->extradata) goto nomem;
        memcpy(d->ctx->extradata, cfg->extra, cfg->extra_len);
        d->ctx->extradata_size = (int)cfg->extra_len;
    } else {
        d->parser = av_parser_init(AV_CODEC_ID_H264);
        if (!d->parser) goto nomem;
    }
    int cores = cfg->cores ? cfg->cores : 2;
    d->ctx->thread_count = cores;
    d->ctx->thread_type = cores > 1 ? FF_THREAD_FRAME : 0;
    shim_set_prio(cfg->prio);
    int r = avcodec_open2(d->ctx, codec, NULL);
    if (r < 0) {
        papp_log("h264: open failed (%d)\n", r);
        avcodec_free_context(&d->ctx);
        av_frame_free(&d->frame);
        if (d->parser) av_parser_close(d->parser);
        av_free(d);
        return r == AVERROR(ENOMEM) ? PAPP_E_NOMEM : PAPP_E_ARG;
    }
    *out = d;
    return PAPP_E_OK;
nomem:
    if (d->ctx) avcodec_free_context(&d->ctx);
    if (d->frame) av_frame_free(&d->frame);
    av_free(d);
    return PAPP_E_NOMEM;
}

/* Hands the queue to the decoder until it wants its output taken first */
static void pump(dec_t *d)
{
    while (d->q_count) {
        AVPacket *p = d->q[d->q_head];
        int r = avcodec_send_packet(d->ctx, p);
        if (r == AVERROR(EAGAIN)) return;
        if (r < 0 && r != AVERROR_EOF) d->bad++;
        av_packet_free(&d->q[d->q_head]);
        d->q_head++;
        d->q_count--;
    }
    d->q_head = 0;
    if (d->eos && !d->eos_sent) {
        int r = avcodec_send_packet(d->ctx, NULL);
        if (r != AVERROR(EAGAIN)) d->eos_sent = true;
    }
}

static int enqueue(dec_t *d, const uint8_t *data, int len, int64_t pts)
{
    if (d->q_head + d->q_count == d->q_cap) {
        if (d->q_head) {
            memmove(d->q, d->q + d->q_head, d->q_count * sizeof(*d->q));
            d->q_head = 0;
        } else {
            int cap = d->q_cap ? d->q_cap * 2 : 8;
            AVPacket **n = av_realloc_array(d->q, cap, sizeof(*n));
            if (!n) return PAPP_E_NOMEM;
            d->q = n;
            d->q_cap = cap;
        }
    }
    AVPacket *p = av_packet_alloc();
    if (!p || av_new_packet(p, len) < 0) {
        av_packet_free(&p);
        return PAPP_E_NOMEM;
    }
    memcpy(p->data, data, len);
    p->pts = pts;
    d->q[d->q_head + d->q_count] = p;
    d->q_count++;
    return PAPP_E_OK;
}

static int do_send(void *ctx, const void *data, size_t len, int64_t pts)
{
    dec_t *d = ctx;
    pump(d);
    if (!data) {
        if (d->parser && !d->eos) {              /* the parser's last access unit */
            uint8_t *au;
            int au_len;
            av_parser_parse2(d->parser, d->ctx, &au, &au_len, NULL, 0, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (au_len) enqueue(d, au, au_len, d->parser->pts);
        }
        d->eos = true;
        pump(d);
        return PAPP_E_OK;
    }
    if (d->eos) return PAPP_E_ARG;
    if (d->q_count) return PAPP_E_LIMIT;         /* output waiting: receive it first */
    if (len > 0x7FFFFFF) return PAPP_E_ARG;
    int r = PAPP_E_OK;
    if (!d->parser) {
        r = enqueue(d, data, (int)len, pts);
    } else {
        const uint8_t *p = data;
        int left = (int)len;
        while (left > 0 && r == PAPP_E_OK) {
            uint8_t *au;
            int au_len;
            int used = av_parser_parse2(d->parser, d->ctx, &au, &au_len, p, left, pts, AV_NOPTS_VALUE, 0);
            if (used < 0) return PAPP_E_IO;
            p += used;
            left -= used;
            if (au_len) r = enqueue(d, au, au_len, d->parser->pts);
        }
    }
    pump(d);
    return r;
}

static int do_receive_frame(void *ctx, papp_dec_frame_t *f)
{
    dec_t *d = ctx;
    av_frame_unref(d->frame);
    for (int tries = 0; tries < 2; tries++) {
        int r = avcodec_receive_frame(d->ctx, d->frame);
        if (r == AVERROR_EOF) return PAPP_E_EOF;
        if (r == AVERROR(EAGAIN)) {
            if (!d->q_count && !(d->eos && !d->eos_sent)) return 0;
            pump(d);
            continue;
        }
        if (r < 0) return PAPP_E_IO;
        AVFrame *fr = d->frame;
        if (fr->format != AV_PIX_FMT_YUV420P && fr->format != AV_PIX_FMT_YUVJ420P) {
            av_frame_unref(fr);
            return PAPP_E_UNSUPPORTED;           /* 4:2:2, 4:4:4, 10-bit: not for blit_yuv */
        }
        memset(f, 0, sizeof(*f));
        f->y = fr->data[0];
        f->u = fr->data[1];
        f->v = fr->data[2];
        f->w = (uint16_t)fr->width;
        f->h = (uint16_t)fr->height;
        f->stride_y = (uint16_t)fr->linesize[0];
        f->stride_uv = (uint16_t)fr->linesize[1];
        f->pts = fr->pts != AV_NOPTS_VALUE ? fr->pts : fr->best_effort_timestamp;
        f->keyframe = (fr->flags & AV_FRAME_FLAG_KEY) != 0;
        if (fr->color_range == AVCOL_RANGE_JPEG || fr->format == AV_PIX_FMT_YUVJ420P) f->yuv_flags |= PAPP_YUV_FULL_RANGE;
        if (fr->colorspace == AVCOL_SPC_BT709 || (fr->colorspace == AVCOL_SPC_UNSPECIFIED && fr->height >= 720))
            f->yuv_flags |= PAPP_YUV_BT709;
        pump(d);                                 /* room again: give it the next access unit */
        return 1;
    }
    return 0;
}

static int h264_receive_audio(void *ctx, int16_t *pcm, size_t max, papp_dec_audio_t *info) { return PAPP_E_ARG; }

static void do_flush(void *ctx)
{
    dec_t *d = ctx;
    av_frame_unref(d->frame);
    while (d->q_count) {
        av_packet_free(&d->q[d->q_head]);
        d->q_head++;
        d->q_count--;
    }
    d->q_head = 0;
    avcodec_flush_buffers(d->ctx);
    if (d->parser) {
        av_parser_close(d->parser);
        d->parser = av_parser_init(AV_CODEC_ID_H264);
    }
    d->eos = d->eos_sent = false;
}

static void do_close(void *ctx)
{
    dec_t *d = ctx;
    do_flush(d);
    if (d->parser) av_parser_close(d->parser);
    av_frame_free(&d->frame);
    avcodec_free_context(&d->ctx);
    av_free(d->q);
    av_free(d);
}

/* -- The module's functions: each a job on the decoder's runner -- */

static int j_open(job_t *j)    { void *ctx = NULL; int r = do_open(j->cfg, &ctx); j->d = ctx; return r; }
static int j_send(job_t *j)    { return do_send(j->d, j->data, j->len, j->pts); }
static int j_receive(job_t *j) { return do_receive_frame(j->d, j->f); }
static int j_flush(job_t *j)   { do_flush(j->d); return 0; }
static int j_close(job_t *j)   { do_close(j->d); return 0; }

static int h264_open(const papp_dec_cfg_t *cfg, void **out)
{
    int cores = cfg->cores ? cfg->cores : 2;
    runner_t *r = runner_new(cores, cfg->prio);
    if (!r) return PAPP_E_NOMEM;
    job_t j = { .fn = j_open, .cfg = cfg };
    int ret = run(r, &j);
    if (ret != PAPP_E_OK) {
        runner_end(r);
        return ret;
    }
    j.d->runner = r;
    *out = j.d;
    return PAPP_E_OK;
}

static int h264_send(void *ctx, const void *data, size_t len, int64_t pts)
{
    dec_t *d = ctx;
    job_t j = { .fn = j_send, .d = d, .data = data, .len = len, .pts = pts };
    return run(d->runner, &j);
}

static int h264_receive_frame(void *ctx, papp_dec_frame_t *f)
{
    dec_t *d = ctx;
    job_t j = { .fn = j_receive, .d = d, .f = f };
    return run(d->runner, &j);
}

static void h264_flush(void *ctx)
{
    dec_t *d = ctx;
    job_t j = { .fn = j_flush, .d = d };
    run(d->runner, &j);
}

static void h264_close(void *ctx)
{
    dec_t *d = ctx;
    runner_t *r = d->runner;
    job_t j = { .fn = j_close, .d = d };
    run(r, &j);
    runner_end(r);
}

static const papp_codec_module_t s_table = {
    .magic = PAPP_CODEC_MODULE_MAGIC, .version = PAPP_CODEC_MODULE_VER, .max_cores = 2,
    .name = "FFmpeg " FFMPEG_VERSION_STR " h264",
    .supports = h264_supports, .open = h264_open, .send = h264_send, .receive_frame = h264_receive_frame,
    .receive_audio = h264_receive_audio, .flush = h264_flush, .close = h264_close,
};

const void *papp_module_table(void)
{
    shim_init();
    av_log_set_callback(log_cb);
    return &s_table;
}
