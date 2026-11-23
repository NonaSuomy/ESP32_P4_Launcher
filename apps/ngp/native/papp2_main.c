/* Native PAPP2 frontend for the Race Neo Geo Pocket / Pocket Color core. */
#define RETRO_COMPAT_IMPLEMENTATION

#include "papp/papp.h"
#include "types.h"
#include "retro_compat.h"
#include "race-memory.h"
#include "graphics.h"
#include "input.h"
#include "tlcs900h.h"
#include "flash.h"
#include "neopopsound.h"
#include "cz80_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

PAPP_MANIFEST(.id = "emu.ngp", .name = "Neo Geo Pocket", .version = PAPP_APP_VERSION,
              .main_stack = 32768);

/* Race's renderer and memory mapper use these hardware-neutral globals. */
unsigned short *drawBuffer;
volatile unsigned g_frame_ready;
unsigned char *rasterY;
unsigned char *frame0Pri;
unsigned char *frame1Pri;
unsigned char *color_switch;
unsigned char *scanlineY;
unsigned char *scrollSpriteX;
unsigned char *scrollSpriteY;
unsigned char *sprite_palette_numbers;
unsigned char *sprite_table;
unsigned short *patterns;
unsigned char *oowSelect;
unsigned short *oowTable;
unsigned char *wndTopLeftY;
unsigned char *wndSizeY;
unsigned char *wndSizeX;
unsigned char *bgSelect;
unsigned char *bw_palette_table;
unsigned short *palette_table;
unsigned short *bgTable;
unsigned char *wndTopLeftX;
unsigned char *scrollFrontY;
unsigned char *scrollFrontX;
unsigned short *tile_table_front;
unsigned char *scrollBackY;
unsigned short *tile_table_back;
unsigned char *scrollBackX;
unsigned char *pattern_table;
int m_bIsActive = 1;
int gfx_hacks;
int tipo_consola;
char retro_save_directory[3] = "/";

static struct ngp_screen s_screen;
static papp_surface_t *s_surface;
static papp_audio_stream_t *s_audio;
static uint16_t *s_audio_psg;
static uint16_t *s_audio_dac;
static int16_t *s_audio_mix;

enum {
    NGP_AUDIO_RATE = 22050,
    NGP_AUDIO_SAMPLES = 368,
    NGP_SCREEN_WIDTH = 160,
    NGP_SCREEN_HEIGHT = 152,
    NGP_SCREEN_STRIDE = 260,
};

extern int Cz80_allocate_flag_tables(void);
extern void audio_dac_init(void);
extern void tlcs_reset(void) __attribute__((weak));
extern unsigned char ngpInputState;

static void map_vdp_tables(void)
{
    sprite_table = (unsigned char *)get_address(0x00008800);
    pattern_table = (unsigned char *)get_address(0x0000A000);
    patterns = (unsigned short *)pattern_table;
    tile_table_front = (unsigned short *)get_address(0x00009000);
    tile_table_back = (unsigned short *)get_address(0x00009800);
    palette_table = (unsigned short *)get_address(0x00008200);
    bw_palette_table = (unsigned char *)get_address(0x00008100);
    sprite_palette_numbers = (unsigned char *)get_address(0x00008C00);
    scanlineY = (unsigned char *)get_address(0x00008009);
    frame0Pri = (unsigned char *)get_address(0x00008000);
    frame1Pri = (unsigned char *)get_address(0x00008030);
    wndTopLeftX = (unsigned char *)get_address(0x00008002);
    wndTopLeftY = (unsigned char *)get_address(0x00008003);
    wndSizeX = (unsigned char *)get_address(0x00008004);
    wndSizeY = (unsigned char *)get_address(0x00008005);
    scrollSpriteX = (unsigned char *)get_address(0x00008020);
    scrollSpriteY = (unsigned char *)get_address(0x00008021);
    scrollFrontX = (unsigned char *)get_address(0x00008032);
    scrollFrontY = (unsigned char *)get_address(0x00008033);
    scrollBackX = (unsigned char *)get_address(0x00008034);
    scrollBackY = (unsigned char *)get_address(0x00008035);
    bgSelect = (unsigned char *)get_address(0x00008118);
    bgTable = (unsigned short *)get_address(0x000083E0);
    oowSelect = (unsigned char *)get_address(0x00008012);
    oowTable = (unsigned short *)get_address(0x000083F0);
    color_switch = (unsigned char *)get_address(0x00006F91);
    rasterY = scanlineY;
}

static void set_boot_defaults(void)
{
    tlcsMemWriteB(0x00006F91, tlcsMemReadB(0x00200023));
    if (tipo_consola == 1) tlcsMemWriteB(0x00006F91, 0x00);
    tlcsMemWriteB(0x00006F87, 0x01); /* English */
    tlcsMemWriteB(0x00004000, tlcsMemReadB(0x00004000) | 0xC0);
    tlcsMemWriteB(0x00006F84, 0x40);
    tlcsMemWriteB(0x00006F85, 0x00);
    tlcsMemWriteB(0x00006F86, 0x00);
}

void graphics_paint(unsigned char render)
{
    if (render && s_surface && drawBuffer)
        papp_disp_present_ext(s_surface, drawBuffer);
}

static int load_rom(const char *path, uint8_t **rom_out, size_t *size_out)
{
    FILE *file = fopen(path, "rb");
    if (!file) return -1;

    int result = -1;
    if (fseek(file, 0, SEEK_END) != 0) goto done;
    long length = ftell(file);
    if (length < 64 || length > 16 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0)
        goto done;

    uint8_t *rom = papp_alloc((size_t)length, PAPP_MEM_LARGE, 16);
    if (!rom) goto done;
    if (fread(rom, 1, (size_t)length, file) != (size_t)length) {
        papp_free(rom);
        goto done;
    }

    *rom_out = rom;
    *size_out = (size_t)length;
    result = 0;
done:
    fclose(file);
    return result;
}

static void update_input(void)
{
    papp_gamepad_t pad = {0};
    (void)papp_input_gamepad(0, &pad);
    ngpInputState = 0;
    if (pad.buttons & PAPP_BTN_UP) ngpInputState |= 1u << 0;
    if (pad.buttons & PAPP_BTN_DOWN) ngpInputState |= 1u << 1;
    if (pad.buttons & PAPP_BTN_LEFT) ngpInputState |= 1u << 2;
    if (pad.buttons & PAPP_BTN_RIGHT) ngpInputState |= 1u << 3;
    if (pad.buttons & PAPP_BTN_A) ngpInputState |= 1u << 4;
    if (pad.buttons & PAPP_BTN_B) ngpInputState |= 1u << 5;
    if (pad.buttons & (PAPP_BTN_START | PAPP_BTN_SELECT)) ngpInputState |= 1u << 6;
}

static void submit_audio(void)
{
    sound_update(s_audio_psg, NGP_AUDIO_SAMPLES * (int)sizeof(uint16_t));
    dac_update(s_audio_dac, NGP_AUDIO_SAMPLES * (int)sizeof(uint16_t));

    for (unsigned i = 0; i < NGP_AUDIO_SAMPLES; ++i) {
        int32_t sample = (int16_t)s_audio_psg[i] + (int16_t)s_audio_dac[i];
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        s_audio_mix[2 * i] = (int16_t)sample;
        s_audio_mix[2 * i + 1] = (int16_t)sample;
    }

    if (!s_audio) return;
    size_t written = 0;
    while (written < NGP_AUDIO_SAMPLES && !papp_exit_requested()) {
        size_t count = papp_audio_write(s_audio, s_audio_mix + written * 2,
                                        NGP_AUDIO_SAMPLES - written, 100);
        if (count == 0) {
            if (papp_exit_requested()) break;
            papp_delay_ms(1);
        } else {
            written += count;
        }
    }
}

static int open_emulator_resources(void)
{
    s_screen.w = NGP_SCREEN_STRIDE;
    s_screen.h = NGP_SCREEN_HEIGHT;
    s_screen.pixels = papp_alloc(NGP_SCREEN_STRIDE * NGP_SCREEN_HEIGHT * sizeof(uint16_t),
                                 PAPP_MEM_LARGE, 16);
    s_audio_psg = papp_alloc(NGP_AUDIO_SAMPLES * sizeof(uint16_t), PAPP_MEM_LARGE, 16);
    s_audio_dac = papp_alloc(NGP_AUDIO_SAMPLES * sizeof(uint16_t), PAPP_MEM_LARGE, 16);
    s_audio_mix = papp_alloc(NGP_AUDIO_SAMPLES * 2 * sizeof(int16_t), PAPP_MEM_LARGE, 16);
    if (!s_screen.pixels || !s_audio_psg || !s_audio_dac || !s_audio_mix) return -1;

    memset(s_screen.pixels, 0, NGP_SCREEN_STRIDE * NGP_SCREEN_HEIGHT * sizeof(uint16_t));
    memset(s_audio_psg, 0, NGP_AUDIO_SAMPLES * sizeof(uint16_t));
    memset(s_audio_dac, 0, NGP_AUDIO_SAMPLES * sizeof(uint16_t));

    papp_surface_cfg_t display_cfg = {
        .width = NGP_SCREEN_WIDTH,
        .height = NGP_SCREEN_HEIGHT,
        .format = PAPP_PIXFMT_RGB565,
        .scale = PAPP_SCALE_FIT_ASPECT,
        .buffers = 2,
    };
    s_surface = papp_disp_surface_create(&display_cfg);

    papp_audio_cfg_t audio_cfg = {
        .rate = NGP_AUDIO_RATE,
        .channels = 2,
        .format = PAPP_AUDIO_S16,
    };
    s_audio = papp_audio_open(&audio_cfg);
    return s_surface ? 0 : -1;
}

static void close_emulator_resources(void)
{
    if (s_audio) {
        papp_audio_clear(s_audio);
        papp_audio_close(s_audio);
        s_audio = NULL;
    }
    if (s_surface) {
        papp_disp_surface_destroy(s_surface);
        s_surface = NULL;
    }
    papp_free(s_screen.pixels);
    papp_free(s_audio_psg);
    papp_free(s_audio_dac);
    papp_free(s_audio_mix);
    s_screen.pixels = NULL;
    s_audio_psg = NULL;
    s_audio_dac = NULL;
    s_audio_mix = NULL;
    drawBuffer = NULL;
}

static int run_rom(const char *path)
{
    uint8_t *rom = NULL;
    size_t rom_size = 0;
    int memory_initialized = 0;
    int result = -1;

    if (load_rom(path, &rom, &rom_size) != 0) {
        papp_ui_message("Neo Geo Pocket", "Could not open the selected ROM.");
        return -1;
    }
    if (open_emulator_resources() != 0) {
        papp_ui_message("Neo Geo Pocket", "Not enough memory to start the emulator.");
        goto cleanup;
    }

    const char *extension = strrchr(path, '.');
    tipo_consola = extension && strcasecmp(extension, ".ngp") == 0;
    m_emuInfo.machine = tipo_consola ? NGP : NGPC;
    m_emuInfo.romSize = (int)rom_size;
    screen = &s_screen;
    drawBuffer = (unsigned short *)s_screen.pixels;

    if (!ngp_mem_set_rom(rom, rom_size)) {
        papp_ui_message("Neo Geo Pocket", "Could not map the selected ROM.");
        goto cleanup;
    }
    memory_initialized = 1;
    if (!ngp_mem_alloc_init(0x21000u, 0x10000u)) {
        papp_ui_message("Neo Geo Pocket", "Not enough memory for emulator state.");
        goto cleanup;
    }

    papp_log("NGP PAPP2: loading %s (%u bytes)\n", path, (unsigned)rom_size);
    Cz80_allocate_flag_tables();
    ngp_mem_init();
    map_vdp_tables();
    if (bgTable) bgTable[0] = 0xFFFF;
    if (bgSelect) *bgSelect |= 0x80;
    tlcs_init();
    if (tlcs_reset) tlcs_reset();
    Z80_Init();
    Z80_Reset();
    set_boot_defaults();
    if (!graphics_init()) {
        papp_ui_message("Neo Geo Pocket", "Could not initialize graphics.");
        goto cleanup_core;
    }
    if (wndSizeX && wndSizeY && (*wndSizeX == 0 || *wndSizeY == 0)) {
        *wndTopLeftX = 0;
        *wndTopLeftY = 0;
        *wndSizeX = NGP_SCREEN_WIDTH;
        *wndSizeY = NGP_SCREEN_HEIGHT;
    }
    if (frame0Pri) *frame0Pri |= 0xC0;
    tlcsMemWriteB(0x00004000, 0xC0);
    audio_dac_init();
    sound_init(NGP_AUDIO_RATE);

    {
        static const char *const menu_items[] = {"Resume Game", "Exit Game"};
        const uint32_t frame_cycles = 5700000u / 60u;
        int menu_was_down = 0;
        int volume_was_down = 0;
        int exit_game = 0;
        int64_t next_frame = papp_time_us();
        uint32_t frames = 0;
        papp_log("NGP PAPP2: running %s\n", path);

        while (!papp_exit_requested() && !exit_game) {
            papp_gamepad_t pad = {0};
            (void)papp_input_gamepad(0, &pad);
            update_input();

            int menu_down = !!(pad.buttons & PAPP_BTN_X);
            int volume_down = !!(pad.buttons & PAPP_BTN_Y);
            if (menu_down && !menu_was_down) {
                if (s_audio) papp_audio_pause(s_audio, true);
                int selection = papp_ui_menu("NEO GEO POCKET", menu_items, 2, 0);
                if (s_audio) papp_audio_pause(s_audio, false);
                if (selection == 1) exit_game = 1;
            }
            if (volume_down && !volume_was_down) (void)papp_ui_volume();
            menu_was_down = menu_down;
            volume_was_down = volume_down;
            if (papp_exit_requested()) break;

            tlcs_execute((int)frame_cycles);
            graphics_paint(1);
            submit_audio();

            ++frames;
            next_frame += 16667;
            int64_t now = papp_time_us();
            if (next_frame < now - 250000) next_frame = now;
            while (!papp_exit_requested() && (now = papp_time_us()) < next_frame)
                papp_delay_ms(1);
            if ((frames & 599u) == 0u)
                papp_log("NGP PAPP2: frames=%u\n", (unsigned)frames);
        }
    }
    result = 0;

cleanup_core:
    flashShutdown();
    if (memory_initialized) ngp_mem_free();
cleanup:
    close_emulator_resources();
    papp_free(rom);
    return result;
}

int main(void)
{
    papp_pick_cfg_t picker = {
        .title = "Neo Geo Pocket",
        .root = "/sd/roms/ngp",
        .extensions = ".ngp,.ngc",
    };
    papp_pick_result_t selected = {0};

    while (!papp_exit_requested()) {
        int action = papp_ui_pick_file(&picker, &selected);
        if (action == PAPP_PICK_CANCEL) break;
        (void)run_rom(selected.path);
    }
    return 0;
}
