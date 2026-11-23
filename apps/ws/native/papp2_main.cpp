/* Direct PAPP2 frontend for Oswan WonderSwan and WonderSwan Color. */
#include "papp/papp.h"
extern "C" {
#include "WS.h"
#include "WSRender.h"
#include "WSInput.h"
#include "WSFileio.h"
#include "WSApu.h"
}

#include <new>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
__attribute__((section(".papp_manifest"), used))
extern const papp_manifest_t __papp_manifest = {
    .magic=PAPP_MANIFEST_MAGIC, .sdk_version=PAPP_SDK_VERSION,
    .id="emu.wonderswan", .name="WonderSwan", .version=PAPP_APP_VERSION,
    .main_stack=32768
};
}

static papp_surface_t *s_surface;
static papp_audio_stream_t *s_audio;
static int16_t *s_audio_frame;
static uint16_t *s_frame;
static uint8_t *s_rom;
static int s_can_exit;
static int s_leave_game;
static int s_apu_ready;
static int s_render_ready;
static int s_rom_ready;

/* The P4 display path presents at 60 Hz. Oswan emits four held samples at
   each of 159 HBlank intervals per frame: 159 * 4 * 60 = 38,160 Hz. */
static const uint32_t WS_AUDIO_RATE = 38160;

extern "C" void init_ModRM_tables(void);
extern "C" void WsAllocateBuffers(void);

void *operator new(size_t n) noexcept { return papp_alloc(n, PAPP_MEM_LARGE, 16); }
void *operator new[](size_t n) noexcept { return papp_alloc(n, PAPP_MEM_LARGE, 16); }
void operator delete(void *p) noexcept { papp_free(p); }
void operator delete[](void *p) noexcept { papp_free(p); }
void operator delete(void *p, size_t) noexcept { papp_free(p); }
void operator delete[](void *p, size_t) noexcept { papp_free(p); }

extern "C" unsigned long SDL_UXTimerRead(void)
{
    return (unsigned long)papp_time_us();
}

extern "C" int ws_input_poll(int mode)
{
    papp_gamepad_t pad = {0};
    papp_input_gamepad(0, &pad);
    if (s_can_exit && (papp_exit_requested() || (pad.buttons & PAPP_BTN_L3))) {
        /* Finish the current emulation frame and unwind normally. Jumping out
           of the CPU core skips its frame/interrupt stack and made teardown
           unreliable on device. */
        s_leave_game = 1;
        return 0;
    }
    int state = 0;
    if (mode == 0) {
        if (pad.buttons & PAPP_BTN_UP) state |= 1 << 4;
        if (pad.buttons & PAPP_BTN_RIGHT) state |= 1 << 5;
        if (pad.buttons & PAPP_BTN_DOWN) state |= 1 << 6;
        if (pad.buttons & PAPP_BTN_LEFT) state |= 1 << 7;
    } else {
        if (pad.buttons & PAPP_BTN_UP) state |= 1 << 0;
        if (pad.buttons & PAPP_BTN_RIGHT) state |= 1 << 1;
        if (pad.buttons & PAPP_BTN_DOWN) state |= 1 << 2;
        if (pad.buttons & PAPP_BTN_LEFT) state |= 1 << 3;
    }
    if (pad.buttons & PAPP_BTN_A) state |= 1 << 10;
    if (pad.buttons & PAPP_BTN_B) state |= 1 << 11;
    if (pad.buttons & PAPP_BTN_SELECT) state |= 1 << 8;
    if (pad.buttons & PAPP_BTN_START) state |= 1 << 9;
    return state;
}

static void audio_submit(void)
{
    int available = apuBufLen();
    if (!s_audio || available <= 0) return;
    if (available > SND_RNGSIZE) available = SND_RNGSIZE;
    int32_t read = rBuf;
    for (int i = 0; i < available; ++i) {
        s_audio_frame[2*i] = sndbuffer[0][read];
        s_audio_frame[2*i+1] = sndbuffer[1][read];
        if (++read >= SND_RNGSIZE) read = 0;
    }
    /* The launcher can accept fewer frames than requested when its stream
       ring is full or the write times out. Keep unaccepted samples in the
       emulator ring so a transient scheduling delay does not become a gap. */
    size_t written = papp_audio_write(s_audio, s_audio_frame, (size_t)available, 1000);
    if (written > (size_t)available) written = (size_t)available;
    rBuf = (rBuf + (int32_t)written) % SND_RNGSIZE;
}

extern "C" void ws_graphics_paint(void)
{
    if (!FrameBuffer || !s_surface || !s_frame) return;
    for (int y = 0; y < LCD_MAIN_H; ++y)
        memcpy(s_frame + y * LCD_MAIN_W, FrameBuffer + y * SCREEN_WIDTH,
               LCD_MAIN_W * sizeof(uint16_t));
    papp_disp_present_ext(s_surface, s_frame);
    audio_submit();
}

static void cleanup(void)
{
    s_can_exit = 0;
    if (s_rom_ready) { WsRelease(); s_rom_ready = 0; }
    if (s_render_ready) { FreeBuffers(); s_render_ready = 0; }
    if (s_apu_ready) { apuEnd(); s_apu_ready = 0; }
    if (s_audio) { papp_audio_close(s_audio); s_audio = NULL; }
    if (s_surface) { papp_disp_surface_destroy(s_surface); s_surface = NULL; }
    papp_free(s_audio_frame); s_audio_frame = NULL;
    papp_free(s_frame); s_frame = NULL;
    papp_free(s_rom); s_rom = NULL;
}

static int play(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0, SEEK_END) != 0) {
        if (file) fclose(file);
        papp_ui_message("WonderSwan", "Could not open the selected ROM.");
        return -1;
    }
    long size = ftell(file);
    if (size < 10 || size > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        papp_ui_message("WonderSwan", "ROM size is outside the supported range.");
        return -1;
    }
    s_rom = (uint8_t *)papp_alloc((size_t)size, PAPP_MEM_LARGE, 16);
    s_frame = (uint16_t *)papp_alloc(LCD_MAIN_W * LCD_MAIN_H * sizeof(uint16_t),
                                     PAPP_MEM_LARGE | PAPP_MEM_DMA, 64);
    s_audio_frame = (int16_t *)papp_alloc(SND_RNGSIZE * 2u * sizeof(int16_t),
                                          PAPP_MEM_LARGE, 16);
    if (!s_rom || !s_frame || !s_audio_frame ||
        fread(s_rom, 1, (size_t)size, file) != (size_t)size) {
        fclose(file); cleanup(); return -1;
    }
    fclose(file);

    papp_surface_cfg_t display = { .width=LCD_MAIN_W, .height=LCD_MAIN_H,
        .format=PAPP_PIXFMT_RGB565, .scale=PAPP_SCALE_FIT_ASPECT, .buffers=2 };
    s_surface = papp_disp_surface_create(&display);
    papp_audio_cfg_t audio = { .rate=WS_AUDIO_RATE, .channels=2, .format=PAPP_AUDIO_S16,
        .latency_ms=100 };
    s_audio = papp_audio_open(&audio);
    if (!s_surface || !s_audio) { cleanup(); return -1; }

    WsAllocateBuffers();
    s_render_ready = 1;
    s_rom_ready = 1;
    if (!ROMMap || !RAMMap || !IO || !IRAM) {
        papp_ui_message("WonderSwan", "Not enough memory for emulator state.");
        cleanup();
        return -1;
    }
    AllocateBuffers();
    if (!Palette || !SprTMap || !FrameBuffer) {
        papp_ui_message("WonderSwan", "Not enough memory for the display buffers.");
        cleanup();
        return -1;
    }
    s_apu_ready = 1;
    apuInit();
    int result = -1;
    papp_log("WonderSwan PAPP2: loading %s (%ld bytes)\n", path, size);
    if (WsCreateFromMemory(s_rom, (size_t)size) != 0) {
        papp_ui_message("WonderSwan", "ROM mapping failed.");
    } else {
        s_rom_ready = 1;
        s_leave_game = 0;
        s_can_exit = 1;
        /* Queue two emulated frames before playback. The display callback
           feeds audio at frame boundaries, so this cushion absorbs a slow
           render/present without letting the mixer empty the stream ring. */
        papp_audio_pause(s_audio, true);
        for (int i = 0; i < 2 && !s_leave_game && !papp_exit_requested(); ++i)
            WsRun();
        papp_audio_pause(s_audio, false);
        uint32_t previous = (uint32_t)(papp_time_us() / 1000);
        int menu_was_down = 0, volume_was_down = 0;
        while (!papp_exit_requested() && !s_leave_game) {
            papp_gamepad_t pad = {0};
            papp_input_gamepad(0, &pad);
            if (pad.buttons & PAPP_BTN_L3) break;
            int menu_down = !!(pad.buttons & PAPP_BTN_X);
            int volume_down = !!(pad.buttons & PAPP_BTN_Y);
            if (menu_down && !menu_was_down) {
                papp_audio_pause(s_audio, true);
                static const char *const items[] = { "RESUME GAME", "EXIT GAME" };
                int choice = papp_ui_menu("WONDERSWAN", items, 2, 0);
                papp_audio_pause(s_audio, false);
                if (choice == 1) break;
            }
            if (volume_down && !volume_was_down) papp_ui_volume();
            menu_was_down = menu_down;
            volume_was_down = volume_down;
            WsRun();
            if (s_leave_game || papp_exit_requested()) break;
            uint32_t now = (uint32_t)(papp_time_us() / 1000);
            uint32_t elapsed = now - previous;
            if (elapsed < 13) papp_delay_ms(13 - elapsed);
            previous = now;
        }
        result = 0;
    }
    s_can_exit = 0;
    if (s_audio) {
        papp_audio_stats_t stats = {0};
        papp_audio_stats(s_audio, &stats);
        papp_log("WonderSwan audio: played=%llu queued=%lu/%lu underruns=%lu\n",
                 (unsigned long long)stats.played_frames,
                 (unsigned long)stats.queued_frames,
                 (unsigned long)stats.capacity_frames,
                 (unsigned long)stats.underruns);
        papp_audio_pause(s_audio, true);
        papp_audio_clear(s_audio);
    }
    cleanup();
    return result;
}

int main(int argc, char **argv)
{
    init_ModRM_tables();
    if (argc > 1) return play(argv[1]);
    papp_pick_cfg_t cfg = { .title="WonderSwan", .root="/sd/roms/ws",
        .extensions=".ws,.wsc" };
    static papp_pick_result_t selected;
    while (!papp_exit_requested()) {
        if (papp_ui_pick_file(&cfg, &selected) == PAPP_PICK_CANCEL) break;
        play(selected.path);
    }
    return 0;
}
