/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The KVM's OS Services settings as the browser sees them
 * (getOsServicesSettings / setOsServicesSettings), their NVS record, and
 * the session settings they produce. The JSON names and values follow the
 * OS Services settings format.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "os_services_session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OSVC_ADAPTATION_CAPACITY 40U

typedef enum {
    OSVC_RESOLUTION_720P = 0,
    OSVC_RESOLUTION_1080P,
    OSVC_RESOLUTION_1440P,
    OSVC_RESOLUTION_2160P,
    OSVC_RESOLUTION_COUNT,
} osvc_resolution_t;

typedef struct {
    bool terminal_enabled;
    bool files_enabled;
    bool video_enabled;
    bool clipboard_enabled;
    /* The display the sender captures; empty lets it choose. */
    char capture_display[OSVC_CAPTURE_DISPLAY_CAPACITY];
    osvc_resolution_t resolution;
    osvc_layout_t display_mode;
    /* "" or "off" (encoder defaults; "auto" is a legacy alias), or
     * "fixed:<kbps>:<maxfps>". Kept as the browser sent it. */
    char adaptation[OSVC_ADAPTATION_CAPACITY];
} osvc_bridge_settings_t;

void osvc_bridge_settings_defaults(osvc_bridge_settings_t *settings);
bool osvc_bridge_settings_equal(const osvc_bridge_settings_t *left,
                                const osvc_bridge_settings_t *right);

/* "fixed:<kbps>:<maxfps>" gives both numbers; the off forms give zeros.
 * Returns false for any other text. */
bool osvc_parse_adaptation(const char *text, uint32_t *kbps, uint32_t *fps);

const char *osvc_resolution_name(osvc_resolution_t resolution);
bool osvc_resolution_size(osvc_resolution_t resolution, uint32_t *width,
                          uint32_t *height);

/*
 * Lays the fields present in `request` (a JSON object) over `base`, as the
 * OS Services settings format defines: an omitted field keeps its value. A
 * field of the wrong type or an invalid value fails the whole request, with
 * a reason in *error, and leaves *out untouched. Unknown fields are ignored.
 */
bool osvc_bridge_settings_merge(const osvc_bridge_settings_t *base,
                                const cJSON *request,
                                osvc_bridge_settings_t *out,
                                const char **error);

/* Adds the settings' fields to `object`. */
bool osvc_bridge_settings_to_json(const osvc_bridge_settings_t *settings,
                                  cJSON *object);

/* The settings the session applies. */
void osvc_bridge_settings_session(const osvc_bridge_settings_t *settings,
                                  osvc_settings_t *session);

/* The NVS record: versioned, with a CRC, so a torn or foreign blob loads
 * as defaults. */
#define OSVC_SETTINGS_RECORD_LEN 320U
void osvc_bridge_settings_encode(const osvc_bridge_settings_t *settings,
                                 uint8_t record[OSVC_SETTINGS_RECORD_LEN]);
bool osvc_bridge_settings_decode(const uint8_t *record, size_t length,
                                 osvc_bridge_settings_t *settings);

#ifdef __cplusplus
}
#endif
