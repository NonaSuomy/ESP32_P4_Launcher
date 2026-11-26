#pragma once

/* ScummVM's private platform contract for the native PAPP2 port. This is
 * deliberately separate from the PAX ABI app_services_t contract. */
#ifdef __cplusplus
#define _Static_assert static_assert
#endif
#include "papp/papp.h"
#ifdef __cplusplus
#undef _Static_assert
#endif
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

enum {
    PAX_INPUT_UP = 0, PAX_INPUT_RIGHT, PAX_INPUT_DOWN, PAX_INPUT_LEFT,
    PAX_INPUT_SELECT, PAX_INPUT_START, PAX_INPUT_A, PAX_INPUT_B,
    PAX_INPUT_X, PAX_INPUT_Y, PAX_INPUT_L, PAX_INPUT_R,
    PAX_INPUT_MENU, PAX_INPUT_VOLUME, PAX_INPUT_MAX
};

#define PAX_MEM_CAP_SPIRAM   (1u << 10)
#define PAX_MEM_CAP_INTERNAL (1u << 11)
#define PAX_MEM_CAP_DMA      (1u << 2)

typedef struct { int values[PAX_INPUT_MAX]; } pax_gamepad_state_t;
typedef struct { int key; int down; } pax_keyboard_event_t;
typedef struct { uint64_t size; int64_t mtime; uint32_t is_dir; uint32_t reserved; } pax_file_stat_t;

typedef struct {
    void (*display_clear)(uint16_t color);
    void (*display_flush)(void);
    void (*display_write_frame_custom)(const uint16_t *, uint16_t, uint16_t, float, bool);
    void (*audio_init)(int sample_rate);
    void (*audio_submit)(short *stereo_buf, int frame_count);
    void (*input_gamepad_read)(pax_gamepad_state_t *state);
    void *(*file_open)(const char *, const char *);
    int (*file_close)(void *);
    size_t (*file_read)(void *, size_t, size_t, void *);
    size_t (*file_write)(const void *, size_t, size_t, void *);
    int (*file_seek)(void *, long, int);
    long (*file_tell)(void *);
    void *(*mem_realloc)(void *, size_t);
    void (*mem_free)(void *);
    void *(*mem_caps_alloc)(size_t, uint32_t);
    int (*log_printf)(const char *, ...);
    void (*delay_ms)(int);
    int64_t (*get_time_us)(void);
    int (*task_create)(void (*)(void *), const char *, uint32_t, void *, int, void *, int);
    void (*task_delete)(void *);
    int (*touch_read)(int *, int *);
    int (*input_l3_read)(void);
    int (*input_mouse_read)(int *, int *, int *);
    int (*input_keyboard_read)(pax_keyboard_event_t *);
    int (*file_mkdir)(const char *);
    int (*file_remove)(const char *);
    int (*file_rename)(const char *, const char *);
    int (*file_stat)(const char *, pax_file_stat_t *);
    int (*file_list_dir)(const char *, char *, int);
} ScummVMPlatformApi;

#ifdef __cplusplus
extern "C" {
#endif
extern const ScummVMPlatformApi *_pax_svc;
void scummvm_papp2_api_init(ScummVMPlatformApi *api);
void scummvm_papp2_api_shutdown(void);
#ifdef __cplusplus
}
#endif
