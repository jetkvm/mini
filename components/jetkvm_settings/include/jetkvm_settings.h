/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define JETKVM_SETTINGS_SCHEMA_VERSION 6U
#define JETKVM_KEYBOARD_LAYOUT_CAPACITY 16U
#define JETKVM_DEFAULT_KEYBOARD_LAYOUT "en-US"
#define JETKVM_LEGACY_KEYBOARD_LAYOUT "en_US"
/* 0 selects Auto: the encoder follows receiver feedback up to the High
 * rate. Manual presets are 100..1000 permille of the mode bitrate. */
#define JETKVM_AUTO_VIDEO_QUALITY_PERMILLE 0U
#define JETKVM_DEFAULT_VIDEO_QUALITY_PERMILLE JETKVM_AUTO_VIDEO_QUALITY_PERMILLE
#define JETKVM_MIN_VIDEO_QUALITY_PERMILLE 100U
#define JETKVM_MAX_VIDEO_QUALITY_PERMILLE 1000U
#define JETKVM_VIDEO_CODEC_PREFERENCE_CAPACITY 8U
#define JETKVM_DEFAULT_VIDEO_CODEC_PREFERENCE "auto"
#define JETKVM_DEFAULT_VIDEO_SLEEP_AFTER_SECONDS (-1)

/* EDID presets (tools/edid/generate_presets.py). A settings record saved with
 * the removed 720p60 preset (1) loads as DELL D2721H. */
typedef enum {
    JETKVM_VIDEO_EDID_PRESET_DEFAULT = 0,
    JETKVM_VIDEO_EDID_PRESET_DELL_D2721H = 1,
} jetkvm_video_edid_preset_t;

#define JETKVM_DEFAULT_VIDEO_EDID_PRESET JETKVM_VIDEO_EDID_PRESET_DEFAULT

typedef struct {
    uint32_t schema_version;
    char keyboard_layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    uint32_t video_quality_permille;
    char video_codec_preference[JETKVM_VIDEO_CODEC_PREFERENCE_CAPACITY];
    uint32_t video_edid_preset;
    uint32_t host_display_idle_enabled;
    int32_t video_sleep_after_seconds;
} jetkvm_settings_t;

typedef struct {
    bool initialized;
    bool recovered_defaults;
    bool settings_migrated;
    esp_err_t load_error;
    esp_err_t persistence_error;
} jetkvm_settings_status_t;

/** Populate the current schema, en-US, quality 1.0, and codec preference auto. */
void jetkvm_settings_defaults(jetkvm_settings_t *settings);

/**
 * Validate a canonical layout tag.
 *
 * Tags are 2..15 ASCII characters, consist only of alphanumerics and single
 * hyphen separators, and may not begin or end with a separator.  The legacy
 * en_US spelling is intentionally not canonical.
 */
bool jetkvm_settings_keyboard_layout_valid(const char *layout);

/** Copy and canonicalize a layout; legacy en_US is migrated to en-US. */
esp_err_t jetkvm_settings_assign_keyboard_layout(
    jetkvm_settings_t *settings, const char *layout, bool *legacy_migrated);

/** True for Auto (0) or a manual preset in 100..1000 permille. */
bool jetkvm_settings_video_quality_permille_valid(uint32_t permille);

/** Normalize and assign the upstream stream-quality factor: 0 (Auto) or 0.1..1.0. */
esp_err_t jetkvm_settings_assign_video_quality(
    jetkvm_settings_t *settings, double factor);

/** Assign a codec preference implementable by the P4 (auto or h264). */
esp_err_t jetkvm_settings_assign_video_codec_preference(
    jetkvm_settings_t *settings, const char *codec);

/** Assign one of the EDID presets (jetkvm_video_edid_preset_t). */
esp_err_t jetkvm_settings_assign_video_edid_preset(
    jetkvm_settings_t *settings, jetkvm_video_edid_preset_t preset);

/** Enable or disable hiding the HDMI display while no browser session exists. */
esp_err_t jetkvm_settings_assign_host_display_idle(
    jetkvm_settings_t *settings, bool enabled);

/** Store -1 for disabled, 0 for the upstream default, or a positive delay. */
esp_err_t jetkvm_settings_assign_video_sleep_after(
    jetkvm_settings_t *settings, int32_t duration_seconds);

/** Validate a complete in-memory settings snapshot. */
bool jetkvm_settings_validate(const jetkvm_settings_t *settings);

/**
 * Load the versioned NVS record.
 *
 * The output is initialized to safe defaults before any storage access.  A
 * corrupt, unknown, or invalid record therefore returns its precise error
 * while leaving a usable en-US/quality-1.0 snapshot in `settings`.
 */
esp_err_t jetkvm_settings_load(jetkvm_settings_t *settings,
                               bool *migration_required);

/** Commit a validated canonical snapshot to the dedicated NVS namespace. */
esp_err_t jetkvm_settings_save(const jetkvm_settings_t *settings);

/** Load and publish runtime settings. Defaults remain usable after an error. */
esp_err_t jetkvm_settings_init(void);

/** True once init has published either stored settings or safe defaults. */
bool jetkvm_settings_is_initialized(void);

/** Copy the current keyboard layout into a caller-owned buffer. */
esp_err_t jetkvm_settings_get_keyboard_layout(char *output,
                                              size_t output_capacity);

/** Persist first, then atomically publish the canonical keyboard layout. */
esp_err_t jetkvm_settings_set_keyboard_layout(const char *layout);

esp_err_t jetkvm_settings_get_video_quality(double *factor);

/** Persist first, then publish the canonical factor in runtime settings. */
esp_err_t jetkvm_settings_set_video_quality(double factor);

esp_err_t jetkvm_settings_get_video_codec_preference(
    char *output, size_t output_capacity);

/** Persist first, then publish the canonical codec preference. */
esp_err_t jetkvm_settings_set_video_codec_preference(const char *codec);

esp_err_t jetkvm_settings_get_video_edid_preset(
    jetkvm_video_edid_preset_t *preset);

/** Persist first, then publish the selected EDID preset. */
esp_err_t jetkvm_settings_set_video_edid_preset(
    jetkvm_video_edid_preset_t preset);

esp_err_t jetkvm_settings_get_host_display_idle(bool *enabled);

/** Persist first, then publish the host-display idle policy. */
esp_err_t jetkvm_settings_set_host_display_idle(bool enabled);

esp_err_t jetkvm_settings_get_video_sleep_after(int32_t *duration_seconds);

/** Persist first, then publish the HDMI sleep delay. */
esp_err_t jetkvm_settings_set_video_sleep_after(int32_t duration_seconds);

/** Snapshot retained load/save diagnostics without exposing stored data. */
esp_err_t jetkvm_settings_get_status(jetkvm_settings_status_t *status);

#ifdef __cplusplus
}
#endif
