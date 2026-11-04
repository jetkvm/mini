#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_settings.h"
#include "mock_nvs.h"

#define TEST_RECORD_MAGIC 0x54534b4aU

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    char keyboard_layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    uint32_t video_quality_permille;
    char video_codec_preference[JETKVM_VIDEO_CODEC_PREFERENCE_CAPACITY];
    uint32_t video_edid_preset;
    uint32_t host_display_idle_enabled;
    int32_t video_sleep_after_seconds;
    uint32_t crc32;
} test_record_t;

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    char keyboard_layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    uint32_t video_quality_permille;
    char video_codec_preference[JETKVM_VIDEO_CODEC_PREFERENCE_CAPACITY];
    uint32_t video_edid_preset;
    uint32_t host_display_idle_enabled;
    uint32_t crc32;
} test_record_v5_t;

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    char keyboard_layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    uint32_t video_quality_permille;
    char video_codec_preference[JETKVM_VIDEO_CODEC_PREFERENCE_CAPACITY];
    uint32_t video_edid_preset;
    uint32_t crc32;
} test_record_v4_t;

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    char keyboard_layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    uint32_t video_quality_permille;
    char video_codec_preference[JETKVM_VIDEO_CODEC_PREFERENCE_CAPACITY];
    uint32_t crc32;
} test_record_v3_t;

typedef struct {
    uint32_t magic;
    uint32_t schema_version;
    char keyboard_layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    uint32_t video_quality_permille;
    uint32_t crc32;
} test_record_v2_t;

static uint32_t crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return ~crc;
}

static test_record_t raw_record(const char *layout)
{
    test_record_t record = {
        .magic = TEST_RECORD_MAGIC,
        .schema_version = JETKVM_SETTINGS_SCHEMA_VERSION,
    };
    size_t length = strlen(layout);
    assert(length < sizeof(record.keyboard_layout));
    memcpy(record.keyboard_layout, layout, length + 1U);
    record.video_quality_permille =
        JETKVM_DEFAULT_VIDEO_QUALITY_PERMILLE;
    memcpy(record.video_codec_preference,
           JETKVM_DEFAULT_VIDEO_CODEC_PREFERENCE,
           sizeof(JETKVM_DEFAULT_VIDEO_CODEC_PREFERENCE));
    record.video_sleep_after_seconds =
        JETKVM_DEFAULT_VIDEO_SLEEP_AFTER_SECONDS;
    record.crc32 = crc32(&record, offsetof(test_record_t, crc32));
    return record;
}

static void assert_default(const jetkvm_settings_t *settings)
{
    assert(settings->schema_version == JETKVM_SETTINGS_SCHEMA_VERSION);
    assert(strcmp(settings->keyboard_layout,
                  JETKVM_DEFAULT_KEYBOARD_LAYOUT) == 0);
    assert(settings->video_quality_permille ==
           JETKVM_DEFAULT_VIDEO_QUALITY_PERMILLE);
    assert(strcmp(settings->video_codec_preference,
                  JETKVM_DEFAULT_VIDEO_CODEC_PREFERENCE) == 0);
    assert(settings->video_edid_preset ==
           JETKVM_DEFAULT_VIDEO_EDID_PRESET);
    assert(settings->host_display_idle_enabled == 0U);
    assert(settings->video_sleep_after_seconds ==
           JETKVM_DEFAULT_VIDEO_SLEEP_AFTER_SECONDS);
    assert(jetkvm_settings_validate(settings));
}

static void test_core_validation_and_migration(void)
{
    const char *valid[] = {
        "en-US", "de-DE", "zh-Hans-CN", "x-1",
    };
    for (size_t i = 0U; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        assert(jetkvm_settings_keyboard_layout_valid(valid[i]));
    }
    const char *invalid[] = {
        "", "e", "-en", "en-", "en--US", "en_US", "en US",
        "en/US", "0123456789abcdef", "\x01" "en-US",
    };
    for (size_t i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        assert(!jetkvm_settings_keyboard_layout_valid(invalid[i]));
    }

    jetkvm_settings_t settings;
    jetkvm_settings_defaults(&settings);
    bool migrated = false;
    assert(jetkvm_settings_assign_keyboard_layout(
               &settings, "en_US", &migrated) == ESP_OK);
    assert(migrated);
    assert_default(&settings);
    assert(jetkvm_settings_assign_keyboard_layout(
               &settings, "fr-FR", &migrated) == ESP_OK);
    assert(!migrated);
    assert(strcmp(settings.keyboard_layout, "fr-FR") == 0);
    assert(jetkvm_settings_assign_keyboard_layout(
               &settings, "fr_FR", NULL) == ESP_ERR_INVALID_ARG);
    assert(strcmp(settings.keyboard_layout, "fr-FR") == 0);
    assert(jetkvm_settings_assign_video_quality(&settings, 0.5) == ESP_OK);
    assert(settings.video_quality_permille == 500U);
    assert(jetkvm_settings_assign_video_quality(&settings, 0.099) ==
           ESP_ERR_INVALID_ARG);
    assert(settings.video_quality_permille == 500U);
    assert(jetkvm_settings_assign_video_quality(&settings, -0.5) ==
           ESP_ERR_INVALID_ARG);
    assert(settings.video_quality_permille == 500U);
    /* 0 is Auto and persists as permille 0. */
    assert(jetkvm_settings_assign_video_quality(&settings, 0.0) == ESP_OK);
    assert(settings.video_quality_permille ==
           JETKVM_AUTO_VIDEO_QUALITY_PERMILLE);
    assert(jetkvm_settings_validate(&settings));
    assert(jetkvm_settings_video_quality_permille_valid(0U));
    assert(!jetkvm_settings_video_quality_permille_valid(1U));
    assert(!jetkvm_settings_video_quality_permille_valid(99U));
    assert(jetkvm_settings_video_quality_permille_valid(100U));
    assert(jetkvm_settings_video_quality_permille_valid(1000U));
    assert(!jetkvm_settings_video_quality_permille_valid(1001U));
    assert(jetkvm_settings_assign_video_quality(&settings, 0.5) == ESP_OK);
    assert(jetkvm_settings_assign_video_codec_preference(
               &settings, "h264") == ESP_OK);
    assert(strcmp(settings.video_codec_preference, "h264") == 0);
    assert(jetkvm_settings_assign_video_codec_preference(
               &settings, "h265") == ESP_ERR_INVALID_ARG);
    assert(strcmp(settings.video_codec_preference, "h264") == 0);
    assert(jetkvm_settings_assign_video_edid_preset(
               &settings, JETKVM_VIDEO_EDID_PRESET_DELL_D2721H) == ESP_OK);
    assert(settings.video_edid_preset == JETKVM_VIDEO_EDID_PRESET_DELL_D2721H);
    assert(jetkvm_settings_assign_video_edid_preset(
               &settings, (jetkvm_video_edid_preset_t)2) ==
           ESP_ERR_INVALID_ARG);
    assert(settings.video_edid_preset == JETKVM_VIDEO_EDID_PRESET_DELL_D2721H);
    assert(jetkvm_settings_assign_host_display_idle(
               &settings, true) == ESP_OK);
    assert(settings.host_display_idle_enabled == 1U);
    assert(jetkvm_settings_assign_host_display_idle(
               &settings, false) == ESP_OK);
    assert(settings.host_display_idle_enabled == 0U);
    assert(jetkvm_settings_assign_video_sleep_after(
               &settings, 90) == ESP_OK);
    assert(settings.video_sleep_after_seconds == 90);
    assert(jetkvm_settings_assign_video_sleep_after(
               &settings, -1) == ESP_OK);
    assert(settings.video_sleep_after_seconds == -1);
    assert(jetkvm_settings_assign_video_sleep_after(
               &settings, -2) == ESP_ERR_INVALID_ARG);
    assert(settings.video_sleep_after_seconds == -1);
}

static void test_nvs_records(void)
{
    mock_nvs_reset();
    jetkvm_settings_t settings;
    bool migrated = true;
    assert(jetkvm_settings_load(&settings, &migrated) == ESP_OK);
    assert(!migrated);
    assert_default(&settings);

    assert(jetkvm_settings_assign_keyboard_layout(
               &settings, "ja-JP", NULL) == ESP_OK);
    assert(jetkvm_settings_assign_video_quality(&settings, 0.5) == ESP_OK);
    assert(jetkvm_settings_assign_video_codec_preference(
               &settings, "h264") == ESP_OK);
    assert(jetkvm_settings_assign_video_edid_preset(
               &settings, JETKVM_VIDEO_EDID_PRESET_DELL_D2721H) == ESP_OK);
    assert(jetkvm_settings_assign_host_display_idle(
               &settings, true) == ESP_OK);
    assert(jetkvm_settings_assign_video_sleep_after(
               &settings, 90) == ESP_OK);
    assert(jetkvm_settings_save(&settings) == ESP_OK);
    assert(mock_nvs_committed_size() == sizeof(test_record_t));

    jetkvm_settings_t loaded;
    memset(&loaded, 0xa5, sizeof(loaded));
    assert(jetkvm_settings_load(&loaded, &migrated) == ESP_OK);
    assert(!migrated);
    assert(strcmp(loaded.keyboard_layout, "ja-JP") == 0);
    assert(loaded.video_quality_permille == 500U);
    assert(strcmp(loaded.video_codec_preference, "h264") == 0);
    assert(loaded.video_edid_preset == JETKVM_VIDEO_EDID_PRESET_DELL_D2721H);
    assert(loaded.host_display_idle_enabled == 1U);
    assert(loaded.video_sleep_after_seconds == 90);

    test_record_v5_t v5 = {
        .magic = TEST_RECORD_MAGIC,
        .schema_version = 5U,
        .keyboard_layout = "es-ES",
        .video_quality_permille = 875U,
        .video_codec_preference = "h264",
        .video_edid_preset = JETKVM_VIDEO_EDID_PRESET_DELL_D2721H,
        .host_display_idle_enabled = 1U,
    };
    v5.crc32 = crc32(&v5, offsetof(test_record_v5_t, crc32));
    mock_nvs_replace_committed(&v5, sizeof(v5));
    assert(jetkvm_settings_load(&loaded, &migrated) == ESP_OK);
    assert(migrated);
    assert(strcmp(loaded.keyboard_layout, "es-ES") == 0);
    assert(loaded.video_quality_permille == 875U);
    assert(strcmp(loaded.video_codec_preference, "h264") == 0);
    assert(loaded.video_edid_preset == JETKVM_VIDEO_EDID_PRESET_DELL_D2721H);
    assert(loaded.host_display_idle_enabled == 1U);
    assert(loaded.video_sleep_after_seconds ==
           JETKVM_DEFAULT_VIDEO_SLEEP_AFTER_SECONDS);

    test_record_v4_t v4 = {
        .magic = TEST_RECORD_MAGIC,
        .schema_version = 4U,
        .keyboard_layout = "nl-NL",
        .video_quality_permille = 750U,
        .video_codec_preference = "h264",
        .video_edid_preset = JETKVM_VIDEO_EDID_PRESET_DELL_D2721H,
    };
    v4.crc32 = crc32(&v4, offsetof(test_record_v4_t, crc32));
    mock_nvs_replace_committed(&v4, sizeof(v4));
    assert(jetkvm_settings_load(&loaded, &migrated) == ESP_OK);
    assert(migrated);
    assert(strcmp(loaded.keyboard_layout, "nl-NL") == 0);
    assert(loaded.video_quality_permille == 750U);
    assert(strcmp(loaded.video_codec_preference, "h264") == 0);
    assert(loaded.video_edid_preset == JETKVM_VIDEO_EDID_PRESET_DELL_D2721H);
    assert(loaded.host_display_idle_enabled == 0U);
    assert(loaded.video_sleep_after_seconds ==
           JETKVM_DEFAULT_VIDEO_SLEEP_AFTER_SECONDS);

    test_record_v3_t v3 = {
        .magic = TEST_RECORD_MAGIC,
        .schema_version = 3U,
        .keyboard_layout = "ko-KR",
        .video_quality_permille = 625U,
        .video_codec_preference = "h264",
    };
    v3.crc32 = crc32(&v3, offsetof(test_record_v3_t, crc32));
    mock_nvs_replace_committed(&v3, sizeof(v3));
    assert(jetkvm_settings_load(&loaded, &migrated) == ESP_OK);
    assert(migrated);
    assert(strcmp(loaded.keyboard_layout, "ko-KR") == 0);
    assert(loaded.video_quality_permille == 625U);
    assert(strcmp(loaded.video_codec_preference, "h264") == 0);
    assert(loaded.video_edid_preset == JETKVM_DEFAULT_VIDEO_EDID_PRESET);
    assert(loaded.host_display_idle_enabled == 0U);
    assert(loaded.video_sleep_after_seconds ==
           JETKVM_DEFAULT_VIDEO_SLEEP_AFTER_SECONDS);
    assert(jetkvm_settings_save(&loaded) == ESP_OK);
    assert(mock_nvs_committed_size() == sizeof(test_record_t));

    test_record_v2_t v2 = {
        .magic = TEST_RECORD_MAGIC,
        .schema_version = 2U,
        .keyboard_layout = "fr-FR",
        .video_quality_permille = 375U,
    };
    v2.crc32 = crc32(&v2, offsetof(test_record_v2_t, crc32));
    mock_nvs_replace_committed(&v2, sizeof(v2));
    assert(jetkvm_settings_load(&loaded, &migrated) == ESP_OK);
    assert(migrated);
    assert(strcmp(loaded.keyboard_layout, "fr-FR") == 0);
    assert(loaded.video_quality_permille == 375U);
    assert(strcmp(loaded.video_codec_preference, "auto") == 0);
    assert(loaded.video_edid_preset == JETKVM_DEFAULT_VIDEO_EDID_PRESET);
    assert(jetkvm_settings_save(&loaded) == ESP_OK);
    assert(mock_nvs_committed_size() == sizeof(test_record_t));

    /* Restore the original v3 record for the transaction/corruption checks. */
    assert(jetkvm_settings_assign_keyboard_layout(
               &settings, "ja-JP", NULL) == ESP_OK);
    assert(jetkvm_settings_save(&settings) == ESP_OK);

    assert(jetkvm_settings_assign_keyboard_layout(
               &settings, "de-DE", NULL) == ESP_OK);
    mock_nvs_fail_next_commit();
    assert(jetkvm_settings_save(&settings) == ESP_FAIL);
    assert(jetkvm_settings_load(&loaded, NULL) == ESP_OK);
    assert(strcmp(loaded.keyboard_layout, "ja-JP") == 0);

    size_t size = mock_nvs_committed_size();
    mock_nvs_corrupt_byte(size - 1U, 0x01U);
    memset(&loaded, 0xa5, sizeof(loaded));
    assert(jetkvm_settings_load(&loaded, NULL) == ESP_ERR_INVALID_CRC);
    assert_default(&loaded);

    test_record_t record = raw_record("en_US");
    mock_nvs_replace_committed(&record, sizeof(record));
    assert(jetkvm_settings_load(&loaded, &migrated) == ESP_OK);
    assert(migrated);
    assert_default(&loaded);
    assert(jetkvm_settings_save(&loaded) == ESP_OK);
    assert(jetkvm_settings_load(&loaded, &migrated) == ESP_OK);
    assert(!migrated);
    assert_default(&loaded);

    record = raw_record("bad_layout");
    mock_nvs_replace_committed(&record, sizeof(record));
    assert(jetkvm_settings_load(&loaded, NULL) == ESP_ERR_INVALID_STATE);
    assert_default(&loaded);

    record = raw_record("fr-FR");
    record.schema_version++;
    record.crc32 = crc32(&record, offsetof(test_record_t, crc32));
    mock_nvs_replace_committed(&record, sizeof(record));
    assert(jetkvm_settings_load(&loaded, NULL) == ESP_ERR_INVALID_VERSION);
    assert_default(&loaded);

    mock_nvs_replace_committed(&record, sizeof(record) - 1U);
    assert(jetkvm_settings_load(&loaded, NULL) == ESP_ERR_INVALID_SIZE);
    assert_default(&loaded);
}

static void test_runtime_corruption_fallback_and_transaction(void)
{
    test_record_t corrupt = raw_record("sv-SE");
    corrupt.crc32 ^= 1U;
    mock_nvs_replace_committed(&corrupt, sizeof(corrupt));

    assert(jetkvm_settings_init() == ESP_ERR_INVALID_CRC);
    assert(jetkvm_settings_is_initialized());
    char layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    assert(jetkvm_settings_get_keyboard_layout(
               layout, sizeof(layout)) == ESP_OK);
    assert(strcmp(layout, JETKVM_DEFAULT_KEYBOARD_LAYOUT) == 0);
    assert(jetkvm_settings_get_keyboard_layout(layout, 2U) ==
           ESP_ERR_INVALID_SIZE);

    jetkvm_settings_status_t status;
    assert(jetkvm_settings_get_status(&status) == ESP_OK);
    assert(status.initialized);
    assert(status.recovered_defaults);
    assert(status.load_error == ESP_ERR_INVALID_CRC);

    unsigned commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_keyboard_layout("en_US") == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before + 1U);
    assert(jetkvm_settings_get_keyboard_layout(
               layout, sizeof(layout)) == ESP_OK);
    assert(strcmp(layout, "en-US") == 0);

    commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_keyboard_layout("en-US") == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before + 1U);

    assert(jetkvm_settings_set_keyboard_layout("pl-PL") == ESP_OK);
    assert(jetkvm_settings_get_keyboard_layout(
               layout, sizeof(layout)) == ESP_OK);
    assert(strcmp(layout, "pl-PL") == 0);

    double quality = 1.0;
    assert(jetkvm_settings_get_video_quality(&quality) == ESP_OK);
    assert(quality == (double)JETKVM_DEFAULT_VIDEO_QUALITY_PERMILLE / 1000.0);
    commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_video_quality(0.5) == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before + 1U);
    assert(jetkvm_settings_get_video_quality(&quality) == ESP_OK);
    assert(quality == 0.5);
    commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_video_quality(0.5) == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before);

    mock_nvs_fail_next_commit();
    assert(jetkvm_settings_set_video_quality(0.25) == ESP_FAIL);
    assert(jetkvm_settings_get_video_quality(&quality) == ESP_OK);
    assert(quality == 0.5);
    assert(jetkvm_settings_set_video_quality(0.05) == ESP_ERR_INVALID_ARG);
    commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_video_quality(0.0) == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before + 1U);
    assert(jetkvm_settings_get_video_quality(&quality) == ESP_OK);
    assert(quality == 0.0);
    assert(jetkvm_settings_set_video_quality(0.5) == ESP_OK);

    char codec[JETKVM_VIDEO_CODEC_PREFERENCE_CAPACITY];
    assert(jetkvm_settings_get_video_codec_preference(
               codec, sizeof(codec)) == ESP_OK);
    assert(strcmp(codec, "auto") == 0);
    commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_video_codec_preference("h264") == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before + 1U);
    assert(jetkvm_settings_get_video_codec_preference(
               codec, sizeof(codec)) == ESP_OK);
    assert(strcmp(codec, "h264") == 0);
    commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_video_codec_preference("h264") == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before);
    mock_nvs_fail_next_commit();
    assert(jetkvm_settings_set_video_codec_preference("auto") == ESP_FAIL);
    assert(jetkvm_settings_get_video_codec_preference(
               codec, sizeof(codec)) == ESP_OK);
    assert(strcmp(codec, "h264") == 0);
    assert(jetkvm_settings_set_video_codec_preference("h265") ==
           ESP_ERR_INVALID_ARG);

    jetkvm_video_edid_preset_t edid_preset =
        JETKVM_VIDEO_EDID_PRESET_DELL_D2721H;
    assert(jetkvm_settings_get_video_edid_preset(&edid_preset) == ESP_OK);
    assert(edid_preset == JETKVM_DEFAULT_VIDEO_EDID_PRESET);
    commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_video_edid_preset(
               JETKVM_VIDEO_EDID_PRESET_DELL_D2721H) == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before + 1U);
    assert(jetkvm_settings_get_video_edid_preset(&edid_preset) == ESP_OK);
    assert(edid_preset == JETKVM_VIDEO_EDID_PRESET_DELL_D2721H);
    commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_video_edid_preset(
               JETKVM_VIDEO_EDID_PRESET_DELL_D2721H) == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before);
    mock_nvs_fail_next_commit();
    assert(jetkvm_settings_set_video_edid_preset(
               JETKVM_VIDEO_EDID_PRESET_DEFAULT) == ESP_FAIL);
    assert(jetkvm_settings_get_video_edid_preset(&edid_preset) == ESP_OK);
    assert(edid_preset == JETKVM_VIDEO_EDID_PRESET_DELL_D2721H);
    assert(jetkvm_settings_set_video_edid_preset(
               (jetkvm_video_edid_preset_t)2) == ESP_ERR_INVALID_ARG);

    bool host_display_idle = true;
    assert(jetkvm_settings_get_host_display_idle(&host_display_idle) == ESP_OK);
    assert(!host_display_idle);
    commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_host_display_idle(true) == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before + 1U);
    assert(jetkvm_settings_get_host_display_idle(&host_display_idle) == ESP_OK);
    assert(host_display_idle);

    int32_t video_sleep_after = 0;
    assert(jetkvm_settings_get_video_sleep_after(&video_sleep_after) == ESP_OK);
    assert(video_sleep_after == JETKVM_DEFAULT_VIDEO_SLEEP_AFTER_SECONDS);
    commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_video_sleep_after(90) == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before + 1U);
    assert(jetkvm_settings_get_video_sleep_after(&video_sleep_after) == ESP_OK);
    assert(video_sleep_after == 90);
    commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_video_sleep_after(90) == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before);
    mock_nvs_fail_next_commit();
    assert(jetkvm_settings_set_video_sleep_after(-1) == ESP_FAIL);
    assert(jetkvm_settings_get_video_sleep_after(&video_sleep_after) == ESP_OK);
    assert(video_sleep_after == 90);
    assert(jetkvm_settings_set_video_sleep_after(-2) == ESP_ERR_INVALID_ARG);
    assert(jetkvm_settings_get_video_sleep_after(&video_sleep_after) == ESP_OK);
    assert(video_sleep_after == 90);
    commits_before = mock_nvs_commit_count();
    assert(jetkvm_settings_set_host_display_idle(true) == ESP_OK);
    assert(mock_nvs_commit_count() == commits_before);
    mock_nvs_fail_next_commit();
    assert(jetkvm_settings_set_host_display_idle(false) == ESP_FAIL);
    assert(jetkvm_settings_get_host_display_idle(&host_display_idle) == ESP_OK);
    assert(host_display_idle);

    mock_nvs_fail_next_commit();
    assert(jetkvm_settings_set_keyboard_layout("it-IT") == ESP_FAIL);
    assert(jetkvm_settings_get_keyboard_layout(
               layout, sizeof(layout)) == ESP_OK);
    assert(strcmp(layout, "pl-PL") == 0);
    assert(jetkvm_settings_get_status(&status) == ESP_OK);
    assert(status.persistence_error == ESP_FAIL);

    assert(jetkvm_settings_set_keyboard_layout("bad_layout") ==
           ESP_ERR_INVALID_ARG);
    assert(jetkvm_settings_get_keyboard_layout(
               layout, sizeof(layout)) == ESP_OK);
    assert(strcmp(layout, "pl-PL") == 0);
}

int main(void)
{
    test_core_validation_and_migration();
    test_nvs_records();
    test_runtime_corruption_fallback_and_transaction();
    puts("JetKVM versioned settings + keyboard layout: OK");
    return 0;
}
