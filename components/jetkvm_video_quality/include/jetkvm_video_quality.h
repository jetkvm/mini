#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0 is Auto: the encoder follows receiver feedback up to the High rate. */
#define JETKVM_VIDEO_QUALITY_AUTO_PERMILLE 0U
#define JETKVM_VIDEO_QUALITY_DEFAULT_PERMILLE JETKVM_VIDEO_QUALITY_AUTO_PERMILLE
#define JETKVM_VIDEO_QUALITY_MIN_PERMILLE 100U
#define JETKVM_VIDEO_QUALITY_MAX_PERMILLE 1000U

/**
 * Publish a quality factor: 0 (Auto) or the upstream UI range 0.1..1.0.
 *
 * Values are normalized to one-per-mille precision. The latest value replaces
 * the previous value atomically; no command FIFO is created.
 */
bool jetkvm_video_quality_set(double factor);

double jetkvm_video_quality_get(void);
uint32_t jetkvm_video_quality_get_permille(void);

/** Increments only when the normalized latest value actually changes. */
uint32_t jetkvm_video_quality_revision(void);

#ifdef __cplusplus
}
#endif
