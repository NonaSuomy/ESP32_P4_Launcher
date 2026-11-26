#define PAPP_APP_SIDE 1
#include "scummvm_papp2_api.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

namespace {

static const papp_if_fs_t *s_fs;
static papp_surface_t *s_surface;
static uint16_t s_surface_w, s_surface_h;
static papp_audio_stream_t *s_audio;
static uint32_t s_audio_rate;
static int64_t s_mouse_sample_us;
static float s_mouse_remainder_x;
static float s_mouse_remainder_y;

struct Task {
    papp_thread_t *thread;
};
struct TaskStart {
    void (*fn)(void *);
    void *arg;
};

static papp_surface_t *surface_for(uint16_t width, uint16_t height) {
    if (s_surface && (s_surface_w != width || s_surface_h != height)) {
        papp_disp_surface_destroy(s_surface);
        s_surface = nullptr;
    }
    if (!s_surface) {
        papp_surface_cfg_t cfg = {
            .width = width, .height = height, .format = PAPP_PIXFMT_RGB565,
            .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1,
        };
        s_surface = papp_disp_surface_create(&cfg);
        if (s_surface) {
            s_surface_w = width;
            s_surface_h = height;
        }
    }
    return s_surface;
}

static void native_display_clear(uint16_t color) {
    papp_surface_t *surface = surface_for(s_surface ? s_surface_w : 800,
                                           s_surface ? s_surface_h : 480);
    if (!surface) return;
    int stride = 0;
    uint16_t *pixels = static_cast<uint16_t *>(papp_disp_lock(surface, &stride));
    if (!pixels || stride < s_surface_w) return;
    for (uint16_t y = 0; y < s_surface_h; ++y)
        for (uint16_t x = 0; x < s_surface_w; ++x)
            pixels[(size_t)y * stride + x] = color;
    papp_disp_present(surface);
}

static void native_display_frame(const uint16_t *pixels, uint16_t width,
                                 uint16_t height, float, bool byte_swap) {
    if (!pixels || !width || !height) return;
    papp_surface_t *surface = surface_for(width, height);
    if (!surface) return;
    if (!byte_swap) {
        papp_disp_present_ext(surface, pixels);
        return;
    }
    int stride = 0;
    uint16_t *dst = static_cast<uint16_t *>(papp_disp_lock(surface, &stride));
    if (!dst || stride < width) return;
    for (uint16_t y = 0; y < height; ++y) {
        const uint16_t *src_row = pixels + (size_t)y * width;
        uint16_t *dst_row = dst + (size_t)y * stride;
        for (uint16_t x = 0; x < width; ++x)
            dst_row[x] = static_cast<uint16_t>((src_row[x] >> 8) | (src_row[x] << 8));
    }
    papp_disp_present(surface);
}

static void native_audio_init(int rate) {
    if (s_audio && s_audio_rate == static_cast<uint32_t>(rate)) return;
    if (s_audio) papp_audio_close(s_audio);
    papp_audio_cfg_t cfg = {
        .rate = static_cast<uint32_t>(rate), .channels = 2,
        .format = PAPP_AUDIO_S16, .latency_ms = 80,
    };
    s_audio = papp_audio_open(&cfg);
    s_audio_rate = s_audio ? static_cast<uint32_t>(rate) : 0;
}

static void native_audio_submit(short *stereo, int frames) {
    if (s_audio && stereo && frames > 0)
        papp_audio_write(s_audio, stereo, static_cast<size_t>(frames), 1000);
}

static void native_gamepad_read(pax_gamepad_state_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    papp_gamepad_t pad{};
    if (!papp_input_gamepad(0, &pad)) return;
    const uint32_t b = pad.buttons;
    out->values[PAX_INPUT_UP] = (b & PAPP_BTN_UP) != 0;
    out->values[PAX_INPUT_RIGHT] = (b & PAPP_BTN_RIGHT) != 0;
    out->values[PAX_INPUT_DOWN] = (b & PAPP_BTN_DOWN) != 0;
    out->values[PAX_INPUT_LEFT] = (b & PAPP_BTN_LEFT) != 0;
    out->values[PAX_INPUT_SELECT] = (b & PAPP_BTN_SELECT) != 0;
    out->values[PAX_INPUT_START] = (b & PAPP_BTN_START) != 0;
    out->values[PAX_INPUT_A] = (b & PAPP_BTN_A) != 0;
    out->values[PAX_INPUT_B] = (b & PAPP_BTN_B) != 0;
    out->values[PAX_INPUT_X] = (b & PAPP_BTN_X) != 0;
    out->values[PAX_INPUT_Y] = (b & PAPP_BTN_Y) != 0;
    out->values[PAX_INPUT_L] = (b & PAPP_BTN_L1) != 0;
    out->values[PAX_INPUT_R] = (b & PAPP_BTN_R1) != 0;
    out->values[PAX_INPUT_MENU] = papp_exit_requested() || (b & PAPP_BTN_HOME) != 0;
}

/* ScummVM uses relative mouse motion, while the PAPP2 API reports absolute
 * stick positions. Convert the right stick to a velocity so pointer speed is
 * stable across the varying poll rate of the ScummVM event loop. */
static int native_mouse_read(int *dx, int *dy, int *buttons) {
    if (!dx || !dy || !buttons) return 0;

    papp_gamepad_t pad{};
    if (!papp_input_gamepad(0, &pad)) {
        s_mouse_sample_us = 0;
        s_mouse_remainder_x = 0.0f;
        s_mouse_remainder_y = 0.0f;
        return 0;
    }

    const int64_t now = papp_time_us();
    int64_t elapsed_us = s_mouse_sample_us ? now - s_mouse_sample_us : 16667;
    s_mouse_sample_us = now;
    if (elapsed_us < 0) elapsed_us = 0;
    if (elapsed_us > 50000) elapsed_us = 50000;

    const auto stick_velocity = [](int16_t axis) -> float {
        const float value = static_cast<float>(axis) / 32767.0f;
        const float magnitude = value < 0.0f ? -value : value;
        constexpr float deadzone = 0.18f;
        if (magnitude <= deadzone) return 0.0f;
        const float scaled = (magnitude - deadzone) / (1.0f - deadzone);
        const float eased = scaled * scaled;
        return (value < 0.0f ? -eased : eased) * 650.0f;
    };

    const float seconds = static_cast<float>(elapsed_us) / 1000000.0f;
    s_mouse_remainder_x += stick_velocity(pad.rx) * seconds;
    s_mouse_remainder_y += stick_velocity(pad.ry) * seconds;
    *dx = static_cast<int>(s_mouse_remainder_x);
    *dy = static_cast<int>(s_mouse_remainder_y);
    s_mouse_remainder_x -= static_cast<float>(*dx);
    s_mouse_remainder_y -= static_cast<float>(*dy);

    /* The right shoulder is the mouse's primary button. */
    *buttons = (pad.buttons & PAPP_BTN_R1) ? 1 : 0;
    return 1;
}

static int native_keyboard_read(pax_keyboard_event_t *out) {
    if (!out) return 0;
    papp_input_event_t ev{};
    while (papp_input_poll_event(&ev)) {
        if (ev.type != PAPP_EV_KEY_DOWN && ev.type != PAPP_EV_KEY_UP) continue;
        int key = ev.text;
        switch (ev.code) {
        case 0x52: key = 273; break; /* up */
        case 0x51: key = 274; break; /* down */
        case 0x4f: key = 275; break; /* right */
        case 0x50: key = 276; break; /* left */
        case 0x28: key = 13; break;  /* enter */
        case 0x29: key = 27; break;  /* escape */
        case 0x2a: key = 8; break;   /* backspace */
        case 0x2b: key = 9; break;   /* tab */
        default: break;
        }
        out->key = key;
        out->down = ev.type == PAPP_EV_KEY_DOWN;
        return 1;
    }
    return 0;
}

static int native_touch_read(int *x, int *y) {
    papp_touch_point_t point{};
    if (papp_disp_touch(nullptr, &point, 1) <= 0) return 0;
    papp_disp_info_t info{};
    papp_disp_info(&info);
    if (x) *x = info.width ? (int)point.x * 800 / info.width : point.x;
    if (y) *y = info.height ? (int)point.y * 480 / info.height : point.y;
    return 1;
}

static void *native_file_open(const char *path, const char *mode) {
    return s_fs && path && mode ? s_fs->open(path, mode) : nullptr;
}
static int native_file_close(void *file) { return s_fs && file ? s_fs->close((papp_file_t *)file) : -1; }
static size_t native_file_read(void *ptr, size_t size, size_t count, void *file) {
    if (!s_fs || !file || !size || count > SIZE_MAX / size) return 0;
    return s_fs->read((papp_file_t *)file, ptr, size * count) / size;
}
static size_t native_file_write(const void *ptr, size_t size, size_t count, void *file) {
    if (!s_fs || !file || !size || count > SIZE_MAX / size) return 0;
    return s_fs->write((papp_file_t *)file, ptr, size * count) / size;
}
static int native_file_seek(void *file, long offset, int whence) {
    return s_fs && file ? s_fs->seek((papp_file_t *)file, offset, whence) : -1;
}
static long native_file_tell(void *file) { return s_fs && file ? s_fs->tell((papp_file_t *)file) : -1; }
static int native_file_mkdir(const char *path) { return s_fs ? s_fs->mkdir(path) : -1; }
static int native_file_remove(const char *path) { return s_fs ? s_fs->remove(path) : -1; }
static int native_file_rename(const char *from, const char *to) {
    return s_fs && s_fs->hdr.version >= 2 ? s_fs->rename(from, to) : -1;
}
static int native_file_list_dir(const char *path, char *buf, int len) {
    if (!s_fs || !path || !buf || len <= 0 || s_fs->hdr.version < 2 ||
        !s_fs->opendir || !s_fs->readdir || !s_fs->closedir)
        return -1;

    papp_dir_t *dir = s_fs->opendir(path);
    if (!dir) return -1;

    size_t used = 0;
    int count = 0;
    int result = 0;
    papp_dirent_t entry{};
    while ((result = s_fs->readdir(dir, &entry)) > 0) {
        size_t name_len = 0;
        while (name_len < sizeof(entry.name) && entry.name[name_len]) ++name_len;
        if (name_len == 0 || name_len == sizeof(entry.name)) continue;

        const size_t extra = entry.is_dir ? 1u : 0u;
        if (used + name_len + extra + 1 > (size_t)len) break;
        memcpy(buf + used, entry.name, name_len);
        used += name_len;
        if (entry.is_dir) buf[used++] = '/';
        buf[used++] = '\0';
        ++count;
    }
    s_fs->closedir(dir);
    if (result < 0) return -1;
    return count;
}
static int native_file_stat(const char *path, pax_file_stat_t *out) {
    if (!s_fs || !path || !out) return -1;
    memset(out, 0, sizeof(*out));
    if (s_fs->hdr.version >= 2 && s_fs->stat) {
        papp_stat_t info{};
        if (s_fs->stat(path, &info) != 0) return -1;
        out->size = info.size > 0 ? (uint64_t)info.size : 0;
        out->mtime = info.mtime;
        out->is_dir = info.is_dir != 0;
        return 0;
    }
    long size = s_fs->size ? s_fs->size(path) : -1;
    if (size < 0) return -1;
    out->size = (uint64_t)size;
    return 0;
}

static void *native_mem_caps_alloc(size_t size, uint32_t caps) {
    uint32_t papp_caps = 0;
    if (caps & PAX_MEM_CAP_SPIRAM) papp_caps |= PAPP_MEM_LARGE;
    if (caps & PAX_MEM_CAP_INTERNAL) papp_caps |= PAPP_MEM_FAST;
    if (caps & PAX_MEM_CAP_DMA) papp_caps |= PAPP_MEM_DMA;
    return papp_alloc(size, papp_caps, (caps & PAX_MEM_CAP_DMA) ? 64 : 0);
}
static void *native_mem_realloc(void *ptr, size_t size) { return papp_realloc(ptr, size); }
static void native_mem_free(void *ptr) { papp_free(ptr); }

static int native_log_printf(const char *fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    papp_log("%s", buffer);
    return n;
}
static void native_delay_ms(int ms) { papp_delay_ms(ms > 0 ? (uint32_t)ms : 0); }
static int64_t native_time_us(void) { return papp_time_us(); }

static int native_task_trampoline(void *arg) {
    TaskStart *start = static_cast<TaskStart *>(arg);
    void (*fn)(void *) = start->fn;
    void *fn_arg = start->arg;
    papp_free(start);
    fn(fn_arg);
    return 0;
}
static int native_task_create(void (*fn)(void *), const char *name, uint32_t stack,
                              void *arg, int priority, void *out_handle, int core) {
    if (!fn || !out_handle) return -1;
    TaskStart *start = static_cast<TaskStart *>(papp_alloc(sizeof(*start), PAPP_MEM_FAST, 0));
    if (!start) return -1;
    Task *task = static_cast<Task *>(papp_alloc(sizeof(*task), PAPP_MEM_FAST, 0));
    if (!task) { papp_free(start); return -1; }
    start->fn = fn;
    start->arg = arg;
    papp_thread_cfg_t cfg = {
        .name = name, .stack_size = stack,
        .prio = priority >= 5 ? PAPP_PRIO_HIGH : PAPP_PRIO_NORMAL,
        .core = (int8_t)(core == 0 || core == 1 ? core : -1),
        .flags = stack > 32768 ? PAPP_THREAD_STACK_PSRAM : 0,
    };
    papp_thread_t *thread = papp_thread_create(native_task_trampoline, start, &cfg);
    if (!thread) { papp_free(start); papp_free(task); return -1; }
    task->thread = thread;
    *(void **)out_handle = task;
    return 0;
}
static void native_task_delete(void *handle) {
    Task *task = static_cast<Task *>(handle);
    if (!task) return;
    papp_thread_join(task->thread, PAPP_WAIT_FOREVER, nullptr);
    papp_free(task);
}

} // namespace

void scummvm_papp2_api_init(ScummVMPlatformApi *api) {
    if (!api) return;
    memset(api, 0, sizeof(*api));
    s_fs = papp_fs();
    api->display_clear = native_display_clear;
    api->display_flush = []() {};
    api->display_write_frame_custom = native_display_frame;
    api->audio_init = native_audio_init;
    api->audio_submit = native_audio_submit;
    api->input_gamepad_read = native_gamepad_read;
    api->file_open = native_file_open;
    api->file_close = native_file_close;
    api->file_read = native_file_read;
    api->file_write = native_file_write;
    api->file_seek = native_file_seek;
    api->file_tell = native_file_tell;
    api->mem_realloc = native_mem_realloc;
    api->mem_free = native_mem_free;
    api->mem_caps_alloc = native_mem_caps_alloc;
    api->log_printf = native_log_printf;
    api->delay_ms = native_delay_ms;
    api->get_time_us = native_time_us;
    api->task_create = native_task_create;
    api->task_delete = native_task_delete;
    api->touch_read = native_touch_read;
    api->input_l3_read = []() { return papp_exit_requested() ? 1 : 0; };
    api->input_mouse_read = native_mouse_read;
    api->input_keyboard_read = native_keyboard_read;
    api->file_mkdir = native_file_mkdir;
    api->file_remove = native_file_remove;
    api->file_rename = native_file_rename;
    api->file_stat = native_file_stat;
    api->file_list_dir = native_file_list_dir;
}

void scummvm_papp2_api_shutdown(void) {
    s_mouse_sample_us = 0;
    s_mouse_remainder_x = 0.0f;
    s_mouse_remainder_y = 0.0f;
    if (s_audio) {
        papp_audio_clear(s_audio);
        papp_audio_close(s_audio);
        s_audio = nullptr;
        s_audio_rate = 0;
    }
    if (s_surface) {
        papp_disp_surface_destroy(s_surface);
        s_surface = nullptr;
        s_surface_w = 0;
        s_surface_h = 0;
    }
}
