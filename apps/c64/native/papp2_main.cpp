/* Direct PAPP2 host for Frodo C64. */
#define PAX_APP_SIDE 1
#include "esp_heap_caps.h"
#include "psram_app.h"
#include <papp/papp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
__attribute__((section(".papp_manifest"),
               used)) extern const papp_manifest_t __papp_manifest = {
    .magic = PAPP_MANIFEST_MAGIC,
    .sdk_version = PAPP_SDK_VERSION,
    .id = "emu.c64",
    .name = "Commodore 64",
    .version = PAPP_APP_VERSION,
    .main_stack = 32768};
int app_entry(const app_services_t *);
void app_return_to_launcher(void);
}
static papp_surface_t *surface;
static uint16_t surface_width;
static uint16_t surface_height;
static papp_audio_stream_t *audio;
static papp_gamepad_t pad{};
static papp_disp_info_t display{};
static const papp_if_fs_t *fs;
static char selected_rom[256];
static pax_game_menu_page_t menu_page{};
static pax_game_menu_action_fn_t menu_action;
static void *menu_context;
static const char *menu_title = "COMMODORE 64";
static bool menu_visible, menu_button_down;
struct TaskCtx {
  void (*fn)(void *);
  void *arg;
  papp_thread_t *thread;
};

extern "C" void pax_cleanup_fds(void) {}
extern "C" void pax_cleanup_heap(void) {}
extern "C" void pax_close_streams(void) {}
extern "C" void *heap_caps_malloc(size_t n, uint32_t caps) {
  return papp_alloc(
      n, (caps & MALLOC_CAP_SPIRAM) ? PAPP_MEM_LARGE : PAPP_MEM_ANY, 16);
}
extern "C" void *heap_caps_calloc(size_t n, size_t z, uint32_t caps) {
  if (z && n > SIZE_MAX / z)
    return NULL;
  size_t bytes = n * z;
  void *p = heap_caps_malloc(bytes, caps);
  if (p)
    memset(p, 0, bytes);
  return p;
}
extern "C" void *heap_caps_aligned_alloc(size_t a, size_t n, uint32_t caps) {
  return papp_alloc(
      n, (caps & MALLOC_CAP_SPIRAM) ? PAPP_MEM_LARGE : PAPP_MEM_ANY, a);
}
extern "C" void heap_caps_free(void *p) { papp_free(p); }

static int log_printf(const char *fmt, ...) {
  char b[384];
  va_list a;
  va_start(a, fmt);
  int n = vsnprintf(b, sizeof(b), fmt, a);
  va_end(a);
  papp_log("%s", b);
  return n;
}
static int log_vprintf(const char *fmt, va_list a) {
  char b[384];
  int n = vsnprintf(b, sizeof(b), fmt, a);
  papp_log("%s", b);
  return n;
}
static void *mem_alloc(size_t n) { return papp_alloc(n, PAPP_MEM_ANY, 16); }
static void *mem_calloc(size_t n, size_t z) {
  if (z && n > SIZE_MAX / z)
    return NULL;
  void *p = mem_alloc(n * z);
  if (p)
    memset(p, 0, n * z);
  return p;
}
static void *mem_realloc(void *p, size_t n) { return papp_realloc(p, n); }
static void mem_free(void *p) { papp_free(p); }
static void *mem_caps(size_t n, uint32_t caps) {
  return papp_alloc(
      n, (caps & PAX_MEM_CAP_SPIRAM) ? PAPP_MEM_LARGE : PAPP_MEM_ANY, 16);
}
static void delay_ms(int ms) {
  if (ms > 0)
    papp_delay_ms((uint32_t)ms);
}
static int64_t time_us() { return papp_time_us(); }
static void audio_init(int rate) {
  if (audio) {
    papp_audio_close(audio);
    audio = NULL;
  }
  if (rate > 0) {
    papp_audio_cfg_t c = {
        .rate = (uint32_t)rate, .channels = 2, .format = PAPP_AUDIO_S16};
    audio = papp_audio_open(&c);
  }
}
static void audio_submit(short *samples, int frames) {
  if (!audio || !samples || frames <= 0)
    return;

  /* A PAPP write can accept fewer frames than requested when its timeout
   * expires. Keep the remainder instead of silently dropping it; the
   * blocking writes also pace the emulator to the audio clock. */
  size_t written = 0;
  const size_t total = (size_t)frames;
  while (written < total && !papp_exit_requested()) {
    size_t n = papp_audio_write(audio, samples + written * 2,
                                total - written, 100);
    if (!n) {
      if (papp_exit_requested())
        break;
      papp_delay_ms(1);
      continue;
    }
    written += n;
  }
}

static papp_surface_t *surface_for(uint16_t w, uint16_t h) {
  if (surface && (surface_width != w || surface_height != h)) {
    papp_disp_surface_destroy(surface);
    surface = NULL;
  }
  if (!surface) {
    papp_surface_cfg_t cfg = {.width = w,
                              .height = h,
                              .format = PAPP_PIXFMT_RGB565,
                              .scale = PAPP_SCALE_FIT_ASPECT,
                              .buffers = 1};
    surface = papp_disp_surface_create(&cfg);
    if (surface) {
      surface_width = w;
      surface_height = h;
    }
  }
  return surface;
}

static void frame(const uint16_t *src, uint16_t w, uint16_t h, float scale,
                  bool swap) {
  (void)scale;
  if (!src || !w || !h)
    return;
  papp_surface_t *s = surface_for(w, h);
  if (!s)
    return;

  if (!swap) {
    /* The P4 PPA scales this native-resolution frame to the display. The
     * pixel buffer is contiguous RGB565 and remains owned by the core until
     * the next frame callback. */
    papp_disp_present_ext(s, src);
    return;
  }

  int stride = 0;
  uint16_t *dst = (uint16_t *)papp_disp_lock(s, &stride);
  if (!dst)
    return;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      uint16_t pixel = src[y * w + x];
      dst[y * stride + x] = (uint16_t)((pixel >> 8) | (pixel << 8));
    }
  }
  papp_disp_present(s);
}
static void clear(uint16_t color) {
  if (!surface)
    return;
  int stride = 0;
  uint16_t *dst = (uint16_t *)papp_disp_lock(surface, &stride);
  if (dst) {
    for (int y = 0; y < surface_height; y++)
      for (int x = 0; x < surface_width; x++)
        dst[y * stride + x] = color;
    papp_disp_present(surface);
  }
}
static void flush() {}
static void get_size(int *w, int *h) {
  if (w)
    *w = display.width;
  if (h)
    *h = display.height;
}
static int set_canvas(int w, int h) {
  (void)w;
  (void)h;
  return 0;
}
static void set_scale(float x, float y) {
  (void)x;
  (void)y;
}
static void input_read(pax_gamepad_state_t *out) {
  papp_input_gamepad(0, &pad);
  memset(out, 0, sizeof(*out));
  if ((pad.buttons & PAPP_BTN_L1) && !menu_button_down && menu_visible) {
    if (!menu_page.count && menu_action)
      menu_action(menu_context, PAX_GAME_MENU_ACTION_OPEN, 0);
    const char *items[PAX_GAME_MENU_MAX_ITEMS + 1];
    int n = (int)menu_page.count;
    if (n > PAX_GAME_MENU_MAX_ITEMS)
      n = PAX_GAME_MENU_MAX_ITEMS;
    for (int i = 0; i < n; i++)
      items[i] = menu_page.items[i].label;
    items[n] = "EXIT GAME";
    int choice =
        papp_ui_menu(menu_title, items, n + 1, (int)menu_page.selected);
    if (choice == n)
      app_return_to_launcher();
    else if (choice >= 0 && choice < n && menu_action)
      menu_action(menu_context, menu_page.items[choice].action, 0);
  }
  menu_button_down = !!(pad.buttons & PAPP_BTN_L1);
  if (pad.buttons & PAPP_BTN_UP)
    out->values[PAX_INPUT_UP] = 1;
  if (pad.buttons & PAPP_BTN_DOWN)
    out->values[PAX_INPUT_DOWN] = 1;
  if (pad.buttons & PAPP_BTN_LEFT)
    out->values[PAX_INPUT_LEFT] = 1;
  if (pad.buttons & PAPP_BTN_RIGHT)
    out->values[PAX_INPUT_RIGHT] = 1;
  if (pad.buttons & PAPP_BTN_SELECT)
    out->values[PAX_INPUT_SELECT] = 1;
  if (pad.buttons & PAPP_BTN_START)
    out->values[PAX_INPUT_START] = 1;
  if (pad.buttons & PAPP_BTN_A)
    out->values[PAX_INPUT_A] = 1;
  if (pad.buttons & PAPP_BTN_B)
    out->values[PAX_INPUT_B] = 1;
  if (pad.buttons & PAPP_BTN_X)
    out->values[PAX_INPUT_X] = 1;
  if (pad.buttons & PAPP_BTN_Y)
    out->values[PAX_INPUT_Y] = 1;
  if (pad.buttons & PAPP_BTN_L1)
    out->values[PAX_INPUT_L] = 1;
  if (pad.buttons & PAPP_BTN_R1)
    out->values[PAX_INPUT_R] = 1;
  if ((pad.buttons & PAPP_BTN_L3) || papp_exit_requested())
    out->values[PAX_INPUT_MENU] = 1;
}
static int input_l3() {
  papp_input_gamepad(0, &pad);
  return papp_exit_requested() || !!(pad.buttons & PAPP_BTN_L3);
}
static int input_keyboard(pax_keyboard_event_t *out) {
  papp_input_event_t ev;
  while (papp_input_poll_event(&ev)) {
    if (ev.type != PAPP_EV_KEY_DOWN && ev.type != PAPP_EV_KEY_UP)
      continue;
    out->down = ev.type == PAPP_EV_KEY_DOWN;
    if (ev.text)
      out->key = ev.text;
    else
      switch (ev.code) {
      case 40:
        out->key = PAX_KEY_ENTER;
        break;
      case 41:
        out->key = 27;
        break;
      case 42:
        out->key = 8;
        break;
      case 73:
        out->key = 147;
        break;
      case 74:
        out->key = 151;
        break;
      case 79:
        out->key = PAX_KEY_RIGHT;
        break;
      case 80:
        out->key = PAX_KEY_LEFT;
        break;
      case 81:
        out->key = PAX_KEY_DOWN;
        break;
      case 82:
        out->key = PAX_KEY_UP;
        break;
      case 58:
        out->key = 135;
        break;
      case 59:
        out->key = 136;
        break;
      case 60:
        out->key = 137;
        break;
      case 61:
        out->key = 138;
        break;
      case 62:
        out->key = 139;
        break;
      case 63:
        out->key = 140;
        break;
      case 64:
        out->key = 141;
        break;
      case 65:
        out->key = 142;
        break;
      default:
        continue;
      }
    return 1;
  }
  return 0;
}
static int game_menu_register(const char *title, pax_game_menu_action_fn_t cb,
                              void *ctx) {
  menu_title = title ? title : "COMMODORE 64";
  menu_action = cb;
  menu_context = ctx;
  menu_visible = cb != NULL;
  return 0;
}
static int game_menu_set_page(const pax_game_menu_page_t *page) {
  if (!page) {
    memset(&menu_page, 0, sizeof(menu_page));
    return 0;
  }
  menu_page = *page;
  menu_visible = true;
  return 0;
}
static void *file_open(const char *p, const char *m) {
  if (!selected_rom[0] && p && !strcmp(p, "/sd/roms/pax/c64.rom"))
    return NULL;
  return fs ? fs->open(p, m) : NULL;
}
static int file_close(void *f) {
  return fs && f ? fs->close((papp_file_t *)f) : -1;
}
static size_t file_read(void *p, size_t s, size_t n, void *f) {
  return fs && f && s ? fs->read((papp_file_t *)f, p, s * n) / s : 0;
}
static size_t file_write(const void *p, size_t s, size_t n, void *f) {
  return fs && f && s ? fs->write((papp_file_t *)f, p, s * n) / s : 0;
}
static int file_seek(void *f, long o, int w) {
  return fs && f ? fs->seek((papp_file_t *)f, o, w) : -1;
}
static long file_tell(void *f) {
  return fs && f ? fs->tell((papp_file_t *)f) : -1;
}
static char *rom_path_get() { return selected_rom; }
static void rom_path_set(const char *p) {
  if (p)
    snprintf(selected_rom, sizeof(selected_rom), "%s", p);
}
static int task_trampoline(void *p) {
  auto *c = (TaskCtx *)p;
  c->fn(c->arg);
  return 0;
}
static int task_create(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, int prio, void *out, int core) {
  if (!fn || !out)
    return -1;
  /* Keep C64 audio on the emulator thread. PAPP audio writes block while
   * the output ring is full, which provides clock pacing without a second
   * high-priority task competing with frame rendering on this core. */
  if (name && !strcmp(name, "pax_audio"))
    return -1;
  auto *c = (TaskCtx *)papp_alloc(sizeof(TaskCtx), PAPP_MEM_ANY, 16);
  if (!c)
    return -1;
  c->fn = fn;
  c->arg = arg;
  papp_thread_cfg_t cfg = {
      .name = name,
      .stack_size = stack < 8192 ? 8192 : stack,
      .prio = (uint8_t)(prio >= 3 ? PAPP_PRIO_HIGH : PAPP_PRIO_NORMAL),
      .core = (int8_t)(core < 0 ? -1 : core)};
  c->thread = papp_thread_create(task_trampoline, c, &cfg);
  if (!c->thread) {
    papp_free(c);
    return -1;
  }
  *(void **)out = c;
  return 0;
}
static void task_delete(void *h) {
  auto *c = (TaskCtx *)h;
  if (c) {
    papp_thread_join(c->thread, PAPP_WAIT_FOREVER, NULL);
    papp_free(c);
  }
}
static bool exit_requested() { return papp_exit_requested(); }

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  fs = papp_fs();
  if (!fs) {
    papp_ui_message("Commodore 64", "PAPP2 filesystem unavailable.");
    return -1;
  }
  papp_pick_cfg_t pc = {.title = "Commodore 64 disk/tape (Cancel for BASIC)",
                        .root = "/sd/roms/c64",
                        .extensions = ".d64,.t64"};
  papp_pick_result_t pr{};
  if (papp_ui_pick_file(&pc, &pr) == PAPP_PICK_RUN)
    snprintf(selected_rom, sizeof(selected_rom), "%s", pr.path);
  if (!papp_disp_info(&display))
    return -1;
  app_services_t svc{};
  svc.abi_version = PAX_ABI_VERSION;
  svc.display_write_frame_custom = frame;
  svc.display_clear = clear;
  svc.display_flush = flush;
  svc.display_set_scale = set_scale;
  svc.display_get_size = get_size;
  svc.display_set_canvas = set_canvas;
  svc.audio_init = audio_init;
  svc.audio_submit = audio_submit;
  svc.input_gamepad_read = input_read;
  svc.input_l3_read = input_l3;
  svc.input_keyboard_read = input_keyboard;
  svc.exit_requested = exit_requested;
  svc.file_open = file_open;
  svc.file_close = file_close;
  svc.file_read = file_read;
  svc.file_write = file_write;
  svc.file_seek = file_seek;
  svc.file_tell = file_tell;
  svc.mem_alloc = mem_alloc;
  svc.mem_calloc = mem_calloc;
  svc.mem_realloc = mem_realloc;
  svc.mem_free = mem_free;
  svc.mem_caps_alloc = mem_caps;
  svc.log_printf = log_printf;
  svc.log_vprintf = log_vprintf;
  svc.delay_ms = delay_ms;
  svc.get_time_us = time_us;
  svc.settings_rom_path_get = rom_path_get;
  svc.settings_rom_path_set = rom_path_set;
  svc.task_create = task_create;
  svc.task_delete = task_delete;
  svc.game_menu_register = game_menu_register;
  svc.game_menu_set_page = game_menu_set_page;
  int rc = app_entry(&svc);
  if (audio)
    papp_audio_close(audio);
  papp_disp_surface_destroy(surface);
  return rc;
}
