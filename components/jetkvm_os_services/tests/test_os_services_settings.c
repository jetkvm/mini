/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/*
 * The browser's OS Services settings against the OS Services settings
 * format: the defaults and their JSON, how a request merges and what it
 * rejects, and the NVS record.
 * Records are re-sealed with zlib's CRC-32, not the code under test, so a
 * tampered field is caught by the field check and not by the CRC.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "cJSON.h"
#include "os_services_settings.h"
#include "os_services_wire.h"

/* The default settings as the settings format writes them: compact JSON
 * with the fields in this order. */
static const char k_format_defaults[] =
    "{\"terminal_enabled\":true,\"files_enabled\":true,"
    "\"capture_display\":\"\",\"video_enabled\":true,"
    "\"resolution\":\"2560x1440\",\"display_mode\":\"extend\","
    "\"clipboard_enabled\":true}";

static char *settings_json(const osvc_bridge_settings_t *settings)
{
    cJSON *object = cJSON_CreateObject();
    assert(object != NULL);
    assert(osvc_bridge_settings_to_json(settings, object));
    char *text = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    assert(text != NULL);
    return text;
}

/* Merges `request` (JSON text) over `base`. */
static bool merge_text(const osvc_bridge_settings_t *base, const char *request,
                       osvc_bridge_settings_t *out, const char **error)
{
    cJSON *parsed = cJSON_Parse(request);
    assert(parsed != NULL);
    *error = NULL;
    const bool merged = osvc_bridge_settings_merge(base, parsed, out, error);
    cJSON_Delete(parsed);
    assert(merged == (*error == NULL));
    return merged;
}

static void expect_rejected(const char *request, const char *reason)
{
    osvc_bridge_settings_t base;
    osvc_bridge_settings_defaults(&base);
    osvc_bridge_settings_t out;
    memset(&out, 0x5a, sizeof(out));
    const osvc_bridge_settings_t untouched = out;
    const char *error;
    if (merge_text(&base, request, &out, &error)) {
        fprintf(stderr, "accepted %s\n", request);
        abort();
    }
    if (strcmp(error, reason) != 0) {
        fprintf(stderr, "%s: got \"%s\", want \"%s\"\n", request, error,
                reason);
        abort();
    }
    assert(memcmp(&out, &untouched, sizeof(out)) == 0);
}

static void test_defaults_match_the_settings_format(void)
{
    osvc_bridge_settings_t settings;
    osvc_bridge_settings_defaults(&settings);
    char *text = settings_json(&settings);
    if (strcmp(text, k_format_defaults) != 0) {
        fprintf(stderr, "defaults: %s\n", text);
        abort();
    }
    cJSON_free(text);

    osvc_settings_t session;
    osvc_bridge_settings_session(&settings, &session);
    assert(session.video_enabled && session.clipboard_enabled &&
           session.terminal_enabled && session.files_enabled);
    assert(session.width == 2560U && session.height == 1440U);
    assert(session.layout == OSVC_LAYOUT_EXTEND);
    assert(session.target_kbps == 0U && session.maximum_fps == 0U);
    assert(session.capture_display[0] == '\0');
}

static void test_merge_keeps_omitted_fields(void)
{
    osvc_bridge_settings_t base;
    osvc_bridge_settings_defaults(&base);
    base.files_enabled = false;
    strcpy(base.capture_display, "DP-1");
    strcpy(base.adaptation, "fixed:6000:30");

    osvc_bridge_settings_t out;
    const char *error;
    /* Only the clipboard changes; null and unknown fields change nothing,
     * as the settings format defines. */
    assert(merge_text(&base,
                      "{\"clipboard_enabled\":false,\"resolution\":null,"
                      "\"future_field\":[1,2],\"Video_Enabled\":false}",
                      &out, &error));
    osvc_bridge_settings_t expected = base;
    expected.clipboard_enabled = false;
    assert(osvc_bridge_settings_equal(&out, &expected));
    assert(!osvc_bridge_settings_equal(&out, &base));

    /* An empty request is the base. */
    assert(merge_text(&base, "{}", &out, &error));
    assert(osvc_bridge_settings_equal(&out, &base));

    /* Every field at once, as the settings page sends them. */
    assert(merge_text(&base,
                      "{\"terminal_enabled\":false,\"files_enabled\":true,"
                      "\"capture_display\":\"\\\\\\\\.\\\\DISPLAY2\","
                      "\"video_enabled\":false,\"resolution\":\"1280x720\","
                      "\"display_mode\":\"mirror\",\"adaptation\":\"off\","
                      "\"clipboard_enabled\":true}",
                      &out, &error));
    assert(!out.terminal_enabled && out.files_enabled && !out.video_enabled &&
           out.clipboard_enabled);
    assert(strcmp(out.capture_display, "\\\\.\\DISPLAY2") == 0);
    assert(out.resolution == OSVC_RESOLUTION_720P);
    assert(out.display_mode == OSVC_LAYOUT_MIRROR);
    assert(strcmp(out.adaptation, "off") == 0);

    char *text = settings_json(&out);
    assert(strcmp(text,
                  "{\"terminal_enabled\":false,\"files_enabled\":true,"
                  "\"capture_display\":\"\\\\\\\\.\\\\DISPLAY2\","
                  "\"video_enabled\":false,\"resolution\":\"1280x720\","
                  "\"display_mode\":\"mirror\",\"adaptation\":\"off\","
                  "\"clipboard_enabled\":true}") == 0);
    cJSON_free(text);

    /* An empty adaptation clears a fixed one and is left out of the JSON. */
    assert(merge_text(&base, "{\"adaptation\":\"\"}", &out, &error));
    assert(out.adaptation[0] == '\0');
    text = settings_json(&out);
    assert(strstr(text, "adaptation") == NULL);
    cJSON_free(text);

    /* A request may name each resolution the Mini offers. */
    static const struct {
        const char *name;
        uint32_t width;
        uint32_t height;
    } resolutions[] = {
        {"1280x720", 1280U, 720U},
        {"1920x1080", 1920U, 1080U},
        {"2560x1440", 2560U, 1440U},
        {"3840x2160", 3840U, 2160U},
    };
    for (size_t i = 0U; i < sizeof(resolutions) / sizeof(resolutions[0]);
         ++i) {
        char request[64];
        snprintf(request, sizeof(request), "{\"resolution\":\"%s\"}",
                 resolutions[i].name);
        assert(merge_text(&base, request, &out, &error));
        osvc_settings_t session;
        osvc_bridge_settings_session(&out, &session);
        assert(session.width == resolutions[i].width);
        assert(session.height == resolutions[i].height);
        assert(strcmp(osvc_resolution_name(out.resolution),
                      resolutions[i].name) == 0);
    }
}

static void test_merge_rejects_invalid_settings(void)
{
    expect_rejected("[]", "settings must be an object");
    expect_rejected("\"video_enabled\"", "settings must be an object");
    expect_rejected("{\"video_enabled\":1}", "video_enabled must be a boolean");
    expect_rejected("{\"clipboard_enabled\":\"true\"}",
                    "clipboard_enabled must be a boolean");
    expect_rejected("{\"terminal_enabled\":{}}",
                    "terminal_enabled must be a boolean");
    expect_rejected("{\"files_enabled\":0}", "files_enabled must be a boolean");
    /* A valid field before an invalid one is not applied either. */
    expect_rejected("{\"video_enabled\":false,\"resolution\":\"640x480\"}",
                    "unsupported OS Services resolution");
    expect_rejected("{\"resolution\":1080}",
                    "unsupported OS Services resolution");
    expect_rejected("{\"resolution\":\"1920X1080\"}",
                    "unsupported OS Services resolution");
    expect_rejected("{\"resolution\":\"7680x4320\"}",
                    "unsupported OS Services resolution");
    expect_rejected("{\"display_mode\":\"extended\"}",
                    "unsupported OS Services display mode");
    expect_rejected("{\"display_mode\":true}",
                    "unsupported OS Services display mode");
    expect_rejected("{\"capture_display\":\"DP-1\\n\"}",
                    "invalid capture display name");
    expect_rejected("{\"capture_display\":\"DP\\r1\"}",
                    "invalid capture display name");
    expect_rejected("{\"capture_display\":7}", "invalid capture display name");

    static const char *const adaptations[] = {
        "on", "fixed", "fixed:", "fixed:1", "fixed:1:", "fixed::30",
        "fixed:a:30", "fixed:6000:3o", "fixed:6000:30:1", "fixed:-1:30",
        "fixed:4294967296:30", "Fixed:6000:30", " off",
    };
    for (size_t i = 0U; i < sizeof(adaptations) / sizeof(adaptations[0]);
         ++i) {
        char request[96];
        snprintf(request, sizeof(request), "{\"adaptation\":\"%s\"}",
                 adaptations[i]);
        expect_rejected(request, "invalid adaptation value");
    }
    expect_rejected("{\"adaptation\":30}", "invalid adaptation value");
}

static void test_capture_display_limits(void)
{
    osvc_bridge_settings_t base;
    osvc_bridge_settings_defaults(&base);
    osvc_bridge_settings_t out;
    const char *error;
    char request[600];

    /* 256 bytes is the settings format's limit. */
    char name[OSVC_INFO_DISPLAY_MAX + 2U];
    memset(name, 'D', OSVC_INFO_DISPLAY_MAX);
    name[OSVC_INFO_DISPLAY_MAX] = '\0';
    snprintf(request, sizeof(request), "{\"capture_display\":\"%s\"}", name);
    assert(merge_text(&base, request, &out, &error));
    assert(strlen(out.capture_display) == OSVC_INFO_DISPLAY_MAX);

    name[OSVC_INFO_DISPLAY_MAX] = 'D';
    name[OSVC_INFO_DISPLAY_MAX + 1U] = '\0';
    snprintf(request, sizeof(request), "{\"capture_display\":\"%s\"}", name);
    expect_rejected(request, "invalid capture display name");

    /* Bytes, not characters: 128 two-byte characters fit, 129 do not. */
    char wide[600] = "{\"capture_display\":\"";
    for (int i = 0; i < 128; ++i) {
        strcat(wide, "\xc3\xa9");
    }
    strcat(wide, "\"}");
    assert(merge_text(&base, wide, &out, &error));
    assert(strlen(out.capture_display) == 256U);
    wide[strlen(wide) - 2U] = '\0';
    strcat(wide, "\xc3\xa9\"}");
    expect_rejected(wide, "invalid capture display name");

    /* The sender receives the name as UTF-8 text. */
    expect_rejected("{\"capture_display\":\"DP\xff\"}",
                    "invalid capture display name");
}

static void test_adaptation_reaches_the_session(void)
{
    uint32_t kbps;
    uint32_t fps;
    assert(osvc_parse_adaptation("fixed:6000:30", &kbps, &fps));
    assert(kbps == 6000U && fps == 30U);
    assert(osvc_parse_adaptation("fixed:4294967295:0", &kbps, &fps));
    assert(kbps == UINT32_MAX && fps == 0U);
    assert(osvc_parse_adaptation("auto", &kbps, &fps));
    assert(kbps == 0U && fps == 0U);
    assert(!osvc_parse_adaptation("fixed:1:2:3", &kbps, &fps));

    osvc_bridge_settings_t settings;
    osvc_bridge_settings_defaults(&settings);
    strcpy(settings.adaptation, "fixed:2500:15");
    settings.display_mode = OSVC_LAYOUT_MIRROR;
    settings.resolution = OSVC_RESOLUTION_1080P;
    settings.terminal_enabled = false;
    strcpy(settings.capture_display, "HDMI-A-1");
    osvc_settings_t session;
    osvc_bridge_settings_session(&settings, &session);
    assert(session.target_kbps == 2500U && session.maximum_fps == 15U);
    assert(session.layout == OSVC_LAYOUT_MIRROR);
    assert(session.width == 1920U && session.height == 1080U);
    assert(!session.terminal_enabled && session.files_enabled);
    assert(strcmp(session.capture_display, "HDMI-A-1") == 0);
}

/* Field offsets of the record, written out so a layout change shows up
 * here: magic, version, length, flags, resolution, display mode, a zero
 * byte, the adaptation (40 bytes), the capture display (257 bytes), zero
 * padding, then the CRC of everything before it. */
#define REC_FLAGS 8U
#define REC_RESOLUTION 9U
#define REC_DISPLAY_MODE 10U
#define REC_RESERVED 11U
#define REC_ADAPTATION 12U
#define REC_CAPTURE_DISPLAY 52U
#define REC_PADDING (52U + 257U)
#define REC_CRC 316U

static void reseal(uint8_t *record)
{
    const uLong crc = crc32(0L, record, REC_CRC);
    osvc_put_u32(record + REC_CRC, (uint32_t)crc);
}

static bool decodes(const uint8_t *record)
{
    osvc_bridge_settings_t settings;
    return osvc_bridge_settings_decode(record, OSVC_SETTINGS_RECORD_LEN,
                                       &settings);
}

static void test_record_round_trip_and_layout(void)
{
    assert(OSVC_SETTINGS_RECORD_LEN == 320U);
    osvc_bridge_settings_t settings;
    osvc_bridge_settings_defaults(&settings);
    settings.files_enabled = false;
    settings.resolution = OSVC_RESOLUTION_1080P;
    settings.display_mode = OSVC_LAYOUT_MIRROR;
    strcpy(settings.adaptation, "fixed:4294967295:4294967295");
    memset(settings.capture_display, 'x', OSVC_INFO_DISPLAY_MAX);
    settings.capture_display[OSVC_INFO_DISPLAY_MAX] = '\0';

    uint8_t record[OSVC_SETTINGS_RECORD_LEN];
    osvc_bridge_settings_encode(&settings, record);
    assert(memcmp(record, "JKOS", 4U) == 0);
    assert(osvc_get_u16(record + 4) == 1U);
    assert(osvc_get_u16(record + 6) == OSVC_SETTINGS_RECORD_LEN);
    /* terminal, video, clipboard; not files. */
    assert(record[REC_FLAGS] == 0x0dU);
    assert(record[REC_RESOLUTION] == 1U);
    assert(record[REC_DISPLAY_MODE] == (uint8_t)OSVC_LAYOUT_MIRROR);
    assert(record[REC_RESERVED] == 0U);
    assert(memcmp(record + REC_ADAPTATION, "fixed:4294967295:4294967295",
                  28U) == 0);
    assert(record[REC_CAPTURE_DISPLAY] == 'x' &&
           record[REC_CAPTURE_DISPLAY + 255U] == 'x' &&
           record[REC_CAPTURE_DISPLAY + 256U] == 0U);
    assert(osvc_get_u32(record + REC_CRC) ==
           (uint32_t)crc32(0L, record, REC_CRC));

    osvc_bridge_settings_t decoded;
    assert(osvc_bridge_settings_decode(record, sizeof(record), &decoded));
    assert(osvc_bridge_settings_equal(&decoded, &settings));
    char *a = settings_json(&decoded);
    char *b = settings_json(&settings);
    assert(strcmp(a, b) == 0);
    cJSON_free(a);
    cJSON_free(b);

    /* Every setting combination of the flags survives. */
    for (unsigned flags = 0U; flags < 16U; ++flags) {
        osvc_bridge_settings_defaults(&settings);
        settings.terminal_enabled = (flags & 1U) != 0U;
        settings.files_enabled = (flags & 2U) != 0U;
        settings.video_enabled = (flags & 4U) != 0U;
        settings.clipboard_enabled = (flags & 8U) != 0U;
        osvc_bridge_settings_encode(&settings, record);
        assert(osvc_bridge_settings_decode(record, sizeof(record), &decoded));
        assert(osvc_bridge_settings_equal(&decoded, &settings));
    }

    /* A short or long blob, as a different record size would be. */
    assert(!osvc_bridge_settings_decode(record, sizeof(record) - 1U,
                                        &decoded));
    uint8_t longer[OSVC_SETTINGS_RECORD_LEN + 1U];
    memcpy(longer, record, sizeof(record));
    longer[sizeof(record)] = 0U;
    assert(!osvc_bridge_settings_decode(longer, sizeof(longer), &decoded));
}

static void test_record_rejects_damage(void)
{
    osvc_bridge_settings_t settings;
    osvc_bridge_settings_defaults(&settings);
    strcpy(settings.adaptation, "fixed:6000:30");
    strcpy(settings.capture_display, "DP-2");
    uint8_t good[OSVC_SETTINGS_RECORD_LEN];
    osvc_bridge_settings_encode(&settings, good);
    assert(decodes(good));

    /* Any flipped bit, CRC included, is caught. */
    for (size_t bit = 0U; bit < OSVC_SETTINGS_RECORD_LEN * 8U; ++bit) {
        uint8_t record[OSVC_SETTINGS_RECORD_LEN];
        memcpy(record, good, sizeof(record));
        record[bit / 8U] ^= (uint8_t)(1U << (bit % 8U));
        if (decodes(record)) {
            fprintf(stderr, "bit %zu flip accepted\n", bit);
            abort();
        }
    }

    /* A torn write: the second half still erased. */
    uint8_t record[OSVC_SETTINGS_RECORD_LEN];
    memcpy(record, good, sizeof(record));
    memset(record + 160, 0xff, sizeof(record) - 160U);
    assert(!decodes(record));
    memset(record, 0, sizeof(record));
    assert(!decodes(record));

    /* Each field checked on its own, with a valid CRC. */
    static const struct {
        size_t offset;
        uint8_t value;
    } fields[] = {
        {0U, 'X'},                         /* magic */
        {4U, 2U},                          /* version */
        {6U, 0x41U},                       /* length */
        {REC_FLAGS, 0x10U},                /* unknown flag */
        {REC_RESOLUTION, 4U},              /* resolution */
        {REC_DISPLAY_MODE, 2U},            /* display mode */
        {REC_RESERVED, 1U},                /* reserved byte */
        {REC_ADAPTATION, 'X'},             /* "Xixed:6000:30" */
        {REC_ADAPTATION + 20U, 'x'},       /* text after the terminator */
        {REC_CAPTURE_DISPLAY + 2U, '\n'},  /* "DP\n2" */
        {REC_CAPTURE_DISPLAY + 1U, 0xffU}, /* not UTF-8 */
        {REC_CAPTURE_DISPLAY + 200U, 'x'}, /* text after the terminator */
        {REC_PADDING, 1U},                 /* padding */
        {REC_CRC - 1U, 1U},                /* padding */
    };
    for (size_t i = 0U; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        memcpy(record, good, sizeof(record));
        record[fields[i].offset] = fields[i].value;
        reseal(record);
        if (decodes(record)) {
            fprintf(stderr, "field at %zu accepted\n", fields[i].offset);
            abort();
        }
    }

    /* Strings with no terminator. */
    memcpy(record, good, sizeof(record));
    memset(record + REC_ADAPTATION, 'a', 40U);
    reseal(record);
    assert(!decodes(record));
    memcpy(record, good, sizeof(record));
    memset(record + REC_CAPTURE_DISPLAY, 'a', 257U);
    reseal(record);
    assert(!decodes(record));

    /* Resealing the good record changes nothing. */
    memcpy(record, good, sizeof(record));
    reseal(record);
    assert(memcmp(record, good, sizeof(record)) == 0);
}

/* A record saved with 2160p loads as 2160p with its other settings intact,
 * also one saved by the firmware that loaded 2160p as 1440p. */
static void test_record_with_2160p_loads_as_2160p(void)
{
    osvc_bridge_settings_t settings;
    osvc_bridge_settings_defaults(&settings);
    settings.terminal_enabled = false;
    settings.resolution = OSVC_RESOLUTION_720P;
    settings.display_mode = OSVC_LAYOUT_MIRROR;
    strcpy(settings.adaptation, "fixed:8000:30");
    strcpy(settings.capture_display, "HDMI-A-2");
    uint8_t record[OSVC_SETTINGS_RECORD_LEN];
    osvc_bridge_settings_encode(&settings, record);
    record[REC_RESOLUTION] = 3U;
    reseal(record);

    osvc_bridge_settings_t decoded;
    assert(osvc_bridge_settings_decode(record, sizeof(record), &decoded));
    osvc_bridge_settings_t expected = settings;
    expected.resolution = OSVC_RESOLUTION_2160P;
    assert(osvc_bridge_settings_equal(&decoded, &expected));
    char *text = settings_json(&decoded);
    assert(strstr(text, "\"resolution\":\"3840x2160\"") != NULL);
    cJSON_free(text);
    osvc_settings_t session;
    osvc_bridge_settings_session(&decoded, &session);
    assert(session.width == 3840U && session.height == 2160U);
}

int main(void)
{
    test_defaults_match_the_settings_format();
    test_merge_keeps_omitted_fields();
    test_merge_rejects_invalid_settings();
    test_capture_display_limits();
    test_adaptation_reaches_the_session();
    test_record_round_trip_and_layout();
    test_record_rejects_damage();
    test_record_with_2160p_loads_as_2160p();
    puts("os_services settings tests passed");
    return 0;
}
