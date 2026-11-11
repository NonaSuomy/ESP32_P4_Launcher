/*
 * snes_run.c — Entry point for SNES (snes9x) emulator on ESP32-P4
 *
 * Loads a .smc/.sfc ROM from SD card into PSRAM, runs the snes9x emulator,
 * handles in-game menu, and returns to the launcher when the user exits.
 */

#include "snes_run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_log.h"

/* snes9x core headers */
#include "snes9x/snes9x.h"
#include "snes9x/soundux.h"
#include "snes9x/memmap.h"
#include "snes9x/apu.h"
#include "snes9x/display.h"
#include "snes9x/cpuexec.h"
#include "snes9x/gfx.h"
#include "snes9x/ppu.h"
#include "snes9x/fxemu.h"
#include "snes9x/fxinst.h"
#include "snes9x/save.h"

/* Odroid platform */
#include "odroid_settings.h"
#include "odroid_audio.h"
#include "odroid_input.h"
#include "odroid_system.h"
#include "odroid_display.h"
#include "odroid_sdcard.h"
#include "odroid_menu.h"

static const char *TAG = "SNES_RUN";

/* ─── Configuration ─────────────────────────────────────────────── */
#define SNES_FB_W         256
#define SNES_FB_H         224
#define SNES_FB_H_EXT     239
#define AUDIO_SAMPLE_RATE 32000
#define AUDIO_FRAG_SIZE   512
#define TARGET_FPS        60
#define SNES_MAX_FRAMESKIP 3   /* max consecutive render-skips to hold 60 FPS game speed */

/* ─── Globals required by snes9x core ──────────────────────────── */
bool overclock_cycles = false;
int one_c = 4, slow_one_c = 5, two_c = 6;
extern SGFX GFX;

/* ─── Module state ─────────────────────────────────────────────── */
static volatile bool snes_quit_flag = false;
static volatile bool snes_audio_resync = false;  /* signal audio task to reset time base */
static volatile bool videoTaskRunning = false;
static volatile bool audioTaskRunning = false;

static uint16_t *snes_fb[2] = { NULL, NULL };  /* double-buffer for SNES output */
static uint8_t  *snes_subscreen = NULL;         /* dedicated SubScreen buffer */
static int snes_fb_write = 0;
static uint16_t *volatile snes_fb_busy;        /* buffer the video task is presenting (read by the PPA) */
static QueueHandle_t vidQueue;
static TaskHandle_t videoTaskHandle;

/* Audio offload — double-buffered audio, mixed and played on Core 1 */
static TaskHandle_t audioTaskHandle;
/* Frames handed to the audio task / finished by it (snes_wait_idle) */
static volatile uint32_t audio_req = 0, audio_done = 0;

#define AUD_QUEUE_DEPTH 2
static int16_t *audio_dma_buf[AUD_QUEUE_DEPTH] = { NULL, NULL };

/* Audio buffer (mixing — used by audio task on Core 1) */
static int16_t *audio_buf = NULL;

/* Coherent audio (Phase 50): the DSP advances exactly one emulated frame
 * (FRAME_SRC samples) per frame — locked to the SPC700 — then is time-stretched
 * to real time so game + sound stay in sync and the I2S never underruns. With
 * the 60 FPS cap the stretch is 1:1 at 60 FPS, up to ~60/46 in heavy scenes. */
#define FRAME_SRC    (AUDIO_SAMPLE_RATE / TARGET_FPS)   /* 533 stereo frames / emulated frame */
#define AUD_OUT_MAX  800                                 /* max stretched output frames */
static int16_t *coherent_buf = NULL;                     /* FRAME_SRC frames from MixStereo */

/* ─── snes9x platform callbacks ─────────────────────────────────── */

/* Pristine copies of GFX buffer pointers — defined in gfx.c, used to
 * detect/repair corruption (Load-access-fault crash on DKC level entry). */
extern uint8_t *saved_ZBuffer;
extern uint8_t *saved_SubZBuffer;

/*
 * Allocate Z-buffers at 512-pixel width (not 256) so that SNES mode 5/6
 * rendering (RenderedScreenWidth = 512) cannot overrun the buffers.
 */
#define ZBUF_WIDTH  512

bool S9xInitDisplay(void)
{
    GFX.Pitch = SNES_FB_W * 2;
    GFX.ZPitch = SNES_FB_W;
    GFX.Screen = (uint8_t *)snes_fb[0];

    /* Dedicated SubScreen buffer — required for color math and OBJ processing.
     * gfx.c uses SubScreen as scratch for OBJ line data and as the color
     * addition/subtraction source. Sharing with Screen causes corruption. */
    GFX.SubScreen = snes_subscreen;

    GFX.ZBuffer = (uint8_t *)heap_caps_malloc(ZBUF_WIDTH * SNES_FB_H_EXT,
                                               MALLOC_CAP_SPIRAM);
    GFX.SubZBuffer = (uint8_t *)heap_caps_malloc(ZBUF_WIDTH * SNES_FB_H_EXT,
                                                  MALLOC_CAP_SPIRAM);
    if (!GFX.ZBuffer || !GFX.SubZBuffer) {
        ESP_LOGE(TAG, "Failed to allocate Z-buffers");
        return false;
    }

    /* Keep pristine copies so we can repair corruption later */
    saved_ZBuffer    = GFX.ZBuffer;
    saved_SubZBuffer = GFX.SubZBuffer;
    ESP_LOGI(TAG, "ZBuffer=%p  SubZBuffer=%p  (512×%d)",
             GFX.ZBuffer, GFX.SubZBuffer, SNES_FB_H_EXT);
    return true;
}

void S9xDeinitDisplay(void) {}

void S9xToggleSoundChannel(int32_t channel) {}

bool S9xReadMousePosition(int32_t which, int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool S9xReadSuperScopePosition(int32_t *x, int32_t *y, uint32_t *buttons)
{
    return false;
}

bool JustifierOffscreen(void) { return true; }
void JustifierButtons(uint32_t *justifiers) { (void)justifiers; }

/* ─── Joypad mapping ────────────────────────────────────────────── */
/*
 * Native SNES controller → SNES button masks.
 * The USB adapter reports buttons in SNES label order (A/B/X/Y match the
 * physical SNES face buttons); L1/L2 both map to SNES L, R1/R2 to SNES R.
 * MENU and VOLUME are handled separately (touch shoulder virtual buttons).
 */
uint32_t S9xReadJoypad(int32_t port)
{
    if (port != 0) return 0;

    odroid_gamepad_state gp;
    odroid_input_gamepad_read(&gp);

    uint32_t joy = 0;

    if (gp.values[ODROID_INPUT_UP])     joy |= SNES_UP_MASK;
    if (gp.values[ODROID_INPUT_DOWN])   joy |= SNES_DOWN_MASK;
    if (gp.values[ODROID_INPUT_LEFT])   joy |= SNES_LEFT_MASK;
    if (gp.values[ODROID_INPUT_RIGHT])  joy |= SNES_RIGHT_MASK;

    if (gp.values[ODROID_INPUT_A])      joy |= SNES_A_MASK;
    if (gp.values[ODROID_INPUT_B])      joy |= SNES_B_MASK;
    if (gp.values[ODROID_INPUT_X])      joy |= SNES_X_MASK;
    if (gp.values[ODROID_INPUT_Y])      joy |= SNES_Y_MASK;

    if (gp.values[ODROID_INPUT_L])      joy |= SNES_TL_MASK;
    if (gp.values[ODROID_INPUT_R])      joy |= SNES_TR_MASK;

    if (gp.values[ODROID_INPUT_SELECT]) joy |= SNES_SELECT_MASK;
    if (gp.values[ODROID_INPUT_START])  joy |= SNES_START_MASK;

    return joy;
}

/* ─── Video Task ────────────────────────────────────────────────── */
/*
 * Runs on Core 1. Receives SNES framebuffers via queue, copies them
 * centered into the 320×240 LCD buffer, and pushes to the display.
 */
static void snes_video_task(void *arg)
{
    uint16_t *frame = NULL;
    videoTaskRunning = true;

    while (1) {
        xQueuePeek(vidQueue, &frame, portMAX_DELAY);

        if (frame == (uint16_t *)1) break;  /* Quit sentinel */
        snes_fb_busy = frame;

        /* BUILD_PIXEL_RGB565 now produces true 6-bit green at render time,
         * so no post-render green expansion loop is needed. */

        /* Direct 2× scale + 270° rotate — no 320×240 staging buffer */
        ili9341_write_frame_rgb565_custom(frame, SNES_FB_W, SNES_FB_H,
                                           2.0f, false);
        snes_fb_busy = NULL;

        xQueueReceive(vidQueue, &frame, portMAX_DELAY);
    }

    videoTaskRunning = false;
    vTaskDelete(NULL);
}

/* ─── Audio Task ────────────────────────────────────────────────── */
/*
 * Runs on Core 1. Receives mixed audio buffers via queue and submits
 * them to I2S (blocking), keeping the emulation loop on Core 0 unblocked.
 */
static void snes_audio_task(void *arg)
{
    audioTaskRunning = true;
    int dma_idx = 0;
    /* Coherent audio: each emulated frame, mix exactly FRAME_SRC samples so the
     * DSP advances one frame in lockstep with the SPC700 (no drift vs. the game),
     * then time-stretch that to the real-time sample count so the I2S stays fed.
     * Sound and video stay in sync; in sub-60 scenes the whole thing (audio too)
     * runs mildly slow together instead of the audio drifting ahead. */
    int64_t audio_start_time = esp_timer_get_time();
    int64_t samples_produced = 0;   /* real-time output samples fed to I2S */

    while (1) {
        /* Wait for the emu loop's per-frame signal */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (snes_quit_flag) break;
        uint32_t req = audio_req;

        if (snes_audio_resync) {
            snes_audio_resync = false;
            audio_start_time = esp_timer_get_time();
            samples_produced = 0;
        }

        /* 1. Produce exactly ONE emulated frame of DSP audio (coherent). */
        S9xMixSamples(coherent_buf, FRAME_SRC * 2);

        /* 2. Real-time output count (wall clock) so the I2S never underruns. */
        int64_t now = esp_timer_get_time();
        int64_t target = ((now - audio_start_time) * AUDIO_SAMPLE_RATE) / 1000000;
        int out_n = (int)(target - samples_produced);
        if (out_n < FRAME_SRC) out_n = FRAME_SRC;   /* 60 FPS-capped: usual path */
        if (out_n > AUD_OUT_MAX) {                   /* clamp catch-up burst (post-pause) */
            out_n = AUD_OUT_MAX;
            audio_start_time = now - (samples_produced * 1000000 / AUDIO_SAMPLE_RATE);
        }

        /* 3. Time-stretch FRAME_SRC coherent frames -> out_n output frames
         *    (linear interpolation, stereo). out_n >= FRAME_SRC, so this only
         *    ever stretches (mild slow-down), never compresses. */
        int16_t *dest = audio_dma_buf[dma_idx];
        if (out_n == FRAME_SRC) {
            memcpy(dest, coherent_buf, FRAME_SRC * 4);
        } else {
            float ratio = (float)(FRAME_SRC - 1) / (float)(out_n - 1);
            for (int i = 0; i < out_n; i++) {
                float sp = i * ratio;
                int   i0 = (int)sp;
                float fr = sp - i0;
                int   i1 = (i0 + 1 < FRAME_SRC) ? i0 + 1 : i0;
                dest[i*2]   = (int16_t)(coherent_buf[i0*2]   + (int)((coherent_buf[i1*2]   - coherent_buf[i0*2])   * fr));
                dest[i*2+1] = (int16_t)(coherent_buf[i0*2+1] + (int)((coherent_buf[i1*2+1] - coherent_buf[i0*2+1]) * fr));
            }
        }
        odroid_audio_submit(dest, out_n);
        samples_produced += out_n;
        dma_idx = (dma_idx + 1) % AUD_QUEUE_DEPTH;
        audio_done = req;
    }

    audioTaskRunning = false;
    vTaskDelete(NULL);
}

/* ─── Save state helpers ───────────────────────────────────────── */

static char *snes_get_save_path(void)
{
    char *romName = odroid_settings_RomFilePath_get();
    if (!romName) return NULL;
    char *fileName = odroid_util_GetFileNameWithoutExtension(romName);
    free(romName);
    if (!fileName) return NULL;
    char pathBuf[256];
    snprintf(pathBuf, sizeof(pathBuf), "/sd/odroid/data/snes/%s.sav", fileName);
    free(fileName);
    return strdup(pathBuf);
}

static char *snes_get_sram_path(void)
{
    char *romName = odroid_settings_RomFilePath_get();
    if (!romName) return NULL;
    char *fileName = odroid_util_GetFileNameWithoutExtension(romName);
    free(romName);
    if (!fileName) return NULL;
    char pathBuf[256];
    snprintf(pathBuf, sizeof(pathBuf), "/sd/odroid/data/snes/%s.srm", fileName);
    free(fileName);
    return strdup(pathBuf);
}

static void snes_load_sram(void)
{
    if (Memory.SRAMSize == 0) return;  /* ROM has no battery save */

    char *path = snes_get_sram_path();
    if (!path) return;

    int sram_bytes = (Memory.SRAMMask + 1);
    if (sram_bytes <= 0 || sram_bytes > 0x20000) { free(path); return; }

    esp_err_t r = odroid_sdcard_open("/sd");
    if (r != ESP_OK) { free(path); return; }

    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(Memory.SRAM, 1, sram_bytes, f);
        fclose(f);
        ESP_LOGI(TAG, "SRAM loaded: %d bytes from %s", (int)n, path);
    } else {
        ESP_LOGI(TAG, "No SRAM file found: %s (new game)", path);
    }

    odroid_sdcard_close();
    free(path);
}

static void snes_save_sram(void)
{
    if (Memory.SRAMSize == 0) return;  /* ROM has no battery save */

    char *path = snes_get_sram_path();
    if (!path) return;

    int sram_bytes = (Memory.SRAMMask + 1);
    if (sram_bytes <= 0 || sram_bytes > 0x20000) { free(path); return; }

    esp_err_t r = odroid_sdcard_open("/sd");
    if (r != ESP_OK) { free(path); return; }

    mkdir("/sd/odroid", 0775);
    mkdir("/sd/odroid/data", 0775);
    mkdir("/sd/odroid/data/snes", 0775);

    FILE *f = fopen(path, "wb");
    if (f) {
        size_t n = fwrite(Memory.SRAM, 1, sram_bytes, f);
        fclose(f);
        ESP_LOGI(TAG, "SRAM saved: %d bytes to %s", (int)n, path);
    } else {
        ESP_LOGE(TAG, "Failed to write SRAM: %s", path);
    }

    odroid_sdcard_close();
    free(path);
}

static bool snes_check_save_exists(void)
{
    char *path = snes_get_save_path();
    if (!path) return false;
    struct stat st;
    bool exists = (stat(path, &st) == 0);
    free(path);
    return exists;
}

static bool snes_do_save_state(void)
{
    char *path = snes_get_save_path();
    if (!path) return false;

    esp_err_t r = odroid_sdcard_open("/sd");
    if (r != ESP_OK) { free(path); return false; }

    mkdir("/sd/odroid", 0775);
    mkdir("/sd/odroid/data", 0775);
    mkdir("/sd/odroid/data/snes", 0775);

    bool ok = S9xSaveState(path);
    free(path);
    odroid_sdcard_close();
    return ok;
}

static bool snes_do_load_state(void)
{
    char *path = snes_get_save_path();
    if (!path) return false;

    esp_err_t r = odroid_sdcard_open("/sd");
    if (r != ESP_OK) { free(path); return false; }

    bool ok = S9xLoadState(path);
    free(path);
    odroid_sdcard_close();
    return ok;
}

static bool snes_delete_save(void)
{
    char *path = snes_get_save_path();
    if (!path) return false;
    odroid_sdcard_open("/sd");
    bool ok = (remove(path) == 0);
    odroid_sdcard_close();
    free(path);
    return ok;
}

/* Power-cycle the console, as after LoadROM (battery SRAM is kept) */
static void snes_restart(void)
{
    S9xReset();
    if (Settings.SuperFX) {
        S9xResetSuperFX();
        GSU.pvRam = SuperFX.pvRam;
        GSU.nRamBanks = SuperFX.nRamBanks;
    }
}

/* The launcher draws the menu and the volume dialog: wait until the video task
 * has presented its last frame and the audio task has mixed its frame, so
 * neither touches the display or the core state while the dialog is up */
static void snes_wait_idle(void)
{
    for (int i = 0; i < 100; i++) {
        if (uxQueueMessagesWaiting(vidQueue) == 0 && audio_done == audio_req) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ─── In-game menu (launcher-drawn, odroid_menu.h) ─────────────── */
/* Returns true when the game should exit */
static bool snes_show_menu(void)
{
    snes_wait_idle();
    const char *note = NULL;
    for (;;) {
        switch (odroid_emu_menu(note, snes_check_save_exists() ? EMU_MENU_HAS_SAVE : 0)) {
        case EMU_MENU_SAVE:
            note = snes_do_save_state() ? "saved" : "save failed";
            continue;
        case EMU_MENU_DELETE:
            note = snes_delete_save() ? "save deleted" : "delete failed";
            continue;
        case EMU_MENU_LOAD:
            if (!snes_do_load_state()) { note = "load failed"; continue; }
            return false;
        case EMU_MENU_RESTART:
            snes_restart();
            return false;
        case EMU_MENU_EXIT:
            return true;
        default:
            return false;
        }
    }
}

/* ─── Main emulation logic ──────────────────────────────────────── */

static void snes_init_core(void)
{
    Settings.CyclesPercentage = 100;
    Settings.H_Max = SNES_CYCLES_PER_SCANLINE;
    Settings.FrameTimePAL = 20000;
    Settings.FrameTimeNTSC = 16667;
    Settings.ControllerOption = SNES_JOYPAD;
    Settings.HBlankStart = (256 * Settings.H_Max) / SNES_HCOUNTER_MAX;
    Settings.SoundPlaybackRate = AUDIO_SAMPLE_RATE;
    Settings.DisableSoundEcho = false;  /* echo/reverb — many games need it (DKC, FF6, etc.) */
    Settings.InterpolatedSound = true;  /* linear interpolation — removes aliasing */

    if (!S9xInitMemory()) {
        ESP_LOGE(TAG, "S9xInitMemory failed");
        return;
    }
    if (!S9xInitAPU()) {
        ESP_LOGE(TAG, "S9xInitAPU failed");
        return;
    }
    if (!S9xInitSound(0, 0)) {
        ESP_LOGE(TAG, "S9xInitSound failed");
        return;
    }

    /* S9xInitDisplay MUST be called before S9xInitGFX so that
       GFX.Screen, GFX.SubScreen, GFX.Pitch, ZBuffer etc. are
       set before S9xInitGFX reads them to compute GFX.Delta. */
    if (!S9xInitDisplay()) {
        ESP_LOGE(TAG, "S9xInitDisplay failed");
        return;
    }

    if (!S9xInitGFX()) {
        ESP_LOGE(TAG, "S9xInitGFX failed");
        return;
    }

    S9xSetPlaybackRate(Settings.SoundPlaybackRate);

    ESP_LOGI(TAG, "snes9x core initialized");
}

void snes_run(const char *rom_path)
{
    ESP_LOGI(TAG, "Starting SNES emulator, ROM=%s", rom_path);

    snes_quit_flag = false;

    /* ── Allocate buffers ── */
    /* SNES double-buffer framebuffers */
    for (int i = 0; i < 2; i++) {
        snes_fb[i] = (uint16_t *)heap_caps_calloc(1,
            SNES_FB_W * SNES_FB_H_EXT * sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!snes_fb[i]) {
            ESP_LOGE(TAG, "Failed to allocate SNES FB %d", i);
            return;
        }
    }
    snes_fb_write = 0;

    /* SubScreen buffer — needed for color math and OBJ processing.
     * Kept in PSRAM: internal SRAM has no room for it after WRAM+VRAM (~192 KB),
     * and pushing it internal starves the APU init (S9xInitAPU) → crash. */
    snes_subscreen = (uint8_t *)heap_caps_calloc(1,
        SNES_FB_W * SNES_FB_H_EXT * sizeof(uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!snes_subscreen) {
        ESP_LOGE(TAG, "Failed to allocate SubScreen buffer");
        return;
    }

    /* Audio buffer (mixing side — used by audio task on Core 1) */
    audio_buf = (int16_t *)heap_caps_malloc(AUDIO_FRAG_SIZE * 4,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!audio_buf) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer");
        goto cleanup;
    }

    /* Audio DMA double-buffers — sized for the max stretched output */
    for (int i = 0; i < AUD_QUEUE_DEPTH; i++) {
        audio_dma_buf[i] = (int16_t *)heap_caps_malloc(AUD_OUT_MAX * 4,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!audio_dma_buf[i]) {
            ESP_LOGE(TAG, "Failed to allocate audio DMA buffer %d", i);
            goto cleanup;
        }
    }

    /* Coherent-mix source — one emulated frame of DSP output (not DMA → PSRAM) */
    coherent_buf = (int16_t *)heap_caps_malloc(FRAME_SRC * 4, MALLOC_CAP_SPIRAM);
    if (!coherent_buf) {
        ESP_LOGE(TAG, "Failed to allocate coherent audio buffer");
        goto cleanup;
    }

    /* ── Initialize snes9x core ── */
    snes_init_core();

    /* Verify critical GFX buffers were allocated — snes_init_core returns void */
    if (!GFX.ZBuffer || !GFX.SubZBuffer || !Memory.VRAM) {
        ESP_LOGE(TAG, "Core init failed: ZBuffer=%p SubZBuffer=%p VRAM=%p",
                 GFX.ZBuffer, GFX.SubZBuffer, Memory.VRAM);
        goto cleanup;
    }

    /* ── Load ROM from SD card ── */
    esp_err_t r = odroid_sdcard_open("/sd");
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "SD card open failed (%d)", (int)r);
        goto cleanup;
    }

    /* Read ROM file into Memory.ROM (allocated by S9xInitMemory) */
    {
        /* S9xInitMemory allocates Memory.ROM but doesn't set ROM_Size.
         * We need to allocate enough space for the ROM. */
        FILE *f = fopen(rom_path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "Failed to open ROM: %s", rom_path);
            odroid_sdcard_close();
            goto cleanup;
        }

        fseek(f, 0, SEEK_END);
        long rom_size = ftell(f);
        fseek(f, 0, SEEK_SET);

        ESP_LOGI(TAG, "ROM size: %ld bytes", rom_size);

        if (rom_size > MAX_ROM_SIZE) {
            ESP_LOGE(TAG, "ROM too large (%ld > %d)", rom_size, MAX_ROM_SIZE);
            fclose(f);
            odroid_sdcard_close();
            goto cleanup;
        }

        /* Memory.ROM was allocated by S9xInitMemory — check it */
        if (!Memory.ROM) {
            Memory.ROM = (uint8_t *)heap_caps_calloc(1, MAX_ROM_SIZE,
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!Memory.ROM) {
                ESP_LOGE(TAG, "Failed to allocate ROM buffer");
                fclose(f);
                odroid_sdcard_close();
                goto cleanup;
            }
        }

        size_t bytes_read = fread(Memory.ROM, 1, rom_size, f);
        fclose(f);

        /* Skip 512-byte SMC copier header if present.
         * LoadROM() detects it but the memmove is commented out in this
         * fork, so we strip it here before calling LoadROM(). */
        if ((bytes_read & 0x7FF) == 512) {
            ESP_LOGI(TAG, "Stripping 512-byte SMC header");
            memmove(Memory.ROM, Memory.ROM + 512, bytes_read - 512);
            bytes_read -= 512;
        }

        Memory.ROM_Size = bytes_read;

        ESP_LOGI(TAG, "ROM loaded: %d bytes", (int)bytes_read);
    }

    odroid_sdcard_close();

    /* Initialize ROM (detect type, apply fixes, etc.) */
    if (!LoadROM(NULL)) {
        ESP_LOGE(TAG, "LoadROM failed — ROM format not recognized");
        goto cleanup;
    }

    ESP_LOGI(TAG, "ROM: %s (%s, %s)", Memory.ROMName,
             Memory.HiROM ? "HiROM" : "LoROM",
             Settings.PAL ? "PAL" : "NTSC");
    ESP_LOGI(TAG, "ROMType=0x%02X, SuperFX=%d", Memory.ROMType, Settings.SuperFX);

    /* ── SuperFX / GSU co-processor init ── */
    if (Settings.SuperFX) {
        ESP_LOGI(TAG, "SuperFX ROM detected — using Memory.SRAM as GSU RAM");
        ESP_LOGI(TAG, "Pre-init SRAMSize=%d SRAMMask=0x%lX",
                 Memory.SRAMSize, (unsigned long)Memory.SRAMMask);
        /* Use Memory.SRAM (128KB, already allocated) as GSU RAM.
         * CPU accesses banks $60-$7D via MAP_LOROM_SRAM → Memory.SRAM.
         * GSU accesses via pvRam → also Memory.SRAM.
         * Both CPU and GSU share the same physical buffer. */
        S9xInitSuperFX();
        SuperFX.pvRam = Memory.SRAM;
        SuperFX.nRamBanks = 2;  /* 128KB = 2 x 64KB banks */
        /* Ensure SRAMMask covers full 128KB for GSU bank access */
        if (Memory.SRAMMask < 0x1FFFF)
            Memory.SRAMMask = 0x1FFFF;  /* 128KB-1 */
        S9xResetSuperFX();
        /* Ensure GSU.pvRam matches after reset */
        GSU.pvRam = SuperFX.pvRam;
        GSU.nRamBanks = SuperFX.nRamBanks;
        ESP_LOGI(TAG, "SuperFX initialized, %d KB GSU RAM (shared SRAM), SRAMMask=0x%lX",
                 SuperFX.nRamBanks * 64, (unsigned long)Memory.SRAMMask);
        ESP_LOGI(TAG, "FillRAM[3030]=%02X [3031]=%02X [303B]=%02X after reset",
                 Memory.FillRAM[0x3030], Memory.FillRAM[0x3031], Memory.FillRAM[0x303B]);
    }

    /* S9xInitDisplay already called inside snes_init_core() */

    /* ── Load SRAM (battery saves) from SD — skip for SuperFX (uses SRAM as GSU RAM) ── */
    if (!Settings.SuperFX) {
        snes_load_sram();
    }

    /* ── Initialize audio ── */
    odroid_audio_init(AUDIO_SAMPLE_RATE);

    /* SNES has native X/Y face buttons — don't alias them to Menu/Volume */
    odroid_input_xy_menu_disable = true;

    /* ── Create video queue and task ── */
    vidQueue = xQueueCreate(1, sizeof(uint16_t *));
    xTaskCreatePinnedToCore(snes_video_task, "snes_video", 4096,
                            NULL, 5, &videoTaskHandle, 1);

    /* ── Create audio task (Core 1, signal-based) ── */
    xTaskCreatePinnedToCore(snes_audio_task, "snes_audio", 4096,
                            NULL, 6, &audioTaskHandle, 1);

    /* ── Auto-resume: load save state if launcher requested it ── */
    if (odroid_settings_StartAction_get() == ODROID_START_ACTION_RESTART) {
        odroid_settings_StartAction_set(ODROID_START_ACTION_NORMAL);
        if (snes_check_save_exists()) {
            ESP_LOGI(TAG, "Auto-resuming from save state");
            snes_do_load_state();
        }
    }

    /* Report memory headroom after all allocations (Phase 50 tuning) */
    printf("SNES HEAP: internal free=%u B (largest block=%u B), PSRAM free=%u B\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* ── Emulation loop ── */
    int frame_no = 0;
    int64_t fps_timer = esp_timer_get_time();
    const int64_t target_frame_us = Settings.PAL ? 20000 : 16667;
    int64_t frame_deadline = esp_timer_get_time() + target_frame_us;  /* real-time schedule */
    int consec_skips = 0;

    IPPU.RenderThisFrame = true;

    ESP_LOGI(TAG, "Entering emulation loop (%s, target %lld us/frame)",
             Settings.PAL ? "PAL" : "NTSC", target_frame_us);
    if (Settings.SuperFX) {
        ESP_LOGI(TAG, "PRE-LOOP FillRAM[3030]=%02X [3031]=%02X [303B]=%02X",
                 Memory.FillRAM[0x3030], Memory.FillRAM[0x3031], Memory.FillRAM[0x303B]);
    }

    /* Profiling accumulators */
    int64_t prof_cpu_acc = 0, prof_audio_acc = 0, prof_vid_acc = 0;
    int prof_cnt = 0, prof_skip = 0, prof_busy = 0;

    odroid_gamepad_state gp_prev;
    odroid_input_gamepad_read(&gp_prev);

    while (!snes_quit_flag) {
        /* Check for menu / volume button (edge-triggered) */
        odroid_gamepad_state gp;
        odroid_input_gamepad_read(&gp);

        if (gp.values[ODROID_INPUT_VOLUME] && !gp_prev.values[ODROID_INPUT_VOLUME]) {
            snes_wait_idle();
            odroid_emu_volume();
            snes_audio_resync = true;
            fps_timer = esp_timer_get_time();
            frame_deadline = esp_timer_get_time() + target_frame_us;
            consec_skips = 0;
        }

        if (gp.values[ODROID_INPUT_MENU] && !gp_prev.values[ODROID_INPUT_MENU]) {
            if (snes_show_menu()) {
                snes_quit_flag = true;
                break;
            }
            /* After resume, re-seed timers */
            snes_audio_resync = true;
            fps_timer = esp_timer_get_time();
            frame_deadline = esp_timer_get_time() + target_frame_us;
            consec_skips = 0;
            frame_no = 0;
        }

        gp_prev = gp;

        /* Run one SNES frame */
        int64_t t_cpu0 = esp_timer_get_time();
        S9xMainLoop();
        int64_t t_cpu1 = esp_timer_get_time();

        /* Audio — signal Core 1 audio task to mix and play */
        audio_req++;
        xTaskNotifyGive(audioTaskHandle);
        int64_t t_aud1 = esp_timer_get_time();

        /* Video — send frame to display task */
        if (IPPU.RenderThisFrame) {
            int next = snes_fb_write ^ 1;
            void *arg = (void *)snes_fb[snes_fb_write];
            xQueueOverwrite(vidQueue, &arg);
            snes_fb_write = next;

            /* Point snes9x to the other buffer */
            GFX.Screen = (uint8_t *)snes_fb[snes_fb_write];
            /* SubScreen stays at its dedicated buffer — do NOT alias to Screen */
        }
        int64_t t_vid1 = esp_timer_get_time();

        /* Accumulate profiling */
        prof_cpu_acc   += (t_cpu1 - t_cpu0);
        prof_audio_acc += (t_aud1 - t_cpu1);
        prof_vid_acc   += (t_vid1 - t_aud1);
        prof_cnt++;
        if (!IPPU.RenderThisFrame) prof_skip++;

        /* ── Real-time pacing + auto frame-skip (Neo Geo style) ──
         * Advance the game logic at a true 60 FPS vs. the wall clock. When a
         * rendered frame overruns, SKIP rendering the next frame(s) to catch
         * back up — the game keeps advancing, only the display drops frames.
         * A render-skipped frame is well under budget, so this holds full game
         * speed (and, since audio is frame-locked, real-time audio) except in
         * the very heaviest scenes, where it resyncs and lets the game slow. */
        IPPU.RenderThisFrame = true;                  /* default: render next frame */
        frame_deadline += target_frame_us;
        int64_t now = esp_timer_get_time();
        if (now < frame_deadline) {
            /* On/ahead of schedule → wait to the deadline (holds 60 FPS) */
            int64_t d;
            while ((d = frame_deadline - esp_timer_get_time()) > 0) {
                if (d > 1500) vTaskDelay(1);          /* yield ~1ms; else spin final <1.5ms */
            }
            consec_skips = 0;
        } else if (consec_skips < SNES_MAX_FRAMESKIP) {
            IPPU.RenderThisFrame = false;             /* behind → skip next render to catch up */
            consec_skips++;
        } else {
            frame_deadline = now;                     /* can't keep up even skipping → slow down */
            consec_skips = 0;
        }
        /* The next frame renders into snes_fb[snes_fb_write]. If the video task is still presenting
         * that buffer (display slower than the game: 46.7 Hz panel, 50 Hz PAL), rendering into it
         * would show a half-drawn frame (mostly backdrop: a black flash); skip this render instead */
        if (IPPU.RenderThisFrame && snes_fb_busy == snes_fb[snes_fb_write]) {
            IPPU.RenderThisFrame = false;
            prof_busy++;
        }
        now = esp_timer_get_time();                   /* real frame-end time for the FPS calc */

        /* FPS + profiling log (every ~2 seconds) */
        frame_no++;
        if (now - fps_timer > 2000000) {
            int fps = (int)(frame_no * 1000000LL / (now - fps_timer));
            printf("SNES PROF (%d frames, %d skip, %d busy): CPU=%.1fms  AUD=%.1fms  VID=%.1fms  total=%.1fms  FPS=%d\n",
                   prof_cnt, prof_skip, prof_busy,
                   prof_cpu_acc / (prof_cnt * 1000.0f),
                   prof_audio_acc / (prof_cnt * 1000.0f),
                   prof_vid_acc / (prof_cnt * 1000.0f),
                   (prof_cpu_acc + prof_audio_acc + prof_vid_acc) / (prof_cnt * 1000.0f),
                   fps);
            if (Settings.SuperFX) {
                printf("  SFX: GO=%d SFR=0x%04X R15=0x%04lX PBR=%lu R12=0x%04lX R13=0x%04lX PC=%02X:%04X Bright=%d Blank=%d\n",
                       (Memory.FillRAM[0x3030] & 0x20) ? 1 : 0,
                       Memory.FillRAM[0x3030] | (Memory.FillRAM[0x3031] << 8),
                       (unsigned long)GSU.avReg[15], (unsigned long)GSU.vPrgBankReg,
                       (unsigned long)GSU.avReg[12], (unsigned long)GSU.avReg[13],
                       ICPU.Registers.PB, ICPU.Registers.PC,
                       PPU.Brightness, PPU.ForcedBlanking);
                /* One-time diagnostics */
                {
                    static int dump_count = 0;
                    if (dump_count < 5) {
                        dump_count++;
                        uint8_t nmitimen = Memory.FillRAM[0x4200];
                        printf("  NMITIMEN=$%02X NMI=%s SP=$%04X DB=%02X\n",
                            nmitimen, (nmitimen & 0x80) ? "ON" : "OFF",
                            ICPU.Registers.S.W, ICPU.Registers.DB);
                        /* Dump SRAM around $4CB0 (what the game's loop reads at $704CB4) */
                        printf("  SRAM@4CB0: ");
                        for (int i = 0; i < 32; i++)
                            printf("%02X", Memory.SRAM[0x4CB0 + i]);
                        printf("\n");
                        /* Dump first 32 bytes of SRAM to see if anything got written */
                        printf("  SRAM@0000: ");
                        for (int i = 0; i < 32; i++)
                            printf("%02X", Memory.SRAM[i]);
                        printf("\n");
                        /* Key SFX control regs */
                        printf("  SFX regs: SCMR=%02X RAMBR=%02X CFGR=%02X CLSR=%02X SCBR=%02X\n",
                            Memory.FillRAM[0x303A], Memory.FillRAM[0x303C],
                            Memory.FillRAM[0x3037], Memory.FillRAM[0x3039],
                            Memory.FillRAM[0x3038]);
                        /* Dump raw ROM around $553A (GSU code at $D53A mirror) */
                        printf("  ROM@553A: ");
                        for (int i = 0; i < 32; i++)
                            printf("%02X ", Memory.ROM[0x553A + i]);
                        printf("\n");
                        printf("  SRAMSize=%d SRAMMask=0x%lX\n",
                            Memory.SRAMSize, (unsigned long)Memory.SRAMMask);
                    }
                }
            }
            prof_cpu_acc = prof_audio_acc = prof_vid_acc = 0;
            prof_cnt = prof_skip = prof_busy = 0;
            frame_no = 0;
            fps_timer = now;
        }
    }

    /* ── Shutdown ── */
    ESP_LOGI(TAG, "Exiting emulation loop");

    /* Save SRAM (battery saves) to SD before cleanup — skip for SuperFX */
    if (!Settings.SuperFX) {
        snes_save_sram();
    }

    /* Signal video task to exit */
    {
        uint16_t *sentinel = (uint16_t *)1;
        xQueueOverwrite(vidQueue, &sentinel);
        for (int i = 0; i < 50 && videoTaskRunning; i++)
            vTaskDelay(pdMS_TO_TICKS(20));
    }

    /* Signal audio task to exit */
    {
        xTaskNotifyGive(audioTaskHandle);  /* snes_quit_flag is set, task will exit */
        for (int i = 0; i < 50 && audioTaskRunning; i++)
            vTaskDelay(pdMS_TO_TICKS(20));
    }

    vQueueDelete(vidQueue);
    vidQueue = NULL;

    odroid_audio_terminate();
    S9xDeinitGFX();
    S9xDeinitAPU();
    S9xDeinitMemory();

cleanup:
    for (int i = 0; i < 2; i++) {
        if (snes_fb[i]) { heap_caps_free(snes_fb[i]); snes_fb[i] = NULL; }
    }
    if (snes_subscreen) { heap_caps_free(snes_subscreen); snes_subscreen = NULL; }
    if (audio_buf) { heap_caps_free(audio_buf); audio_buf = NULL; }
    if (coherent_buf) { heap_caps_free(coherent_buf); coherent_buf = NULL; }
    for (int i = 0; i < AUD_QUEUE_DEPTH; i++) {
        if (audio_dma_buf[i]) { heap_caps_free(audio_dma_buf[i]); audio_dma_buf[i] = NULL; }
    }
    if (GFX.ZBuffer) { heap_caps_free(GFX.ZBuffer); GFX.ZBuffer = NULL; }
    if (GFX.SubZBuffer) { heap_caps_free(GFX.SubZBuffer); GFX.SubZBuffer = NULL; }
    saved_ZBuffer = NULL;
    saved_SubZBuffer = NULL;

    ESP_LOGI(TAG, "SNES emulator shutdown complete");
}
