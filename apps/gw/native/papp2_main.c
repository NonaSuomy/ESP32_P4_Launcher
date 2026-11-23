/* Direct PAPP2 frontend for the LCD-Game-Emulator Game & Watch core. */
#include "papp/papp.h"
#include "gw_system.h"
#include "gw_romloader.h"
#include "rom_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

PAPP_MANIFEST(.id="emu.gw", .name="Game & Watch", .version=PAPP_APP_VERSION,
              .main_stack=32768);

unsigned char *ROM_DATA;
unsigned int ROM_DATA_LENGTH;
static uint16_t *s_frame;
static papp_surface_t *s_surface;
static papp_audio_stream_t *s_audio;
static papp_thread_t *s_video_thread;

#define GW_VIDEO_SLOTS 3u
#define GW_VIDEO_PIXELS (GW_SCREEN_WIDTH * GW_SCREEN_HEIGHT)
enum { SLOT_FREE, SLOT_WRITING, SLOT_READY, SLOT_READING };
static struct {
    uint16_t *pixels[GW_VIDEO_SLOTS];
    volatile unsigned state[GW_VIDEO_SLOTS];
    unsigned sequence[GW_VIDEO_SLOTS];
    unsigned next_sequence;
    unsigned dropped;
    volatile unsigned stop;
    volatile unsigned done;
} s_video;

void HCD_shutdown(void) {}

void *my_special_alloc(unsigned char speed, unsigned char bytes, unsigned long size)
{
    (void)speed;
    (void)bytes;
    void *p = malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

static int video_worker(void *unused)
{
    (void)unused;
    for (;;) {
        int selected = -1;
        unsigned oldest = UINT32_MAX;
        for (unsigned i = 0; i < GW_VIDEO_SLOTS; ++i) {
            if (__atomic_load_n(&s_video.state[i], __ATOMIC_ACQUIRE) == SLOT_READY &&
                s_video.sequence[i] < oldest) {
                oldest = s_video.sequence[i];
                selected = (int)i;
            }
        }
        if (selected >= 0) {
            unsigned expected = SLOT_READY;
            if (__atomic_compare_exchange_n(&s_video.state[selected], &expected,
                    SLOT_READING, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
                if (s_surface) papp_disp_present_ext(s_surface, s_video.pixels[selected]);
                __atomic_store_n(&s_video.state[selected], SLOT_FREE, __ATOMIC_RELEASE);
                continue;
            }
        }
        if (__atomic_load_n(&s_video.stop, __ATOMIC_ACQUIRE)) break;
        papp_delay_ms(1);
    }
    __atomic_store_n(&s_video.done, 1, __ATOMIC_RELEASE);
    return 0;
}

static int video_start(void)
{
    memset(&s_video, 0, sizeof(s_video));
    for (unsigned i = 0; i < GW_VIDEO_SLOTS; ++i) {
        s_video.pixels[i] = papp_alloc(GW_VIDEO_PIXELS * sizeof(uint16_t), PAPP_MEM_LARGE, 16);
        if (!s_video.pixels[i]) return -1;
    }
    papp_thread_cfg_t cfg = { .stack_size=8192 };
    s_video_thread = papp_thread_create(video_worker, NULL, &cfg);
    return s_video_thread ? 0 : -1;
}

static void video_submit(const uint16_t *frame)
{
    if (!s_video_thread) {
        if (s_surface) papp_disp_present_ext(s_surface, frame);
        return;
    }
    for (unsigned n = 0; n < GW_VIDEO_SLOTS; ++n) {
        const unsigned i = (s_video.next_sequence + n) % GW_VIDEO_SLOTS;
        unsigned expected = SLOT_FREE;
        if (!__atomic_compare_exchange_n(&s_video.state[i], &expected, SLOT_WRITING,
                0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) continue;
        memcpy(s_video.pixels[i], frame, GW_VIDEO_PIXELS * sizeof(uint16_t));
        s_video.sequence[i] = s_video.next_sequence++;
        __atomic_store_n(&s_video.state[i], SLOT_READY, __ATOMIC_RELEASE);
        return;
    }
    __atomic_fetch_add(&s_video.dropped, 1, __ATOMIC_RELAXED);
}

static void video_stop(void)
{
    if (s_video_thread) {
        __atomic_store_n(&s_video.stop, 1, __ATOMIC_RELEASE);
        int code = 0;
        papp_thread_join(s_video_thread, PAPP_WAIT_FOREVER, &code);
        s_video_thread = NULL;
    }
    for (unsigned i = 0; i < GW_VIDEO_SLOTS; ++i) {
        papp_free(s_video.pixels[i]);
        s_video.pixels[i] = NULL;
    }
}

unsigned int gw_get_buttons(void)
{
    papp_gamepad_t pad = {0};
    papp_input_gamepad(0, &pad);
    unsigned int buttons = 0;
    if (pad.buttons & PAPP_BTN_LEFT) buttons |= GW_BUTTON_LEFT;
    if (pad.buttons & PAPP_BTN_UP) buttons |= GW_BUTTON_UP;
    if (pad.buttons & PAPP_BTN_RIGHT) buttons |= GW_BUTTON_RIGHT;
    if (pad.buttons & PAPP_BTN_DOWN) buttons |= GW_BUTTON_DOWN;
    if (pad.buttons & PAPP_BTN_A) buttons |= GW_BUTTON_A;
    if (pad.buttons & PAPP_BTN_B) buttons |= GW_BUTTON_B;
    if (pad.buttons & PAPP_BTN_SELECT) buttons |= GW_BUTTON_TIME;
    if (pad.buttons & PAPP_BTN_START) buttons |= GW_BUTTON_GAME;
    return buttons;
}

static void audio_submit(void)
{
    static int16_t frame[GW_AUDIO_BUFFER_LENGTH * 2];
    for (unsigned i = 0; i < GW_AUDIO_BUFFER_LENGTH; ++i) {
        int16_t sample = (int16_t)((gw_audio_buffer[i] & 3u) * 7000);
        frame[2 * i] = sample;
        frame[2 * i + 1] = sample;
    }
    if (s_audio) {
        size_t written = 0;
        while (written < GW_AUDIO_BUFFER_LENGTH && !papp_exit_requested()) {
            size_t n = papp_audio_write(s_audio, frame + written * 2,
                                        GW_AUDIO_BUFFER_LENGTH - written, 100);
            if (!n) {
                if (papp_exit_requested()) break;
                papp_delay_ms(1);
                continue;
            }
            written += n;
        }
    }
    gw_audio_buffer_copied = true;
}

static void set_emulated_time(void)
{
    time_t now = time(NULL);
    struct tm local;
    if (!localtime_r(&now, &local)) return;
    gw_time_t t = { (unsigned char)local.tm_hour, (unsigned char)local.tm_min,
                    (unsigned char)local.tm_sec };
    gw_system_set_time(t);
}

static int play(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { papp_ui_message("Game & Watch", "Could not open the selected ROM."); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long end = ftell(f);
    if (end <= 0 || end > 400000 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        papp_ui_message("Game & Watch", "The selected .gw ROM exceeds the supported size.");
        return -1;
    }
    ROM_DATA = papp_alloc((size_t)end, PAPP_MEM_LARGE, 16);
    s_frame = papp_alloc(GW_VIDEO_PIXELS * sizeof(uint16_t), PAPP_MEM_LARGE, 16);
    if (!ROM_DATA || !s_frame || fread(ROM_DATA, 1, (size_t)end, f) != (size_t)end) {
        fclose(f);
        papp_free(ROM_DATA); ROM_DATA = NULL;
        papp_free(s_frame); s_frame = NULL;
        return -1;
    }
    fclose(f);
    ROM_DATA_LENGTH = (unsigned int)end;
    memset(s_frame, 0, GW_VIDEO_PIXELS * sizeof(uint16_t));

    papp_surface_cfg_t disp = { .width=GW_SCREEN_WIDTH, .height=GW_SCREEN_HEIGHT,
        .format=PAPP_PIXFMT_RGB565, .scale=PAPP_SCALE_FIT_ASPECT, .buffers=2 };
    s_surface = papp_disp_surface_create(&disp);
    papp_audio_cfg_t audio_cfg = { .rate=GW_AUDIO_FREQ, .channels=2, .format=PAPP_AUDIO_S16 };
    s_audio = papp_audio_open(&audio_cfg);
    if (!s_surface || !s_audio) goto cleanup;
    if (!gw_system_romload()) {
        papp_ui_message("Game & Watch", "The .gw ROM could not be decoded.");
        goto cleanup;
    }
    gw_system_sound_init();
    if (!gw_system_config()) {
        papp_ui_message("Game & Watch", "This Game & Watch CPU is not supported.");
        goto cleanup;
    }
    gw_system_start();
    gw_system_reset();
    set_emulated_time();
    video_start();

    int menu_was_down = 0, volume_was_down = 0;
    unsigned frames = 0;
    for (;;) {
        papp_gamepad_t pad = {0};
        papp_input_gamepad(0, &pad);
        if (papp_exit_requested()) break;
        int menu_down = !!(pad.buttons & PAPP_BTN_X);
        int volume_down = !!(pad.buttons & PAPP_BTN_Y);
        if (menu_down && !menu_was_down) {
            papp_audio_pause(s_audio, true);
            static const char *const items[] = { "RESUME GAME", "EXIT GAME" };
            int choice = papp_ui_menu("GAME & WATCH", items, 2, 0);
            papp_audio_pause(s_audio, false);
            if (choice == 1) break;
        }
        if (volume_down && !volume_was_down) papp_ui_volume();
        menu_was_down = menu_down;
        volume_was_down = volume_down;

        gw_system_run(GW_SYSTEM_CYCLES);
        if ((frames++ & 1u) == 0u) {
            gw_system_blit(s_frame);
            video_submit(s_frame);
        }
        audio_submit();
    }

cleanup:
    video_stop();
    if (GW_ROM) { papp_free(GW_ROM); GW_ROM = NULL; }
    if (s_audio) { papp_audio_close(s_audio); s_audio = NULL; }
    if (s_surface) { papp_disp_surface_destroy(s_surface); s_surface = NULL; }
    papp_free(s_frame); s_frame = NULL;
    papp_free(ROM_DATA); ROM_DATA = NULL; ROM_DATA_LENGTH = 0;
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) return play(argv[1]);
    papp_pick_cfg_t cfg = { .title="Game & Watch", .root="/sd/roms/gw",
        .extensions=".gw,.lcd" };
    static papp_pick_result_t selected;
    while (!papp_exit_requested()) {
        int action = papp_ui_pick_file(&cfg, &selected);
        if (action == PAPP_PICK_CANCEL) break;
        play(selected.path);
    }
    return 0;
}
