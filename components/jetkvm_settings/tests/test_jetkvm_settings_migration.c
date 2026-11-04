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
    uint32_t crc32;
} test_record_t;

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

int main(void)
{
    test_record_t legacy = {
        .magic = TEST_RECORD_MAGIC,
        .schema_version = 1U,
        .keyboard_layout = JETKVM_LEGACY_KEYBOARD_LAYOUT,
    };
    legacy.crc32 = crc32(&legacy, offsetof(test_record_t, crc32));
    mock_nvs_reset();
    mock_nvs_replace_committed(&legacy, sizeof(legacy));

    assert(jetkvm_settings_init() == ESP_OK);
    assert(jetkvm_settings_is_initialized());
    assert(mock_nvs_commit_count() == 1U);

    char layout[JETKVM_KEYBOARD_LAYOUT_CAPACITY];
    assert(jetkvm_settings_get_keyboard_layout(
               layout, sizeof(layout)) == ESP_OK);
    assert(strcmp(layout, JETKVM_DEFAULT_KEYBOARD_LAYOUT) == 0);

    jetkvm_settings_status_t status;
    assert(jetkvm_settings_get_status(&status) == ESP_OK);
    assert(status.settings_migrated);
    assert(!status.recovered_defaults);
    assert(status.load_error == ESP_OK);
    assert(status.persistence_error == ESP_OK);

    jetkvm_settings_t persisted;
    bool migrated = true;
    assert(jetkvm_settings_load(&persisted, &migrated) == ESP_OK);
    assert(!migrated);
    assert(strcmp(persisted.keyboard_layout,
                  JETKVM_DEFAULT_KEYBOARD_LAYOUT) == 0);
    assert(persisted.video_quality_permille ==
           JETKVM_DEFAULT_VIDEO_QUALITY_PERMILLE);
    assert(strcmp(persisted.video_codec_preference,
                  JETKVM_DEFAULT_VIDEO_CODEC_PREFERENCE) == 0);
    assert(persisted.video_edid_preset ==
           JETKVM_DEFAULT_VIDEO_EDID_PRESET);
    assert(persisted.host_display_idle_enabled == 0U);
    assert(persisted.video_sleep_after_seconds ==
           JETKVM_DEFAULT_VIDEO_SLEEP_AFTER_SECONDS);

    puts("JetKVM legacy en_US migration commit: OK");
    return 0;
}
