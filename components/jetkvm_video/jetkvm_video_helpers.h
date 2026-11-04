#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_h264_types.h"
#include "hal/cam_ctlr_types.h"

const char *h264_raw_fmt_name(esp_h264_raw_format_t fmt);
size_t h264_raw_frame_size(uint16_t width, uint16_t height,
                           esp_h264_raw_format_t fmt);
void h264_raw_luma_range(const uint8_t *src, size_t raw_size,
                         esp_h264_raw_format_t fmt,
                         uint8_t *out_min, uint8_t *out_max);
const char *cam_yuv_color_name(cam_ctlr_color_t color);
size_t h264_hw_database_bytes(uint16_t width, uint16_t height);
uint32_t video_rate_x100(uint32_t frames, int64_t elapsed_us);
uint32_t video_timing_percentile(uint32_t *samples, uint16_t count,
                                 uint32_t percentile);
uint32_t video_timing_average(const uint32_t *samples, uint16_t count);
uint32_t video_timing_maximum(const uint32_t *samples, uint16_t count);
