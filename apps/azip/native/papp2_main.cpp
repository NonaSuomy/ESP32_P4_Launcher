/* Direct PAPP2 frontend for AZIP, an Infocom/Inform Z-machine interpreter. */
#include "papp/papp.h"
#include "ztypes.h"
#include "font8x16.h"

#include <new>
#include <setjmp.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

extern "C" {
__attribute__((section(".papp_manifest"), used))
extern const papp_manifest_t __papp_manifest = {
    .magic=PAPP_MANIFEST_MAGIC, .sdk_version=PAPP_SDK_VERSION,
    .id="emu.azip", .name="AZIP Z-machine", .version=PAPP_APP_VERSION,
    .main_stack=32768
};
}

static jmp_buf s_exit_env;
static uint16_t *s_frame;
static uint8_t *s_story;
static size_t s_story_size;
static char s_story_path[512];
static char s_save_path[512];
static papp_surface_t *s_surface;
static int s_cursor_x, s_cursor_y, s_last_a, s_last_b;

static constexpr int SCREEN_W = 400, SCREEN_H = 240, CHAR_W = 8, CHAR_H = 16;
static constexpr int SCREEN_COLS = SCREEN_W / CHAR_W, SCREEN_ROWS = SCREEN_H / CHAR_H;

void *operator new(size_t size) noexcept { return papp_alloc(size, PAPP_MEM_LARGE, 16); }
void *operator new[](size_t size) noexcept { return papp_alloc(size, PAPP_MEM_LARGE, 16); }
void operator delete(void *p) noexcept { papp_free(p); }
void operator delete[](void *p) noexcept { papp_free(p); }
void operator delete(void *p, size_t) noexcept { papp_free(p); }
void operator delete[](void *p, size_t) noexcept { papp_free(p); }

static void clear_frame(void)
{
    if (s_frame) memset(s_frame, 0, SCREEN_W * SCREEN_H * sizeof(*s_frame));
}

static void draw_char(int x, int y, unsigned char c)
{
    if (!s_frame || c < 32 || c > 126) return;
    const uint16_t ink = PAPP_RGB565(224, 238, 224);
    const uint8_t *glyph = FONT_8x16[c - 32];
    for (int row = 0; row < CHAR_H; ++row) {
        uint16_t *dst = s_frame + (y + row) * SCREEN_W + x;
        for (int col = 0; col < CHAR_W; ++col)
            dst[col] = glyph[row] & (uint8_t)(0x80u >> col) ? ink : 0;
    }
}

static void flush_frame(void)
{
    if (s_surface && s_frame) papp_disp_present_ext(s_surface, s_frame);
}

static void scroll_text(void)
{
    for (int y = CHAR_H; y < SCREEN_H; ++y)
        memmove(s_frame + (y - CHAR_H) * SCREEN_W, s_frame + y * SCREEN_W,
                SCREEN_W * sizeof(*s_frame));
    memset(s_frame + (SCREEN_H - CHAR_H) * SCREEN_W, 0,
           CHAR_H * SCREEN_W * sizeof(*s_frame));
    s_cursor_y = SCREEN_ROWS - 1;
}

static void next_line(void)
{
    s_cursor_x = 0;
    if (++s_cursor_y >= SCREEN_ROWS) scroll_text();
}

void azip_process_output(int c)
{
    if (c == '\r') return;
    if (c == '\n') { next_line(); flush_frame(); return; }
    if (c == '\b') {
        if (s_cursor_x > 0) {
            --s_cursor_x;
            draw_char(s_cursor_x * CHAR_W, s_cursor_y * CHAR_H, ' ');
        }
        return;
    }
    if (c < 32 || c > 126) return;
    if (s_cursor_x >= SCREEN_COLS) next_line();
    draw_char(s_cursor_x * CHAR_W, s_cursor_y * CHAR_H, (unsigned char)c);
    if ((++s_cursor_x & 15) == 0) flush_frame();
}

static int poll_keyboard(int *value)
{
    papp_input_event_t event;
    while (papp_input_poll_event(&event)) {
        if (event.type != PAPP_EV_KEY_DOWN) continue;
        if (event.code == 0x28) { *value = '\r'; return 1; }
        if (event.code == 0x2a) { *value = '\b'; return 1; }
        if (event.code == 0x2b) { *value = '\t'; return 1; }
        if (event.code == 0x4f) { *value = 0x82; return 1; }
        if (event.code == 0x50) { *value = 0x83; return 1; }
        if (event.code == 0x51) { *value = 0x81; return 1; }
        if (event.code == 0x52) { *value = 0x80; return 1; }
        if (event.text > 0 && event.text < 128) { *value = event.text; return 1; }
    }
    return 0;
}

static int poll_gamepad(int *value)
{
    papp_gamepad_t pad = {0};
    papp_input_gamepad(0, &pad);
    if (papp_exit_requested()) longjmp(s_exit_env, 1);
    int a = !!(pad.buttons & PAPP_BTN_A), b = !!(pad.buttons & PAPP_BTN_B);
    int result = 0;
    if (a && !s_last_a) { *value = ' '; result = 1; }
    else if (b && !s_last_b) { *value = '\r'; result = 1; }
    else if (pad.buttons & PAPP_BTN_UP) { *value = 0x80; result = 1; }
    else if (pad.buttons & PAPP_BTN_DOWN) { *value = 0x81; result = 1; }
    s_last_a = a;
    s_last_b = b;
    return result;
}

static int poll_input(int *value)
{
    if (poll_keyboard(value)) return 1;
    return poll_gamepad(value);
}

int azip_read_character(void)
{
    for (;;) {
        int c = 0;
        if (poll_input(&c)) return c;
        papp_delay_ms(8);
    }
}

int azip_read_line(int buflen, unsigned long addr, int *read_size)
{
    if (buflen < 1) return '\r';
    int count = read_size ? *read_size : 0;
    if (count < 0) count = 0;
    if (count >= buflen) count = buflen - 1;
    flush_frame();
    for (;;) {
        int c = 0;
        if (!poll_input(&c)) { papp_delay_ms(8); continue; }
        if (c == 0x80 || c == 0x81 || c == 0x82 || c == 0x83 || c == '\t') continue;
        if (c == 8 || c == 127 || c == '\b') {
            if (count > 0) {
                --count;
                set_byte(addr + (unsigned long)count, 0);
                azip_process_output('\b');
                flush_frame();
            }
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (read_size) *read_size = count;
            azip_process_output('\n');
            return '\r';
        }
        if (c < 32 || c > 126 || count >= buflen - 1) continue;
        set_byte(addr + (unsigned long)count, (zbyte_t)c);
        ++count;
        azip_process_output(c);
    }
}

static uint32_t s_random_state = 0x6d2b79f5u;
unsigned long azip_random(void)
{
    s_random_state = s_random_state * 1664525u + 1013904223u;
    return s_random_state;
}
void azip_seed(unsigned int seed) { s_random_state = seed ? seed : 1u; }

typedef struct __attribute__((packed)) {
    uint32_t magic, version, story_size, dynamic_size, pc;
    uint16_t sp, fp, frame_count, interpreter_state, interpreter_status, reserved;
} azip_save_header_t;
static const uint32_t AZIP_SAVE_MAGIC = 0x415A5356u;

int azip_save_state(void)
{
    if (!s_story || !s_save_path[0]) return 0;
    unsigned long dynamic_size = (unsigned long)h_data_size * 2ul;
    if (dynamic_size > azip_story_size()) dynamic_size = azip_story_size();
    azip_save_header_t header = { AZIP_SAVE_MAGIC, 1u,
        (uint32_t)azip_story_size(), (uint32_t)dynamic_size, (uint32_t)pc,
        sp, fp, frame_count, (uint16_t)interpreter_state,
        (uint16_t)interpreter_status, 0 };
    FILE *file = fopen(s_save_path, "wb");
    if (!file) return 0;
    int ok = fwrite(&header, 1, sizeof(header), file) == sizeof(header) &&
        fwrite(s_story, 1, dynamic_size, file) == dynamic_size &&
        fwrite(stack, 1, sizeof(stack), file) == sizeof(stack);
    fclose(file);
    return ok;
}

int azip_restore_state(void)
{
    if (!s_story || !s_save_path[0]) return 0;
    FILE *file = fopen(s_save_path, "rb");
    if (!file) return 0;
    azip_save_header_t header;
    int ok = fread(&header, 1, sizeof(header), file) == sizeof(header);
    unsigned long dynamic_size = (unsigned long)h_data_size * 2ul;
    if (dynamic_size > azip_story_size()) dynamic_size = azip_story_size();
    if (!ok || header.magic != AZIP_SAVE_MAGIC || header.version != 1u ||
        header.story_size != azip_story_size() || header.dynamic_size != dynamic_size) ok = 0;
    if (ok) ok = fread(s_story, 1, dynamic_size, file) == dynamic_size;
    if (ok) ok = fread(stack, 1, sizeof(stack), file) == sizeof(stack);
    fclose(file);
    if (ok) {
        pc = header.pc; sp = header.sp; fp = header.fp; frame_count = header.frame_count;
        interpreter_state = header.interpreter_state;
        interpreter_status = header.interpreter_status;
    }
    return ok;
}

static void cleanup(void)
{
    if (s_surface) { papp_disp_surface_destroy(s_surface); s_surface = nullptr; }
    papp_free(s_frame); s_frame = nullptr;
    papp_free(s_story); s_story = nullptr;
    s_story_size = 0;
}

static int play(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file) fclose(file);
        papp_ui_message("AZIP", "Could not open this story file.");
        return -1;
    }
    long size = ftell(file);
    if (size < 64 || size > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file); papp_ui_message("AZIP", "Story file size is not supported."); return -1;
    }
    s_story = (uint8_t *)papp_alloc((size_t)size, PAPP_MEM_LARGE, 16);
    if (!s_story || fread(s_story, 1, (size_t)size, file) != (size_t)size) {
        fclose(file); cleanup(); return -1;
    }
    fclose(file);
    s_story_size = (size_t)size;
    strncpy(s_story_path, path, sizeof(s_story_path) - 1);
    s_story_path[sizeof(s_story_path) - 1] = '\0';
    size_t path_len = strlen(s_story_path);
    if (path_len + 5 >= sizeof(s_save_path)) { cleanup(); return -1; }
    memcpy(s_save_path, s_story_path, path_len);
    memcpy(s_save_path + path_len, ".sav", 5);

    s_frame = (uint16_t *)papp_alloc((size_t)SCREEN_W * SCREEN_H * sizeof(*s_frame),
                                     PAPP_MEM_LARGE, 16);
    papp_surface_cfg_t cfg = { .width=SCREEN_W, .height=SCREEN_H,
        .format=PAPP_PIXFMT_RGB565, .scale=PAPP_SCALE_FIT_ASPECT, .buffers=2 };
    s_surface = papp_disp_surface_create(&cfg);
    if (!s_frame || !s_surface) { cleanup(); return -1; }
    clear_frame(); flush_frame();
    azip_set_story(s_story, (unsigned long)s_story_size);
    s_last_a = s_last_b = 0;
    int result = 0;
    if (setjmp(s_exit_env) == 0) {
        initialize_screen();
        configure(V1, V8);
        z_restart();
        interpret();
    }
    cleanup();
    return result;
}

int main(int argc, char **argv)
{
    if (argc > 1) return play(argv[1]);
    papp_pick_cfg_t cfg = { .title="AZIP Z-machine", .root="/sd/roms/azip",
        .extensions=".dat,.z1,.z2,.z3,.z4,.z5,.z6,.z7,.z8" };
    static papp_pick_result_t selected;
    while (!papp_exit_requested()) {
        if (papp_ui_pick_file(&cfg, &selected) == PAPP_PICK_CANCEL) break;
        play(selected.path);
    }
    return 0;
}
