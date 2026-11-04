#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_usb_config.h"

static void test_defaults_match_safe_p4_contract(void)
{
    jetkvm_usb_config_t config;
    memset(&config, 0xa5, sizeof(config));
    jetkvm_usb_config_defaults(&config);

    assert(config.schema_version == JETKVM_USB_CONFIG_SCHEMA_VERSION);
    assert(config.vendor_id == 0x399bU);
    assert(config.product_id == 0x0001U);
    assert(strcmp(config.serial_number, "") == 0);
    assert(strcmp(config.manufacturer, "JetKVM") == 0);
    assert(strcmp(config.product, "USB Emulation Device") == 0);
    assert(config.keyboard);
    assert(config.absolute_mouse);
    assert(config.relative_mouse);
    assert(config.mass_storage);
    assert(!config.serial_console);
    assert(config.audio);
    assert(jetkvm_usb_config_validate(&config));
}

static void test_identity_validation_and_atomic_assignment(void)
{
    jetkvm_usb_config_t config;
    jetkvm_usb_config_defaults(&config);
    assert(jetkvm_usb_config_set_identity(
               &config, 0x303aU, 0x400cU, "P4 123", "Jet KVM",
               "P4 USB") == ESP_OK);
    assert(config.vendor_id == 0x303aU);
    assert(config.product_id == 0x400cU);
    assert(strcmp(config.serial_number, "P4 123") == 0);
    assert(strcmp(config.manufacturer, "Jet KVM") == 0);
    assert(strcmp(config.product, "P4 USB") == 0);
    assert(jetkvm_usb_config_validate(&config));

    char maximum[JETKVM_USB_STRING_CAPACITY];
    memset(maximum, 'X', sizeof(maximum));
    maximum[sizeof(maximum) - 1U] = '\0';
    assert(jetkvm_usb_config_set_identity(
               &config, 1U, 1U, maximum, maximum, maximum) == ESP_OK);
    assert(strlen(config.product) == 31U);

    jetkvm_usb_config_t before = config;
    char too_long[JETKVM_USB_STRING_CAPACITY + 1U];
    memset(too_long, 'Y', sizeof(too_long));
    too_long[sizeof(too_long) - 1U] = '\0';
    assert(jetkvm_usb_config_set_identity(
               &config, 1U, 1U, too_long, "JetKVM", "Product") ==
           ESP_ERR_INVALID_ARG);
    assert(memcmp(&config, &before, sizeof(config)) == 0);

    const char control[] = {'A', '\x1f', '\0'};
    const char delete_byte[] = {'A', '\x7f', '\0'};
    assert(jetkvm_usb_config_set_identity(
               &config, 1U, 1U, control, "JetKVM", "Product") ==
           ESP_ERR_INVALID_ARG);
    assert(jetkvm_usb_config_set_identity(
               &config, 1U, 1U, "", delete_byte, "Product") ==
           ESP_ERR_INVALID_ARG);
    assert(jetkvm_usb_config_set_identity(
               &config, 0U, 1U, "", "JetKVM", "Product") ==
           ESP_ERR_INVALID_ARG);
    assert(jetkvm_usb_config_set_identity(
               &config, 1U, 0U, "", "JetKVM", "Product") ==
           ESP_ERR_INVALID_ARG);
    assert(memcmp(&config, &before, sizeof(config)) == 0);

    assert(jetkvm_usb_config_set_identity(
               &config, 1U, 1U, "", "", "") == ESP_OK);
    assert(config.serial_number[0] == '\0');
    assert(config.manufacturer[0] == '\0');
    assert(config.product[0] == '\0');
    assert(jetkvm_usb_config_validate(&config));
}

static void test_device_capability_validation_is_fail_closed(void)
{
    jetkvm_usb_config_t config;
    jetkvm_usb_config_defaults(&config);
    assert(jetkvm_usb_config_set_devices(
               &config, false, true, false, false, false, false) == ESP_OK);
    assert(!config.keyboard);
    assert(config.absolute_mouse);
    assert(!config.relative_mouse);
    assert(jetkvm_usb_config_validate(&config));

    jetkvm_usb_config_t before = config;
    assert(jetkvm_usb_config_set_devices(
               &config, true, true, true, true, false, false) == ESP_OK);
    assert(config.mass_storage);
    assert(jetkvm_usb_config_validate(&config));
    config = before;
    assert(jetkvm_usb_config_set_devices(
               &config, true, true, true, false, true, false) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(jetkvm_usb_config_set_devices(
               &config, false, false, false, false, false, true) == ESP_OK);
    assert(config.audio && jetkvm_usb_config_validate(&config));
    config = before;
    assert(jetkvm_usb_config_set_devices(
               &config, false, false, false, false, false, false) ==
           ESP_ERR_NOT_SUPPORTED);
    assert(memcmp(&config, &before, sizeof(config)) == 0);

    config.mass_storage = true;
    assert(jetkvm_usb_config_validate(&config));
    config.mass_storage = false;
    config.serial_console = true;
    assert(!jetkvm_usb_config_validate(&config));
    config.serial_console = false;
    config.audio = true;
    assert(jetkvm_usb_config_validate(&config));
    config.audio = false;
    config.keyboard = false;
    config.absolute_mouse = false;
    config.relative_mouse = false;
    assert(!jetkvm_usb_config_validate(&config));
}

static void test_complete_config_validation(void)
{
    jetkvm_usb_config_t config;
    jetkvm_usb_config_defaults(&config);
    config.schema_version++;
    assert(!jetkvm_usb_config_validate(&config));
    jetkvm_usb_config_defaults(&config);
    config.vendor_id = 0U;
    assert(!jetkvm_usb_config_validate(&config));
    jetkvm_usb_config_defaults(&config);
    memset(config.product, 'Z', sizeof(config.product));
    assert(!jetkvm_usb_config_validate(&config));

    jetkvm_usb_config_defaults(&config);
    jetkvm_usb_config_t equivalent = config;
    memset(equivalent.product + strlen(equivalent.product) + 1U, 0xa5,
           sizeof(equivalent.product) - strlen(equivalent.product) - 1U);
    assert(jetkvm_usb_config_equal(&config, &equivalent));
    equivalent.keyboard = !equivalent.keyboard;
    assert(!jetkvm_usb_config_equal(&config, &equivalent));
}

static void test_generation_serial_arithmetic(void)
{
    assert(jetkvm_usb_config_generation_next(0U) == 1U);
    assert(jetkvm_usb_config_generation_next(1U) == 2U);
    assert(jetkvm_usb_config_generation_next(UINT32_MAX) == 1U);

    assert(jetkvm_usb_config_generation_is_newer(2U, 1U));
    assert(!jetkvm_usb_config_generation_is_newer(1U, 2U));
    assert(jetkvm_usb_config_generation_is_newer(1U, UINT32_MAX));
    assert(!jetkvm_usb_config_generation_is_newer(UINT32_MAX, 1U));
    assert(!jetkvm_usb_config_generation_is_newer(0U, 1U));
    assert(!jetkvm_usb_config_generation_is_newer(1U, 0U));
    assert(!jetkvm_usb_config_generation_is_newer(7U, 7U));
    assert(!jetkvm_usb_config_generation_is_newer(
        UINT32_C(0x80000001), 1U));
    assert(!jetkvm_usb_config_generation_is_newer(
        1U, UINT32_C(0x80000001)));
}

static void test_legacy_default_identity_migrates(void)
{
    jetkvm_usb_config_t config;
    jetkvm_usb_config_defaults(&config);
    assert(jetkvm_usb_config_set_identity(
               &config, 0x1d6bU, 0x0104U, "device-123", "JetKVM",
               "USB Emulation Device") == ESP_OK);
    config.audio = false;
    assert(jetkvm_usb_config_migrate_default_identity(&config));
    assert(config.vendor_id == 0x399bU);
    assert(config.product_id == 0x0001U);
    assert(strcmp(config.serial_number, "device-123") == 0);
    assert(strcmp(config.manufacturer, "JetKVM") == 0);
    assert(strcmp(config.product, "USB Emulation Device") == 0);
    assert(!config.audio);
    /* Already current: nothing to do. */
    assert(!jetkvm_usb_config_migrate_default_identity(&config));
    assert(!jetkvm_usb_config_migrate_default_identity(NULL));
}

static void test_custom_identity_is_not_migrated(void)
{
    static const struct {
        uint16_t vendor_id;
        uint16_t product_id;
        const char *manufacturer;
        const char *product;
    } cases[] = {
        /* The old default ID with strings a user chose. */
        {0x1d6bU, 0x0104U, "Logitech (x64)", "USB Emulation Device"},
        {0x1d6bU, 0x0104U, "JetKVM", "My KVM"},
        {0x1d6bU, 0x0104U, "JetKVM", "USB Emulation Devic"},
        /* Default strings on another ID. */
        {0x1d6bU, 0x0105U, "JetKVM", "USB Emulation Device"},
        {0x046dU, 0xc52bU, "JetKVM", "USB Emulation Device"},
    };
    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        jetkvm_usb_config_t config;
        jetkvm_usb_config_defaults(&config);
        assert(jetkvm_usb_config_set_identity(
                   &config, cases[i].vendor_id, cases[i].product_id, "s",
                   cases[i].manufacturer, cases[i].product) == ESP_OK);
        jetkvm_usb_config_t before = config;
        assert(!jetkvm_usb_config_migrate_default_identity(&config));
        assert(memcmp(&config, &before, sizeof(config)) == 0);
    }
}

int main(void)
{
    test_defaults_match_safe_p4_contract();
    test_identity_validation_and_atomic_assignment();
    test_device_capability_validation_is_fail_closed();
    test_complete_config_validation();
    test_generation_serial_arithmetic();
    test_legacy_default_identity_migrates();
    test_custom_identity_is_not_migrated();
    puts("jetkvm_usb_config core tests: PASS");
    return 0;
}
