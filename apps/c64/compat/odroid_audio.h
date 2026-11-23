#pragma once

/* Audio is submitted through app_services_t by PAX entry points. */
#include <stdint.h>
typedef enum {
    ODROID_VOLUME_LEVEL0 = 0,
    ODROID_VOLUME_LEVEL1,
    ODROID_VOLUME_LEVEL2,
    ODROID_VOLUME_LEVEL3,
    ODROID_VOLUME_LEVEL4
} odroid_volume_level;
#define ODROID_VOLUME_LEVEL_COUNT 5
#ifdef __cplusplus
extern "C" {
#endif
void odroid_audio_init(int sample_rate);
void odroid_audio_terminate(void);
void odroid_audio_volume_set(int volume);
odroid_volume_level odroid_audio_volume_get(void);
void odroid_audio_volume_change(void);
void odroid_audio_submit(short *stereo_buf, int frame_count);
void odroid_audio_submit_zero(void);
int odroid_audio_sample_rate_get(void);
#ifdef __cplusplus
}
#endif
