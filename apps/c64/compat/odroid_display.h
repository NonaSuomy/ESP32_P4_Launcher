#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void ili9341_init(void);
void ili9341_write_frame_rgb565(const uint16_t *buffer);
void ili9341_write_frame_rgb565_ex(const uint16_t *buffer, bool byte_swap_input);
void ili9341_write_frame_rgb565_custom(const uint16_t *buffer, uint16_t in_w,
                                       uint16_t in_h, float scale,
                                       bool byte_swap_input);
#ifdef __cplusplus
}
#endif
