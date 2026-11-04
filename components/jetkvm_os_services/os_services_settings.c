/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "os_services_settings.h"

#include <string.h>

#include "os_services_wire.h"

#define RECORD_MAGIC 0x534f4b4aU /* "JKOS" */
#define RECORD_VERSION 1U
#define RECORD_FLAGS 8U
#define RECORD_RESOLUTION 9U
#define RECORD_DISPLAY_MODE 10U
#define RECORD_ADAPTATION 12U
#define RECORD_CAPTURE_DISPLAY (RECORD_ADAPTATION + OSVC_ADAPTATION_CAPACITY)
#define RECORD_CRC (OSVC_SETTINGS_RECORD_LEN - 4U)

_Static_assert(RECORD_CAPTURE_DISPLAY + OSVC_CAPTURE_DISPLAY_CAPACITY <=
                   RECORD_CRC,
               "the settings record must hold the capture display name");

enum {
    FLAG_TERMINAL = 1U << 0,
    FLAG_FILES = 1U << 1,
    FLAG_VIDEO = 1U << 2,
    FLAG_CLIPBOARD = 1U << 3,
    FLAG_ALL = 0x0fU,
};

static const struct {
    const char *name;
    uint32_t width;
    uint32_t height;
} s_resolutions[OSVC_RESOLUTION_COUNT] = {
    [OSVC_RESOLUTION_720P] = {"1280x720", 1280U, 720U},
    [OSVC_RESOLUTION_1080P] = {"1920x1080", 1920U, 1080U},
    [OSVC_RESOLUTION_1440P] = {"2560x1440", 2560U, 1440U},
    [OSVC_RESOLUTION_2160P] = {"3840x2160", 3840U, 2160U},
};

void osvc_bridge_settings_defaults(osvc_bridge_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->terminal_enabled = true;
    settings->files_enabled = true;
    settings->video_enabled = true;
    settings->clipboard_enabled = true;
    settings->resolution = OSVC_RESOLUTION_1440P;
    settings->display_mode = OSVC_LAYOUT_EXTEND;
}

bool osvc_bridge_settings_equal(const osvc_bridge_settings_t *left,
                                const osvc_bridge_settings_t *right)
{
    return left->terminal_enabled == right->terminal_enabled &&
           left->files_enabled == right->files_enabled &&
           left->video_enabled == right->video_enabled &&
           left->clipboard_enabled == right->clipboard_enabled &&
           left->resolution == right->resolution &&
           left->display_mode == right->display_mode &&
           strcmp(left->capture_display, right->capture_display) == 0 &&
           strcmp(left->adaptation, right->adaptation) == 0;
}

static bool parse_decimal(const char *text, size_t length, uint32_t *value)
{
    if (length == 0U || length > 10U) {
        return false;
    }
    uint64_t result = 0U;
    for (size_t i = 0U; i < length; ++i) {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        result = result * 10U + (uint64_t)(text[i] - '0');
    }
    if (result > UINT32_MAX) {
        return false;
    }
    *value = (uint32_t)result;
    return true;
}

bool osvc_parse_adaptation(const char *text, uint32_t *kbps, uint32_t *fps)
{
    *kbps = 0U;
    *fps = 0U;
    if (text[0] == '\0' || strcmp(text, "off") == 0 ||
        strcmp(text, "auto") == 0) {
        return true;
    }
    static const char prefix[] = "fixed:";
    if (strncmp(text, prefix, sizeof(prefix) - 1U) != 0) {
        return false;
    }
    const char *rate = text + sizeof(prefix) - 1U;
    const char *colon = strchr(rate, ':');
    if (colon == NULL) {
        return false;
    }
    uint32_t parsed_kbps;
    uint32_t parsed_fps;
    if (!parse_decimal(rate, (size_t)(colon - rate), &parsed_kbps) ||
        !parse_decimal(colon + 1, strlen(colon + 1), &parsed_fps)) {
        return false;
    }
    *kbps = parsed_kbps;
    *fps = parsed_fps;
    return true;
}

const char *osvc_resolution_name(osvc_resolution_t resolution)
{
    return resolution < OSVC_RESOLUTION_COUNT ?
               s_resolutions[resolution].name : "";
}

bool osvc_resolution_size(osvc_resolution_t resolution, uint32_t *width,
                          uint32_t *height)
{
    if (resolution >= OSVC_RESOLUTION_COUNT) {
        return false;
    }
    *width = s_resolutions[resolution].width;
    *height = s_resolutions[resolution].height;
    return true;
}

static bool valid_capture_display(const char *text, size_t length)
{
    if (length > OSVC_INFO_DISPLAY_MAX) {
        return false;
    }
    for (size_t i = 0U; i < length; ++i) {
        if (text[i] == '\0' || text[i] == '\r' || text[i] == '\n') {
            return false;
        }
    }
    return osvc_utf8_valid((const uint8_t *)text, length);
}

/* A field of the request; null counts as omitted, as the settings format
 * defines. */
static const cJSON *request_field(const cJSON *request, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(request, name);
    return cJSON_IsNull(item) ? NULL : item;
}

static bool merge_bool(const cJSON *request, const char *name, bool *value,
                       const char **error, const char *message)
{
    const cJSON *item = request_field(request, name);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsBool(item)) {
        *error = message;
        return false;
    }
    *value = cJSON_IsTrue(item);
    return true;
}

bool osvc_bridge_settings_merge(const osvc_bridge_settings_t *base,
                                const cJSON *request,
                                osvc_bridge_settings_t *out,
                                const char **error)
{
    if (!cJSON_IsObject(request)) {
        *error = "settings must be an object";
        return false;
    }
    osvc_bridge_settings_t merged = *base;
    if (!merge_bool(request, "terminal_enabled", &merged.terminal_enabled,
                    error, "terminal_enabled must be a boolean") ||
        !merge_bool(request, "files_enabled", &merged.files_enabled, error,
                    "files_enabled must be a boolean") ||
        !merge_bool(request, "video_enabled", &merged.video_enabled, error,
                    "video_enabled must be a boolean") ||
        !merge_bool(request, "clipboard_enabled", &merged.clipboard_enabled,
                    error, "clipboard_enabled must be a boolean")) {
        return false;
    }

    const cJSON *item = request_field(request, "capture_display");
    if (item != NULL) {
        if (!cJSON_IsString(item) || item->valuestring == NULL ||
            !valid_capture_display(item->valuestring,
                                   strlen(item->valuestring))) {
            *error = "invalid capture display name";
            return false;
        }
        memcpy(merged.capture_display, item->valuestring,
               strlen(item->valuestring) + 1U);
    }

    item = request_field(request, "resolution");
    if (item != NULL) {
        osvc_resolution_t found = OSVC_RESOLUTION_COUNT;
        for (size_t i = 0U; cJSON_IsString(item) && i < OSVC_RESOLUTION_COUNT;
             ++i) {
            if (strcmp(item->valuestring, s_resolutions[i].name) == 0) {
                found = (osvc_resolution_t)i;
            }
        }
        if (found == OSVC_RESOLUTION_COUNT) {
            *error = "unsupported OS Services resolution";
            return false;
        }
        merged.resolution = found;
    }

    item = request_field(request, "display_mode");
    if (item != NULL) {
        if (cJSON_IsString(item) && strcmp(item->valuestring, "mirror") == 0) {
            merged.display_mode = OSVC_LAYOUT_MIRROR;
        } else if (cJSON_IsString(item) &&
                   strcmp(item->valuestring, "extend") == 0) {
            merged.display_mode = OSVC_LAYOUT_EXTEND;
        } else {
            *error = "unsupported OS Services display mode";
            return false;
        }
    }

    item = request_field(request, "adaptation");
    if (item != NULL) {
        uint32_t kbps;
        uint32_t fps;
        if (!cJSON_IsString(item) ||
            strlen(item->valuestring) >= OSVC_ADAPTATION_CAPACITY ||
            !osvc_parse_adaptation(item->valuestring, &kbps, &fps)) {
            *error = "invalid adaptation value";
            return false;
        }
        memcpy(merged.adaptation, item->valuestring,
               strlen(item->valuestring) + 1U);
    }
    *out = merged;
    return true;
}

bool osvc_bridge_settings_to_json(const osvc_bridge_settings_t *settings,
                                  cJSON *object)
{
    if (!cJSON_AddBoolToObject(object, "terminal_enabled",
                               settings->terminal_enabled) ||
        !cJSON_AddBoolToObject(object, "files_enabled",
                               settings->files_enabled) ||
        !cJSON_AddStringToObject(object, "capture_display",
                                 settings->capture_display) ||
        !cJSON_AddBoolToObject(object, "video_enabled",
                               settings->video_enabled) ||
        !cJSON_AddStringToObject(object, "resolution",
                                 osvc_resolution_name(settings->resolution)) ||
        !cJSON_AddStringToObject(object, "display_mode",
                                 settings->display_mode == OSVC_LAYOUT_MIRROR ?
                                     "mirror" : "extend")) {
        return false;
    }
    /* The settings format omits an empty adaptation. */
    if (settings->adaptation[0] != '\0' &&
        !cJSON_AddStringToObject(object, "adaptation", settings->adaptation)) {
        return false;
    }
    return cJSON_AddBoolToObject(object, "clipboard_enabled",
                                 settings->clipboard_enabled) != NULL;
}

void osvc_bridge_settings_session(const osvc_bridge_settings_t *settings,
                                  osvc_settings_t *session)
{
    memset(session, 0, sizeof(*session));
    session->clipboard_enabled = settings->clipboard_enabled;
    session->terminal_enabled = settings->terminal_enabled;
    session->files_enabled = settings->files_enabled;
    session->video_enabled = settings->video_enabled;
    memcpy(session->capture_display, settings->capture_display,
           sizeof(session->capture_display));
    if (!osvc_resolution_size(settings->resolution, &session->width,
                              &session->height)) {
        osvc_resolution_size(OSVC_RESOLUTION_1440P, &session->width,
                             &session->height);
    }
    session->layout = settings->display_mode;
    if (!osvc_parse_adaptation(settings->adaptation, &session->target_kbps,
                               &session->maximum_fps)) {
        session->target_kbps = 0U;
        session->maximum_fps = 0U;
    }
}

/* strnlen, which strict C11 does not declare. */
static size_t bounded_length(const char *text, size_t capacity)
{
    size_t length = 0U;
    while (length < capacity && text[length] != '\0') {
        ++length;
    }
    return length;
}

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

void osvc_bridge_settings_encode(const osvc_bridge_settings_t *settings,
                                 uint8_t record[OSVC_SETTINGS_RECORD_LEN])
{
    memset(record, 0, OSVC_SETTINGS_RECORD_LEN);
    osvc_put_u32(record, RECORD_MAGIC);
    osvc_put_u16(record + 4, RECORD_VERSION);
    osvc_put_u16(record + 6, OSVC_SETTINGS_RECORD_LEN);
    record[RECORD_FLAGS] =
        (uint8_t)((settings->terminal_enabled ? FLAG_TERMINAL : 0U) |
                  (settings->files_enabled ? FLAG_FILES : 0U) |
                  (settings->video_enabled ? FLAG_VIDEO : 0U) |
                  (settings->clipboard_enabled ? FLAG_CLIPBOARD : 0U));
    record[RECORD_RESOLUTION] = (uint8_t)settings->resolution;
    record[RECORD_DISPLAY_MODE] = (uint8_t)settings->display_mode;
    memcpy(record + RECORD_ADAPTATION, settings->adaptation,
           bounded_length(settings->adaptation,
                          OSVC_ADAPTATION_CAPACITY - 1U));
    memcpy(record + RECORD_CAPTURE_DISPLAY, settings->capture_display,
           bounded_length(settings->capture_display,
                          OSVC_CAPTURE_DISPLAY_CAPACITY - 1U));
    osvc_put_u32(record + RECORD_CRC, crc32(record, RECORD_CRC));
}

static bool decode_string(const uint8_t *field, size_t capacity, char *out)
{
    const size_t length = bounded_length((const char *)field, capacity);
    if (length == capacity) {
        return false;
    }
    for (size_t i = length; i < capacity; ++i) {
        if (field[i] != 0U) {
            return false;
        }
    }
    memcpy(out, field, length + 1U);
    return true;
}

bool osvc_bridge_settings_decode(const uint8_t *record, size_t length,
                                 osvc_bridge_settings_t *settings)
{
    if (length != OSVC_SETTINGS_RECORD_LEN ||
        osvc_get_u32(record) != RECORD_MAGIC ||
        osvc_get_u16(record + 4) != RECORD_VERSION ||
        osvc_get_u16(record + 6) != OSVC_SETTINGS_RECORD_LEN ||
        osvc_get_u32(record + RECORD_CRC) != crc32(record, RECORD_CRC)) {
        return false;
    }
    const uint8_t flags = record[RECORD_FLAGS];
    if ((flags & ~FLAG_ALL) != 0U ||
        record[RECORD_RESOLUTION] >= OSVC_RESOLUTION_COUNT ||
        record[RECORD_DISPLAY_MODE] > OSVC_LAYOUT_EXTEND ||
        record[RECORD_DISPLAY_MODE + 1U] != 0U) {
        return false;
    }
    for (size_t i = RECORD_CAPTURE_DISPLAY + OSVC_CAPTURE_DISPLAY_CAPACITY;
         i < RECORD_CRC; ++i) {
        if (record[i] != 0U) {
            return false;
        }
    }
    osvc_bridge_settings_t decoded;
    memset(&decoded, 0, sizeof(decoded));
    uint32_t kbps;
    uint32_t fps;
    if (!decode_string(record + RECORD_ADAPTATION, OSVC_ADAPTATION_CAPACITY,
                       decoded.adaptation) ||
        !decode_string(record + RECORD_CAPTURE_DISPLAY,
                       OSVC_CAPTURE_DISPLAY_CAPACITY,
                       decoded.capture_display) ||
        !osvc_parse_adaptation(decoded.adaptation, &kbps, &fps) ||
        !valid_capture_display(decoded.capture_display,
                               strlen(decoded.capture_display))) {
        return false;
    }
    decoded.terminal_enabled = (flags & FLAG_TERMINAL) != 0U;
    decoded.files_enabled = (flags & FLAG_FILES) != 0U;
    decoded.video_enabled = (flags & FLAG_VIDEO) != 0U;
    decoded.clipboard_enabled = (flags & FLAG_CLIPBOARD) != 0U;
    decoded.resolution = (osvc_resolution_t)record[RECORD_RESOLUTION];
    decoded.display_mode = (osvc_layout_t)record[RECORD_DISPLAY_MODE];
    *settings = decoded;
    return true;
}
