/* Synchronous SID output adapter for a PSRAM-loaded PAX. */

#include "psram_app.h"
#include "VIC.h"
#include <string.h>

#define EMU_AUDIO_SLOTS 6
#include "audio_queue.h"

#define PAX_AUDIO_FRAMES 256
#define PAX_AUDIO_SUBMIT_FRAMES 882 /* 44.1 kHz / 50 Hz video frame */

static int16_t pax_sid_mono[PAX_AUDIO_FRAMES];
static int16_t pax_sid_stereo[PAX_AUDIO_SUBMIT_FRAMES * 2];
static int pax_sid_pending;
static int pax_sid_output_pending;
static bool pax_sid_paused;
static int pax_sid_divisor;
static bool pax_sid_audio_started;

void DigitalRenderer::init_sound(void)
{
    pax_sid_pending = 0;
    pax_sid_output_pending = 0;
    pax_sid_divisor = 0;
    pax_sid_paused = false;
    pax_sid_audio_started = false;
    ready = true;
    emu_audio_start((int)SAMPLE_FREQ);
}

void DigitalRenderer::EmulateLine(void)
{
    if (!ready || pax_sid_paused) return;

    pax_sid_divisor += SAMPLE_FREQ;
    while (pax_sid_divisor >= 0) {
        pax_sid_divisor -= TOTAL_RASTERS * SCREEN_FREQ;
        ++pax_sid_pending;
    }

    sample_buf[sample_in_ptr] = volume;
    sample_in_ptr = (sample_in_ptr + 1) % SAMPLE_BUF_SIZE;

    if (pax_sid_pending >= PAX_AUDIO_FRAMES) {
        pax_sid_pending -= PAX_AUDIO_FRAMES;
        calc_buffer(pax_sid_mono, PAX_AUDIO_FRAMES * 2);

        /* Frodo's original renderer includes the SID's volume-dependent DC
         * offset in every sample.  That is useful for an analogue SID model,
         * but it becomes an audible constant buzz when the voices are idle
         * and the samples are sent to a digital I2S amplifier.  Keep the
         * rendered waveform for active voices, but emit true digital silence
         * while the SID has no envelope currently producing sound. */
        int16_t block_min = pax_sid_mono[0];
        int16_t block_max = pax_sid_mono[0];
        for (int i = 1; i < PAX_AUDIO_FRAMES; ++i) {
            if (pax_sid_mono[i] < block_min)
                block_min = pax_sid_mono[i];
            if (pax_sid_mono[i] > block_max)
                block_max = pax_sid_mono[i];
        }
        // A silent SID block is commonly a constant DC value (for example
        // 8192), not literal zero, so checking only the absolute peak is not
        // sufficient.  A small threshold avoids waking the speaker for a
        // negligible filter residue while preserving quiet real waveforms.
        const bool has_waveform = (static_cast<int>(block_max) - static_cast<int>(block_min)) > 8;
        if (!has_waveform)
            memset(pax_sid_mono, 0, sizeof(pax_sid_mono));

        if (has_waveform) pax_sid_audio_started = true;
        /* Keep the sample clock continuous after sound starts. Skipping
         * silent blocks starves the output queue during quiet passages. */
        if (pax_sid_audio_started) {
            for (int i = 0; i < PAX_AUDIO_FRAMES; ++i) {
                const int output = pax_sid_output_pending++;
                pax_sid_stereo[output * 2] = pax_sid_mono[i];
                pax_sid_stereo[output * 2 + 1] = pax_sid_mono[i];
                if (pax_sid_output_pending == PAX_AUDIO_SUBMIT_FRAMES) {
                    emu_audio_submit(pax_sid_stereo, PAX_AUDIO_SUBMIT_FRAMES);
                    pax_sid_output_pending = 0;
                }
            }
        }
    }
}

void DigitalRenderer::changeVolumeLevel(void) {}
void DigitalRenderer::Pause(void) { pax_sid_paused = true; }
void DigitalRenderer::Resume(void) { pax_sid_paused = false; }
DigitalRenderer::~DigitalRenderer(void) { emu_audio_finish(); }
