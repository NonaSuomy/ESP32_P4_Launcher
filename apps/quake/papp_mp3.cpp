/*
 * Small C ABI wrapper around the PacketVideo decoder used by ESPHome's
 * micro-mp3 component.  The Quake PAX is linked without a C++ runtime, so
 * this deliberately exposes only the decoder's C API to pax_snd.c.
 *
 * The decoder sources are licensed under the Apache License 2.0.  Each
 * vendored source file retains its upstream copyright and license notice.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "pvmp3_framedecoder.h"
#include "pvmp3decoder_api.h"

typedef struct {
    void *decoder_memory;
    tPVMP3DecoderExternal ext;
} pax_mp3_decoder_t;

extern "C" {

void *pax_mp3_create(void)
{
    pax_mp3_decoder_t *decoder =
        (pax_mp3_decoder_t *)calloc(1, sizeof(pax_mp3_decoder_t));
    if (!decoder)
        return NULL;

    uint32_t memory_size = pvmp3_decoderMemRequirements();
    decoder->decoder_memory = calloc(1, memory_size);
    if (!decoder->decoder_memory) {
        free(decoder);
        return NULL;
    }

    memset(&decoder->ext, 0, sizeof(decoder->ext));
    decoder->ext.equalizerType = flat;
    decoder->ext.crcEnabled = 0;
    pvmp3_InitDecoder(&decoder->ext, decoder->decoder_memory);
    return decoder;
}

void pax_mp3_reset(void *opaque)
{
    pax_mp3_decoder_t *decoder = (pax_mp3_decoder_t *)opaque;
    if (!decoder)
        return;

    memset(&decoder->ext, 0, sizeof(decoder->ext));
    decoder->ext.equalizerType = flat;
    decoder->ext.crcEnabled = 0;
    pvmp3_InitDecoder(&decoder->ext, decoder->decoder_memory);
}

void pax_mp3_destroy(void *opaque)
{
    pax_mp3_decoder_t *decoder = (pax_mp3_decoder_t *)opaque;
    if (!decoder)
        return;

    free(decoder->decoder_memory);
    free(decoder);
}

/*
 * Decode one MP3 frame from a contiguous input window.
 *
 * The returned sample count is the total number of interleaved int16 samples
 * in output (not the per-channel count).  On a successful frame the caller
 * must advance input by consumed bytes and retain any bytes after that point.
 */
int pax_mp3_decode(void *opaque, const uint8_t *input, int input_length,
                    int16_t *output, int output_capacity_samples,
                    int *consumed, int *samples, int *sample_rate,
                    int *channels)
{
    pax_mp3_decoder_t *decoder = (pax_mp3_decoder_t *)opaque;
    if (!decoder || !input || input_length <= 0 || !output ||
        output_capacity_samples <= 0)
        return -1;

    decoder->ext.pInputBuffer = (uint8_t *)input;
    decoder->ext.inputBufferCurrentLength = input_length;
    decoder->ext.inputBufferMaxLength = input_length;
    decoder->ext.inputBufferUsedLength = 0;
    decoder->ext.pOutputBuffer = output;
    decoder->ext.outputFrameSize = output_capacity_samples;

    ERROR_CODE result = pvmp3_framedecoder(&decoder->ext,
                                            decoder->decoder_memory);

    if (consumed)
        *consumed = decoder->ext.inputBufferUsedLength;
    if (samples)
        *samples = decoder->ext.outputFrameSize;
    if (sample_rate)
        *sample_rate = decoder->ext.samplingRate;
    if (channels)
        *channels = decoder->ext.num_channels;

    return (int)result;
}

} /* extern "C" */
