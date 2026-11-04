#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_hid_profiles.h"
#include "mock_nvs.h"

static jetkvm_control_keyboard_macro_list_t sample_macros(void)
{
    jetkvm_control_keyboard_macro_list_t macros = {.count = 1U};
    jetkvm_control_keyboard_macro_t *macro = &macros.macros[0];
    strcpy(macro->id, "e2e-f24");
    strcpy(macro->name, "E2E F24");
    macro->sort_order = 0;
    macro->step_count = 2U;
    macro->steps[0].key_count = 1U;
    macro->steps[0].key_ids[0] = 71U; /* F24 in the canonical table. */
    macro->steps[0].modifier_count = 1U;
    macro->steps[0].modifier_ids[0] = 2U; /* ShiftLeft. */
    macro->steps[0].delay_ms = 50U;
    macro->steps[1].delay_ms = 2000U;
    return macros;
}

static void test_defaults_and_validation(void)
{
    jetkvm_control_keyboard_macro_list_t macros;
    jetkvm_control_jiggler_config_t config;
    bool enabled = true;
    jetkvm_hid_profiles_defaults(&macros, &enabled, &config);
    assert(macros.count == 0U);
    assert(!enabled);
    assert(config.inactivity_limit_seconds == 60U);
    assert(config.jitter_percentage == 25U);
    assert(strcmp(config.schedule_cron_tab, "0 * * * * *") == 0);
    assert(strcmp(config.timezone, "UTC") == 0);
    assert(jetkvm_hid_profiles_macros_valid(&macros));
    assert(jetkvm_hid_profiles_jiggler_config_valid(&config));

    macros = sample_macros();
    assert(jetkvm_hid_profiles_macros_valid(&macros));
    macros.macros[0].steps[0].key_ids[0] = 230U;
    assert(!jetkvm_hid_profiles_macros_valid(&macros));
    macros = sample_macros();
    macros.macros[0].steps[0].delay_ms = 49U;
    assert(!jetkvm_hid_profiles_macros_valid(&macros));
}

static void test_cron(void)
{
    assert(jetkvm_hid_profiles_cron_valid("*/30 * * * * *"));
    assert(jetkvm_hid_profiles_cron_valid("0 * 9-17 * * 1-5"));
    assert(jetkvm_hid_profiles_cron_valid("0,15,30,45 */2 * * * *"));
    assert(!jetkvm_hid_profiles_cron_valid("* * * * *"));
    assert(!jetkvm_hid_profiles_cron_valid("*/0 * * * * *"));
    assert(!jetkvm_hid_profiles_cron_valid("60 * * * * *"));
    assert(!jetkvm_hid_profiles_cron_valid("0 * * * * Europe/Berlin"));

    struct tm monday_business = {
        .tm_sec = 0,
        .tm_min = 14,
        .tm_hour = 9,
        .tm_mday = 6,
        .tm_mon = 6,
        .tm_wday = 1,
    };
    assert(jetkvm_hid_profiles_cron_matches(
        "0 * 9-17 * * 1-5", &monday_business));
    monday_business.tm_sec = 1;
    assert(!jetkvm_hid_profiles_cron_matches(
        "0 * 9-17 * * 1-5", &monday_business));
    monday_business.tm_sec = 0;
    monday_business.tm_wday = 0;
    assert(!jetkvm_hid_profiles_cron_matches(
        "0 * 9-17 * * 1-5", &monday_business));
}

static void test_nvs_transaction_and_recovery(void)
{
    mock_nvs_reset();
    jetkvm_control_keyboard_macro_list_t macros;
    jetkvm_control_jiggler_config_t config;
    bool enabled = true;
    assert(jetkvm_hid_profiles_load(&macros, &enabled, &config) == ESP_OK);
    assert(macros.count == 0U && !enabled);

    macros = sample_macros();
    config.inactivity_limit_seconds = 2U;
    config.jitter_percentage = 0U;
    strcpy(config.schedule_cron_tab, "*/2 * * * * *");
    assert(jetkvm_hid_profiles_save(&macros, true, &config) == ESP_OK);
    assert(mock_nvs_committed_size() > 0U);
    assert(mock_nvs_committed_size() < 16U * 1024U);

    jetkvm_control_keyboard_macro_list_t loaded;
    jetkvm_control_jiggler_config_t loaded_config;
    bool loaded_enabled = false;
    assert(jetkvm_hid_profiles_load(
               &loaded, &loaded_enabled, &loaded_config) == ESP_OK);
    assert(loaded_enabled);
    assert(loaded.count == 1U);
    assert(strcmp(loaded.macros[0].name, "E2E F24") == 0);
    assert(loaded.macros[0].steps[0].key_ids[0] == 71U);
    assert(strcmp(loaded_config.schedule_cron_tab,
                  "*/2 * * * * *") == 0);

    mock_nvs_fail_next_commit();
    assert(jetkvm_hid_profiles_save(&macros, false, &config) == ESP_FAIL);
    assert(jetkvm_hid_profiles_load(
               &loaded, &loaded_enabled, &loaded_config) == ESP_OK);
    assert(loaded_enabled);

    mock_nvs_corrupt_byte(mock_nvs_committed_size() - 1U, 1U);
    memset(&loaded, 0xa5, sizeof(loaded));
    loaded_enabled = true;
    assert(jetkvm_hid_profiles_load(
               &loaded, &loaded_enabled, &loaded_config) ==
           ESP_ERR_INVALID_CRC);
    assert(loaded.count == 0U && !loaded_enabled);
    assert(strcmp(loaded_config.schedule_cron_tab,
                  JETKVM_CONTROL_JIGGLER_DEFAULT_CRON) == 0);
}

int main(void)
{
    test_defaults_and_validation();
    test_cron();
    test_nvs_transaction_and_recovery();
    puts("JetKVM HID profiles: OK");
    return 0;
}

