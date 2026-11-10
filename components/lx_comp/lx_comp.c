/*
 * Display compositor — see lx_comp.h.
 *
 * Coordinates: apps and the launcher think in landscape (W x H). The panel may
 * be mounted rotated (bsp_info()->display_rot, counter-clockwise like the PPA's
 * rotation angle), so a landscape rect is mapped to its panel rect here and the
 * PPA rotates the pixels on the way.
 */
#include "lx_comp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "lx_comp";

#define MAX_BUFFERS   2
#define FLIP_WAIT_MS  100
#define PPA_SCALE_Q   16          /* PPA scale has 4 fractional bits */

struct lx_surface {
    int w, h;
    lx_scale_t scale;
    int nbuf, cur;
    uint16_t *buf[MAX_BUFFERS];
    lx_rect_t dst;                /* landscape rect on the display */
    float sx, sy;                 /* quantised PPA scale factors   */
    uint32_t frames;
    int64_t  t_first, t_ppa, t_wait;   /* stats */
};

static SemaphoreHandle_t s_lock;
static ppa_client_handle_t s_srm;
static ppa_client_handle_t s_blend;        /* lx_comp_blit, registered on first use */
static ppa_client_handle_t s_fill;         /* lx_comp_fill */
static void *s_tmp;                        /* lx_comp_blit: the scaled/turned picture */
static size_t s_tmp_size;
static lx_surface_t *s_surface;           /* v1: one surface at a time */
static bool s_fb_clean[2];                /* fb holds only black outside the surface rect */
static int s_dw, s_dh;                    /* landscape display size */

/* Async present: the worker waits for the flip, scales with the PPA and flips while the app
 * goes on with its next frame. One frame in flight: s_idle is taken by the presenter when it
 * queues a job and given back by the worker when the job is done. The worker holds s_lock
 * while it works, so everything else that takes s_lock is ordered with it. */
static SemaphoreHandle_t s_go, s_idle;
static struct { lx_surface_t *s; const void *src; } s_job;

static void wait_idle(void)
{
    if (!s_idle) return;
    xSemaphoreTake(s_idle, portMAX_DELAY);
    xSemaphoreGive(s_idle);
}

static esp_err_t present_job(lx_surface_t *s, const void *src);

static void present_worker(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_go, portMAX_DELAY);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_job.s && s_job.s == s_surface) present_job(s_job.s, s_job.src);
        s_job.s = NULL;
        xSemaphoreGive(s_lock);
        xSemaphoreGive(s_idle);
    }
}

esp_err_t lx_comp_init(void)
{
    if (s_lock) return ESP_OK;
    if (bsp_display_fb_count() < 2) return ESP_ERR_INVALID_STATE;
    const bsp_info_t *b = bsp_info();
    bool portrait = b->display_rot == 90 || b->display_rot == 270;
    s_dw = portrait ? b->display_h : b->display_w;
    s_dh = portrait ? b->display_w : b->display_h;

    ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_SRM, .max_pending_trans_num = 1 };
    esp_err_t err = ppa_register_client(&cfg, &s_srm);
    if (err != ESP_OK) return err;
    s_lock = xSemaphoreCreateMutex();
    s_go = xSemaphoreCreateBinary();
    SemaphoreHandle_t idle = xSemaphoreCreateBinary();
    if (s_go && idle) {
        xSemaphoreGive(idle);
        /* internal stack: the PPA driver and the flip may touch flash-cache-sensitive code */
        if (xTaskCreatePinnedToCore(present_worker, "lx_present", 4096, NULL, 15, NULL, tskNO_AFFINITY) == pdPASS)
            s_idle = idle;
    }
    if (!s_idle) ESP_LOGW(TAG, "no present worker: presents are synchronous");
    ESP_LOGI(TAG, "display %dx%d (panel %ux%u, rot %u)", s_dw, s_dh, b->display_w, b->display_h, b->display_rot);
    return ESP_OK;
}

void lx_comp_display_size(int *w, int *h)
{
    *w = s_dw;
    *h = s_dh;
}

/* Landscape rect -> panel native rect, inverse of the mapping in bsp.h. */
static void to_panel(const lx_rect_t *r, int *px, int *py)
{
    switch (bsp_info()->display_rot) {
    case 270: *px = s_dh - r->y - r->h; *py = r->x; break;
    case 90:  *px = r->y;               *py = s_dw - r->x - r->w; break;
    default:  *px = r->x;               *py = r->y; break;
    }
}

static float quantise(float s)
{
    int q = (int)(s * PPA_SCALE_Q);
    if (q < 1) q = 1;
    return (float)q / PPA_SCALE_Q;
}

static void layout(lx_surface_t *s)
{
    float fx = (float)s_dw / s->w, fy = (float)s_dh / s->h;
    switch (s->scale) {
    case LX_SCALE_STRETCH:
        s->sx = quantise(fx);
        s->sy = quantise(fy);
        break;
    case LX_SCALE_INTEGER: {
        int k = (int)(fx < fy ? fx : fy);
        s->sx = s->sy = k < 1 ? quantise(fx < fy ? fx : fy) : k;
        break;
    }
    case LX_SCALE_NONE:
        s->sx = s->sy = 1.0f;
        if (s->w > s_dw || s->h > s_dh) s->sx = s->sy = quantise(fx < fy ? fx : fy);
        break;
    default:
        s->sx = s->sy = quantise(fx < fy ? fx : fy);
        break;
    }
    s->dst.w = (int)(s->sx * s->w);
    s->dst.h = (int)(s->sy * s->h);
    s->dst.x = (s_dw - s->dst.w) / 2;
    s->dst.y = (s_dh - s->dst.h) / 2;
}

lx_surface_t *lx_comp_surface_create(int w, int h, lx_scale_t scale, int buffers)
{
    if (!s_lock || w <= 0 || h <= 0 || w > 2048 || h > 2048) return NULL;
    if (buffers < 1) buffers = 1;
    if (buffers > MAX_BUFFERS) buffers = MAX_BUFFERS;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    lx_surface_t *s = NULL;
    if (s_surface) {
        ESP_LOGW(TAG, "only one surface supported for now");
        goto out;
    }
    s = heap_caps_calloc(1, sizeof(*s), MALLOC_CAP_INTERNAL);
    if (!s) goto out;
    s->w = w; s->h = h; s->scale = scale; s->nbuf = buffers;
    size_t bytes = ((size_t)w * h * 2 + 63) & ~(size_t)63;
    for (int i = 0; i < buffers; i++) {
        s->buf[i] = heap_caps_aligned_calloc(64, 1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s->buf[i]) {
            for (int j = 0; j < i; j++) heap_caps_free(s->buf[j]);
            heap_caps_free(s);
            s = NULL;
            goto out;
        }
    }
    layout(s);
    s_fb_clean[0] = s_fb_clean[1] = false;
    s_surface = s;
    ESP_LOGI(TAG, "surface %dx%d x%d -> %dx%d at %d,%d (scale %.4g x %.4g)", w, h, buffers,
             s->dst.w, s->dst.h, s->dst.x, s->dst.y, s->sx, s->sy);
out:
    xSemaphoreGive(s_lock);
    return s;
}

static void destroy_locked(lx_surface_t *s)
{
    heap_caps_free(s_tmp);
    s_tmp = NULL;
    s_tmp_size = 0;
    if (s->frames > 1) {
        int64_t dt = esp_timer_get_time() - s->t_first;
        ESP_LOGI(TAG, "surface %dx%d: %lu frames, %.1f fps, PPA %.2f + flip wait %.2f ms/frame", s->w, s->h,
                 (unsigned long)s->frames, (s->frames - 1) * 1e6 / (dt ? dt : 1),
                 s->t_ppa / 1000.0 / s->frames, s->t_wait / 1000.0 / s->frames);
    }
    for (int i = 0; i < s->nbuf; i++) heap_caps_free(s->buf[i]);
    heap_caps_free(s);
    if (s == s_surface) s_surface = NULL;
}

void lx_comp_surface_destroy(lx_surface_t *s)
{
    if (!s || !s_lock) return;
    wait_idle();                            /* a queued frame still uses the surface */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s == s_surface) destroy_locked(s);
    xSemaphoreGive(s_lock);
}

void lx_comp_destroy_all(void)
{
    if (!s_lock) return;
    wait_idle();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_surface) destroy_locked(s_surface);
    xSemaphoreGive(s_lock);
}

uint16_t *lx_comp_lock(lx_surface_t *s, int *stride_px)
{
    if (!s || s != s_surface) return NULL;
    if (stride_px) *stride_px = s->w;
    return s->buf[s->cur];
}

static int blit_bpp(lx_blit_format_t f) { return f == LX_BLIT_ARGB8888 ? 4 : f == LX_BLIT_A8 ? 1 : 2; }

static color_pixel_rgb888_data_t rgb888(uint16_t c)
{
    return (color_pixel_rgb888_data_t){ .r = (uint8_t)((c >> 11) << 3), .g = (uint8_t)(((c >> 5) & 63) << 2),
                                        .b = (uint8_t)((c & 31) << 3) };
}

/* Scale / turn / mirror b's picture with the PPA's SRM engine into s_tmp; *out gets s_tmp as a
 * picture of the same format. The SRM only invalidates its output (no write-back), so it never
 * writes into the app's buffer directly: the blend below mixes s_tmp in. Called with s_lock. */
static esp_err_t transform(const lx_blit_t *b, int stride, lx_blit_t *out)
{
    int qx = b->scale_x16 < 1 ? 1 : b->scale_x16 > 255 ? 255 : b->scale_x16;
    int qy = b->scale_y16 < 1 ? 1 : b->scale_y16 > 255 ? 255 : b->scale_y16;
    int sw = (qx >> 4) * b->w + (qx & 15) * b->w / 16;      /* as the driver works it out */
    int sh = (qy >> 4) * b->h + (qy & 15) * b->h / 16;
    bool turned = b->rotate & 1;
    int tw = turned ? sh : sw, th = turned ? sw : sh;
    if (tw <= 0 || th <= 0) return ESP_ERR_INVALID_ARG;
    int bpp = blit_bpp(b->format);
    size_t need = ((size_t)tw * th * bpp + 63) & ~(size_t)63;
    if (need > s_tmp_size) {
        heap_caps_free(s_tmp);
        s_tmp = heap_caps_aligned_alloc(64, need, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        s_tmp_size = s_tmp ? need : 0;
        if (!s_tmp) return ESP_ERR_NO_MEM;
    }
    /* the SRM may round the last line or column away: start from "nothing there" */
    if (b->format == LX_BLIT_RGB565 && b->colorkey) {
        uint16_t *p = s_tmp;
        for (int i = 0; i < tw * th; i++) p[i] = b->key;
    } else {
        memset(s_tmp, 0, (size_t)tw * th * bpp);
    }
    esp_cache_msync(s_tmp, need, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    ppa_srm_color_mode_t cm = b->format == LX_BLIT_ARGB8888 ? PPA_SRM_COLOR_MODE_ARGB8888 : PPA_SRM_COLOR_MODE_RGB565;
    ppa_srm_oper_config_t op = {
        .in = { .buffer = b->pixels, .pic_w = stride, .pic_h = b->h, .block_w = b->w, .block_h = b->h, .srm_cm = cm },
        .out = { .buffer = s_tmp, .buffer_size = need, .pic_w = tw, .pic_h = th, .srm_cm = cm },
        .rotation_angle = b->rotate == 1 ? PPA_SRM_ROTATION_ANGLE_90 : b->rotate == 2 ? PPA_SRM_ROTATION_ANGLE_180 :
                          b->rotate == 3 ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = qx / 16.0f,
        .scale_y = qy / 16.0f,
        .mirror_x = b->mirror_x,
        .mirror_y = b->mirror_y,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_srm, &op);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "blit: PPA SRM failed: %s", esp_err_to_name(err));
        return err;
    }
    *out = *b;
    out->pixels = s_tmp;
    out->w = tw;
    out->h = th;
    return ESP_OK;
}

/* The PPA blend engine draws a picture into the surface's current buffer, in place (the buffer
 * is both its background and its output; the driver writes back and invalidates the rows). */
esp_err_t lx_comp_blit(lx_surface_t *s, const lx_blit_t *b)
{
    if (!s || !s_lock || !b || !b->pixels || b->w <= 0 || b->h <= 0 || (unsigned)b->format > LX_BLIT_A8)
        return ESP_ERR_INVALID_ARG;
    int stride = b->stride ? b->stride : b->w;
    if (stride < b->w) return ESP_ERR_INVALID_ARG;
    bool moved = b->rotate || b->scale_x16 != 16 || b->scale_y16 != 16 || b->mirror_x || b->mirror_y;
    if (moved && b->format == LX_BLIT_A8) return ESP_ERR_INVALID_ARG;   /* the SRM has no A8 */
    if (b->alpha == 0) return ESP_OK;                                  /* invisible */

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (s != s_surface) goto out;
    if (!s_blend) {
        ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_BLEND, .max_pending_trans_num = 1 };
        err = ppa_register_client(&cfg, &s_blend);
        if (err != ESP_OK) goto out;
    }
    lx_blit_t t;
    if (moved) {
        err = transform(b, stride, &t);
        if (err != ESP_OK) goto out;
        b = &t;
        stride = t.w;
    }
    /* clip to the surface */
    int sx = 0, sy = 0, dx = b->x, dy = b->y, w = b->w, h = b->h;
    if (dx < 0) { sx = -dx; w += dx; dx = 0; }
    if (dy < 0) { sy = -dy; h += dy; dy = 0; }
    if (dx + w > s->w) w = s->w - dx;
    if (dy + h > s->h) h = s->h - dy;
    err = ESP_OK;
    if (w <= 0 || h <= 0) goto out;                     /* all outside */

    uint16_t *buf = s->buf[s->cur];
    size_t bytes = ((size_t)s->w * s->h * 2 + 63) & ~(size_t)63;
    /* key range in RGB888: the 565 value with the low bits anything (covers either expansion) */
    color_pixel_rgb888_data_t lo = rgb888(b->key);
    color_pixel_rgb888_data_t hi = { .r = (uint8_t)(lo.r | 7), .g = (uint8_t)(lo.g | 3), .b = (uint8_t)(lo.b | 7) };
    ppa_blend_color_mode_t fcm = b->format == LX_BLIT_ARGB8888 ? PPA_BLEND_COLOR_MODE_ARGB8888 :
                                 b->format == LX_BLIT_A8 ? PPA_BLEND_COLOR_MODE_A8 : PPA_BLEND_COLOR_MODE_RGB565;
    ppa_blend_oper_config_t op = {
        .in_bg = { .buffer = buf, .pic_w = s->w, .pic_h = s->h, .block_w = w, .block_h = h,
                   .block_offset_x = dx, .block_offset_y = dy, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .in_fg = { .buffer = b->pixels, .pic_w = stride, .pic_h = b->h, .block_w = w, .block_h = h,
                   .block_offset_x = sx, .block_offset_y = sy, .blend_cm = fcm },
        .out = { .buffer = buf, .buffer_size = bytes, .pic_w = s->w, .pic_h = s->h,
                 .block_offset_x = dx, .block_offset_y = dy, .blend_cm = PPA_BLEND_COLOR_MODE_RGB565 },
        .bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .fg_alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .fg_fix_rgb_val = rgb888(b->color),
        .fg_ck_en = b->colorkey && b->format != LX_BLIT_A8,
        .fg_ck_rgb_low_thres = lo,
        .fg_ck_rgb_high_thres = hi,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    if (b->alpha > 0 && b->alpha < 255) {
        if (b->format == LX_BLIT_RGB565) {              /* no alpha of its own: set it */
            op.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
            op.fg_alpha_fix_val = (uint32_t)b->alpha;
        } else {                                        /* scale the picture's own alpha */
            op.fg_alpha_update_mode = PPA_ALPHA_SCALE;
            op.fg_alpha_scale_ratio = b->alpha / 256.0f;
        }
    }
    err = ppa_do_blend(s_blend, &op);
    if (err != ESP_OK) ESP_LOGE(TAG, "PPA blend failed: %s", esp_err_to_name(err));
out:
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t lx_comp_fill(lx_surface_t *s, int x, int y, int w, int h, uint16_t color)
{
    if (!s || !s_lock) return ESP_ERR_INVALID_ARG;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (s != s_surface) goto out;
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    err = ESP_OK;
    if (w <= 0 || h <= 0) goto out;
    if (!s_fill) {
        ppa_client_config_t cfg = { .oper_type = PPA_OPERATION_FILL, .max_pending_trans_num = 1 };
        err = ppa_register_client(&cfg, &s_fill);
        if (err != ESP_OK) goto out;
    }
    color_pixel_rgb888_data_t c = rgb888(color);
    ppa_fill_oper_config_t op = {
        .out = { .buffer = s->buf[s->cur], .buffer_size = ((size_t)s->w * s->h * 2 + 63) & ~(size_t)63,
                 .pic_w = s->w, .pic_h = s->h, .block_offset_x = x, .block_offset_y = y,
                 .fill_cm = PPA_FILL_COLOR_MODE_RGB565 },
        .fill_block_w = w,
        .fill_block_h = h,
        .fill_argb_color = { .a = 255, .r = c.r, .g = c.g, .b = c.b },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    err = ppa_do_fill(s_fill, &op);         /* the driver writes back and invalidates the rows */
    if (err != ESP_OK) ESP_LOGE(TAG, "PPA fill failed: %s", esp_err_to_name(err));
out:
    xSemaphoreGive(s_lock);
    return err;
}

lx_rect_t lx_comp_surface_rect(const lx_surface_t *s)
{
    return s->dst;
}

/* Wait for the flip, scale src onto the back framebuffer, flip. Called with s_lock. */
static esp_err_t present_job(lx_surface_t *s, const void *src)
{
    const bsp_info_t *b = bsp_info();
    int64_t t_w = esp_timer_get_time();
    bsp_display_wait_flip(FLIP_WAIT_MS);
    s->t_wait += esp_timer_get_time() - t_w;
    int back = bsp_display_visible() ^ 1;
    uint16_t *fb = bsp_display_fb(back);
    size_t fb_bytes = (size_t)b->display_w * b->display_h * 2;
    if (!s_fb_clean[back]) {
        memset(fb, 0, fb_bytes);
        esp_cache_msync(fb, fb_bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        s_fb_clean[back] = true;
    }

    int px, py;
    to_panel(&s->dst, &px, &py);
    ppa_srm_rotation_angle_t angle = b->display_rot == 90  ? PPA_SRM_ROTATION_ANGLE_90 :
                                     b->display_rot == 180 ? PPA_SRM_ROTATION_ANGLE_180 :
                                     b->display_rot == 270 ? PPA_SRM_ROTATION_ANGLE_270 :
                                                             PPA_SRM_ROTATION_ANGLE_0;
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = src,
            .pic_w = s->w, .pic_h = s->h,
            .block_w = s->w, .block_h = s->h,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = fb,
            .buffer_size = fb_bytes,
            .pic_w = b->display_w, .pic_h = b->display_h,
            .block_offset_x = px, .block_offset_y = py,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = angle,
        .scale_x = s->sx,
        .scale_y = s->sy,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = ppa_do_scale_rotate_mirror(s_srm, &op);
    int64_t t1 = esp_timer_get_time();
    if (err == ESP_OK) {
        bsp_display_show(back);
        if (s->frames++ == 0) s->t_first = t1;
        s->t_ppa += t1 - t0;
    } else {
        ESP_LOGE(TAG, "PPA SRM failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* pixels: the app's own frame (present_ext) or NULL = the surface's current buffer. With two
 * buffers the frame is handed to the worker and this returns at once (an app frame is first
 * copied into the surface's spare buffer, so the app may reuse its own right away); with one
 * buffer the app draws into the very buffer being scaled, so it waits as before. */
static esp_err_t present_pixels(lx_surface_t *s, const void *pixels)
{
    if (!s || !s_lock) return ESP_ERR_INVALID_ARG;
    int64_t t_w = esp_timer_get_time();
    if (s_idle) xSemaphoreTake(s_idle, portMAX_DELAY);     /* the previous frame is out */
    int64_t waited = esp_timer_get_time() - t_w;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (s != s_surface) goto fail;
    s->t_wait += waited;
    if (!s_idle || s->nbuf < 2) {
        err = present_job(s, pixels ? pixels : s->buf[s->cur]);
        if (err == ESP_OK && !pixels) s->cur = (s->cur + 1) % s->nbuf;
        goto fail;                                      /* not queued: release both */
    }
    const void *src = s->buf[s->cur];
    if (pixels) memcpy(s->buf[s->cur], pixels, (size_t)s->w * s->h * 2);   /* the driver writes it back */
    s->cur = (s->cur + 1) % s->nbuf;
    s_job.s = s;
    s_job.src = src;
    xSemaphoreGive(s_lock);
    xSemaphoreGive(s_go);
    return ESP_OK;
fail:
    xSemaphoreGive(s_lock);
    if (s_idle) xSemaphoreGive(s_idle);
    return err;
}

esp_err_t lx_comp_present(lx_surface_t *s)
{
    return present_pixels(s, NULL);
}

esp_err_t lx_comp_present_ext(lx_surface_t *s, const void *pixels)
{
    return pixels ? present_pixels(s, pixels) : ESP_ERR_INVALID_ARG;
}

esp_err_t lx_comp_launcher_present(const uint16_t *frame)
{
    if (!s_lock || !frame) return ESP_ERR_INVALID_ARG;
    wait_idle();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_surface) {                       /* an app owns the display */
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const bsp_info_t *b = bsp_info();
    size_t bytes = (size_t)s_dw * s_dh * 2;
    esp_cache_msync((void *)frame, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    bsp_display_wait_flip(FLIP_WAIT_MS);
    int back = bsp_display_visible() ^ 1;
    uint16_t *fb = bsp_display_fb(back);
    ppa_srm_oper_config_t op = {
        .in = {
            .buffer = frame,
            .pic_w = s_dw, .pic_h = s_dh,
            .block_w = s_dw, .block_h = s_dh,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = fb,
            .buffer_size = (size_t)b->display_w * b->display_h * 2,
            .pic_w = b->display_w, .pic_h = b->display_h,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .rotation_angle = b->display_rot == 90  ? PPA_SRM_ROTATION_ANGLE_90 :
                          b->display_rot == 180 ? PPA_SRM_ROTATION_ANGLE_180 :
                          b->display_rot == 270 ? PPA_SRM_ROTATION_ANGLE_270 : PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t err = ppa_do_scale_rotate_mirror(s_srm, &op);
    if (err == ESP_OK) {
        s_fb_clean[back] = false;
        bsp_display_show(back);
    } else {
        ESP_LOGE(TAG, "launcher present: PPA SRM failed: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(s_lock);
    return err;
}

int lx_comp_touch_read(lx_surface_t *s, bsp_touch_point_t *pts, int max)
{
    if (!s || s != s_surface) return -1;
    bsp_touch_point_t raw[BSP_TOUCH_MAX_POINTS];
    int n = bsp_touch_read(raw, BSP_TOUCH_MAX_POINTS), out = 0;
    for (int i = 0; i < n && out < max; i++) {
        int x = raw[i].x - s->dst.x, y = raw[i].y - s->dst.y;
        if (x < 0 || y < 0 || x >= s->dst.w || y >= s->dst.h) continue;
        pts[out] = raw[i];
        pts[out].x = x * s->w / s->dst.w;
        pts[out].y = y * s->h / s->dst.h;
        out++;
    }
    return n < 0 ? n : out;
}

static uint16_t *launcher_begin(bool over_app)
{
    if (!s_lock) return NULL;
    wait_idle();                        /* the app's last frame is on screen first */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_surface && !over_app) {
        xSemaphoreGive(s_lock);
        return NULL;
    }
    bsp_display_wait_flip(FLIP_WAIT_MS);
    int back = bsp_display_visible() ^ 1;
    s_fb_clean[back] = false;           /* the app's next present redraws the letterbox */
    return bsp_display_fb(back);
}

uint16_t *lx_comp_launcher_begin(void)
{
    return launcher_begin(false);
}

uint16_t *lx_comp_takeover_begin(void)
{
    return launcher_begin(true);
}

void lx_comp_copy_visible(uint16_t *dst)
{
    const bsp_info_t *b = bsp_info();
    size_t bytes = (size_t)b->display_w * b->display_h * 2;
    uint16_t *src = bsp_display_fb(bsp_display_visible());
    if (src != dst) memcpy(dst, src, bytes);
}

esp_err_t lx_comp_screenshot(char *path, size_t len)
{
    const bsp_info_t *b = bsp_info();
    int pw = b->display_w, ph = b->display_h, rot = b->display_rot;
    int w = rot == 90 || rot == 270 ? ph : pw, h = rot == 90 || rot == 270 ? pw : ph;
    int stride = (w * 2 + 3) & ~3;
    char name[48];
    struct stat st;
    mkdir("/sd/screenshots", 0775);
    for (int i = 1; i < 1000; i++) {
        snprintf(name, sizeof(name), "/sd/screenshots/shot_%03d.bmp", i);
        if (stat(name, &st) != 0) break;
    }
    uint16_t *row = heap_caps_calloc(1, stride, MALLOC_CAP_SPIRAM);
    FILE *f = row ? fopen(name, "wb") : NULL;
    if (!f) {
        free(row);
        return ESP_FAIL;
    }
    uint32_t hdr[16] = { 0 };           /* from byte 2: the 2-byte magic comes first */
    uint32_t image = (uint32_t)stride * h;
    hdr[0] = 66 + image;                /* file size */
    hdr[2] = 66;                        /* pixels offset */
    hdr[3] = 40;                        /* BITMAPINFOHEADER */
    hdr[4] = w;
    hdr[5] = h;                         /* positive: bottom row first */
    hdr[6] = 1 | (16u << 16);           /* planes, bits */
    hdr[7] = 3;                         /* BI_BITFIELDS */
    hdr[8] = image;
    hdr[13] = 0xF800;
    hdr[14] = 0x07E0;
    hdr[15] = 0x001F;
    fwrite("BM", 1, 2, f);
    fwrite(hdr, 4, 16, f);
    const uint16_t *px = bsp_display_fb(bsp_display_visible());
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            row[x] = rot == 270 ? px[x * pw + (h - 1 - y)] : rot == 90 ? px[(w - 1 - x) * pw + y] : px[y * pw + x];
        }
        fwrite(row, 1, stride, f);
    }
    fclose(f);
    free(row);
    ESP_LOGI(TAG, "saved %s", name);
    if (path) snprintf(path, len, "%s", name);
    return ESP_OK;
}

void lx_comp_launcher_end(void)
{
    const bsp_info_t *b = bsp_info();
    int back = bsp_display_visible() ^ 1;
    esp_cache_msync(bsp_display_fb(back), (size_t)b->display_w * b->display_h * 2,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    bsp_display_show(back);
    xSemaphoreGive(s_lock);
}
