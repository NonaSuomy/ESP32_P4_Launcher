/*
 * Display compositor (ARCHITECTURE §7.3, first cut).
 *
 * Owns the panel framebuffers. Apps draw into surfaces of any size; present()
 * scales + rotates the surface onto the hidden framebuffer with one PPA SRM
 * operation and flips at the next refresh. Launcher screens draw directly into
 * a framebuffer through lx_comp_launcher_begin/end while no surface exists.
 *
 * v1 limits: one surface at a time, RGB565 only, no overlay layer yet.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "bsp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LX_SCALE_FIT_ASPECT = 0,    /* largest size that keeps the aspect ratio */
    LX_SCALE_INTEGER,           /* largest whole-number factor              */
    LX_SCALE_STRETCH,           /* fill the display                         */
    LX_SCALE_NONE,              /* 1:1, centred                             */
} lx_scale_t;

typedef struct lx_surface lx_surface_t;

typedef struct {
    int x, y, w, h;             /* landscape display coordinates */
} lx_rect_t;

esp_err_t lx_comp_init(void);

/* Display size as seen by apps (landscape, rotation already applied). */
void lx_comp_display_size(int *w, int *h);

lx_surface_t *lx_comp_surface_create(int w, int h, lx_scale_t scale, int buffers);
void          lx_comp_surface_destroy(lx_surface_t *s);
void          lx_comp_destroy_all(void);          /* app teardown */
uint16_t     *lx_comp_lock(lx_surface_t *s, int *stride_px);
esp_err_t     lx_comp_present(lx_surface_t *s);
esp_err_t     lx_comp_present_ext(lx_surface_t *s, const void *pixels);   /* surface-sized RGB565, no copy */
lx_rect_t     lx_comp_surface_rect(const lx_surface_t *s);
/* PPA drawing into the buffer lock() returns (clipped to the surface): sprites */
typedef enum { LX_BLIT_RGB565, LX_BLIT_ARGB8888, LX_BLIT_A8 } lx_blit_format_t;
typedef struct {
    const void *pixels;
    int w, h, stride;           /* stride in pixels, 0 = w */
    lx_blit_format_t format;
    int x, y;                   /* top left of the drawn (scaled, turned) picture */
    bool colorkey;              /* pixels of colour key are left out */
    uint16_t key;
    uint16_t color;             /* A8: the colour */
    int alpha;                  /* 0..255 for the whole picture, -1 = as it is */
    int rotate;                 /* quarter turns counter-clockwise */
    int scale_x16, scale_y16;   /* sixteenths: 16 = 1.0 */
    bool mirror_x, mirror_y;
} lx_blit_t;
esp_err_t     lx_comp_blit(lx_surface_t *s, const lx_blit_t *b);
esp_err_t     lx_comp_fill(lx_surface_t *s, int x, int y, int w, int h, uint16_t color);
/* Touch points mapped into surface coordinates; points outside it are dropped. */
int           lx_comp_touch_read(lx_surface_t *s, bsp_touch_point_t *pts, int max);

/* Launcher drawing: returns the hidden framebuffer (panel native layout), or
 * NULL while an app surface owns the display. end() flips it on screen. */
uint16_t *lx_comp_launcher_begin(void);
void      lx_comp_launcher_end(void);
/* Same, but also while an app surface exists: for launcher screens the app
 * itself called up (UI interface) while it waits for them. The app's surface
 * reappears with its next present. Pair with lx_comp_launcher_end(). */
uint16_t *lx_comp_takeover_begin(void);
/* Launcher screens drawn into a full landscape frame (display size, RGB565,
 * 64-byte aligned): rotated onto the panel by the PPA and shown. Does nothing
 * (ESP_ERR_INVALID_STATE) while an app surface owns the display. */
esp_err_t lx_comp_launcher_present(const uint16_t *frame);
/* Copies what is on screen into dst (a buffer from *_begin), to draw on top of it. */
void      lx_comp_copy_visible(uint16_t *dst);
/* The visible picture, landscape, as /sd/screenshots/shot_NNN.bmp; path (may be NULL) gets its name. */
esp_err_t lx_comp_screenshot(char *path, size_t len);

#ifdef __cplusplus
}
#endif
