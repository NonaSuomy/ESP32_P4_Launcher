/*
 * CODC (papp_abi.h): decoders for video and audio, and the P4's hardware JPEG engine.
 *
 * MP3 and AAC are built in (esp_audio_codec); H.264 is a codec module from the SD card (CODEC_DIR: FFmpeg's
 * decoder, too big for the launcher's slot), loaded on an app's first open and unloaded when it ends.
 *
 * JPEG: decode and encode in RGB565.
 *
 * The engine reads and writes memory over the 2D-DMA, whose output buffers must be cache-line aligned and sized,
 * so the pictures go through buffers of the launcher's own (grown as needed, freed when the app ends) and are
 * copied to and from the app's memory: about 1-2 ms for a 640x360 frame, much less than decoding it in software.
 * The engines are made on the first call and deleted when the app ends; one picture at a time (s_lock).
 */
#include "lx_services.h"

#include <string.h>
#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <sys/stat.h>
#include "esp_timer.h"
#include "lx_app.h"
#include "esp_audio_simple_dec.h"
#include "esp_mp3_dec.h"
#include "esp_aac_dec.h"

static const char *TAG = "lx_codec";

#define JPEG_TIMEOUT_MS  1000

static SemaphoreHandle_t s_lock;
static jpeg_decoder_handle_t s_dec;
static jpeg_encoder_handle_t s_enc;
static void  *s_out;                 /* the engine's output: decoded pixels / encoded JPEG */
static size_t s_out_size;
static void  *s_in;                  /* encoder input when the app's picture has a stride */
static size_t s_in_size;

static int err_of(esp_err_t e)
{
    return e == ESP_OK ? PAPP_E_OK : e == ESP_ERR_NO_MEM ? PAPP_E_NOMEM : e == ESP_ERR_INVALID_ARG ? PAPP_E_ARG :
           e == ESP_ERR_INVALID_SIZE ? PAPP_E_LIMIT : PAPP_E_IO;
}

static SemaphoreHandle_t s_dec_lock;           /* the decoder slots and the module list */

void lx_svc_codec_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_dec_lock = xSemaphoreCreateMutex();
    esp_mp3_dec_register();
    esp_aac_dec_register();
}

static void take(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

/* A buffer of the engine's alignment, at least size bytes */
static void *grow(void **buf, size_t *have, size_t size, bool enc, bool input)
{
    if (size <= *have) return *buf;
    heap_caps_free(*buf);
    *buf = NULL;
    *have = 0;
    size_t got = 0;
    if (enc) {
        jpeg_encode_memory_alloc_cfg_t c = { .buffer_direction = input ? JPEG_ENC_ALLOC_INPUT_BUFFER : JPEG_ENC_ALLOC_OUTPUT_BUFFER };
        *buf = jpeg_alloc_encoder_mem(size, &c, &got);
    } else {
        jpeg_decode_memory_alloc_cfg_t c = { .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER };
        *buf = jpeg_alloc_decoder_mem(size, &c, &got);
    }
    if (*buf) *have = got;
    return *buf;
}

static int codc_jpeg_info(const void *jpg, size_t len, uint16_t *w, uint16_t *h)
{
    if (!jpg || len < 4) return PAPP_E_ARG;
    SVC_ENTER();
    jpeg_decode_picture_info_t pi = { 0 };
    esp_err_t e = jpeg_decoder_get_info(jpg, len, &pi);
    SVC_LEAVE();
    if (e != ESP_OK || !pi.width || !pi.height) return PAPP_E_ARG;
    if (w) *w = (uint16_t)pi.width;
    if (h) *h = (uint16_t)pi.height;
    return PAPP_E_OK;
}

static int codc_jpeg_decode(const void *jpg, size_t len, uint16_t *out, int stride_px, size_t out_size,
                            uint16_t *w_out, uint16_t *h_out)
{
    if (!jpg || len < 4 || !out || stride_px < 0) return PAPP_E_ARG;
    SVC_ENTER();
    int r;
    jpeg_decode_picture_info_t pi = { 0 };
    if (jpeg_decoder_get_info(jpg, len, &pi) != ESP_OK || !pi.width || !pi.height || pi.width > 8192 || pi.height > 8192) {
        r = PAPP_E_ARG;
        goto done;
    }
    int w = (int)pi.width, h = (int)pi.height, stride = stride_px ? stride_px : w;
    if ((w & 7) || (h & 7)) {                           /* the engine's limit */
        r = PAPP_E_UNSUPPORTED;
        goto done;
    }
    if (stride < w || (size_t)stride * (h - 1) * 2 + (size_t)w * 2 > out_size) {
        r = PAPP_E_LIMIT;
        goto done;
    }
    /* The engine writes whole blocks: 16 x 16 pixels for 4:2:0, 16 x 8 for 4:2:2, 8 x 8 otherwise */
    int bw = pi.sample_method == JPEG_DOWN_SAMPLING_YUV420 || pi.sample_method == JPEG_DOWN_SAMPLING_YUV422 ? 16 : 8;
    int bh = pi.sample_method == JPEG_DOWN_SAMPLING_YUV420 ? 16 : 8;
    int pw = (w + bw - 1) / bw * bw, ph = (h + bh - 1) / bh * bh;
    take();
    esp_err_t e = ESP_OK;
    if (!s_dec) {
        jpeg_decode_engine_cfg_t c = { .timeout_ms = JPEG_TIMEOUT_MS };
        e = jpeg_new_decoder_engine(&c, &s_dec);
    }
    if (e == ESP_OK && !grow(&s_out, &s_out_size, (size_t)pw * ph * 2, false, false)) e = ESP_ERR_NO_MEM;
    if (e == ESP_OK) {
        jpeg_decode_cfg_t cfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,        /* little-endian RGB565, as the display */
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t got = 0;
        e = jpeg_decoder_process(s_dec, &cfg, jpg, len, s_out, s_out_size, &got);
    }
    if (e == ESP_OK) {
        const uint16_t *src = s_out;
        for (int y = 0; y < h; y++) memcpy(out + (size_t)y * stride, src + (size_t)y * pw, (size_t)w * 2);
        if (w_out) *w_out = (uint16_t)w;
        if (h_out) *h_out = (uint16_t)h;
    } else {
        ESP_LOGW(TAG, "jpeg decode %dx%d: %s", w, h, esp_err_to_name(e));
    }
    xSemaphoreGive(s_lock);
    r = e == ESP_ERR_INVALID_ARG || e == ESP_ERR_INVALID_STATE || e == ESP_FAIL || e == ESP_ERR_TIMEOUT ? PAPP_E_IO : err_of(e);
done:
    SVC_LEAVE();
    return r;
}

static int codc_jpeg_encode(const uint16_t *pixels, int w, int h, int stride_px, int quality, void *out,
                            size_t out_size)
{
    if (!pixels || !out || w < 1 || h < 1 || w > 8192 || h > 8192 || stride_px < 0 || quality < 0 || quality > 100)
        return PAPP_E_ARG;
    int stride = stride_px ? stride_px : w;
    if (stride < w) return PAPP_E_ARG;
    SVC_ENTER();
    take();
    esp_err_t e = ESP_OK;
    if (!s_enc) {
        jpeg_encode_engine_cfg_t c = { .timeout_ms = JPEG_TIMEOUT_MS };
        e = jpeg_new_encoder_engine(&c, &s_enc);
    }
    const void *src = pixels;
    size_t src_size = (size_t)w * h * 2;
    if (e == ESP_OK && stride != w) {                   /* the engine takes w x h without gaps */
        if (!grow(&s_in, &s_in_size, src_size, true, true)) e = ESP_ERR_NO_MEM;
        else {
            for (int y = 0; y < h; y++) memcpy((uint16_t *)s_in + (size_t)y * w, pixels + (size_t)y * stride, (size_t)w * 2);
            src = s_in;
        }
    }
    /* A JPEG of an RGB565 picture is (much) smaller than the picture: room for the picture's size is plenty */
    size_t room = out_size < src_size + 4096 ? out_size : src_size + 4096;
    if (e == ESP_OK && !grow(&s_out, &s_out_size, room, true, false)) e = ESP_ERR_NO_MEM;
    uint32_t got = 0;
    if (e == ESP_OK) {
        jpeg_encode_cfg_t cfg = {
            .width = (uint32_t)w, .height = (uint32_t)h,
            .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
            .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
            .image_quality = quality ? (uint32_t)quality : 80,
        };
        e = jpeg_encoder_process(s_enc, &cfg, src, src_size, s_out, room, &got);
    }
    int r;
    if (e == ESP_OK && got > out_size) r = PAPP_E_LIMIT;
    else if (e == ESP_OK) {
        memcpy(out, s_out, got);
        r = (int)got;
    } else {
        /* the engine fails in several ways when the JPEG outgrows its buffer */
        r = e == ESP_ERR_NO_MEM ? PAPP_E_NOMEM : room < src_size + 4096 ? PAPP_E_LIMIT : PAPP_E_IO;
        if (r != PAPP_E_LIMIT) ESP_LOGW(TAG, "jpeg encode %dx%d: %s", w, h, esp_err_to_name(e));
    }
    xSemaphoreGive(s_lock);
    SVC_LEAVE();
    return r;
}

/* ── Decoders ───────────────────────────────────────────────────────────────
 * Each open decoder is a slot: its implementation's table (papp_codec_module_t: built in, or a module's)
 * and the implementation's context. Built-in ones run inside service calls; a module's code is the app's
 * own (it runs on the app's threads, outside SVC_ENTER, like any app code). */

#define MAX_DEC      8
#define MAX_MODS     4
#define CODEC_DIR    "/sd/launcher/codecs"

struct papp_dec {
    const papp_codec_module_t *ops;
    void *ctx;
    bool  builtin;
    bool  video;
};
static struct papp_dec s_decs[MAX_DEC];

static bool is_video(int codec) { return codec == PAPP_CODEC_H264; }

static const char *codec_file(int codec)
{
    switch (codec) {
    case PAPP_CODEC_H264: return "h264";
    case PAPP_CODEC_MP3:  return "mp3";
    case PAPP_CODEC_AAC:  return "aac";
    default:              return NULL;
    }
}

/* -- Built in: MP3 and AAC, esp_audio_codec's simple decoder (finds the frames in any split of the data) -- */

typedef struct {
    esp_audio_simple_dec_handle_t h;
    uint8_t *in;                    /* what was sent and not yet decoded (a frame's start can wait here) */
    size_t   in_len, in_pos, in_cap;
    bool     need_more;             /* the decoder used what it could: send may add */
    bool     eos;
    bool     pts_given;
    int64_t  pts;
    uint8_t  codec;
} adec_t;

static bool adec_supports(int codec) { return codec == PAPP_CODEC_MP3 || codec == PAPP_CODEC_AAC; }

/* AudioSpecificConfig (ISO 14496-3): object type 5 bits (31 = escape), rate index 4 bits (15 = 24-bit rate),
 * channel configuration 4 bits */
typedef struct { const uint8_t *p; size_t n, i; uint32_t bits; int have; bool bad; } bitrd_t;

static int getbits(bitrd_t *b, int nb)
{
    while (b->have < nb) {
        if (b->i >= b->n) { b->bad = true; return 0; }
        b->bits = b->bits << 8 | b->p[b->i++];
        b->have += 8;
    }
    b->have -= nb;
    return (int)((b->bits >> b->have) & ((1u << nb) - 1));
}

static bool parse_asc(const uint8_t *p, size_t n, int *rate, int *ch)
{
    static const int rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000,
                                   11025, 8000, 7350 };
    bitrd_t b = { .p = p, .n = n };
    if (getbits(&b, 5) == 31) getbits(&b, 6);
    int idx = getbits(&b, 4);
    if (idx == 15) {
        int hi = getbits(&b, 12);
        *rate = hi << 12 | getbits(&b, 12);
    } else if (idx < 13) {
        *rate = rates[idx];
    } else {
        return false;
    }
    int c = getbits(&b, 4);
    *ch = c == 1 ? 1 : 2;           /* the decoder down-mixes to what it is told: at most stereo */
    return !b.bad && *rate > 0;
}

static int adec_open(const papp_dec_cfg_t *cfg, void **out)
{
    adec_t *d = calloc(1, sizeof(*d));
    if (!d) return PAPP_E_NOMEM;
    d->codec = cfg->codec;
    esp_aac_dec_cfg_t aac = ESP_AAC_DEC_CONFIG_DEFAULT();
    esp_audio_simple_dec_cfg_t c = { .dec_type = cfg->codec == PAPP_CODEC_MP3 ? ESP_AUDIO_SIMPLE_DEC_TYPE_MP3
                                                                            : ESP_AUDIO_SIMPLE_DEC_TYPE_AAC };
    if (cfg->codec == PAPP_CODEC_AAC) {
        aac.aac_plus_enable = true;
        if (cfg->extra && cfg->extra_len) {        /* raw frames from an MP4: one packet = one frame */
            int rate, ch;
            if (!parse_asc(cfg->extra, cfg->extra_len, &rate, &ch)) { free(d); return PAPP_E_ARG; }
            aac.no_adts_header = true;
            aac.sample_rate = rate;
            aac.channel = (uint8_t)ch;
            c.use_frame_dec = true;
        }
        c.dec_cfg = &aac;
        c.cfg_size = sizeof(aac);
    }
    esp_audio_err_t e = esp_audio_simple_dec_open(&c, &d->h);
    if (e != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "audio decoder open: %d", (int)e);
        free(d);
        return e == ESP_AUDIO_ERR_MEM_LACK ? PAPP_E_NOMEM : PAPP_E_UNSUPPORTED;
    }
    *out = d;
    return PAPP_E_OK;
}

static int adec_send(void *ctx, const void *data, size_t len, int64_t pts)
{
    adec_t *d = ctx;
    if (!data) { d->eos = true; return PAPP_E_OK; }
    if (d->eos) return PAPP_E_ARG;                 /* after the end: flush first */
    if (!len) return PAPP_E_OK;
    if (d->in_pos < d->in_len && !d->need_more) return PAPP_E_LIMIT;
    size_t left = d->in_len - d->in_pos;
    if (left + len > d->in_cap) {
        size_t cap = (left + len + 4095) & ~(size_t)4095;
        uint8_t *n = malloc(cap);
        if (!n) return PAPP_E_NOMEM;
        if (left) memcpy(n, d->in + d->in_pos, left);
        free(d->in);
        d->in = n;
        d->in_cap = cap;
    } else if (left && d->in_pos) {
        memmove(d->in, d->in + d->in_pos, left);
    }
    memcpy(d->in + left, data, len);
    d->in_len = left + len;
    d->in_pos = 0;
    d->need_more = false;
    d->pts = pts;
    d->pts_given = false;
    return PAPP_E_OK;
}

static int adec_receive_frame(void *ctx, papp_dec_frame_t *f) { return PAPP_E_ARG; }

static int adec_receive_audio(void *ctx, int16_t *pcm, size_t max, papp_dec_audio_t *info)
{
    adec_t *d = ctx;
    for (;;) {
        size_t left = d->in_len - d->in_pos;
        if (!left || d->need_more) {
            d->need_more = true;
            return d->eos ? PAPP_E_EOF : 0;
        }
        esp_audio_simple_dec_raw_t raw = { .buffer = d->in + d->in_pos, .len = (uint32_t)left, .eos = d->eos };
        esp_audio_simple_dec_out_t out = { .buffer = (uint8_t *)pcm, .len = (uint32_t)(max * 4) };
        esp_audio_err_t e = esp_audio_simple_dec_process(d->h, &raw, &out);
        if (e == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) return PAPP_E_LIMIT;
        if (e != ESP_AUDIO_ERR_OK && e != ESP_AUDIO_ERR_CONTINUE) {
            /* damaged data: drop what is waiting and carry on with the next packet */
            ESP_LOGD(TAG, "audio decode: %d", (int)e);
            d->in_pos = d->in_len;
            continue;
        }
        d->in_pos += raw.consumed > left ? left : raw.consumed;
        if (out.decoded_size) {
            esp_audio_simple_dec_info_t di = { 0 };
            esp_audio_simple_dec_get_info(d->h, &di);
            int ch = di.channel ? di.channel : 2;
            if (info) {
                memset(info, 0, sizeof(*info));
                info->pts = d->pts_given ? PAPP_PTS_NONE : d->pts;
                info->rate = di.sample_rate;
                info->channels = (uint8_t)ch;
            }
            d->pts_given = true;
            return (int)(out.decoded_size / (2 * ch));
        }
        if (!raw.consumed) d->need_more = true;    /* the rest is the start of a frame */
    }
}

static void adec_flush(void *ctx)
{
    adec_t *d = ctx;
    esp_audio_simple_dec_reset(d->h);
    d->in_len = d->in_pos = 0;
    d->need_more = d->eos = false;
}

static void adec_close(void *ctx)
{
    adec_t *d = ctx;
    esp_audio_simple_dec_close(d->h);
    free(d->in);
    free(d);
}

static const papp_codec_module_t s_builtin_audio = {
    .magic = PAPP_CODEC_MODULE_MAGIC, .version = PAPP_CODEC_MODULE_VER, .max_cores = 1,
    .name = "esp_audio_codec 2.5.0",
    .supports = adec_supports, .open = adec_open, .send = adec_send, .receive_frame = adec_receive_frame,
    .receive_audio = adec_receive_audio, .flush = adec_flush, .close = adec_close,
};

/* -- Modules from the SD card ----------------------------------------------------------------------- */

typedef struct {
    int codec;
    lx_module_t *m;
    const papp_codec_module_t *ops;
} module_t;
static module_t s_mods[MAX_MODS];

static void module_path(int codec, char *buf, size_t n)
{
    snprintf(buf, n, CODEC_DIR "/%s.papp", codec_file(codec));
}

/* The module for codec, loaded on first use (s_dec_lock held) */
static const papp_codec_module_t *module_get(int codec)
{
    for (int i = 0; i < MAX_MODS; i++)
        if (s_mods[i].ops && s_mods[i].codec == codec) return s_mods[i].ops;
    module_t *slot = NULL;
    for (int i = 0; i < MAX_MODS && !slot; i++)
        if (!s_mods[i].m) slot = &s_mods[i];
    if (!slot || !codec_file(codec)) return NULL;
    char path[64], err[128] = "";
    module_path(codec, path, sizeof(path));
    struct stat st;
    if (stat(path, &st) != 0) return NULL;
    lx_module_t *m = NULL;
    papp_start_fn_t entry = NULL;
    int64_t t0 = esp_timer_get_time();
    if (lx_module_load(path, &m, &entry, err, sizeof(err)) != ESP_OK) {
        ESP_LOGW(TAG, "codec module %s: %s", path, err);
        return NULL;
    }
    const papp_codec_module_t *ops = NULL;
    char *argv[2] = { (char *)&ops, NULL };
    int r = entry(PAPP_MODULE_ARGC, argv);      /* the module's start-up, on this (the app's) thread */
    if (r != 0 || !ops || ops->magic != PAPP_CODEC_MODULE_MAGIC || ops->version != PAPP_CODEC_MODULE_VER ||
        !ops->supports || !ops->open || !ops->send || !ops->receive_frame || !ops->receive_audio ||
        !ops->flush || !ops->close) {
        ESP_LOGW(TAG, "codec module %s: not a codec module (start %d)", path, r);
        lx_module_unload(m);
        return NULL;
    }
    slot->codec = codec;
    slot->m = m;
    slot->ops = ops;
    ESP_LOGI(TAG, "codec module %s: %s, up to %d cores (%lld ms)", path, ops->name ? ops->name : "?",
             ops->max_cores, (esp_timer_get_time() - t0) / 1000);
    return ops;
}

static bool dec_valid(const papp_dec_t *d)
{
    return d >= s_decs && d < s_decs + MAX_DEC && d->ops;
}

static int codc_codec_info(int codec, papp_codec_info_t *out)
{
    if (!out || !codec_file(codec)) return PAPP_E_ARG;
    memset(out, 0, sizeof(*out));
    if (s_builtin_audio.supports(codec)) {
        out->available = 1;
        out->max_cores = 1;
        snprintf(out->name, sizeof(out->name), "%s %s", s_builtin_audio.name, codec_file(codec));
        return PAPP_E_OK;
    }
    SVC_ENTER();
    out->module = 1;
    xSemaphoreTake(s_dec_lock, portMAX_DELAY);
    const papp_codec_module_t *ops = NULL;
    for (int i = 0; i < MAX_MODS; i++)
        if (s_mods[i].ops && s_mods[i].codec == codec) ops = s_mods[i].ops;
    xSemaphoreGive(s_dec_lock);
    if (ops) {
        out->available = 1;
        out->max_cores = ops->max_cores;
        snprintf(out->name, sizeof(out->name), "%s", ops->name ? ops->name : "");
    } else {
        char path[64];
        struct stat st;
        module_path(codec, path, sizeof(path));
        out->available = stat(path, &st) == 0;
        snprintf(out->name, sizeof(out->name), "%s.papp", codec_file(codec));
    }
    SVC_LEAVE();
    return PAPP_E_OK;
}

static int codc_open(const papp_dec_cfg_t *cfg, papp_dec_t **out)
{
    if (!cfg || !out || cfg->cores > 2 || (cfg->extra_len && !cfg->extra)) return PAPP_E_ARG;
    *out = NULL;
    if (!codec_file(cfg->codec)) return PAPP_E_UNSUPPORTED;
    SVC_ENTER();
    int r = PAPP_E_OK;
    xSemaphoreTake(s_dec_lock, portMAX_DELAY);
    papp_dec_t *d = NULL;
    for (int i = 0; i < MAX_DEC && !d; i++)
        if (!s_decs[i].ops) d = &s_decs[i];
    const papp_codec_module_t *ops = NULL;
    bool builtin = s_builtin_audio.supports(cfg->codec);
    if (!d) r = PAPP_E_LIMIT;
    else if (builtin) ops = &s_builtin_audio;
    else if (!(ops = module_get(cfg->codec)) || !ops->supports(cfg->codec)) r = PAPP_E_UNSUPPORTED;
    if (r == PAPP_E_OK) {
        d->ops = ops;                               /* the slot is taken while it opens */
        d->builtin = builtin;
        d->video = is_video(cfg->codec);
    }
    xSemaphoreGive(s_dec_lock);
    if (r == PAPP_E_OK) {
        void *ctx = NULL;
        if (builtin) {
            r = ops->open(cfg, &ctx);
        } else {
            SVC_LEAVE();                            /* a module's code is the app's */
            r = ops->open(cfg, &ctx);
            SVC_ENTER();
        }
        if (r == PAPP_E_OK) {
            d->ctx = ctx;
            *out = d;
        } else {
            d->ops = NULL;
        }
    }
    SVC_LEAVE();
    return r;
}

/* Built-in decoders run inside a service call, a module's as the app's own code */
#define DEC_CALL(d, expr) ({ __typeof__(expr) _r; if ((d)->builtin) { SVC_ENTER(); _r = (expr); SVC_LEAVE(); } \
                             else { _r = (expr); } _r; })

static int codc_send(papp_dec_t *d, const void *data, size_t len, int64_t pts)
{
    if (!dec_valid(d) || (!data && len)) return PAPP_E_ARG;
    return DEC_CALL(d, d->ops->send(d->ctx, data, len, pts));
}

static int codc_receive_frame(papp_dec_t *d, papp_dec_frame_t *f)
{
    if (!dec_valid(d) || !f || !d->video) return PAPP_E_ARG;
    return DEC_CALL(d, d->ops->receive_frame(d->ctx, f));
}

static int codc_receive_audio(papp_dec_t *d, int16_t *pcm, size_t max, papp_dec_audio_t *info)
{
    if (!dec_valid(d) || !pcm || !max || d->video) return PAPP_E_ARG;
    return DEC_CALL(d, d->ops->receive_audio(d->ctx, pcm, max, info));
}

static void codc_flush(papp_dec_t *d)
{
    if (!dec_valid(d)) return;
    if (d->builtin) { SVC_ENTER(); d->ops->flush(d->ctx); SVC_LEAVE(); }
    else d->ops->flush(d->ctx);
}

static void codc_close(papp_dec_t *d)
{
    if (!dec_valid(d)) return;
    const papp_codec_module_t *ops = d->ops;
    void *ctx = d->ctx;
    if (d->builtin) { SVC_ENTER(); ops->close(ctx); SVC_LEAVE(); }
    else ops->close(ctx);
    xSemaphoreTake(s_dec_lock, portMAX_DELAY);
    d->ops = NULL;
    d->ctx = NULL;
    xSemaphoreGive(s_dec_lock);
}

const papp_if_codc_t lx_if_codc = {
    .hdr = { PAPP_IF_CODC, PAPP_IF_CODC_VER, sizeof(papp_if_codc_t) },
    .jpeg_info = codc_jpeg_info,
    .jpeg_decode = codc_jpeg_decode,
    .jpeg_encode = codc_jpeg_encode,
    .codec_info = codc_codec_info,
    .open = codc_open,
    .send = codc_send,
    .receive_frame = codc_receive_frame,
    .receive_audio = codc_receive_audio,
    .flush = codc_flush,
    .close = codc_close,
};

/* No app thread is left in a call: the engines, buffers, decoders and modules go with the app. A module's
 * decoders are not closed (their memory and threads are the app's, already reclaimed): the module is
 * unloaded. */
void lx_svc_codec_end_run(void)
{
    if (s_dec) { jpeg_del_decoder_engine(s_dec); s_dec = NULL; }
    if (s_enc) { jpeg_del_encoder_engine(s_enc); s_enc = NULL; }
    heap_caps_free(s_out);
    heap_caps_free(s_in);
    s_out = s_in = NULL;
    s_out_size = s_in_size = 0;
    for (int i = 0; i < MAX_DEC; i++) {
        if (s_decs[i].ops && s_decs[i].builtin && s_decs[i].ctx) s_decs[i].ops->close(s_decs[i].ctx);
        memset(&s_decs[i], 0, sizeof(s_decs[i]));
    }
    for (int i = 0; i < MAX_MODS; i++) {
        if (s_mods[i].m) lx_module_unload(s_mods[i].m);
        memset(&s_mods[i], 0, sizeof(s_mods[i]));
    }
}
