/* Direct PAPP2 frontend for the portable Ardens ATmega32U4 simulator. */
#include "papp/papp.h"
#include "absim.hpp"
#include "absim_strstream.hpp"

#include <new>
#include <stdint.h>
#include <string.h>

extern "C" {
__attribute__((section(".papp_manifest"), used))
extern const papp_manifest_t __papp_manifest = {
    .magic=PAPP_MANIFEST_MAGIC, .sdk_version=PAPP_SDK_VERSION,
    .id="emu.arduboy", .name="Arduboy", .version=PAPP_APP_VERSION,
    .main_stack=32768
};
}

static absim::arduboy_t *s_arduboy;
static uint16_t *s_frame;
static uint8_t *s_rom;
static papp_surface_t *s_surface;
static papp_audio_stream_t *s_audio;
static int16_t s_audio_frame[2048];

void *operator new(size_t size) noexcept
{
    return papp_alloc(size, PAPP_MEM_LARGE, 16);
}
void *operator new[](size_t size) noexcept
{
    return papp_alloc(size, PAPP_MEM_LARGE, 16);
}
void operator delete(void *p) noexcept { papp_free(p); }
void operator delete[](void *p) noexcept { papp_free(p); }
void operator delete(void *p, size_t) noexcept { papp_free(p); }
void operator delete[](void *p, size_t) noexcept { papp_free(p); }

static void cleanup(void)
{
    delete s_arduboy;
    s_arduboy = nullptr;
    papp_free(s_frame);
    s_frame = nullptr;
    papp_free(s_rom);
    s_rom = nullptr;
    if (s_audio) { papp_audio_close(s_audio); s_audio = nullptr; }
    if (s_surface) { papp_disp_surface_destroy(s_surface); s_surface = nullptr; }
}

static void set_buttons(const papp_gamepad_t &pad)
{
    uint8_t pinf = 0xf0;
    uint8_t pine = 1u << 6;
    uint8_t pinb = 1u << 4;
    if (pad.buttons & PAPP_BTN_UP)    pinf &= ~(1u << 7);
    if (pad.buttons & PAPP_BTN_RIGHT) pinf &= ~(1u << 6);
    if (pad.buttons & PAPP_BTN_DOWN)  pinf &= ~(1u << 4);
    if (pad.buttons & PAPP_BTN_LEFT)  pinf &= ~(1u << 5);
    if (pad.buttons & PAPP_BTN_A)     pine &= ~(1u << 6);
    if (pad.buttons & PAPP_BTN_B)     pinb &= ~(1u << 4);
    auto &cpu = s_arduboy->core_state.cpu;
    auto &history = s_arduboy->debugger_state.input_history;
    absim::arduboy_debugger_input_t input{};
    input.cycle = cpu.cycle_count;
    input.pinb = pinb;
    input.pine = pine;
    input.pinf = pinf;
    if (!history.empty() && history.back().cycle == input.cycle) history.back() = input;
    else if (history.empty() || history.back().pinb != pinb ||
             history.back().pine != pine || history.back().pinf != pinf)
        history.push_back(input);
    cpu.data[absim::reg::addr::PINB] = pinb;
    cpu.data[absim::reg::addr::PINE] = pine;
    cpu.data[absim::reg::addr::PINF] = pinf;
}

static void present_frame(void)
{
    const auto &pixels = s_arduboy->peripherals.display.filtered_pixels;
    for (size_t i = 0; i < pixels.size(); ++i) {
        const uint8_t shade = pixels[i];
        s_frame[i] = ((uint16_t)(shade >> 3) << 11) |
                     ((uint16_t)(shade >> 2) << 5) | (shade >> 3);
    }
    papp_disp_present_ext(s_surface, s_frame);
}

static void submit_audio(void)
{
    const auto &samples = s_arduboy->core_state.cpu.sound_buffer;
    size_t out = 0;
    for (int16_t sample : samples) {
        if (out + 2 > sizeof(s_audio_frame) / sizeof(s_audio_frame[0])) break;
        int32_t amplified = (int32_t)sample * 8;
        if (amplified > INT16_MAX) amplified = INT16_MAX;
        if (amplified < INT16_MIN) amplified = INT16_MIN;
        s_audio_frame[out++] = (int16_t)amplified;
        s_audio_frame[out++] = (int16_t)amplified;
    }
    s_arduboy->core_state.cpu.sound_buffer.clear();
    if (out && s_audio) papp_audio_write(s_audio, s_audio_frame, (uint32_t)(out / 2), 100);
}

static int play(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { papp_ui_message("Arduboy", "Could not open the selected game."); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long size = ftell(f);
    if (size <= 0 || size > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f); papp_ui_message("Arduboy", "Game file is empty or too large."); return -1;
    }
    s_rom = (uint8_t *)papp_alloc((size_t)size, PAPP_MEM_LARGE, 16);
    s_frame = (uint16_t *)papp_alloc(128u * 64u * sizeof(uint16_t), PAPP_MEM_LARGE, 16);
    if (!s_rom || !s_frame || fread(s_rom, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); cleanup(); return -1;
    }
    fclose(f);

    s_arduboy = new (std::nothrow) absim::arduboy_t();
    if (!s_arduboy) { cleanup(); return -1; }
    absim::istrstream game((const char *)s_rom, (std::streamsize)size);
    const std::string error = s_arduboy->load_file(path, game);
    if (!error.empty()) {
        papp_ui_message("Arduboy", error.c_str()); cleanup(); return -1;
    }

    papp_surface_cfg_t display = { .width=128, .height=64,
        /* Frames come from Ardens' own contiguous framebuffer via
         * present_ext(). A single surface buffer keeps presentation on the
         * synchronous path and avoids copying every 128x64 frame into a
         * launcher-owned spare buffer before the PPA scales it. */
        .format=PAPP_PIXFMT_RGB565, .scale=PAPP_SCALE_FIT_ASPECT, .buffers=1 };
    s_surface = papp_disp_surface_create(&display);
    papp_audio_cfg_t audio = { .rate=50000, .channels=2, .format=PAPP_AUDIO_S16 };
    s_audio = papp_audio_open(&audio);
    if (!s_surface || !s_audio) { cleanup(); return -1; }

    int64_t deadline = papp_time_us();
    int menu_was_down = 0;
    for (;;) {
        papp_gamepad_t pad = {0};
        papp_input_gamepad(0, &pad);
        if (papp_exit_requested()) break;
        int menu_down = !!(pad.buttons & PAPP_BTN_X);
        if (menu_down && !menu_was_down) {
            papp_audio_pause(s_audio, true);
            static const char *const items[] = { "RESUME GAME", "EXIT GAME" };
            int choice = papp_ui_menu("ARDUBOY", items, 2, 0);
            papp_audio_pause(s_audio, false);
            if (choice == 1) break;
            deadline = papp_time_us();
        }
        menu_was_down = menu_down;
        set_buttons(pad);
        s_arduboy->profiler_state.frame_bytes_total = 1024;
        s_arduboy->core_state.cpu.enabled_autobreaks = 0;
        s_arduboy->debugger_state.allow_nonstep_breakpoints = false;
        s_arduboy->peripherals.display.enable_filter = true;
        s_arduboy->advance(1000000000000ull / 60ull);
        present_frame();
        submit_audio();
        deadline += 16667;
        int64_t now = papp_time_us();
        if (deadline > now) papp_delay_ms((uint32_t)((deadline - now) / 1000));
        else if (now - deadline > 100000) deadline = now;
    }
    cleanup();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) return play(argv[1]);
    papp_pick_cfg_t cfg = { .title="Arduboy", .root="/sd/roms/arduboy",
        .extensions=".hex,.arduboy" };
    static papp_pick_result_t selected;
    while (!papp_exit_requested()) {
        if (papp_ui_pick_file(&cfg, &selected) == PAPP_PICK_CANCEL) break;
        play(selected.path);
    }
    return 0;
}
