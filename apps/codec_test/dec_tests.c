/*
 * codec_test: the CODC decoders on test media in /sd/codec_test (made by make_media.py, transcode.ps1 and
 * mp4_to_packets.py: see README in this folder): tone.mp3, tone.adts, tone.aac.pkt = 10 s of stereo
 * 44.1 kHz, 440 Hz in even seconds and 660 Hz in odd ones; test.h264 / test.h264.pkt = 10 s of 640x360
 * at 25 fps (a yellow square moving right on dark blue).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "papp/papp.h"
#include "dec_tests.h"

#define DIR "/sd/codec_test/"

static int *s_fails, *s_tests;

static void result(bool ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void result(bool ok, const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    (*s_tests)++;
    if (!ok) (*s_fails)++;
    papp_log("%s %s\n", ok ? "ok  " : "FAIL", line);
}

static uint8_t *load(const char *name, size_t *len)
{
    FILE *f = fopen(name, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = papp_alloc(n > 0 ? n : 1, PAPP_MEM_LARGE, 4);
    if (b && fread(b, 1, n, f) != (size_t)n) { papp_free(b); b = NULL; }
    fclose(f);
    *len = (size_t)n;
    return b;
}

/* .pkt files (mp4_to_packets.py) */
typedef struct { const uint8_t *data; uint32_t len; int64_t pts; uint8_t key; } pkt_t;
typedef struct { uint8_t *buf; size_t len, pos; uint32_t codec, timescale, extra_len; const uint8_t *extra; } pktf_t;

static bool pkt_open(pktf_t *p, const char *name)
{
    memset(p, 0, sizeof(*p));
    p->buf = load(name, &p->len);
    if (!p->buf || p->len < 16 || memcmp(p->buf, "PKT1", 4)) return false;
    memcpy(&p->codec, p->buf + 4, 4);
    memcpy(&p->timescale, p->buf + 8, 4);
    memcpy(&p->extra_len, p->buf + 12, 4);
    p->extra = p->buf + 16;
    p->pos = 16 + p->extra_len;
    return true;
}

static bool pkt_next(pktf_t *p, pkt_t *k)
{
    if (p->pos + 16 > p->len) return false;
    memcpy(&k->len, p->buf + p->pos, 4);
    memcpy(&k->pts, p->buf + p->pos + 4, 8);
    k->key = p->buf[p->pos + 12];
    k->data = p->buf + p->pos + 16;
    p->pos += 16 + k->len;
    return p->pos <= p->len;
}

/* -- Audio -- */

typedef struct {
    int64_t samples;
    uint32_t rate;
    int channels;
    int crossings[10];               /* sign changes of the left channel in each second */
    int16_t last;
    int64_t first_pts;
    int pts_count;
} audio_stats_t;

static void audio_feed(audio_stats_t *st, const int16_t *pcm, int n, const papp_dec_audio_t *info)
{
    if (!st->rate) { st->rate = info->rate; st->channels = info->channels; }
    if (info->pts != PAPP_PTS_NONE) { if (!st->pts_count) st->first_pts = info->pts; st->pts_count++; }
    for (int i = 0; i < n; i++) {
        int16_t v = pcm[i * info->channels];
        int sec = (int)((st->samples + i) / (info->rate ? info->rate : 44100));
        if (v > -1000 && v < 1000) continue;     /* hysteresis: only loud samples count */
        if (sec < 10 && st->last && (v > 0) != (st->last > 0)) st->crossings[sec]++;
        st->last = v;
    }
    st->samples += n;
}

static void audio_check(const char *name, const audio_stats_t *st, int64_t us)
{
    bool rate_ok = st->rate == 44100 && st->channels == 2;
    /* encoders add some silence at the start and the end: +- 0.1 s */
    bool len_ok = st->samples > 44100 * 99 / 10 && st->samples < 44100 * 103 / 10;
    bool tone_ok = true;
    for (int s = 1; s < 9; s++) {            /* 440 Hz = 880 crossings a second, 660 Hz = 1320 */
        int want = s & 1 ? 1320 : 880;
        if (st->crossings[s] < want * 95 / 100 || st->crossings[s] > want * 105 / 100) tone_ok = false;
    }
    result(rate_ok && len_ok && tone_ok, "%s: %lu Hz x%d, %.2f s, tone %d/%d Hz, decoded in %lld ms (%.1f%% of a core)",
           name, (unsigned long)st->rate, st->channels, st->samples / 44100.0, st->crossings[2] / 2,
           st->crossings[3] / 2, us / 1000, us / 100000.0);
}

static int16_t s_pcm[PAPP_DEC_AUDIO_MAX * 2];

static int drain_audio(const papp_if_codc_t *c, papp_dec_t *d, audio_stats_t *st, int64_t *us)
{
    for (;;) {
        papp_dec_audio_t info;
        int64_t t0 = papp_time_us();
        int n = c->receive_audio(d, s_pcm, PAPP_DEC_AUDIO_MAX, &info);
        *us += papp_time_us() - t0;
        if (n <= 0) return n;
        audio_feed(st, s_pcm, n, &info);
    }
}

/* A byte stream (MP3, ADTS) sent in pieces of odd sizes, as a file reader would */
static void audio_stream(const papp_if_codc_t *c, int codec, const char *file, const char *name)
{
    size_t len;
    uint8_t *b = load(file, &len);
    if (!b) { papp_log("SKIP %s: no %s\n", name, file); return; }
    papp_dec_cfg_t cfg = { .codec = codec };
    papp_dec_t *d = NULL;
    int r = c->open(&cfg, &d);
    if (r != PAPP_E_OK) { result(false, "%s: open %d", name, r); papp_free(b); return; }
    audio_stats_t st = { 0 };
    int64_t us = 0;
    static const int sizes[] = { 1000, 1, 4096, 333, 7000, 17 };
    size_t pos = 0;
    int k = 0, bad = 0;
    while (pos < len) {
        size_t n = sizes[k++ % 6];
        if (n > len - pos) n = len - pos;
        int64_t t0 = papp_time_us();
        r = c->send(d, b + pos, n, (int64_t)pos);
        us += papp_time_us() - t0;
        if (r == PAPP_E_LIMIT) {                  /* output waiting */
            if (drain_audio(c, d, &st, &us) < 0) bad++;
            continue;
        }
        if (r != PAPP_E_OK) bad++;
        pos += n;
        if (drain_audio(c, d, &st, &us) < 0) bad++;
    }
    c->send(d, NULL, 0, 0);
    r = drain_audio(c, d, &st, &us);
    c->close(d);
    papp_free(b);
    if (bad || r != PAPP_E_EOF) result(false, "%s: %d errors, end %d", name, bad, r);
    else audio_check(name, &st, us);
}

/* Raw AAC frames from an MP4 with the AudioSpecificConfig */
static void audio_packets(const papp_if_codc_t *c, const char *file, const char *name)
{
    pktf_t pf;
    if (!pkt_open(&pf, file)) { papp_log("SKIP %s: no %s\n", name, file); papp_free(pf.buf); return; }
    papp_dec_cfg_t cfg = { .codec = (uint8_t)pf.codec, .extra = pf.extra, .extra_len = pf.extra_len };
    papp_dec_t *d = NULL;
    int r = c->open(&cfg, &d);
    if (r != PAPP_E_OK) { result(false, "%s: open %d", name, r); papp_free(pf.buf); return; }
    audio_stats_t st = { 0 };
    int64_t us = 0;
    int bad = 0, pts_bad = 0;
    pkt_t k;
    while (pkt_next(&pf, &k)) {
        int64_t t0 = papp_time_us();
        r = c->send(d, k.data, k.len, k.pts);
        us += papp_time_us() - t0;
        if (r != PAPP_E_OK) { bad++; continue; }
        int before = st.pts_count;
        if (drain_audio(c, d, &st, &us) < 0) bad++;
        if (st.pts_count > before && st.pts_count == 1 && st.first_pts != k.pts) pts_bad++;
    }
    c->send(d, NULL, 0, 0);
    r = drain_audio(c, d, &st, &us);
    c->close(d);
    papp_free(pf.buf);
    if (bad || pts_bad || r != PAPP_E_EOF) result(false, "%s: %d errors, %d pts wrong, end %d", name, bad, pts_bad, r);
    else audio_check(name, &st, us);
}

/* -- Video -- */

typedef struct {
    int frames, keyframes, bad_size;
    int64_t last_pts;
    int pts_order_bad;
    int square_bad;                  /* frames whose yellow square is not where it should be */
    uint16_t w, h;
} video_stats_t;

/* The square (make_media.py): x = (k * 8) % 720 - 80 .. + 80, y 140..220, yellow on dark blue. Y of the
 * yellow ~ 175, of the blue ~ 30: look along line 180 for where it is bright. */
static void video_feed(video_stats_t *st, const papp_dec_frame_t *f, int k)
{
    if (!st->frames) { st->w = f->w; st->h = f->h; }
    if (f->w != 640 || f->h != 360) st->bad_size++;
    st->frames++;
    if (f->keyframe) st->keyframes++;
    if (st->frames > 1 && f->pts <= st->last_pts) st->pts_order_bad++;
    st->last_pts = f->pts;
    int x0 = (k * 8) % 720 - 80, x1 = x0 + 80;
    const uint8_t *row = f->y + (size_t)180 * f->stride_y;
    int wrong = 0;
    for (int x = 4; x < 636; x += 8) {
        bool inside = x > x0 + 4 && x < x1 - 4, outside = x < x0 - 4 || x > x1 + 4;
        if ((inside && row[x] < 120) || (outside && row[x] > 80)) wrong++;
    }
    if (wrong > 2) st->square_bad++;
}


static int drain_video(const papp_if_codc_t *c, papp_dec_t *d, video_stats_t *st, int64_t *us,
                       int64_t ts_per_frame, papp_surface_t *s)
{
    for (;;) {
        papp_dec_frame_t f;
        int64_t t0 = papp_time_us();
        int r = c->receive_frame(d, &f);
        *us += papp_time_us() - t0;
        if (r != 1) return r;
        video_feed(st, &f, (int)(f.pts / ts_per_frame));
        if (s && (st->frames % 25) == 0) {
            papp_yuv_t y = { .y = f.y, .u = f.u, .v = f.v, .w = f.w, .h = f.h, .stride_y = f.stride_y,
                             .stride_uv = f.stride_uv, .dx = 80, .dy = 60, .flags = f.yuv_flags };
            if (papp_disp_blit_yuv(s, &y) == PAPP_E_OK) papp_disp_present(s);
        }
    }
}

static void video_check(const char *name, const video_stats_t *st, int64_t us, int frames)
{
    bool ok = st->frames == frames && !st->bad_size && !st->pts_order_bad && st->square_bad <= frames / 50 &&
              st->keyframes >= 1;
    result(ok, "%s: %d frames (%d key) %ux%u, %d misplaced, %d out of order, %lld ms = %.1f fps",
           name, st->frames, st->keyframes, st->w, st->h, st->square_bad, st->pts_order_bad, us / 1000,
           us ? st->frames * 1e6 / us : 0.0);
}

static void video_packets(const papp_if_codc_t *c, const char *file, int cores, papp_surface_t *s)
{
    char name[48];
    snprintf(name, sizeof(name), "h264 mp4 packets, %d core%s", cores, cores == 1 ? "" : "s");
    pktf_t pf;
    if (!pkt_open(&pf, file)) { papp_log("SKIP %s: no %s\n", name, file); papp_free(pf.buf); return; }
    papp_dec_cfg_t cfg = { .codec = PAPP_CODEC_H264, .cores = (uint8_t)cores, .prio = PAPP_PRIO_NORMAL,
                           .extra = pf.extra, .extra_len = pf.extra_len };
    papp_dec_t *d = NULL;
    int64_t t0 = papp_time_us();
    int r = c->open(&cfg, &d);
    int64_t open_us = papp_time_us() - t0;
    if (r != PAPP_E_OK) { result(false, "%s: open %d", name, r); papp_free(pf.buf); return; }
    papp_log("     %s: open %lld ms\n", name, open_us / 1000);
    video_stats_t st = { 0 };
    int64_t us = 0;
    int bad = 0, n = 0;
    pkt_t k;
    int64_t tpf = pf.timescale / 25;
    while (pkt_next(&pf, &k)) {
        n++;
        for (;;) {
            t0 = papp_time_us();
            r = c->send(d, k.data, k.len, k.pts);
            us += papp_time_us() - t0;
            if (r != PAPP_E_LIMIT) break;
            if (drain_video(c, d, &st, &us, tpf, s) < 0) { bad++; break; }
        }
        if (r != PAPP_E_OK) bad++;
        if (drain_video(c, d, &st, &us, tpf, s) < 0) bad++;
    }
    c->send(d, NULL, 0, 0);
    r = drain_video(c, d, &st, &us, tpf, s);
    c->close(d);
    papp_free(pf.buf);
    if (bad || r != PAPP_E_EOF) result(false, "%s: %d errors, end %d, %d frames", name, bad, r, st.frames);
    else video_check(name, &st, us, n);
}

/* Annex B, in pieces of any size (pts = byte offset: only their order is checked, and B-frames reorder) */
static void video_annexb(const papp_if_codc_t *c, const char *file, papp_surface_t *s)
{
    const char *name = "h264 annex b stream, 2 cores";
    size_t len;
    uint8_t *b = load(file, &len);
    if (!b) { papp_log("SKIP %s: no %s\n", name, file); return; }
    papp_dec_cfg_t cfg = { .codec = PAPP_CODEC_H264, .cores = 2 };
    papp_dec_t *d = NULL;
    int r = c->open(&cfg, &d);
    if (r != PAPP_E_OK) { result(false, "%s: open %d", name, r); papp_free(b); return; }
    int frames = 0, bad = 0;
    size_t pos = 0;
    int64_t us = 0;
    while (pos < len) {
        size_t n = len - pos < 5000 ? len - pos : 5000;
        r = c->send(d, b + pos, n, 0);
        if (r == PAPP_E_LIMIT) {
            papp_dec_frame_t f;
            while (c->receive_frame(d, &f) == 1) frames++;
            continue;
        }
        if (r != PAPP_E_OK) bad++;
        pos += n;
        papp_dec_frame_t f;
        int64_t t0 = papp_time_us();
        while (c->receive_frame(d, &f) == 1) frames++;
        us += papp_time_us() - t0;
    }
    c->send(d, NULL, 0, 0);
    papp_dec_frame_t f;
    while ((r = c->receive_frame(d, &f)) == 1) frames++;
    c->close(d);
    papp_free(b);
    result(!bad && r == PAPP_E_EOF && frames == 250, "%s: %d frames, %d errors, end %d", name, frames, bad, r);
}

/* The test module apps/codecs/dummy in h264.papp's place: one grey 64x32 frame per packet */
static bool dummy_module(const papp_if_codc_t *c)
{
    papp_dec_cfg_t cfg = { .codec = PAPP_CODEC_H264 };
    papp_dec_t *d = NULL;
    int64_t t0 = papp_time_us();
    int r = c->open(&cfg, &d);
    papp_codec_info_t ci;
    c->codec_info(PAPP_CODEC_H264, &ci);
    if (strncmp(ci.name, "dummy", 5)) {
        if (d) c->close(d);
        return false;
    }
    if (r != PAPP_E_OK) { result(false, "dummy module: open %d", r); return true; }
    papp_log("     dummy module loaded and opened in %lld ms\n",(papp_time_us() - t0) / 1000);
    int good = 0, limit_ok = 0;
    for (int i = 0; i < 5; i++) {
        uint8_t v = (uint8_t)(50 + i * 30);
        if (c->send(d, &v, 1, 1000 + i) != PAPP_E_OK) break;
        if (c->send(d, &v, 1, 1000 + i) == PAPP_E_LIMIT) limit_ok++;
        papp_dec_frame_t f;
        if (c->receive_frame(d, &f) == 1 && f.w == 64 && f.h == 32 && f.y[100] == v && f.pts == 1000 + i) good++;
        if (c->receive_frame(d, &f) != 0) good--;
    }
    c->send(d, NULL, 0, 0);
    papp_dec_frame_t f;
    r = c->receive_frame(d, &f);
    int16_t pcm[16];
    int ra = c->receive_audio(d, pcm, 8, NULL);
    c->close(d);
    result(good == 5 && limit_ok == 5 && r == PAPP_E_EOF && ra == PAPP_E_ARG,
           "dummy module: %d/5 frames, %d/5 LIMIT, end %d, audio on video %d", good, limit_ok, r, ra);
    /* several decoders of one module, and the slot limit */
    papp_dec_t *ds[9];
    int opened = 0;
    for (int i = 0; i < 9; i++) if (c->open(&cfg, &ds[i]) == PAPP_E_OK) opened++; else ds[i] = NULL;
    for (int i = 0; i < 9; i++) if (ds[i]) c->close(ds[i]);
    result(opened == 8, "dummy module: %d decoders open at once (8 allowed)", opened);
    return true;
}

void dec_tests(const papp_if_codc_t *c, papp_surface_t *s, int *fails, int *tests)
{
    s_fails = fails;
    s_tests = tests;
    papp_codec_info_t ci;
    static const char *names[] = { "", "", "h264", "mp3", "aac" };
    for (int k = PAPP_CODEC_H264; k <= PAPP_CODEC_AAC; k++) {
        if (c->codec_info(k, &ci) != PAPP_E_OK) { result(false, "codec_info %s", names[k]); continue; }
        papp_log("     %s: %s%s%s, %s, max cores %d\n", names[k], ci.available ? "available" : "not available",
                 ci.hardware ? ", hardware" : "", ci.module ? ", module" : "", ci.name, ci.max_cores);
    }
    result(c->codec_info(1, &ci) == PAPP_E_ARG && c->codec_info(99, &ci) == PAPP_E_ARG, "codec_info of unknown codecs");
    papp_dec_cfg_t bad = { .codec = 99 };
    papp_dec_t *d;
    result(c->open(&bad, &d) == PAPP_E_UNSUPPORTED, "open of an unknown codec");

    audio_stream(c, PAPP_CODEC_MP3, DIR "tone.mp3", "mp3 stream");
    audio_stream(c, PAPP_CODEC_AAC, DIR "tone.adts", "aac adts stream");
    audio_packets(c, DIR "tone.aac.pkt", "aac mp4 packets");

    if (c->codec_info(PAPP_CODEC_H264, &ci) == PAPP_E_OK && ci.available) {
        if (dummy_module(c)) return;
        video_packets(c, DIR "test.h264.pkt", 1, NULL);
        video_packets(c, DIR "test.h264.pkt", 2, s);
        video_annexb(c, DIR "test.h264", NULL);
    } else {
        papp_log("SKIP h264: no codec module (/sd/launcher/codecs/h264.papp)\n");
    }
}
