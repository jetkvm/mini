#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_usb_config.h"
#include "mock_nvs.h"

#define TEST_RECORD_MAGIC 0x43554b4aU

typedef struct {
    uint32_t magic;
    uint16_t record_version;
    uint16_t record_size;
    uint32_t generation;
    uint32_t config_schema_version;
    uint16_t vendor_id;
    uint16_t product_id;
    char serial_number[JETKVM_USB_STRING_CAPACITY];
    char manufacturer[JETKVM_USB_STRING_CAPACITY];
    char product[JETKVM_USB_STRING_CAPACITY];
    char configuration[JETKVM_USB_STRING_CAPACITY];
    uint8_t keyboard;
    uint8_t absolute_mouse;
    uint8_t relative_mouse;
    uint8_t mass_storage;
    uint8_t serial_console;
    uint8_t audio;
    uint8_t reserved[2];
    uint32_t crc32;
} test_record_t;

_Static_assert(sizeof(test_record_t) == 160U, "test record ABI mismatch");

/* The record that firmware wrote before the configuration string. */
typedef struct {
    uint32_t magic;
    uint16_t record_version;
    uint16_t record_size;
    uint32_t generation;
    uint32_t config_schema_version;
    uint16_t vendor_id;
    uint16_t product_id;
    char serial_number[JETKVM_USB_STRING_CAPACITY];
    char manufacturer[JETKVM_USB_STRING_CAPACITY];
    char product[JETKVM_USB_STRING_CAPACITY];
    uint8_t keyboard;
    uint8_t absolute_mouse;
    uint8_t relative_mouse;
    uint8_t mass_storage;
    uint8_t serial_console;
    uint8_t audio;
    uint8_t reserved[2];
    uint32_t crc32;
} test_record_v1_t;

_Static_assert(sizeof(test_record_v1_t) == 128U,
               "version 1 test record ABI mismatch");

static uint32_t test_crc32(const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t crc = UINT32_MAX;
    for (size_t i = 0U; i < size; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            uint32_t mask = (uint32_t)-(int32_t)(crc & 1U);
            crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static test_record_t committed_record(mock_usb_slot_t slot)
{
    assert(mock_nvs_committed_size(slot) == sizeof(test_record_t));
    test_record_t record;
    memcpy(&record, mock_nvs_committed_data(slot), sizeof(record));
    return record;
}

static void replace_record(mock_usb_slot_t slot, test_record_t *record)
{
    record->crc32 = test_crc32(record, offsetof(test_record_t, crc32));
    mock_nvs_replace_committed(slot, record, sizeof(*record));
}

static jetkvm_usb_config_t make_config(uint16_t product_id,
                                       const char *product)
{
    jetkvm_usb_config_t config;
    jetkvm_usb_config_defaults(&config);
    assert(jetkvm_usb_config_set_identity(
               &config, 0x303aU, product_id, "P4", "JetKVM", product) ==
           ESP_OK);
    return config;
}

static void assert_state_equal(const jetkvm_usb_config_store_state_t *left,
                               const jetkvm_usb_config_store_state_t *right)
{
    assert(left->active_slot == right->active_slot);
    assert(left->generation == right->generation);
    assert(left->defaults_used == right->defaults_used);
    assert(left->degraded == right->degraded);
    assert(left->slot_a_error == right->slot_a_error);
    assert(left->slot_b_error == right->slot_b_error);
}

static void load_empty(jetkvm_usb_config_t *config,
                       jetkvm_usb_config_store_state_t *state)
{
    assert(jetkvm_usb_config_load(config, state) == ESP_OK);
    assert(state->active_slot == JETKVM_USB_CONFIG_SLOT_NONE);
    assert(state->generation == 0U);
    assert(state->defaults_used);
}

static void test_empty_nvs_uses_safe_defaults(void)
{
    mock_nvs_reset();
    jetkvm_usb_config_t config;
    jetkvm_usb_config_store_state_t state;
    load_empty(&config, &state);
    jetkvm_usb_config_t defaults;
    jetkvm_usb_config_defaults(&defaults);
    assert(jetkvm_usb_config_equal(&config, &defaults));
    assert(!state.degraded);
    assert(state.slot_a_error == ESP_ERR_NVS_NOT_FOUND);
    assert(state.slot_b_error == ESP_ERR_NVS_NOT_FOUND);
}

static void test_save_alternates_slots_and_loads_newest(void)
{
    mock_nvs_reset();
    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t state;
    load_empty(&loaded, &state);

    jetkvm_usb_config_t first = make_config(0x4001U, "First");
    assert(jetkvm_usb_config_save(&first, &state) == ESP_OK);
    assert(strcmp(mock_nvs_last_namespace(), "jkvm_usb") == 0);
    assert(strcmp(mock_nvs_last_set_key(), "cfg_a") == 0);
    assert(state.active_slot == JETKVM_USB_CONFIG_SLOT_A);
    assert(state.generation == 1U);
    assert(!state.defaults_used);
    assert(!state.degraded);
    assert(state.slot_a_error == ESP_OK);
    assert(mock_nvs_committed_present(MOCK_USB_SLOT_A));
    assert(!mock_nvs_committed_present(MOCK_USB_SLOT_B));

    jetkvm_usb_config_t second = make_config(0x4002U, "Second");
    assert(jetkvm_usb_config_save(&second, &state) == ESP_OK);
    assert(strcmp(mock_nvs_last_set_key(), "cfg_b") == 0);
    assert(state.active_slot == JETKVM_USB_CONFIG_SLOT_B);
    assert(state.generation == 2U);
    assert(mock_nvs_set_count() == 2U);
    assert(mock_nvs_commit_count() == 2U);

    jetkvm_usb_config_store_state_t reloaded_state;
    assert(jetkvm_usb_config_load(&loaded, &reloaded_state) == ESP_OK);
    assert(jetkvm_usb_config_equal(&loaded, &second));
    assert(reloaded_state.active_slot == JETKVM_USB_CONFIG_SLOT_B);
    assert(reloaded_state.generation == 2U);
    assert(!reloaded_state.defaults_used);
    assert(!reloaded_state.degraded);
}

static void seed_two(jetkvm_usb_config_t *first,
                     jetkvm_usb_config_t *second)
{
    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t state;
    load_empty(&loaded, &state);
    *first = make_config(0x4101U, "Older");
    *second = make_config(0x4102U, "Newer");
    assert(jetkvm_usb_config_save(first, &state) == ESP_OK);
    assert(jetkvm_usb_config_save(second, &state) == ESP_OK);
}

static void test_single_corrupt_slot_falls_back(void)
{
    mock_nvs_reset();
    jetkvm_usb_config_t older;
    jetkvm_usb_config_t newer;
    seed_two(&older, &newer);
    size_t last = mock_nvs_committed_size(MOCK_USB_SLOT_B) - 1U;
    mock_nvs_corrupt_committed(MOCK_USB_SLOT_B, last, 0x01U);

    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t state;
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(jetkvm_usb_config_equal(&loaded, &older));
    assert(state.active_slot == JETKVM_USB_CONFIG_SLOT_A);
    assert(state.generation == 1U);
    assert(state.degraded);
    assert(state.slot_a_error == ESP_OK);
    assert(state.slot_b_error == ESP_ERR_INVALID_CRC);

    mock_nvs_reset();
    seed_two(&older, &newer);
    last = mock_nvs_committed_size(MOCK_USB_SLOT_A) - 1U;
    mock_nvs_corrupt_committed(MOCK_USB_SLOT_A, last, 0x80U);
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(jetkvm_usb_config_equal(&loaded, &newer));
    assert(state.active_slot == JETKVM_USB_CONFIG_SLOT_B);
    assert(state.degraded);
    assert(state.slot_a_error == ESP_ERR_INVALID_CRC);
    assert(state.slot_b_error == ESP_OK);
}

static void test_double_corruption_uses_safe_defaults(void)
{
    mock_nvs_reset();
    jetkvm_usb_config_t older;
    jetkvm_usb_config_t newer;
    seed_two(&older, &newer);
    mock_nvs_corrupt_committed(MOCK_USB_SLOT_A, 0U, 0x01U);
    mock_nvs_corrupt_committed(MOCK_USB_SLOT_B, 0U, 0x01U);

    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t state;
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    jetkvm_usb_config_t defaults;
    jetkvm_usb_config_defaults(&defaults);
    assert(jetkvm_usb_config_equal(&loaded, &defaults));
    assert(state.active_slot == JETKVM_USB_CONFIG_SLOT_NONE);
    assert(state.generation == 0U);
    assert(state.defaults_used);
    assert(state.degraded);
    assert(state.slot_a_error == ESP_ERR_INVALID_STATE);
    assert(state.slot_b_error == ESP_ERR_INVALID_STATE);
}

static void test_unknown_version_and_interrupted_write_fallback(void)
{
    mock_nvs_reset();
    jetkvm_usb_config_t older;
    jetkvm_usb_config_t newer;
    seed_two(&older, &newer);
    test_record_t record = committed_record(MOCK_USB_SLOT_B);
    record.record_version = JETKVM_USB_CONFIG_RECORD_VERSION + 1U;
    replace_record(MOCK_USB_SLOT_B, &record);

    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t state;
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(jetkvm_usb_config_equal(&loaded, &older));
    assert(state.slot_b_error == ESP_ERR_INVALID_VERSION);
    assert(state.degraded);

    uint8_t partial[17] = {0};
    mock_nvs_replace_committed(MOCK_USB_SLOT_B, partial, sizeof(partial));
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(jetkvm_usb_config_equal(&loaded, &older));
    assert(state.slot_b_error == ESP_ERR_INVALID_SIZE);
    assert(state.degraded);
}

static void test_crc_valid_invalid_strings_and_flags_are_rejected(void)
{
    mock_nvs_reset();
    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t state;
    load_empty(&loaded, &state);
    jetkvm_usb_config_t valid = make_config(0x4201U, "Valid");
    assert(jetkvm_usb_config_save(&valid, &state) == ESP_OK);

    test_record_t invalid = committed_record(MOCK_USB_SLOT_A);
    memset(invalid.product, 'Q', sizeof(invalid.product));
    replace_record(MOCK_USB_SLOT_A, &invalid);
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(state.defaults_used);
    assert(state.slot_a_error == ESP_ERR_INVALID_STATE);

    invalid = committed_record(MOCK_USB_SLOT_A);
    memset(invalid.product, 0, sizeof(invalid.product));
    memcpy(invalid.product, "Valid", sizeof("Valid"));
    invalid.keyboard = 2U;
    replace_record(MOCK_USB_SLOT_A, &invalid);
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(state.defaults_used);
    assert(state.slot_a_error == ESP_ERR_INVALID_STATE);

    invalid = committed_record(MOCK_USB_SLOT_A);
    invalid.keyboard = 1U;
    invalid.mass_storage = 2U;
    replace_record(MOCK_USB_SLOT_A, &invalid);
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(state.defaults_used);
    assert(state.slot_a_error == ESP_ERR_INVALID_STATE);

    invalid = committed_record(MOCK_USB_SLOT_A);
    invalid.mass_storage = 0U;
    invalid.keyboard = 0U;
    invalid.absolute_mouse = 0U;
    invalid.relative_mouse = 0U;
    invalid.audio = 0U;
    replace_record(MOCK_USB_SLOT_A, &invalid);
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(state.defaults_used);
    assert(state.slot_a_error == ESP_ERR_INVALID_STATE);
}

static void establish_old_record(jetkvm_usb_config_t *old_config,
                                 jetkvm_usb_config_store_state_t *state)
{
    jetkvm_usb_config_t ignored;
    load_empty(&ignored, state);
    *old_config = make_config(0x4301U, "Committed");
    assert(jetkvm_usb_config_save(old_config, state) == ESP_OK);
}

static void assert_old_record_still_loads(const jetkvm_usb_config_t *old_config)
{
    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t loaded_state;
    assert(jetkvm_usb_config_load(&loaded, &loaded_state) == ESP_OK);
    assert(jetkvm_usb_config_equal(&loaded, old_config));
    assert(loaded_state.active_slot == JETKVM_USB_CONFIG_SLOT_A);
    assert(loaded_state.generation == 1U);
}

static void test_write_and_commit_failure_preserve_metadata(void)
{
    mock_nvs_reset();
    jetkvm_usb_config_t old_config;
    jetkvm_usb_config_store_state_t state;
    establish_old_record(&old_config, &state);
    jetkvm_usb_config_t candidate = make_config(0x4302U, "Candidate");

    jetkvm_usb_config_store_state_t before = state;
    mock_nvs_fail_next_set();
    assert(jetkvm_usb_config_save(&candidate, &state) == ESP_FAIL);
    assert_state_equal(&state, &before);
    assert(!mock_nvs_committed_present(MOCK_USB_SLOT_B));
    assert_old_record_still_loads(&old_config);

    before = state;
    mock_nvs_fail_next_commit();
    assert(jetkvm_usb_config_save(&candidate, &state) == ESP_FAIL);
    assert_state_equal(&state, &before);
    assert(!mock_nvs_committed_present(MOCK_USB_SLOT_B));
    assert_old_record_still_loads(&old_config);
}

static void test_readback_verification_precedes_metadata_update(void)
{
    mock_nvs_reset();
    jetkvm_usb_config_t old_config;
    jetkvm_usb_config_store_state_t state;
    establish_old_record(&old_config, &state);
    jetkvm_usb_config_t candidate = make_config(0x4402U, "Candidate");

    jetkvm_usb_config_store_state_t before = state;
    mock_nvs_corrupt_after_next_commit(sizeof(test_record_t) - 1U, 0x01U);
    assert(jetkvm_usb_config_save(&candidate, &state) ==
           ESP_ERR_INVALID_CRC);
    assert_state_equal(&state, &before);
    assert_old_record_still_loads(&old_config);

    mock_nvs_remove_committed(MOCK_USB_SLOT_B);
    before = state;
    mock_nvs_fail_readback_after_commit();
    assert(jetkvm_usb_config_save(&candidate, &state) == ESP_FAIL);
    assert_state_equal(&state, &before);

    /* Commit succeeded: a later clean load legitimately observes the new slot. */
    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t loaded_state;
    assert(jetkvm_usb_config_load(&loaded, &loaded_state) == ESP_OK);
    assert(jetkvm_usb_config_equal(&loaded, &candidate));
    assert(loaded_state.active_slot == JETKVM_USB_CONFIG_SLOT_B);
    assert(loaded_state.generation == 2U);
}

static void test_generation_wrap_selects_wrapped_record(void)
{
    mock_nvs_reset();
    jetkvm_usb_config_t old_config;
    jetkvm_usb_config_store_state_t state;
    establish_old_record(&old_config, &state);

    test_record_t old_record = committed_record(MOCK_USB_SLOT_A);
    old_record.generation = UINT32_MAX;
    replace_record(MOCK_USB_SLOT_A, &old_record);
    state.active_slot = JETKVM_USB_CONFIG_SLOT_A;
    state.generation = UINT32_MAX;
    state.defaults_used = false;
    state.slot_a_error = ESP_OK;
    state.slot_b_error = ESP_ERR_NVS_NOT_FOUND;

    jetkvm_usb_config_t wrapped = make_config(0x4501U, "Wrapped");
    assert(jetkvm_usb_config_save(&wrapped, &state) == ESP_OK);
    assert(state.active_slot == JETKVM_USB_CONFIG_SLOT_B);
    assert(state.generation == 1U);
    test_record_t new_record = committed_record(MOCK_USB_SLOT_B);
    assert(new_record.generation == 1U);

    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t loaded_state;
    assert(jetkvm_usb_config_load(&loaded, &loaded_state) == ESP_OK);
    assert(jetkvm_usb_config_equal(&loaded, &wrapped));
    assert(loaded_state.active_slot == JETKVM_USB_CONFIG_SLOT_B);
    assert(loaded_state.generation == 1U);
}

static void test_invalid_inputs_and_open_failure_are_safe(void)
{
    mock_nvs_reset();
    jetkvm_usb_config_t config;
    jetkvm_usb_config_store_state_t state;
    load_empty(&config, &state);
    jetkvm_usb_config_store_state_t before = state;
    config.serial_console = true;
    assert(jetkvm_usb_config_save(&config, &state) == ESP_ERR_INVALID_ARG);
    assert_state_equal(&state, &before);
    assert(mock_nvs_set_count() == 0U);

    state.active_slot = JETKVM_USB_CONFIG_SLOT_NONE;
    state.generation = 7U;
    config.serial_console = false;
    assert(jetkvm_usb_config_save(&config, &state) == ESP_ERR_INVALID_ARG);

    memset(&config, 0xa5, sizeof(config));
    memset(&state, 0xa5, sizeof(state));
    mock_nvs_fail_next_open();
    assert(jetkvm_usb_config_load(&config, &state) == ESP_FAIL);
    jetkvm_usb_config_t defaults;
    jetkvm_usb_config_defaults(&defaults);
    assert(jetkvm_usb_config_equal(&config, &defaults));
    assert(state.active_slot == JETKVM_USB_CONFIG_SLOT_NONE);
    assert(state.defaults_used);
}

static void test_load_migrates_legacy_default_identity(void)
{
    mock_nvs_reset();
    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t state;
    load_empty(&loaded, &state);

    /* A record written by firmware that still defaulted to 1d6b:0104. */
    jetkvm_usb_config_t legacy;
    jetkvm_usb_config_defaults(&legacy);
    assert(jetkvm_usb_config_set_identity(
               &legacy, 0x1d6bU, 0x0104U, "SN1", "JetKVM",
               "USB Emulation Device") == ESP_OK);
    legacy.relative_mouse = false;
    assert(jetkvm_usb_config_save(&legacy, &state) == ESP_OK);
    const test_record_t stored = committed_record(MOCK_USB_SLOT_A);
    assert(stored.vendor_id == 0x1d6bU);

    jetkvm_usb_config_store_state_t reloaded_state;
    assert(jetkvm_usb_config_load(&loaded, &reloaded_state) == ESP_OK);
    assert(loaded.vendor_id == 0x399bU);
    assert(loaded.product_id == 0x0001U);
    assert(strcmp(loaded.serial_number, "SN1") == 0);
    assert(!loaded.relative_mouse);
    assert(reloaded_state.active_slot == JETKVM_USB_CONFIG_SLOT_A);
    assert(!reloaded_state.defaults_used);
    /* Loading never writes; the stored record is untouched. */
    assert(mock_nvs_set_count() == 1U);
    const test_record_t after = committed_record(MOCK_USB_SLOT_A);
    assert(memcmp(&stored, &after, sizeof(stored)) == 0);

    /* A custom identity on the old ID survives a reload. */
    jetkvm_usb_config_t custom = legacy;
    assert(jetkvm_usb_config_set_identity(
               &custom, 0x1d6bU, 0x0104U, "SN1", "JetKVM", "Custom") ==
           ESP_OK);
    assert(jetkvm_usb_config_save(&custom, &reloaded_state) == ESP_OK);
    assert(jetkvm_usb_config_load(&loaded, &reloaded_state) == ESP_OK);
    assert(jetkvm_usb_config_equal(&loaded, &custom));
}

static test_record_v1_t make_v1_record(uint32_t generation)
{
    test_record_v1_t record;
    memset(&record, 0, sizeof(record));
    record.magic = TEST_RECORD_MAGIC;
    record.record_version = 1U;
    record.record_size = (uint16_t)sizeof(record);
    record.generation = generation;
    record.config_schema_version = 1U;
    record.vendor_id = 0x046dU;
    record.product_id = 0xc52bU;
    memcpy(record.serial_number, "SN-OLD", sizeof("SN-OLD"));
    memcpy(record.manufacturer, "Logitech", sizeof("Logitech"));
    memcpy(record.product, "Receiver", sizeof("Receiver"));
    record.keyboard = 1U;
    record.absolute_mouse = 1U;
    record.relative_mouse = 0U;
    record.mass_storage = 0U;
    record.serial_console = 0U;
    record.audio = 1U;
    record.crc32 = test_crc32(&record, offsetof(test_record_v1_t, crc32));
    return record;
}

static void test_configuration_string_round_trips(void)
{
    mock_nvs_reset();
    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t state;
    load_empty(&loaded, &state);

    jetkvm_usb_config_t config = make_config(0x4601U, "Configured");
    assert(jetkvm_usb_config_set_configuration_string(
               &config, "RQR24.11_B0036") == ESP_OK);
    assert(jetkvm_usb_config_save(&config, &state) == ESP_OK);
    const test_record_t stored = committed_record(MOCK_USB_SLOT_A);
    assert(stored.record_version == 2U);
    assert(stored.config_schema_version == 2U);
    assert(strcmp(stored.configuration, "RQR24.11_B0036") == 0);

    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(strcmp(loaded.configuration, "RQR24.11_B0036") == 0);
    assert(strcmp(loaded.product, "Configured") == 0);

    /* An empty string is a valid setting (iConfiguration 0). */
    assert(jetkvm_usb_config_set_configuration_string(&config, "") == ESP_OK);
    assert(jetkvm_usb_config_save(&config, &state) == ESP_OK);
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(loaded.configuration[0] == '\0');
    assert(state.active_slot == JETKVM_USB_CONFIG_SLOT_B);
}

static void test_version_1_record_migrates_with_default_configuration(void)
{
    mock_nvs_reset();
    test_record_v1_t v1 = make_v1_record(5U);
    mock_nvs_replace_committed(MOCK_USB_SLOT_A, &v1, sizeof(v1));
    unsigned sets_before = mock_nvs_set_count();

    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t state;
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(state.active_slot == JETKVM_USB_CONFIG_SLOT_A);
    assert(state.generation == 5U);
    assert(!state.defaults_used);
    assert(!state.degraded);
    assert(loaded.schema_version == JETKVM_USB_CONFIG_SCHEMA_VERSION);
    assert(loaded.vendor_id == 0x046dU);
    assert(loaded.product_id == 0xc52bU);
    assert(strcmp(loaded.serial_number, "SN-OLD") == 0);
    assert(strcmp(loaded.manufacturer, "Logitech") == 0);
    assert(strcmp(loaded.product, "Receiver") == 0);
    assert(strcmp(loaded.configuration, "Config 1: HID") == 0);
    assert(loaded.keyboard && loaded.absolute_mouse && loaded.audio);
    assert(!loaded.relative_mouse && !loaded.mass_storage);
    assert(jetkvm_usb_config_validate(&loaded));
    /* Loading never writes; the version 1 record stays as it was. */
    assert(mock_nvs_set_count() == sets_before);
    assert(mock_nvs_committed_size(MOCK_USB_SLOT_A) == sizeof(v1));

    /* The next save writes a version 2 record to the other slot, and that
     * record wins over the version 1 record. */
    assert(jetkvm_usb_config_set_configuration_string(&loaded, "Custom") ==
           ESP_OK);
    assert(jetkvm_usb_config_save(&loaded, &state) == ESP_OK);
    assert(state.active_slot == JETKVM_USB_CONFIG_SLOT_B);
    assert(state.generation == 6U);
    const test_record_t written = committed_record(MOCK_USB_SLOT_B);
    assert(written.record_version == 2U);
    assert(strcmp(written.configuration, "Custom") == 0);

    jetkvm_usb_config_t reloaded;
    jetkvm_usb_config_store_state_t reloaded_state;
    assert(jetkvm_usb_config_load(&reloaded, &reloaded_state) == ESP_OK);
    assert(reloaded_state.active_slot == JETKVM_USB_CONFIG_SLOT_B);
    assert(jetkvm_usb_config_equal(&reloaded, &loaded));
    assert(!reloaded_state.degraded);
}

static void test_version_1_record_is_checked_before_migration(void)
{
    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t state;

    mock_nvs_reset();
    test_record_v1_t v1 = make_v1_record(3U);
    v1.product[0] ^= 0x01;
    mock_nvs_replace_committed(MOCK_USB_SLOT_A, &v1, sizeof(v1));
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(state.defaults_used);
    assert(state.slot_a_error == ESP_ERR_INVALID_CRC);

    /* A 128-byte record must carry version 1; version 2 needs 160 bytes. */
    v1 = make_v1_record(3U);
    v1.record_version = 2U;
    v1.config_schema_version = 2U;
    v1.crc32 = test_crc32(&v1, offsetof(test_record_v1_t, crc32));
    mock_nvs_replace_committed(MOCK_USB_SLOT_A, &v1, sizeof(v1));
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(state.defaults_used);
    assert(state.slot_a_error == ESP_ERR_INVALID_VERSION);

    /* A version 1 record with an invalid flag is rejected after the CRC. */
    v1 = make_v1_record(3U);
    v1.keyboard = 2U;
    v1.crc32 = test_crc32(&v1, offsetof(test_record_v1_t, crc32));
    mock_nvs_replace_committed(MOCK_USB_SLOT_A, &v1, sizeof(v1));
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(state.defaults_used);
    assert(state.slot_a_error == ESP_ERR_INVALID_STATE);

    /* A 160-byte record that says version 1 is rejected too. */
    mock_nvs_reset();
    load_empty(&loaded, &state);
    jetkvm_usb_config_t config = make_config(0x4701U, "Current");
    assert(jetkvm_usb_config_save(&config, &state) == ESP_OK);
    test_record_t record = committed_record(MOCK_USB_SLOT_A);
    record.record_version = 1U;
    record.config_schema_version = 1U;
    replace_record(MOCK_USB_SLOT_A, &record);
    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(state.defaults_used);
    assert(state.slot_a_error == ESP_ERR_INVALID_VERSION);
}

static void test_newer_version_1_record_wins_over_older_version_2(void)
{
    /* Firmware went back to a build without the configuration string and
     * saved once more: its version 1 record is the newest setting. */
    mock_nvs_reset();
    jetkvm_usb_config_t loaded;
    jetkvm_usb_config_store_state_t state;
    load_empty(&loaded, &state);
    jetkvm_usb_config_t config = make_config(0x4801U, "Older v2");
    assert(jetkvm_usb_config_save(&config, &state) == ESP_OK);
    test_record_v1_t v1 = make_v1_record(2U);
    mock_nvs_replace_committed(MOCK_USB_SLOT_B, &v1, sizeof(v1));

    assert(jetkvm_usb_config_load(&loaded, &state) == ESP_OK);
    assert(state.active_slot == JETKVM_USB_CONFIG_SLOT_B);
    assert(strcmp(loaded.product, "Receiver") == 0);
    assert(strcmp(loaded.configuration, "Config 1: HID") == 0);
}

int main(void)
{
    test_empty_nvs_uses_safe_defaults();
    test_save_alternates_slots_and_loads_newest();
    test_single_corrupt_slot_falls_back();
    test_double_corruption_uses_safe_defaults();
    test_unknown_version_and_interrupted_write_fallback();
    test_crc_valid_invalid_strings_and_flags_are_rejected();
    test_write_and_commit_failure_preserve_metadata();
    test_readback_verification_precedes_metadata_update();
    test_generation_wrap_selects_wrapped_record();
    test_invalid_inputs_and_open_failure_are_safe();
    test_load_migrates_legacy_default_identity();
    test_configuration_string_round_trips();
    test_version_1_record_migrates_with_default_configuration();
    test_version_1_record_is_checked_before_migration();
    test_newer_version_1_record_wins_over_older_version_2();
    puts("jetkvm_usb_config NVS tests: PASS");
    return 0;
}
