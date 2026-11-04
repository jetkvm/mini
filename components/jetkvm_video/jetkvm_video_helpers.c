/* Pure video format, memory sizing, and metrics math. */
#include "jetkvm_video_helpers.h"

#include <limits.h>
#include <stdlib.h>

#define H264_HW_DB_MACRO_SIZE       16U
#define H264_HW_DB_ROW_12_BYTES     256U
#define H264_HW_DB_ROW_4_BYTES      128U
#define H264_HW_DB_ALIGNMENT_BYTES  16U

const char *h264_raw_fmt_name(esp_h264_raw_format_t fmt)
{
    switch (fmt) {
    case ESP_H264_RAW_FMT_UYVY:
        return "UYVY";
    case ESP_H264_RAW_FMT_O_UYY_E_VYY:
        return "O_UYY_E_VYY";
    case ESP_H264_RAW_FMT_YUYV:
        return "YUYV";
    case ESP_H264_RAW_FMT_I420:
        return "I420";
    case ESP_H264_RAW_FMT_BGR888:
        return "BGR888";
    case ESP_H264_RAW_FMT_RGB565_LE:
        return "RGB565_LE";
    case ESP_H264_RAW_FMT_VUY:
        return "VUY";
    default:
        return "unknown";
    }
}
size_t h264_raw_frame_size(uint16_t width, uint16_t height, esp_h264_raw_format_t fmt)
{
    size_t pixels = (size_t)width * height;

    switch (fmt) {
    case ESP_H264_RAW_FMT_I420:
    case ESP_H264_RAW_FMT_O_UYY_E_VYY:
        return pixels * 3 / 2;
    case ESP_H264_RAW_FMT_RGB565_LE:
    case ESP_H264_RAW_FMT_UYVY:
    case ESP_H264_RAW_FMT_YUYV:
        return pixels * 2;
    case ESP_H264_RAW_FMT_BGR888:
    case ESP_H264_RAW_FMT_VUY:
    default:
        return pixels * 3;
    }
}

void h264_raw_luma_range(const uint8_t *src, size_t raw_size, esp_h264_raw_format_t fmt,
                                uint8_t *out_min, uint8_t *out_max)
{
    uint8_t y_min = 255;
    uint8_t y_max = 0;

    if (fmt == ESP_H264_RAW_FMT_O_UYY_E_VYY) {
        for (size_t i = 0; i + 2 < raw_size; i += 3) {
            uint8_t y0 = src[i + 1];
            uint8_t y1 = src[i + 2];
            if (y0 < y_min) y_min = y0;
            if (y0 > y_max) y_max = y0;
            if (y1 < y_min) y_min = y1;
            if (y1 > y_max) y_max = y1;
        }
    } else if (fmt == ESP_H264_RAW_FMT_UYVY) {
        for (size_t i = 1; i + 2 < raw_size; i += 4) {
            uint8_t y0 = src[i];
            uint8_t y1 = src[i + 2];
            if (y0 < y_min) y_min = y0;
            if (y0 > y_max) y_max = y0;
            if (y1 < y_min) y_min = y1;
            if (y1 > y_max) y_max = y1;
        }
    } else {
        for (size_t i = 0; i < raw_size; i++) {
            uint8_t y = src[i];
            if (y < y_min) y_min = y;
            if (y > y_max) y_max = y;
        }
    }

    *out_min = y_min;
    *out_max = y_max;
}

const char *cam_yuv_color_name(cam_ctlr_color_t color)
{
    switch (color) {
    case CAM_CTLR_COLOR_YUV420:
        return "O_UYY_E_VYY";
    case CAM_CTLR_COLOR_YUV422_YVYU:
        return "YVYU";
    case CAM_CTLR_COLOR_YUV422_YUYV:
        return "YUYV";
    case CAM_CTLR_COLOR_YUV422_UYVY:
        return "UYVY";
    case CAM_CTLR_COLOR_YUV422_VYUY:
        return "VYUY";
    default:
        return "unknown";
    }
}

size_t h264_hw_database_bytes(uint16_t width, uint16_t height)
{
    const size_t mb_width =
        ((size_t)width + H264_HW_DB_MACRO_SIZE - 1U) /
        H264_HW_DB_MACRO_SIZE;
    const size_t mb_height =
        ((size_t)height + H264_HW_DB_MACRO_SIZE - 1U) /
        H264_HW_DB_MACRO_SIZE;
    if (mb_width == 0U || mb_height == 0U) {
        return 0U;
    }
    return mb_width * (mb_height - 1U) * H264_HW_DB_ROW_12_BYTES +
           mb_width *
               (H264_HW_DB_ROW_12_BYTES + H264_HW_DB_ROW_4_BYTES) +
           mb_width * (mb_height - 1U) * H264_HW_DB_ROW_4_BYTES +
           H264_HW_DB_ALIGNMENT_BYTES;
}

uint32_t video_rate_x100(uint32_t frames, int64_t elapsed_us)
{
    if (elapsed_us <= 0) {
        return 0;
    }
    return (uint32_t)(((uint64_t)frames * 100U * 1000000U) / (uint64_t)elapsed_us);
}

static int video_timing_sample_compare(const void *left, const void *right)
{
    uint32_t a = *(const uint32_t *)left;
    uint32_t b = *(const uint32_t *)right;
    return (a > b) - (a < b);
}

uint32_t video_timing_percentile(uint32_t *samples, uint16_t count,
                                        uint32_t percentile)
{
    if (!count) {
        return 0;
    }
    qsort(samples, count, sizeof(samples[0]), video_timing_sample_compare);
    uint32_t rank = ((uint32_t)count * percentile + 99U) / 100U;
    if (rank == 0) {
        rank = 1;
    } else if (rank > count) {
        rank = count;
    }
    return samples[rank - 1];
}

uint32_t video_timing_average(const uint32_t *samples, uint16_t count)
{
    uint64_t total = 0;
    for (uint16_t i = 0; i < count; i++) {
        total += samples[i];
    }
    return count ? (uint32_t)(total / count) : 0;
}

uint32_t video_timing_maximum(const uint32_t *samples, uint16_t count)
{
    uint32_t maximum = 0;
    for (uint16_t i = 0; i < count; i++) {
        if (samples[i] > maximum) {
            maximum = samples[i];
        }
    }
    return maximum;
}
