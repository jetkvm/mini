#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "jetkvm_control.h"
#include "cJSON.h"
#include "jetkvm_virtual_media_upload_identity.h"

#define TEST_EDID_QUARTER \
    "0000000000000000000000000000000000000000000000000000000000000000"
#define TEST_EDID \
    TEST_EDID_QUARTER TEST_EDID_QUARTER TEST_EDID_QUARTER TEST_EDID_QUARTER

static void expect_rpc(const char *request, const char *expected)
{
    char output[256];
    size_t output_size = 0;
    assert(jetkvm_control_handle_rpc(
        (const uint8_t *)request, strlen(request), output, sizeof(output),
        &output_size) == JETKVM_CONTROL_RESPONSE_READY);
    assert(output_size == strlen(expected));
    assert(strcmp(output, expected) == 0);
}

static void expect_rpc_context(const jetkvm_control_context_t *context,
                               const char *request, const char *expected)
{
    char output[1024];
    size_t output_size = 0;
    assert(jetkvm_control_handle_rpc_with_context(
        context, (const uint8_t *)request, strlen(request), output,
        sizeof(output), &output_size) == JETKVM_CONTROL_RESPONSE_READY);
    if (output_size != strlen(expected) ||
        strcmp(output, expected) != 0) {
        fprintf(stderr, "\nMISMATCH for %s\n  actual:   %.*s\n  expected: %s\n",
                request, (int)output_size, output, expected);
    }
    assert(output_size == strlen(expected));
    assert(strcmp(output, expected) == 0);
}

static jetkvm_quality_change_result_t accept_half_quality(double factor,
                                                          void *ctx)
{
    double *observed = ctx;
    *observed = factor;
    return (factor == 0.5 || factor == 0.0) ? JETKVM_QUALITY_CHANGE_ACCEPTED :
                                             JETKVM_QUALITY_CHANGE_INVALID;
}

typedef struct {
    const char *status;
    unsigned calls;
    bool readable;
} observed_video_log_status_t;

static bool get_video_log_status(char *output, size_t output_capacity,
                                 void *ctx)
{
    observed_video_log_status_t *observed = ctx;
    observed->calls++;
    size_t required = strlen(observed->status) + 1U;
    if (!observed->readable || required > output_capacity) {
        return false;
    }
    memcpy(output, observed->status, required);
    return true;
}

typedef struct {
    char codec[8];
    unsigned calls;
    jetkvm_video_codec_change_result_t result;
} observed_video_codec_t;

static jetkvm_video_codec_change_result_t set_video_codec_preference(
    const char *codec, void *ctx)
{
    observed_video_codec_t *observed = ctx;
    observed->calls++;
    strcpy(observed->codec, codec);
    return observed->result;
}

typedef struct {
    bool paused;
    unsigned calls;
    bool available;
} observed_video_stream_pause_t;

static bool set_video_stream_paused(bool paused, void *ctx)
{
    observed_video_stream_pause_t *observed = ctx;
    observed->calls++;
    observed->paused = paused;
    return observed->available;
}

typedef struct {
    char edid[JETKVM_CONTROL_EDID_HEX_CAPACITY];
    unsigned get_calls;
    unsigned set_calls;
    bool readable;
    jetkvm_edid_change_result_t result;
} observed_edid_t;

static bool get_edid(char *output, size_t output_capacity, void *ctx)
{
    observed_edid_t *observed = ctx;
    observed->get_calls++;
    size_t required = strlen(observed->edid) + 1U;
    if (!observed->readable || output_capacity < required) {
        return false;
    }
    memcpy(output, observed->edid, required);
    return true;
}

static bool get_edid_preset(size_t index, const char **name, char *edid,
                             size_t capacity, void *ctx)
{
    observed_edid_t *observed = ctx;
    *name = NULL;
    if (!observed->readable) return false;
    if (index >= 1U) return true;
    *name = "JetKVM Default";
    assert(capacity > strlen(observed->edid));
    strcpy(edid, observed->edid);
    return true;
}

static jetkvm_edid_change_result_t set_edid(const char *edid, void *ctx)
{
    observed_edid_t *observed = ctx;
    observed->set_calls++;
    if (strlen(edid) >= sizeof(observed->edid)) {
        return JETKVM_EDID_CHANGE_INVALID;
    }
    strcpy(observed->edid, edid);
    return observed->result;
}

typedef struct {
    bool enabled;
    bool readable;
    unsigned get_calls;
    unsigned set_calls;
    jetkvm_host_display_idle_change_result_t result;
} observed_host_display_idle_t;

static bool get_host_display_idle(bool *enabled, void *ctx)
{
    observed_host_display_idle_t *observed = ctx;
    observed->get_calls++;
    if (!observed->readable) {
        return false;
    }
    *enabled = observed->enabled;
    return true;
}

static jetkvm_host_display_idle_change_result_t set_host_display_idle(
    bool enabled, void *ctx)
{
    observed_host_display_idle_t *observed = ctx;
    observed->set_calls++;
    observed->enabled = enabled;
    return observed->result;
}

typedef struct {
    bool enabled;
    int32_t duration;
    bool readable;
    unsigned get_calls;
    unsigned set_calls;
    jetkvm_video_sleep_change_result_t result;
} observed_video_sleep_t;

static bool get_video_sleep(bool *enabled, int32_t *duration, void *ctx)
{
    observed_video_sleep_t *observed = ctx;
    observed->get_calls++;
    if (!observed->readable) {
        return false;
    }
    *enabled = observed->enabled;
    *duration = observed->duration;
    return true;
}

static jetkvm_video_sleep_change_result_t set_video_sleep(
    int32_t duration, void *ctx)
{
    observed_video_sleep_t *observed = ctx;
    observed->set_calls++;
    observed->duration = duration;
    return observed->result;
}

typedef struct {
    jetkvm_control_mqtt_settings_t settings;
    jetkvm_control_mqtt_status_t status;
    jetkvm_control_mqtt_test_result_t test_result;
    jetkvm_mqtt_config_change_result_t set_result;
    bool settings_readable;
    bool status_readable;
    bool test_available;
    unsigned get_settings_calls;
    unsigned set_settings_calls;
    unsigned get_status_calls;
    unsigned test_calls;
} observed_mqtt_t;

static bool get_mqtt_settings(jetkvm_control_mqtt_settings_t *settings,
                              void *ctx)
{
    observed_mqtt_t *observed = ctx;
    observed->get_settings_calls++;
    if (!observed->settings_readable) {
        return false;
    }
    *settings = observed->settings;
    return true;
}

static jetkvm_mqtt_config_change_result_t set_mqtt_settings(
    const jetkvm_control_mqtt_settings_t *settings,
    char *error, size_t error_capacity, void *ctx)
{
    observed_mqtt_t *observed = ctx;
    observed->set_settings_calls++;
    observed->settings = *settings;
    if (observed->set_result != JETKVM_MQTT_CONFIG_CHANGE_ACCEPTED &&
        error != NULL && error_capacity > 0U) {
        snprintf(error, error_capacity, "%s", "mock MQTT settings failure");
    }
    return observed->set_result;
}

static bool get_mqtt_status(jetkvm_control_mqtt_status_t *status, void *ctx)
{
    observed_mqtt_t *observed = ctx;
    observed->get_status_calls++;
    if (!observed->status_readable) {
        return false;
    }
    *status = observed->status;
    return true;
}

static bool test_mqtt_connection(
    const jetkvm_control_mqtt_settings_t *settings,
    jetkvm_control_mqtt_test_result_t *result, void *ctx)
{
    observed_mqtt_t *observed = ctx;
    observed->test_calls++;
    observed->settings = *settings;
    if (!observed->test_available) {
        return false;
    }
    *result = observed->test_result;
    return true;
}

typedef struct {
    jetkvm_control_ota_status_t status;
    bool status_readable;
    bool check_ok;
    bool try_ok;
    bool auto_update;
    bool auto_update_available;
    bool dev_channel;
    bool update_pending;
    bool pending_available;
    char api_url[JETKVM_CONTROL_OTA_API_URL_CAPACITY];
    bool api_url_ok;
    unsigned get_status_calls;
    unsigned check_calls;
    unsigned try_calls;
    unsigned get_auto_calls;
    unsigned set_auto_calls;
    unsigned get_dev_channel_calls;
    unsigned set_dev_channel_calls;
    unsigned is_pending_calls;
    unsigned get_url_calls;
    unsigned set_url_calls;
    char checked_app[JETKVM_CONTROL_OTA_VERSION_CAPACITY];
    char checked_system[JETKVM_CONTROL_OTA_VERSION_CAPACITY];
    bool checked_prerelease;
    char tried_app[JETKVM_CONTROL_OTA_VERSION_CAPACITY];
    char tried_system[JETKVM_CONTROL_OTA_VERSION_CAPACITY];
    bool tried_prerelease;
    bool tried_reset_config;
    bool set_auto_value;
    char set_url_value[JETKVM_CONTROL_OTA_API_URL_CAPACITY];
} observed_ota_t;

static bool get_ota_update_status(jetkvm_control_ota_status_t *status,
                                  void *ctx)
{
    observed_ota_t *observed = ctx;
    observed->get_status_calls++;
    if (!observed->status_readable) {
        return false;
    }
    *status = observed->status;
    return true;
}

static bool check_ota_update(const char *app_constraint,
                             const char *system_constraint,
                             bool include_prerelease,
                             jetkvm_control_ota_status_t *status, void *ctx)
{
    observed_ota_t *observed = ctx;
    observed->check_calls++;
    snprintf(observed->checked_app, sizeof(observed->checked_app), "%s",
             app_constraint);
    snprintf(observed->checked_system, sizeof(observed->checked_system), "%s",
             system_constraint);
    observed->checked_prerelease = include_prerelease;
    if (!observed->check_ok) {
        return false;
    }
    *status = observed->status;
    return true;
}

static bool try_ota_update(const char *app_constraint,
                           const char *system_constraint,
                           bool include_prerelease, bool reset_config,
                           char *error, size_t error_capacity, void *ctx)
{
    observed_ota_t *observed = ctx;
    observed->try_calls++;
    snprintf(observed->tried_app, sizeof(observed->tried_app), "%s",
             app_constraint);
    snprintf(observed->tried_system, sizeof(observed->tried_system), "%s",
             system_constraint);
    observed->tried_prerelease = include_prerelease;
    observed->tried_reset_config = reset_config;
    if (!observed->try_ok && error != NULL && error_capacity > 0U) {
        snprintf(error, error_capacity, "%s", "an update is already in progress");
    }
    return observed->try_ok;
}

static bool get_ota_auto_update(void *ctx)
{
    observed_ota_t *observed = ctx;
    observed->get_auto_calls++;
    return observed->auto_update;
}

static bool set_ota_auto_update(bool enabled, void *ctx)
{
    observed_ota_t *observed = ctx;
    observed->set_auto_calls++;
    observed->set_auto_value = enabled;
    observed->auto_update = enabled;
    return observed->auto_update;
}

static bool get_ota_dev_channel(void *ctx)
{
    observed_ota_t *observed = ctx;
    observed->get_dev_channel_calls++;
    return observed->dev_channel;
}

static bool set_ota_dev_channel(bool enabled, void *ctx)
{
    observed_ota_t *observed = ctx;
    observed->set_dev_channel_calls++;
    observed->dev_channel = enabled;
    return observed->dev_channel;
}

static bool is_ota_update_pending(void *ctx)
{
    observed_ota_t *observed = ctx;
    observed->is_pending_calls++;
    return observed->update_pending;
}

static bool get_ota_update_api_url(char *url, size_t capacity, void *ctx)
{
    observed_ota_t *observed = ctx;
    observed->get_url_calls++;
    if (!observed->api_url_ok ||
        strlen(observed->api_url) >= capacity) {
        return false;
    }
    memcpy(url, observed->api_url, strlen(observed->api_url) + 1U);
    return true;
}

static bool set_ota_update_api_url(const char *url, char *error,
                                   size_t error_capacity, void *ctx)
{
    observed_ota_t *observed = ctx;
    observed->set_url_calls++;
    if (!observed->api_url_ok) {
        if (error != NULL && error_capacity > 0U) {
            snprintf(error, error_capacity, "%s", "invalid update server URL");
        }
        return false;
    }
    snprintf(observed->set_url_value, sizeof(observed->set_url_value), "%s",
             url);
    return true;
}

typedef struct {
    char api_url[128];
    char app_url[128];
} observed_cloud_urls_t;

typedef struct {
    char layout[JETKVM_CONTROL_KEYBOARD_LAYOUT_CAPACITY];
    unsigned get_calls;
    unsigned set_calls;
    bool readable;
} observed_keyboard_layout_t;

typedef struct {
    jetkvm_reboot_request_result_t result;
    unsigned calls;
    bool force;
} observed_reboot_t;

static jetkvm_reboot_request_result_t request_reboot(bool force, void *ctx)
{
    observed_reboot_t *observed = ctx;
    observed->calls++;
    observed->force = force;
    return observed->result;
}

static bool get_keyboard_layout(char *output, size_t output_capacity,
                                void *ctx)
{
    observed_keyboard_layout_t *observed = ctx;
    observed->get_calls++;
    size_t required = strlen(observed->layout) + 1U;
    if (!observed->readable || output_capacity < required) {
        return false;
    }
    memcpy(output, observed->layout, required);
    return true;
}

static jetkvm_keyboard_layout_change_result_t set_keyboard_layout(
    const char *layout, void *ctx)
{
    observed_keyboard_layout_t *observed = ctx;
    observed->set_calls++;
    if (strcmp(layout, "invalid!") == 0) {
        return JETKVM_KEYBOARD_LAYOUT_CHANGE_INVALID;
    }
    if (strcmp(layout, "persist-fail") == 0) {
        return JETKVM_KEYBOARD_LAYOUT_CHANGE_FAILED;
    }
    const char *canonical = strcmp(layout, "en_US") == 0 ? "en-US" : layout;
    if (strlen(canonical) >= sizeof(observed->layout)) {
        return JETKVM_KEYBOARD_LAYOUT_CHANGE_INVALID;
    }
    strcpy(observed->layout, canonical);
    return JETKVM_KEYBOARD_LAYOUT_CHANGE_ACCEPTED;
}

typedef struct {
    unsigned calls;
    jetkvm_hidrpc_event_type_t type;
    jetkvm_hidrpc_keyboard_report_t keyboard;
    jetkvm_hidrpc_abs_mouse_report_t abs_mouse;
    jetkvm_hidrpc_keypress_report_t keypress;
    jetkvm_hidrpc_rel_mouse_report_t rel_mouse;
    jetkvm_hidrpc_wheel_report_t wheel;
    bool macro_is_paste;
    uint32_t macro_step_count;
    jetkvm_hidrpc_macro_step_t macro_steps[2];
} observed_hidrpc_t;

static void observe_hidrpc(const jetkvm_hidrpc_event_t *event, void *ctx)
{
    observed_hidrpc_t *observed = ctx;
    observed->calls++;
    observed->type = event->type;
    switch (event->type) {
    case JETKVM_HIDRPC_EVENT_KEYBOARD_REPORT:
        observed->keyboard = event->data.keyboard;
        break;
    case JETKVM_HIDRPC_EVENT_ABS_MOUSE_REPORT:
        observed->abs_mouse = event->data.abs_mouse;
        break;
    case JETKVM_HIDRPC_EVENT_KEYPRESS_REPORT:
        observed->keypress = event->data.keypress;
        break;
    case JETKVM_HIDRPC_EVENT_REL_MOUSE_REPORT:
        observed->rel_mouse = event->data.rel_mouse;
        break;
    case JETKVM_HIDRPC_EVENT_WHEEL_REPORT:
        observed->wheel = event->data.wheel;
        break;
    case JETKVM_HIDRPC_EVENT_KEYBOARD_MACRO_REPORT:
        observed->macro_is_paste = event->data.macro.is_paste;
        observed->macro_step_count = event->data.macro.step_count;
        for (uint32_t i = 0; i < event->data.macro.step_count && i < 2U;
             ++i) {
            assert(jetkvm_hidrpc_macro_step_at(
                &event->data.macro, i, &observed->macro_steps[i]));
        }
        assert(!jetkvm_hidrpc_macro_step_at(
            &event->data.macro, event->data.macro.step_count,
            &observed->macro_steps[0]));
        break;
    case JETKVM_HIDRPC_EVENT_CANCEL_KEYBOARD_MACRO_REPORT:
    case JETKVM_HIDRPC_EVENT_KEYPRESS_KEEPALIVE_REPORT:
        break;
    }
}

typedef struct {
    jetkvm_keyboard_led_state_t leds;
    jetkvm_hidrpc_keyboard_report_t keys_down;
    jetkvm_usb_state_t usb_state;
    jetkvm_usb_wake_result_t wake_result;
    unsigned wake_calls;
} observed_hid_state_t;

typedef struct {
    uint8_t mac[6];
    char broadcast[16];
    unsigned calls;
    bool result;
    jetkvm_control_wol_device_list_t devices;
    unsigned get_devices_calls;
    unsigned set_devices_calls;
    bool devices_result;
} observed_wol_t;

typedef struct {
    jetkvm_control_keyboard_macro_list_t macros;
    bool jiggler_enabled;
    jetkvm_control_jiggler_config_t jiggler_config;
    bool result;
    jetkvm_jiggler_config_change_result_t config_change_result;
    unsigned get_macros_calls;
    unsigned set_macros_calls;
    unsigned get_jiggler_state_calls;
    unsigned set_jiggler_state_calls;
    unsigned get_jiggler_config_calls;
    unsigned set_jiggler_config_calls;
} observed_hid_profiles_t;

typedef struct {
    jetkvm_usb_configuration_snapshot_t snapshot;
    unsigned calls;
    bool readable;
    jetkvm_usb_configuration_change_result_t descriptor_change_result;
    jetkvm_usb_configuration_change_result_t devices_change_result;
    jetkvm_usb_configuration_change_result_t device_change_result;
    unsigned descriptor_change_calls;
    unsigned devices_change_calls;
    unsigned device_change_calls;
    jetkvm_usb_descriptor_config_t last_descriptor_config;
    jetkvm_usb_devices_t last_devices;
    jetkvm_usb_device_t last_device;
    bool last_device_enabled;
    jetkvm_usb_emulation_change_result_t change_result;
    unsigned change_calls;
    bool last_enabled;
} observed_usb_configuration_t;

static bool get_keyboard_led_state(jetkvm_keyboard_led_state_t *state,
                                   void *ctx)
{
    observed_hid_state_t *observed = ctx;
    *state = observed->leds;
    return true;
}

static bool get_key_down_state(jetkvm_hidrpc_keyboard_report_t *state,
                               void *ctx)
{
    observed_hid_state_t *observed = ctx;
    *state = observed->keys_down;
    return true;
}

static bool get_usb_state(jetkvm_usb_state_t *state, void *ctx)
{
    observed_hid_state_t *observed = ctx;
    *state = observed->usb_state;
    return true;
}

static bool get_usb_configuration(
    jetkvm_usb_configuration_snapshot_t *snapshot, void *ctx)
{
    observed_usb_configuration_t *observed = ctx;
    observed->calls++;
    if (!observed->readable) {
        return false;
    }
    *snapshot = observed->snapshot;
    return true;
}

static jetkvm_usb_emulation_change_result_t set_usb_emulation_enabled(
    bool enabled, void *ctx)
{
    observed_usb_configuration_t *observed = ctx;
    observed->change_calls++;
    observed->last_enabled = enabled;
    return observed->change_result;
}

static jetkvm_usb_configuration_change_result_t set_usb_descriptor_config(
    const jetkvm_usb_descriptor_config_t *config, void *ctx)
{
    observed_usb_configuration_t *observed = ctx;
    observed->descriptor_change_calls++;
    observed->last_descriptor_config = *config;
    return observed->descriptor_change_result;
}

static jetkvm_usb_configuration_change_result_t set_usb_devices(
    const jetkvm_usb_devices_t *devices, void *ctx)
{
    observed_usb_configuration_t *observed = ctx;
    observed->devices_change_calls++;
    observed->last_devices = *devices;
    return observed->devices_change_result;
}

static jetkvm_usb_configuration_change_result_t set_usb_device_state(
    jetkvm_usb_device_t device, bool enabled, void *ctx)
{
    observed_usb_configuration_t *observed = ctx;
    observed->device_change_calls++;
    observed->last_device = device;
    observed->last_device_enabled = enabled;
    return observed->device_change_result;
}

static jetkvm_usb_wake_result_t wake_host(void *ctx)
{
    observed_hid_state_t *observed = ctx;
    observed->wake_calls++;
    return observed->wake_result;
}

static bool send_wol(const uint8_t mac[6], const char *broadcast_ip,
                     void *ctx)
{
    observed_wol_t *observed = ctx;
    observed->calls++;
    memcpy(observed->mac, mac, sizeof(observed->mac));
    snprintf(observed->broadcast, sizeof(observed->broadcast), "%s",
             broadcast_ip);
    return observed->result;
}

static bool get_wol_devices(jetkvm_control_wol_device_list_t *list,
                            void *ctx)
{
    observed_wol_t *observed = ctx;
    observed->get_devices_calls++;
    if (!observed->devices_result) {
        return false;
    }
    *list = observed->devices;
    return true;
}

static bool set_wol_devices(const jetkvm_control_wol_device_list_t *list,
                            void *ctx)
{
    observed_wol_t *observed = ctx;
    observed->set_devices_calls++;
    if (!observed->devices_result) {
        return false;
    }
    observed->devices = *list;
    return true;
}

static bool get_keyboard_macros(
    jetkvm_control_keyboard_macro_list_t *list, void *ctx)
{
    observed_hid_profiles_t *observed = ctx;
    observed->get_macros_calls++;
    if (!observed->result) {
        return false;
    }
    *list = observed->macros;
    return true;
}

static bool set_keyboard_macros(
    const jetkvm_control_keyboard_macro_list_t *list, void *ctx)
{
    observed_hid_profiles_t *observed = ctx;
    observed->set_macros_calls++;
    if (!observed->result) {
        return false;
    }
    observed->macros = *list;
    return true;
}

static bool get_jiggler_state(bool *enabled, void *ctx)
{
    observed_hid_profiles_t *observed = ctx;
    observed->get_jiggler_state_calls++;
    if (!observed->result) {
        return false;
    }
    *enabled = observed->jiggler_enabled;
    return true;
}

static bool set_jiggler_state(bool enabled, void *ctx)
{
    observed_hid_profiles_t *observed = ctx;
    observed->set_jiggler_state_calls++;
    if (!observed->result) {
        return false;
    }
    observed->jiggler_enabled = enabled;
    return true;
}

static bool get_jiggler_config(
    jetkvm_control_jiggler_config_t *config, void *ctx)
{
    observed_hid_profiles_t *observed = ctx;
    observed->get_jiggler_config_calls++;
    if (!observed->result) {
        return false;
    }
    *config = observed->jiggler_config;
    return true;
}

static jetkvm_jiggler_config_change_result_t set_jiggler_config(
    const jetkvm_control_jiggler_config_t *config, void *ctx)
{
    observed_hid_profiles_t *observed = ctx;
    observed->set_jiggler_config_calls++;
    if (observed->config_change_result ==
        JETKVM_JIGGLER_CONFIG_CHANGE_ACCEPTED) {
        observed->jiggler_config = *config;
    }
    return observed->config_change_result;
}

static void expect_hidrpc_event(const uint8_t *input, size_t input_size,
                                observed_hidrpc_t *observed)
{
    uint8_t output[2] = { 0 };
    size_t output_size = 99U;
    jetkvm_hidrpc_context_t context = {
        .on_event = observe_hidrpc,
        .ctx = observed,
    };
    unsigned calls_before = observed->calls;
    assert(jetkvm_control_handle_hidrpc_with_context(
        &context, input, input_size, output, sizeof(output), &output_size) ==
        JETKVM_CONTROL_EVENT_HANDLED);
    assert(output_size == 0U);
    assert(observed->calls == calls_before + 1U);
}

static void expect_malformed_hidrpc(const uint8_t *input, size_t input_size)
{
    uint8_t output[2] = { 0 };
    size_t output_size = 99U;
    observed_hidrpc_t observed = { 0 };
    jetkvm_hidrpc_context_t context = {
        .on_event = observe_hidrpc,
        .ctx = &observed,
    };
    assert(jetkvm_control_handle_hidrpc_with_context(
        &context, input, input_size, output, sizeof(output), &output_size) ==
        JETKVM_CONTROL_MALFORMED_INPUT);
    assert(output_size == 0U);
    assert(observed.calls == 0U);
}

static void test_hidrpc(void)
{
    uint8_t output[2] = { 0 };
    size_t output_size = 0;
    const uint8_t handshake[] = { 0x01, 0x01 };
    assert(jetkvm_control_handle_hidrpc(
        handshake, sizeof(handshake), output, sizeof(output), &output_size) ==
        JETKVM_CONTROL_RESPONSE_READY);
    assert(output_size == sizeof(handshake));
    assert(memcmp(output, handshake, sizeof(handshake)) == 0);

    output_size = 99U;
    assert(jetkvm_control_handle_hidrpc(
        handshake, sizeof(handshake), output, 1U, &output_size) ==
        JETKVM_CONTROL_OUTPUT_TOO_SMALL);
    assert(output_size == 0U);

    observed_hidrpc_t observed = { 0 };

    const uint8_t keyboard[] = {
        0x02, 0xa5, 0x04, 0x05, 0x06, 0x00, 0x00, 0x00,
    };
    expect_hidrpc_event(keyboard, sizeof(keyboard), &observed);
    assert(observed.type == JETKVM_HIDRPC_EVENT_KEYBOARD_REPORT);
    assert(observed.keyboard.modifier == 0xa5);
    assert(memcmp(observed.keyboard.keys, keyboard + 2U,
                  JETKVM_HIDRPC_KEY_COUNT) == 0);

    const uint8_t abs_mouse[] = {
        0x03,
        0x80, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x7f, 0xff,
        0x05,
    };
    expect_hidrpc_event(abs_mouse, sizeof(abs_mouse), &observed);
    assert(observed.type == JETKVM_HIDRPC_EVENT_ABS_MOUSE_REPORT);
    assert(observed.abs_mouse.x == INT32_MIN);
    assert(observed.abs_mouse.y == 32767);
    assert(observed.abs_mouse.buttons == 0x05);

    const uint8_t keypress[] = { 0x05, 0xe1, 0x01 };
    expect_hidrpc_event(keypress, sizeof(keypress), &observed);
    assert(observed.type == JETKVM_HIDRPC_EVENT_KEYPRESS_REPORT);
    assert(observed.keypress.key == 0xe1);
    assert(observed.keypress.pressed);

    const uint8_t rel_mouse[] = { 0x06, 0x80, 0x7f, 0x03 };
    expect_hidrpc_event(rel_mouse, sizeof(rel_mouse), &observed);
    assert(observed.type == JETKVM_HIDRPC_EVENT_REL_MOUSE_REPORT);
    assert(observed.rel_mouse.dx == INT8_MIN);
    assert(observed.rel_mouse.dy == INT8_MAX);
    assert(observed.rel_mouse.buttons == 0x03);

    const uint8_t macro[] = {
        0x07, 0x01, 0x00, 0x00, 0x00, 0x02,
        0x02, 0x04, 0x05, 0x00, 0x00, 0x00, 0x00, 0x01, 0x2c,
        0x00, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
    };
    expect_hidrpc_event(macro, sizeof(macro), &observed);
    assert(observed.type == JETKVM_HIDRPC_EVENT_KEYBOARD_MACRO_REPORT);
    assert(observed.macro_is_paste);
    assert(observed.macro_step_count == 2U);
    assert(observed.macro_steps[0].modifier == 0x02);
    assert(memcmp(observed.macro_steps[0].keys, macro + 7U,
                  JETKVM_HIDRPC_KEY_COUNT) == 0);
    assert(observed.macro_steps[0].delay_ms == 300U);
    assert(observed.macro_steps[1].modifier == 0x00);
    assert(observed.macro_steps[1].keys[0] == 0x06);
    assert(observed.macro_steps[1].delay_ms == UINT16_MAX);

    const uint8_t empty_macro[] = {
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    expect_hidrpc_event(empty_macro, sizeof(empty_macro), &observed);
    assert(observed.macro_step_count == 0U);
    assert(!observed.macro_is_paste);

    const uint8_t cancel_macro[] = { 0x08 };
    expect_hidrpc_event(cancel_macro, sizeof(cancel_macro), &observed);
    assert(observed.type ==
           JETKVM_HIDRPC_EVENT_CANCEL_KEYBOARD_MACRO_REPORT);

    const uint8_t keepalive[] = { 0x09 };
    expect_hidrpc_event(keepalive, sizeof(keepalive), &observed);
    assert(observed.type == JETKVM_HIDRPC_EVENT_KEYPRESS_KEEPALIVE_REPORT);

    const uint8_t unknown[] = { 0xff, 0x01 };
    jetkvm_hidrpc_context_t context = {
        .on_event = observe_hidrpc,
        .ctx = &observed,
    };
    unsigned calls_before = observed.calls;
    output_size = 99U;
    assert(jetkvm_control_handle_hidrpc_with_context(
        &context, unknown, sizeof(unknown), output, sizeof(output),
        &output_size) == JETKVM_CONTROL_NO_RESPONSE);
    assert(output_size == 0U);
    assert(observed.calls == calls_before);

    /* The compatibility wrapper parses reports but deliberately has no sink. */
    output_size = 99U;
    assert(jetkvm_control_handle_hidrpc(
        keypress, sizeof(keypress), output, sizeof(output), &output_size) ==
        JETKVM_CONTROL_NO_RESPONSE);
    assert(output_size == 0U);

    const uint8_t empty[] = { 0 };
    expect_malformed_hidrpc(empty, 0U);
    const uint8_t handshake_short[] = { 0x01 };
    const uint8_t handshake_long[] = { 0x01, 0x01, 0x00 };
    expect_malformed_hidrpc(handshake_short, sizeof(handshake_short));
    expect_malformed_hidrpc(handshake_long, sizeof(handshake_long));
    expect_malformed_hidrpc(keyboard, sizeof(keyboard) - 1U);
    const uint8_t keyboard_long[] = {
        0x02, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    expect_malformed_hidrpc(keyboard_long, sizeof(keyboard_long));
    expect_malformed_hidrpc(abs_mouse, sizeof(abs_mouse) - 1U);
    const uint8_t abs_mouse_long[] = {
        0x03, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    expect_malformed_hidrpc(abs_mouse_long, sizeof(abs_mouse_long));
    expect_malformed_hidrpc(keypress, sizeof(keypress) - 1U);
    const uint8_t keypress_long[] = { 0x05, 0x04, 0x01, 0x00 };
    const uint8_t keypress_bad_bool[] = { 0x05, 0x04, 0x02 };
    expect_malformed_hidrpc(keypress_long, sizeof(keypress_long));
    expect_malformed_hidrpc(keypress_bad_bool, sizeof(keypress_bad_bool));
    expect_malformed_hidrpc(rel_mouse, sizeof(rel_mouse) - 1U);
    const uint8_t rel_mouse_long[] = { 0x06, 0, 0, 0, 0 };
    expect_malformed_hidrpc(rel_mouse_long, sizeof(rel_mouse_long));
    const uint8_t macro_short[] = { 0x07, 0, 0, 0, 0 };
    const uint8_t macro_bad_bool[] = { 0x07, 2, 0, 0, 0, 0 };
    const uint8_t macro_missing_step[] = { 0x07, 0, 0, 0, 0, 1 };
    const uint8_t macro_trailing[] = { 0x07, 0, 0, 0, 0, 0, 0 };
    expect_malformed_hidrpc(macro_short, sizeof(macro_short));
    expect_malformed_hidrpc(macro_bad_bool, sizeof(macro_bad_bool));
    expect_malformed_hidrpc(macro_missing_step, sizeof(macro_missing_step));
    expect_malformed_hidrpc(macro_trailing, sizeof(macro_trailing));

    uint8_t max_macro[6U + JETKVM_HIDRPC_MACRO_MAX_STEPS *
                              JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE] = { 0 };
    max_macro[0] = JETKVM_HIDRPC_TYPE_KEYBOARD_MACRO_REPORT;
    max_macro[4] =
        (uint8_t)(JETKVM_HIDRPC_MACRO_MAX_STEPS >> 8);
    max_macro[5] = (uint8_t)JETKVM_HIDRPC_MACRO_MAX_STEPS;
    expect_hidrpc_event(max_macro, sizeof(max_macro), &observed);
    assert(observed.macro_step_count == JETKVM_HIDRPC_MACRO_MAX_STEPS);

    uint8_t oversized_macro[
        6U + (JETKVM_HIDRPC_MACRO_MAX_STEPS + 1U) *
                 JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE] = { 0 };
    oversized_macro[0] = JETKVM_HIDRPC_TYPE_KEYBOARD_MACRO_REPORT;
    uint32_t oversized_count = JETKVM_HIDRPC_MACRO_MAX_STEPS + 1U;
    oversized_macro[2] = (uint8_t)(oversized_count >> 24);
    oversized_macro[3] = (uint8_t)(oversized_count >> 16);
    oversized_macro[4] = (uint8_t)(oversized_count >> 8);
    oversized_macro[5] = (uint8_t)oversized_count;
    expect_malformed_hidrpc(oversized_macro, sizeof(oversized_macro));
    const uint8_t cancel_long[] = { 0x08, 0 };
    const uint8_t keepalive_long[] = { 0x09, 0 };
    expect_malformed_hidrpc(cancel_long, sizeof(cancel_long));
    expect_malformed_hidrpc(keepalive_long, sizeof(keepalive_long));

    assert(!jetkvm_hidrpc_macro_step_at(NULL, 0U, NULL));
    jetkvm_hidrpc_keyboard_macro_report_t oversized_view = {
        .step_count = JETKVM_HIDRPC_MACRO_MAX_STEPS + 1U,
        .encoded_steps = oversized_macro + 6U,
        .encoded_steps_size = sizeof(oversized_macro) - 6U,
    };
    jetkvm_hidrpc_macro_step_t ignored_step;
    assert(!jetkvm_hidrpc_macro_step_at(&oversized_view, 0U,
                                        &ignored_step));
}

static jetkvm_cloud_config_change_result_t accept_cloud_urls(
    const char *api_url, const char *app_url, void *ctx)
{
    observed_cloud_urls_t *observed = ctx;
    if (strncmp(api_url, "https://", 8U) != 0 ||
        strncmp(app_url, "https://", 8U) != 0) {
        return JETKVM_CLOUD_CONFIG_CHANGE_INVALID;
    }
    assert(strlen(api_url) < sizeof(observed->api_url));
    assert(strlen(app_url) < sizeof(observed->app_url));
    strcpy(observed->api_url, api_url);
    strcpy(observed->app_url, app_url);
    return JETKVM_CLOUD_CONFIG_CHANGE_ACCEPTED;
}

typedef struct {
    jetkvm_control_virtual_media_state_t state;
    jetkvm_control_virtual_media_mode_t mounted_mode;
    char action_filename[JETKVM_VIRTUAL_MEDIA_FILENAME_CAPACITY];
    char action_url[JETKVM_VIRTUAL_MEDIA_URL_CAPACITY];
    unsigned mount_calls;
    unsigned http_mount_calls;
    unsigned builtin_mount_calls;
    unsigned check_url_calls;
    unsigned unmount_calls;
    unsigned format_calls;
    jetkvm_control_storage_format_result_t format_result;
    unsigned delete_calls;
    unsigned upload_calls;
    uint64_t upload_size;
    char upload_sha256[65];
} observed_virtual_media_t;

static bool check_mount_url(const char *url,
                            jetkvm_control_virtual_media_url_info_t *info,
                            void *ctx)
{
    observed_virtual_media_t *observed = ctx;
    observed->check_url_calls++;
    strcpy(observed->action_url, url);
    memset(info, 0, sizeof(*info));
    if (strstr(url, "missing") != NULL) {
        strcpy(info->reason, "The URL is not available.");
    } else {
        info->usable = true;
        info->size = 4194304U;
    }
    return true;
}

static jetkvm_virtual_media_change_result_t mount_http_image(
    const char *url, jetkvm_control_virtual_media_mode_t mode, void *ctx)
{
    observed_virtual_media_t *observed = ctx;
    observed->http_mount_calls++;
    observed->mounted_mode = mode;
    strcpy(observed->action_url, url);
    return strstr(url, "missing") ? JETKVM_VIRTUAL_MEDIA_CHANGE_URL_UNAVAILABLE :
                                    JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
}

static bool get_virtual_media_state(
    jetkvm_control_virtual_media_state_t *state, void *ctx)
{
    *state = ((observed_virtual_media_t *)ctx)->state;
    return true;
}

static bool get_storage_space(jetkvm_control_storage_space_t *space, void *ctx)
{
    (void)ctx;
    space->bytes_used = 4096U;
    space->bytes_free = 8192U;
    return true;
}

static bool list_storage_files(jetkvm_visit_storage_file_fn visit,
                               void *visit_ctx, void *ctx)
{
    (void)ctx;
    jetkvm_control_storage_file_t file = {
        .size = 734003200U,
        .created_at = 0,
    };
    strcpy(file.filename, "installer.iso");
    return visit && visit(&file, visit_ctx);
}

static jetkvm_virtual_media_change_result_t mount_storage_file(
    const char *filename, jetkvm_control_virtual_media_mode_t mode, void *ctx)
{
    observed_virtual_media_t *observed = ctx;
    observed->mount_calls++;
    observed->mounted_mode = mode;
    strcpy(observed->action_filename, filename);
    return JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
}

static jetkvm_virtual_media_change_result_t mount_builtin_installer(void *ctx)
{
    observed_virtual_media_t *observed = ctx;
    observed->builtin_mount_calls++;
    return observed->state.mounted ?
        JETKVM_VIRTUAL_MEDIA_CHANGE_ALREADY_MOUNTED :
        JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
}

static jetkvm_virtual_media_change_result_t unmount_virtual_media(void *ctx)
{
    ((observed_virtual_media_t *)ctx)->unmount_calls++;
    return JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
}

static jetkvm_control_storage_format_result_t format_storage(
    jetkvm_control_storage_space_t *space, void *ctx)
{
    observed_virtual_media_t *observed = ctx;
    observed->format_calls++;
    if (observed->format_result == JETKVM_CONTROL_STORAGE_FORMAT_COMPLETED) {
        space->bytes_used = 196608U;
        space->bytes_free = 31452659712U;
    }
    return observed->format_result;
}

static jetkvm_virtual_media_change_result_t delete_storage_file(
    const char *filename, void *ctx)
{
    observed_virtual_media_t *observed = ctx;
    observed->delete_calls++;
    strcpy(observed->action_filename, filename);
    return strstr(filename, "missing") ?
               JETKVM_VIRTUAL_MEDIA_CHANGE_NOT_FOUND :
               JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
}

static jetkvm_virtual_media_change_result_t start_storage_upload(
    const char *filename, uint64_t size, const char *sha256,
    jetkvm_control_storage_upload_t *upload, void *ctx)
{
    observed_virtual_media_t *observed = ctx;
    observed->upload_calls++;
    observed->upload_size = size;
    strcpy(observed->action_filename, filename);
    strcpy(observed->upload_sha256, sha256);
    upload->already_uploaded_bytes = 65536U;
    strcpy(upload->data_channel, "upload_01234567");
    return JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
}

static bool tls_get(char *mode, size_t mode_capacity, char *certificate, size_t capacity, void *ctx)
{
    (void)ctx;
    assert(mode_capacity > 6 && capacity > 4);
    strcpy(mode, "custom"); strcpy(certificate, "CERT");
    return true;
}
static bool tls_set(const char *mode, const char *certificate, const char *key,
                    char *error, size_t capacity, void *ctx)
{
    unsigned *calls = ctx; ++*calls;
    assert(!strcmp(mode, "custom")); assert(!strcmp(certificate, "CERT")); assert(!strcmp(key, "KEY"));
    snprintf(error, capacity, "Invalid certificate pair");
    return false;
}
static void test_tls_contract(void)
{
    /* No TLS provider (HTTPS not built): the methods do not exist, so the
     * upstream TLS test skips instead of failing. */
    jetkvm_control_context_t none = {0};
    expect_rpc_context(&none, "{\"jsonrpc\":\"2.0\",\"method\":\"getTLSState\",\"params\":{},\"id\":9}",
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
    expect_rpc_context(&none, "{\"jsonrpc\":\"2.0\",\"method\":\"setTLSState\",\"params\":{\"state\":{\"mode\":\"self-signed\"}},\"id\":10}",
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
    /* The deviceCapabilities event carries the getDeviceCapabilities list. */
    char event[256];
    size_t event_length = 0;
    expect_rpc_context(&none, "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceCapabilities\",\"id\":11}",
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"result\":[]}");
    assert(jetkvm_control_render_capabilities_notification(NULL, event, sizeof(event), &event_length));
    assert(strcmp(event, "{\"jsonrpc\":\"2.0\",\"method\":\"deviceCapabilities\",\"params\":[]}") == 0);
    assert(event_length == strlen(event));
    /* A local display that its side buttons drive. */
    jetkvm_control_set_default_local_display_context(&(jetkvm_local_display_context_t){.buttons = true});
    expect_rpc_context(&none, "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceCapabilities\",\"id\":11}",
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"result\":[\"display_buttons\"]}");
    jetkvm_control_set_default_local_display_context(NULL);
    unsigned calls = 0;
    /* A provider that can only read the state is not an HTTPS setting. */
    jetkvm_control_context_t get_only = {.tls = {.get=tls_get, .ctx=&calls}};
    expect_rpc_context(&get_only, "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceCapabilities\",\"id\":7}",
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":[]}");
    jetkvm_control_context_t context = {.tls = {.get=tls_get, .set=tls_set, .ctx=&calls}};
    expect_rpc_context(&context, "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceCapabilities\",\"id\":8}",
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":[\"tls\"]}");
    expect_rpc_context(&context, "{\"jsonrpc\":\"2.0\",\"method\":\"getTLSState\",\"params\":{},\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"mode\":\"custom\",\"certificate\":\"CERT\",\"privateKey\":\"\"}}");
    expect_rpc_context(&context, "{\"jsonrpc\":\"2.0\",\"method\":\"setTLSState\",\"params\":{\"state\":{\"mode\":\"custom\",\"certificate\":\"CERT\",\"privateKey\":\"KEY\"}},\"id\":2}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32603,\"message\":\"TLS configuration failed\",\"data\":\"Invalid certificate pair\"}}");
    expect_rpc_context(&context, "{\"jsonrpc\":\"2.0\",\"method\":\"setTLSState\",\"params\":{\"state\":{\"mode\":\"custom\",\"privateKey\":42}},\"id\":3}",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32602,\"message\":\"Invalid TLS state\"}}");
    assert(calls == 1);
}

static char stored_hostnames[2][JETKVM_EXTRA_HOSTNAME_CAPACITY];
static size_t stored_hostname_count;
static size_t hostnames_get(char (*names)[JETKVM_EXTRA_HOSTNAME_CAPACITY], size_t capacity, void *ctx)
{
    assert(ctx == stored_hostnames && capacity == JETKVM_EXTRA_HOSTNAMES_MAX);
    memcpy(names, stored_hostnames, stored_hostname_count * sizeof(stored_hostnames[0]));
    return stored_hostname_count;
}
static const char *hostnames_set(const char *const *names, size_t count, void *ctx)
{
    assert(ctx == stored_hostnames);
    if (count > 2 || (count && !strcmp(names[count - 1], "bad_name"))) return "Not a host name";
    for (size_t i = 0; i < count; ++i) snprintf(stored_hostnames[i], sizeof(stored_hostnames[i]), "%s", names[i]);
    stored_hostname_count = count;
    return NULL;
}
static void test_extra_hostnames_contract(void)
{
    /* Without a provider the methods do not exist and no capability shows
     * the setting. */
    jetkvm_control_context_t none = {0};
    expect_rpc_context(&none, "{\"jsonrpc\":\"2.0\",\"method\":\"getExtraHostnames\",\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
    jetkvm_control_set_default_extra_hostnames_context(&(jetkvm_extra_hostnames_context_t){
        .get = hostnames_get, .set = hostnames_set, .ctx = stored_hostnames});
    expect_rpc_context(&none, "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceCapabilities\",\"id\":2}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":[\"extra_hostnames\"]}");
    expect_rpc_context(&none, "{\"jsonrpc\":\"2.0\",\"method\":\"getExtraHostnames\",\"id\":3}",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":[]}");
    expect_rpc_context(&none, "{\"jsonrpc\":\"2.0\",\"method\":\"setExtraHostnames\",\"params\":{\"hostnames\":[\"kvm.lab\",\"rack-3\"]},\"id\":4}",
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":[\"kvm.lab\",\"rack-3\"]}");
    expect_rpc_context(&none, "{\"jsonrpc\":\"2.0\",\"method\":\"getExtraHostnames\",\"id\":5}",
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":[\"kvm.lab\",\"rack-3\"]}");
    expect_rpc_context(&none, "{\"jsonrpc\":\"2.0\",\"method\":\"setExtraHostnames\",\"params\":{\"hostnames\":[\"bad_name\"]},\"id\":6}",
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"error\":{\"code\":-32602,\"message\":\"Invalid host names\",\"data\":\"Not a host name\"}}");
    const char *malformed[] = {"{}", "{\"hostnames\":\"kvm\"}", "{\"hostnames\":[\"kvm\",7]}",
        "{\"hostnames\":[\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}"};
    for (size_t i = 0; i < sizeof(malformed) / sizeof(malformed[0]); ++i) {
        char request[256];
        snprintf(request, sizeof(request), "{\"jsonrpc\":\"2.0\",\"method\":\"setExtraHostnames\",\"params\":%s,\"id\":7}", malformed[i]);
        expect_rpc_context(&none, request,
            "{\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{\"code\":-32602,\"message\":\"Invalid params\"}}");
    }
    expect_rpc_context(&none, "{\"jsonrpc\":\"2.0\",\"method\":\"setExtraHostnames\",\"params\":{\"hostnames\":[]},\"id\":8}",
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":[]}");
    jetkvm_control_set_default_extra_hostnames_context(NULL);
}

static unsigned deregister_calls;
static const char *deregister_error;
static const char *deregister_device(void *ctx)
{
    assert(ctx == &deregister_calls);
    ++deregister_calls;
    return deregister_error;
}

static void test_cloud_deregistration(void)
{
    jetkvm_control_context_t context = {
        .deregister_device = deregister_device,
        .deregister_device_ctx = &deregister_calls,
    };
    expect_rpc_context(&context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"deregisterDevice\",\"params\":{},\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}");
    assert(deregister_calls == 1);
    expect_rpc_context(&context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"deregisterDevice\",\"params\":{\"token\":\"wrong\"},\"id\":2}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"method does not accept parameters\"}}");
    assert(deregister_calls == 1);
    deregister_error = "Cloud request failed";
    expect_rpc_context(&context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"deregisterDevice\",\"id\":3}",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Cloud request failed\"}}");
    assert(deregister_calls == 2);
}

static bool audio_value, audio_fail;
static bool audio_get(bool *enabled, void *ctx)
{ (void)ctx; *enabled = audio_value; return !audio_fail; }
static const char *audio_set(bool enabled, void *ctx)
{ (void)ctx; if (audio_fail) return "Save failed"; audio_value = enabled; return NULL; }
static void test_audio_contract(void)
{
    const jetkvm_audio_context_t audio = {.get_enabled = audio_get, .set_enabled = audio_set};
    jetkvm_control_set_default_audio_context(&audio);
    expect_rpc("{\"jsonrpc\":\"2.0\",\"method\":\"setAudioConfig\",\"params\":{\"params\":{\"enabled\":true}},\"id\":8}", "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":null}");
    expect_rpc("{\"jsonrpc\":\"2.0\",\"method\":\"getAudioConfig\",\"id\":8}", "{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{\"enabled\":true}}");
    expect_rpc("{\"jsonrpc\":\"2.0\",\"method\":\"setAudioConfig\",\"params\":{\"enabled\":false},\"id\":8}", "{\"jsonrpc\":\"2.0\",\"id\":8,\"error\":{\"code\":-32602,\"message\":\"Invalid params\"}}");
    expect_rpc("{\"jsonrpc\":\"2.0\",\"method\":\"setAudioConfig\",\"params\":{\"params\":{\"enabled\":1}},\"id\":8}", "{\"jsonrpc\":\"2.0\",\"id\":8,\"error\":{\"code\":-32602,\"message\":\"Invalid params\"}}");
    expect_rpc("{\"jsonrpc\":\"2.0\",\"method\":\"setAudioConfig\",\"params\":{\"params\":{\"enabled\":false,\"x\":0}},\"id\":8}", "{\"jsonrpc\":\"2.0\",\"id\":8,\"error\":{\"code\":-32602,\"message\":\"Invalid params\"}}");
    assert(audio_value); audio_fail = true;
    expect_rpc("{\"jsonrpc\":\"2.0\",\"method\":\"setAudioConfig\",\"params\":{\"params\":{\"enabled\":false}},\"id\":8}", "{\"jsonrpc\":\"2.0\",\"id\":8,\"error\":{\"code\":-32603,\"message\":\"Audio update failed\",\"data\":\"Save failed\"}}");
    assert(audio_value);
    jetkvm_control_set_default_audio_context(NULL);
}


static jetkvm_backlight_settings_t display_test_settings = {35, 0, 0};
static uint16_t display_test_rotation = 270;
static unsigned display_test_writes;
static bool display_test_fail;
static bool display_test_get_bl(jetkvm_backlight_settings_t *s, void *ctx)
{ (void)ctx; *s = display_test_settings; return true; }
static const char *display_test_set_bl(const jetkvm_backlight_settings_t *s, void *ctx)
{ (void)ctx; ++display_test_writes; if (display_test_fail) return "save failed"; display_test_settings = *s; return NULL; }
static bool display_test_get_rotation(uint16_t *r, void *ctx)
{ (void)ctx; *r = display_test_rotation; return true; }
static const char *display_test_set_rotation(uint16_t r, void *ctx)
{ (void)ctx; ++display_test_writes; if (display_test_fail) return "save failed"; display_test_rotation = r; return NULL; }
static void display_test_error(const char *method, const char *params, int code)
{
    char request[512], output[1024]; size_t written;
    snprintf(request, sizeof(request), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}", method, params);
    assert(jetkvm_control_handle_rpc((const uint8_t *)request, strlen(request), output, sizeof(output), &written) == JETKVM_CONTROL_RESPONSE_READY);
    cJSON *root = cJSON_Parse(output);
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    cJSON *actual = cJSON_GetObjectItemCaseSensitive(error, "code");
    assert(cJSON_IsNumber(actual) && actual->valueint == code);
    cJSON_Delete(root);
}
static void test_local_display(void)
{
    jetkvm_local_display_context_t display = {
        .get_backlight = display_test_get_bl, .set_backlight = display_test_set_bl,
        .get_rotation = display_test_get_rotation, .set_rotation = display_test_set_rotation,
    };
    jetkvm_control_set_default_local_display_context(&display);
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getBacklightSettings\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"max_brightness\":35,\"dim_after\":0,\"off_after\":0}}");
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getDisplayRotation\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"rotation\":\"270\"}}");
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setBacklightSettings\",\"params\":{\"params\":{\"max_brightness\":64,\"dim_after\":60,\"off_after\":300}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}");
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setDisplayRotation\",\"params\":{\"params\":{\"rotation\":\"90\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}");
    assert(display_test_writes == 2 && display_test_rotation == 90 && display_test_settings.max_brightness == 64);
    /* A registered display takes all four quarter turns, so the UI offers them. */
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getDeviceCapabilities\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[\"display_quarter_turns\"]}");
    const char *turns[] = {"0", "90", "180", "270"};
    for (size_t i = 0; i < sizeof(turns)/sizeof(turns[0]); ++i) {
        char request[256], response[256];
        snprintf(request, sizeof(request), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setDisplayRotation\",\"params\":{\"params\":{\"rotation\":\"%s\"}}}", turns[i]);
        expect_rpc(request, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}");
        assert(display_test_rotation == 90 * i);
        snprintf(response, sizeof(response), "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"rotation\":\"%s\"}}", turns[i]);
        expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getDisplayRotation\"}", response);
    }
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setDisplayRotation\",\"params\":{\"params\":{\"rotation\":\"90\"}}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}");
    const unsigned writes = display_test_writes;
    const char *bad[] = {
        "{}", "{\"max_brightness\":64,\"dim_after\":0,\"off_after\":0}",
        "{\"params\":{\"max_brightness\":256,\"dim_after\":0,\"off_after\":0}}",
        "{\"params\":{\"max_brightness\":10,\"dim_after\":-1,\"off_after\":0}}",
        "{\"params\":{\"max_brightness\":10,\"dim_after\":0.5,\"off_after\":0}}",
        "{\"params\":{\"max_brightness\":10,\"dim_after\":0,\"off_after\":2147483648}}",
        "{\"params\":{\"max_brightness\":10,\"dim_after\":0,\"off_after\":0,\"extra\":1}}",
    };
    for (size_t i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) display_test_error("setBacklightSettings", bad[i], -32602);
    display_test_error("setDisplayRotation", "{\"params\":{\"rotation\":90}}", -32602);
    display_test_error("setDisplayRotation", "{\"params\":{\"rotation\":\"45\"}}", -32602);
    assert(display_test_writes == writes);
    display_test_fail = true;
    display_test_error("setDisplayRotation", "{\"params\":{\"rotation\":\"180\"}}", -32603);
    assert(display_test_rotation == 90);
    jetkvm_control_set_default_local_display_context(NULL);
}


static unsigned wifi_band_calls;
static jetkvm_wifi_band_t wifi_band_observed;
static const char *observe_wifi_band(jetkvm_wifi_band_t band, void *ctx)
{
    (void)ctx;
    wifi_band_calls++;
    wifi_band_observed = band;
    return NULL;
}
static void test_wifi_band_rpc(void)
{
    jetkvm_control_context_t context = {.set_wifi_band = observe_wifi_band};
    const char *names[] = {"auto", "2.4ghz", "5ghz"};
    for (unsigned i = 0; i < 3; i++) {
        char request[160];
        snprintf(request, sizeof(request),
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setWifiBand\",\"params\":{\"band\":\"%s\"}}", names[i]);
        expect_rpc_context(&context, request, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":true}");
        assert(wifi_band_observed == (jetkvm_wifi_band_t)i);
    }
    expect_rpc_context(&context,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setWifiBand\",\"params\":{\"band\":\"6ghz\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"Wi-Fi band must be auto, 2.4ghz or 5ghz\"}}");
    expect_rpc_context(&context,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setWifiBand\",\"params\":{\"band\":2}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"Wi-Fi band must be auto, 2.4ghz or 5ghz\"}}");
    assert(wifi_band_calls == 3);
    context.set_wifi_band = NULL;
    expect_rpc_context(&context,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setWifiBand\",\"params\":{\"band\":\"auto\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32000,\"message\":\"Wi-Fi settings unavailable\"}}");
    expect_rpc_context(&context,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getWifiSettings\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"available\":false,\"connected\":false,\"applying\":false,\"apply_failed\":false,\"band\":\"auto\",\"ssid\":\"\",\"channel\":0,\"rssi\":0,\"connect_error\":\"\",\"provisioning\":false,\"switching\":false,\"static_ssid\":\"\",\"max_networks\":8,\"networks\":[]}}");
}

static int public_ip_refreshes = -1;

static const char *fake_public_ip(bool refresh, const char *cloud_url, char *ip,
                                  size_t ip_capacity, int64_t *last_updated,
                                  void *ctx)
{
    (void)cloud_url;
    public_ip_refreshes = refresh;
    if (ctx != NULL) return ctx;
    snprintf(ip, ip_capacity, "%s", refresh ? "203.0.113.7" : "");
    *last_updated = 1790000000;
    return NULL;
}

static void test_public_ip_rpc(void)
{
    const char *request =
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getPublicIPAddresses\",\"params\":{\"refresh\":true}}";
    jetkvm_control_context_t context = {.get_public_ip = fake_public_ip};
    /* Not adopted: a refresh never reaches the cloud. */
    expect_rpc_context(&context, request, "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[]}");
    assert(public_ip_refreshes == 0);
    context.cloud_registered = true;
    expect_rpc_context(&context, request,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[{\"ip\":\"203.0.113.7\",\"last_updated\":\"2026-09-21T14:13:20Z\"}]}");
    assert(public_ip_refreshes == 1);
    expect_rpc_context(&context,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getPublicIPAddresses\",\"params\":{\"refresh\":\"true\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"refresh must be a boolean\"}}");
    context.network_ctx = "unexpected status code: 500";
    expect_rpc_context(&context, request,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"unexpected status code: 500\"}}");
}

static bool wifi_settings_with_networks(jetkvm_wifi_settings_t *settings, void *ctx)
{
    (void)ctx;
    *settings = (jetkvm_wifi_settings_t){
        .available = true, .connected = true, .ssid = "Home", .channel = 36, .rssi = -52,
        .network_count = 2,
        .networks = {{.ssid = "Home", .connected = true, .secure = true}, {.ssid = "Closet"}},
    };
    return true;
}
static unsigned wifi_save_calls, wifi_remove_calls, wifi_scan_calls;
static jetkvm_wifi_network_request_t wifi_saved_request;
static bool wifi_scan_refresh;
static const char *wifi_next_error;
static const char *observe_wifi_save(const jetkvm_wifi_network_request_t *request,
                                     bool *connecting, void *ctx)
{
    (void)ctx;
    wifi_save_calls++;
    wifi_saved_request = *request;
    /* The device may test a save that did not ask to connect. */
    *connecting = request->connect || strcmp(request->ssid, "Home") == 0;
    return wifi_next_error;
}
static const char *observe_wifi_remove(const char *ssid, void *ctx)
{
    (void)ctx;
    wifi_remove_calls++;
    assert(strcmp(ssid, "Office") == 0);
    return wifi_next_error;
}
static unsigned wifi_connect_calls;
static char wifi_connect_ssid[33];
static const char *observe_wifi_connect(const char *ssid, void *ctx)
{
    (void)ctx;
    wifi_connect_calls++;
    snprintf(wifi_connect_ssid, sizeof(wifi_connect_ssid), "%s", ssid);
    return wifi_next_error;
}
static bool observe_wifi_scan(bool refresh, jetkvm_wifi_scan_t *scan, void *ctx)
{
    (void)ctx;
    wifi_scan_calls++;
    wifi_scan_refresh = refresh;
    scan->scanning = refresh;
    scan->count = 2;
    scan->entries[0] = (jetkvm_wifi_scan_entry_t){.ssid = "Home", .rssi = -50, .channel = 36, .secure = true, .saved = true};
    scan->entries[1] = (jetkvm_wifi_scan_entry_t){.ssid = "Cafe", .rssi = -70, .channel = 6};
    return true;
}
static void expect_wifi_rpc(const jetkvm_control_context_t *context, const char *method,
                            const char *params, const char *expected_body)
{
    char request[512], expected[1024];
    snprintf(request, sizeof(request), "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}", method, params);
    snprintf(expected, sizeof(expected), "{\"jsonrpc\":\"2.0\",\"id\":1,%s}", expected_body);
    expect_rpc_context(context, request, expected);
}
static void test_wifi_networks_rpc(void)
{
    jetkvm_control_context_t context = {
        .get_wifi_settings = wifi_settings_with_networks,
        .save_wifi_network = observe_wifi_save,
        .remove_wifi_network = observe_wifi_remove,
        .scan_wifi_networks = observe_wifi_scan,
        .connect_wifi_network = observe_wifi_connect,
    };
    expect_wifi_rpc(&context, "getWifiSettings", "{}",
        "\"result\":{\"available\":true,\"connected\":true,\"applying\":false,\"apply_failed\":false,\"band\":\"auto\",\"ssid\":\"Home\",\"channel\":36,\"rssi\":-52,\"connect_error\":\"\",\"provisioning\":false,\"switching\":false,\"static_ssid\":\"\",\"max_networks\":8,"
        "\"networks\":[{\"ssid\":\"Home\",\"connected\":true,\"secure\":true},"
        "{\"ssid\":\"Closet\",\"connected\":false,\"secure\":false}]}");

    /* Save only: connect defaults to false. */
    expect_wifi_rpc(&context, "saveWifiNetwork", "{\"ssid\":\"Parents\",\"password\":\"password1\"}",
                    "\"result\":{\"connecting\":false}");
    assert(wifi_save_calls == 1 && strcmp(wifi_saved_request.ssid, "Parents") == 0 &&
           strcmp(wifi_saved_request.password, "password1") == 0 &&
           !wifi_saved_request.connect);
    expect_wifi_rpc(&context, "saveWifiNetwork", "{\"ssid\":\"Cafe\",\"connect\":true}",
                    "\"result\":{\"connecting\":true}");
    assert(wifi_save_calls == 2 && wifi_saved_request.password[0] == '\0' &&
           wifi_saved_request.connect);
    /* A new password for the current network is tested even without connect. */
    expect_wifi_rpc(&context, "saveWifiNetwork", "{\"ssid\":\"Home\",\"password\":\"new-password\"}",
                    "\"result\":{\"connecting\":true}");

    const char *invalid[][2] = {
        {"{\"password\":\"password1\"}", "SSID must be 1 to 32 bytes"},
        {"{\"ssid\":\"\"}", "SSID must be 1 to 32 bytes"},
        {"{\"ssid\":\"123456789012345678901234567890123\"}", "SSID must be 1 to 32 bytes"},
        {"{\"ssid\":\"Home\",\"password\":\"short\"}", "Password must be at least 8 characters"},
        {"{\"ssid\":\"Home\",\"password\":\"12345678901234567890123456789012345678901234567890123456789012345\"}",
         "Password must be at most 64 characters"},
        {"{\"ssid\":\"Home\",\"password\":12345678}", "Password must be at most 64 characters"},
        {"{\"ssid\":\"Home\",\"connect\":1}", "connect must be a boolean"},
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        char body[160];
        snprintf(body, sizeof(body), "\"error\":{\"code\":-32602,\"message\":\"%s\"}", invalid[i][1]);
        expect_wifi_rpc(&context, "saveWifiNetwork", invalid[i][0], body);
    }
    assert(wifi_save_calls == 3);

    wifi_next_error = "Saved network list is full";
    expect_wifi_rpc(&context, "saveWifiNetwork", "{\"ssid\":\"Ninth\"}",
        "\"error\":{\"code\":-32000,\"message\":\"Saved network list is full\"}");
    wifi_next_error = "The last saved network cannot be removed";
    expect_wifi_rpc(&context, "removeWifiNetwork", "{\"ssid\":\"Office\"}",
        "\"error\":{\"code\":-32000,\"message\":\"The last saved network cannot be removed\"}");
    wifi_next_error = NULL;
    expect_wifi_rpc(&context, "removeWifiNetwork", "{\"ssid\":\"Office\"}", "\"result\":true");
    expect_wifi_rpc(&context, "removeWifiNetwork", "{}",
        "\"error\":{\"code\":-32602,\"message\":\"SSID must be 1 to 32 bytes\"}");
    assert(wifi_remove_calls == 2);

    expect_wifi_rpc(&context, "connectWifiNetwork", "{\"ssid\":\"Parents\"}", "\"result\":true");
    assert(wifi_connect_calls == 1 && strcmp(wifi_connect_ssid, "Parents") == 0);
    wifi_next_error = "The device already uses this network";
    expect_wifi_rpc(&context, "connectWifiNetwork", "{\"ssid\":\"Home\"}",
        "\"error\":{\"code\":-32000,\"message\":\"The device already uses this network\"}");
    wifi_next_error = NULL;
    expect_wifi_rpc(&context, "connectWifiNetwork", "{\"ssid\":7}",
        "\"error\":{\"code\":-32602,\"message\":\"SSID must be 1 to 32 bytes\"}");
    assert(wifi_connect_calls == 2);

    expect_wifi_rpc(&context, "scanWifiNetworks", "{\"refresh\":true}",
        "\"result\":{\"scanning\":true,\"networks\":[{\"ssid\":\"Home\",\"rssi\":-50,\"channel\":36,\"secure\":true,\"saved\":true},"
        "{\"ssid\":\"Cafe\",\"rssi\":-70,\"channel\":6,\"secure\":false,\"saved\":false}]}");
    assert(wifi_scan_refresh);
    expect_wifi_rpc(&context, "scanWifiNetworks", "{}",
        "\"result\":{\"scanning\":false,\"networks\":[{\"ssid\":\"Home\",\"rssi\":-50,\"channel\":36,\"secure\":true,\"saved\":true},"
        "{\"ssid\":\"Cafe\",\"rssi\":-70,\"channel\":6,\"secure\":false,\"saved\":false}]}");
    assert(!wifi_scan_refresh && wifi_scan_calls == 2);

    jetkvm_control_context_t unavailable = {0};
    expect_wifi_rpc(&unavailable, "scanWifiNetworks", "{}",
        "\"error\":{\"code\":-32000,\"message\":\"Wi-Fi settings unavailable\"}");
    expect_wifi_rpc(&unavailable, "saveWifiNetwork", "{\"ssid\":\"Home\"}",
        "\"error\":{\"code\":-32000,\"message\":\"Wi-Fi settings unavailable\"}");
}

static bool wifi_settings_unavailable(jetkvm_wifi_settings_t *settings, void *ctx)
{
    (void)ctx;
    *settings = (jetkvm_wifi_settings_t){0};
    return true;
}

static void test_board_capabilities(void)
{
    /* The Ethernet board's Wi-Fi provider reports no Wi-Fi. */
    jetkvm_control_context_t ethernet = {.get_wifi_settings = wifi_settings_unavailable};
    expect_rpc_context(&ethernet, "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceCapabilities\",\"id\":12}",
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"result\":[]}");
    observed_virtual_media_t media = {0};
    jetkvm_control_context_t wireless = {
        .get_wifi_settings = wifi_settings_with_networks,
        .virtual_media = {.get_state = get_virtual_media_state, .ctx = &media},
    };
    expect_rpc_context(&wireless, "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceCapabilities\",\"id\":13}",
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"result\":[\"removable_storage\",\"wifi\"]}");
    /* The connect-time event carries the same list as the RPC. */
    char event[256];
    size_t event_length = 0;
    assert(jetkvm_control_render_capabilities_notification(&wireless, event, sizeof(event), &event_length));
    assert(strcmp(event, "{\"jsonrpc\":\"2.0\",\"method\":\"deviceCapabilities\",\"params\":[\"removable_storage\",\"wifi\"]}") == 0);
}

/* The USB debugging setting: stored, and taking effect later; the port's
 * state is separate. */
typedef struct {
    bool enabled, active, readable;
    const char *save_error;
    unsigned saves;
} usb_debugging_test_t;
static bool usb_debugging_test_get(bool *enabled, bool *active, void *ctx)
{
    usb_debugging_test_t *test = ctx;
    *enabled = test->enabled;
    *active = test->active;
    return test->readable;
}
static const char *usb_debugging_test_set(bool enabled, void *ctx)
{
    usb_debugging_test_t *test = ctx;
    ++test->saves;
    if (test->save_error) return test->save_error;
    test->enabled = enabled;
    return NULL;
}
static void test_usb_debugging(void)
{
    /* Without a registered setting the UI shows no switch. */
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getDeviceCapabilities\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[]}");
    display_test_error("getUsbDebuggingState", "{}", -32603);
    display_test_error("setUsbDebuggingState", "{\"enabled\":true}", -32603);

    usb_debugging_test_t test = {.enabled = false, .active = false, .readable = true};
    jetkvm_control_set_default_usb_debugging_context(&(jetkvm_usb_debugging_context_t){
        .get = usb_debugging_test_get, .set = usb_debugging_test_set, .ctx = &test});
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getDeviceCapabilities\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[\"usb_debugging\"]}");
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getUsbDebuggingState\",\"params\":{}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"enabled\":false,\"active\":false}}");
    /* Turning it on stores it; the port stays off until the next restart. */
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setUsbDebuggingState\",\"params\":{\"enabled\":true}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"enabled\":true,\"active\":false}}");
    assert(test.saves == 1U && test.enabled);
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getUsbDebuggingState\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"enabled\":true,\"active\":false}}");
    /* A started bench build: on, and turned off for the next restart. */
    test.active = true;
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setUsbDebuggingState\",\"params\":{\"enabled\":false}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"enabled\":false,\"active\":true}}");
    assert(test.saves == 2U && !test.enabled);

    /* Bad parameters change nothing. */
    display_test_error("setUsbDebuggingState", "{}", -32602);
    display_test_error("setUsbDebuggingState", "{\"enabled\":1}", -32602);
    display_test_error("setUsbDebuggingState", "{\"enabled\":\"true\"}", -32602);
    display_test_error("getUsbDebuggingState", "{\"enabled\":true}", -32602);
    assert(test.saves == 2U && !test.enabled);

    /* A failed save is an error, and the setting stays. */
    test.save_error = "NVS write failed";
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"setUsbDebuggingState\",\"params\":{\"enabled\":true}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"NVS write failed\"}}");
    assert(test.saves == 3U && !test.enabled);
    test.readable = false;
    display_test_error("getUsbDebuggingState", "{}", -32603);

    jetkvm_control_set_default_usb_debugging_context(NULL);
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getDeviceCapabilities\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":[]}");
}

void test_os_services(void); /* test_os_services.c */


static unsigned download_starts;
static char download_url[128];
static char download_filename[64];

static bool fake_download_start(const char *url, const char *filename,
                                char *error, size_t capacity, void *ctx)
{
    download_starts++;
    snprintf(download_url, sizeof(download_url), "%s", url);
    snprintf(download_filename, sizeof(download_filename), "%s", filename);
    if (ctx != NULL) {
        snprintf(error, capacity, "%s", (const char *)ctx);
        return false;
    }
    return true;
}

static bool fake_download_state(jetkvm_control_storage_download_state_t *state,
                                void *ctx)
{
    (void)ctx;
    *state = (jetkvm_control_storage_download_state_t){
        .phase = JETKVM_CONTROL_STORAGE_DOWNLOAD_DOWNLOADING,
        .filename = "os.iso",
        .bytes = 4194304,
        .total = 92807168,
        .video_paused = true,
    };
    return true;
}

static void test_storage_download_rpc(void)
{
    /* Without a provider the state is idle and a download cannot start. */
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getStorageDownloadState\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"state\":\"idle\",\"filename\":\"\",\"bytes\":0,\"total\":0,\"reused\":false,\"videoPaused\":false,\"error\":\"\"}}");
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"downloadToStorage\",\"params\":{\"url\":\"https://h/os.iso\",\"filename\":\"os.iso\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"virtual media storage unavailable\"}}");

    jetkvm_storage_download_context_t download = {
        .start = fake_download_start,
        .get_state = fake_download_state,
    };
    jetkvm_control_set_default_storage_download_context(&download);
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"downloadToStorage\",\"params\":{\"url\":\"https://h/os.iso\",\"filename\":\"os.iso\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":null}");
    assert(download_starts == 1 && strcmp(download_url, "https://h/os.iso") == 0 &&
           strcmp(download_filename, "os.iso") == 0);
    const char *bad[] = {
        "{\"url\":\"https://h/os.iso\"}",
        "{\"filename\":\"os.iso\"}",
        "{\"url\":\"https://h/os.iso\",\"filename\":7}",
        "{\"url\":\"https://h/os.iso\",\"filename\":\"os.iso\",\"mode\":\"CDROM\"}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        char request[256];
        snprintf(request, sizeof(request),
                 "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"downloadToStorage\",\"params\":%s}", bad[i]);
        expect_rpc(request,
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"invalid virtual media parameters\"}}");
    }
    assert(download_starts == 1);
    /* A refusal carries the provider's reason. */
    download.ctx = "No TF card is mounted. Insert a TF card, then try again.";
    jetkvm_control_set_default_storage_download_context(&download);
    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"downloadToStorage\",\"params\":{\"url\":\"https://h/os.iso\",\"filename\":\"os.iso\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"No TF card is mounted. Insert a TF card, then try again.\"}}");
    assert(download_starts == 2);

    expect_rpc("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getStorageDownloadState\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"state\":\"downloading\",\"filename\":\"os.iso\",\"bytes\":4194304,\"total\":92807168,\"reused\":false,\"videoPaused\":true,\"error\":\"\"}}");

    /* The peer task's context refuses a start while the media worker is
     * busy, and still reports the download's state. */
    jetkvm_control_context_t peer = {
        .storage_download = {
            .start = fake_download_start,
            .ctx = "another virtual media operation is active",
        },
    };
    expect_rpc_context(&peer,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"downloadToStorage\",\"params\":{\"url\":\"https://h/os.iso\",\"filename\":\"os.iso\"}}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"another virtual media operation is active\"}}");
    expect_rpc_context(&peer,
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"getStorageDownloadState\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"state\":\"downloading\",\"filename\":\"os.iso\",\"bytes\":4194304,\"total\":92807168,\"reused\":false,\"videoPaused\":true,\"error\":\"\"}}");

    jetkvm_control_storage_download_state_t state = {
        .phase = JETKVM_CONTROL_STORAGE_DOWNLOAD_ERROR,
        .filename = "os.iso",
        .bytes = 512,
        .total = 1024,
        .error = "The download server answered HTTP 404.",
    };
    char event[512];
    size_t length = 0;
    assert(jetkvm_control_render_storage_download_event(&state, event, sizeof(event), &length));
    assert(strcmp(event,
        "{\"jsonrpc\":\"2.0\",\"method\":\"storageDownloadState\",\"params\":{\"state\":\"error\",\"filename\":\"os.iso\",\"bytes\":512,\"total\":1024,\"reused\":false,\"videoPaused\":false,\"error\":\"The download server answered HTTP 404.\"}}") == 0);
    assert(length == strlen(event));
    assert(!jetkvm_control_render_storage_download_event(&state, event, 16, &length) && length == 0);
    jetkvm_control_set_default_storage_download_context(NULL);
}

int main(void)
{
    test_os_services();
    test_storage_download_rpc();
    test_wifi_band_rpc();
    test_public_ip_rpc();
    test_wifi_networks_rpc();
    test_hidrpc();
    test_tls_contract();
    test_extra_hostnames_contract();
    test_board_capabilities();
    test_audio_contract();
    test_cloud_deregistration();
    test_usb_debugging();

    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"params\":{},\"id\":\"probe-1\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":\"probe-1\",\"result\":\"pong\"}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"notImplemented\",\"id\":7}",
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
    expect_rpc(
        "not-json",
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}");

    double observed_factor = 0.0;
    observed_video_codec_t observed_video_codec = {
        .result = JETKVM_VIDEO_CODEC_CHANGE_ACCEPTED,
    };
    observed_video_stream_pause_t observed_video_stream_pause = {
        .available = true,
    };
    observed_cloud_urls_t observed_cloud = { 0 };
    observed_hidrpc_t observed_hid = { 0 };
    observed_keyboard_layout_t observed_layout = {
        .layout = "en-US",
        .readable = true,
    };
    observed_reboot_t observed_reboot = {
        .result = JETKVM_REBOOT_REQUEST_ACCEPTED,
    };
    observed_hid_state_t observed_hid_state = {
        .leds = {
            .num_lock = true,
            .caps_lock = false,
            .scroll_lock = true,
            .compose = false,
            .kana = true,
            .shift = true,
        },
        .keys_down = {
            .modifier = 0x22,
            .keys = { 0x04, 0x05, 0x00, 0x00, 0x00, 0x00 },
        },
        .usb_state = JETKVM_USB_STATE_CONFIGURED,
        .wake_result = JETKVM_USB_WAKE_SUCCEEDED,
    };
    observed_wol_t observed_wol = {
        .result = true,
        .devices_result = true,
        .devices = {
            .count = 1U,
            .devices = {{
                .name = "Build host",
                .mac_address = "02:00:00:00:00:01",
            }},
        },
    };
    observed_hid_profiles_t observed_hid_profiles = {
        .jiggler_config = {
            .inactivity_limit_seconds = 60U,
            .jitter_percentage = 25U,
            .schedule_cron_tab = "0 * * * * *",
            .timezone = "UTC",
        },
        .result = true,
        .config_change_result = JETKVM_JIGGLER_CONFIG_CHANGE_ACCEPTED,
    };
    observed_usb_configuration_t observed_usb_configuration = {
        .snapshot = {
            .vendor_id = 0x303a,
            .product_id = 0x400c,
            .serial_number = "ESP32-P4",
            .manufacturer = "JetKVM",
            .product = "JetKVM USB Emulation Device",
            .absolute_mouse = true,
            .relative_mouse = true,
            .keyboard = true,
            .mass_storage = false,
            .serial_console = false,
            .audio = false,
            .emulation_enabled = true,
        },
        .readable = true,
        .descriptor_change_result =
            JETKVM_USB_CONFIGURATION_CHANGE_COMPLETED,
        .devices_change_result = JETKVM_USB_CONFIGURATION_CHANGE_COMPLETED,
        .device_change_result = JETKVM_USB_CONFIGURATION_CHANGE_COMPLETED,
        .change_result = JETKVM_USB_EMULATION_CHANGE_ACCEPTED,
    };
    observed_virtual_media_t observed_virtual_media = {
        .state = {
            .initialized = true,
            .card_present = true,
            .filesystem_mounted = true,
            .mounted = true,
            .mode = JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_CDROM,
            .filename = "installer.iso",
            .size = 734003200U,
        },
    };
    observed_video_log_status_t observed_video_log_status = {
        .status = "JetKVM ESP32-P4 video diagnostics\nmode: 1920x1080@30",
        .readable = true,
    };
    observed_edid_t observed_edid = {
        .edid = TEST_EDID,
        .readable = true,
        .result = JETKVM_EDID_CHANGE_ACCEPTED,
    };
    observed_host_display_idle_t observed_host_display_idle = {
        .readable = true,
        .result = JETKVM_HOST_DISPLAY_IDLE_CHANGE_ACCEPTED,
    };
    observed_video_sleep_t observed_video_sleep = {
        .duration = -1,
        .readable = true,
        .result = JETKVM_VIDEO_SLEEP_CHANGE_ACCEPTED,
    };
    observed_mqtt_t observed_mqtt = {
        .settings = {
            .enabled = true,
            .broker = "mqtt.example.test",
            .port = 8883,
            .username = "operator",
            .password = "********",
            .base_topic = "lab/jetkvm",
            .use_tls = true,
            .tls_insecure = false,
            .enable_ha_discovery = true,
            .enable_actions = true,
            .debounce_ms = 750,
        },
        .status = {
            .connected = false,
            .error = "connection refused",
        },
        .test_result = {
            .success = true,
        },
        .set_result = JETKVM_MQTT_CONFIG_CHANGE_ACCEPTED,
        .settings_readable = true,
        .status_readable = true,
        .test_available = true,
    };
    observed_ota_t observed_ota = {
        .status = {
            .local_app_version = "0.5.0-p4.1",
            .local_system_version = "89abcdef",
            .remote_app_version = "0.5.0-p4.2",
            .remote_app_url = "http://192.168.50.1/releases/app.bin",
            .remote_app_hash =
                "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
            .app_update_available = true,
        },
        .status_readable = true,
        .check_ok = true,
        .try_ok = true,
        .auto_update = false,
        .auto_update_available = true,
        .update_pending = false,
        .pending_available = true,
        .api_url = "http://192.168.50.1:8765",
        .api_url_ok = true,
    };
    const jetkvm_control_context_t context = {
        .video = {
            .ready = true,
            .streaming = JETKVM_VIDEO_STREAM_ACTIVE,
            .error = "",
            .width = 1920,
            .height = 1080,
            .fps = 30.0,
        },
        .app_version = "1.2.3",
        .system_version = "esp-idf-6.0.2",
        .device_id = "e8f60a01b2cf",
        .network_settings = {
            .hostname = "p4-kvm", .dhcp = false,
            .address = "192.168.50.2", .netmask = "255.255.255.0",
            .gateway = "192.168.50.1", .dns = {"8.8.8.8", "1.1.1.1"},
        },
        .network = {
            .interface_name = "eth0",
            .hostname = "p4-kvm",
            .mac_address = "02:00:00:00:00:01",
            .ipv4_address = "192.168.50.2",
            .netmask = "255.255.255.0",
            .gateway = "192.168.50.1",
            .dns_main = "8.8.8.8",
            .dns_backup = "1.1.1.1",
            .last_updated = "2026-07-13T14:00:00Z",
            .up = true,
            .online = true,
            .ipv4_ready = true,
            .dhcp = false,
        },
        .cloud_registered = true,
        .cloud_url = "https://cloud.example.test",
        .cloud_app_url = "https://app.example.test",
        .stream_quality_factor = 0.5,
        .get_video_log_status = get_video_log_status,
        .video_log_status_ctx = &observed_video_log_status,
        .queue_quality_change = accept_half_quality,
        .queue_quality_change_ctx = &observed_factor,
        .video_codec_preference = "auto",
        .set_video_codec_preference = set_video_codec_preference,
        .set_video_codec_preference_ctx = &observed_video_codec,
        .set_video_stream_paused = set_video_stream_paused,
        .set_video_stream_paused_ctx = &observed_video_stream_pause,
        .edid = {
            .get_preset = get_edid_preset,
            .get = get_edid,
            .set = set_edid,
            .ctx = &observed_edid,
        },
        .host_display_idle = {
            .get = get_host_display_idle,
            .set = set_host_display_idle,
            .ctx = &observed_host_display_idle,
        },
        .video_sleep = {
            .get = get_video_sleep,
            .set = set_video_sleep,
            .ctx = &observed_video_sleep,
        },
        .mqtt = {
            .get_settings = get_mqtt_settings,
            .set_settings = set_mqtt_settings,
            .get_status = get_mqtt_status,
            .test_connection = test_mqtt_connection,
            .ctx = &observed_mqtt,
        },
        .ota = {
            .get_update_status = get_ota_update_status,
            .check_update = check_ota_update,
            .try_update = try_ota_update,
            .get_auto_update = get_ota_auto_update,
            .set_auto_update = set_ota_auto_update,
            .get_dev_channel = get_ota_dev_channel,
            .set_dev_channel = set_ota_dev_channel,
            .is_update_pending = is_ota_update_pending,
            .get_update_api_url = get_ota_update_api_url,
            .set_update_api_url = set_ota_update_api_url,
            .ctx = &observed_ota,
        },
        .set_cloud_urls = accept_cloud_urls,
        .set_cloud_urls_ctx = &observed_cloud,
        .keyboard_layout = {
            .get = get_keyboard_layout,
            .set = set_keyboard_layout,
            .ctx = &observed_layout,
        },
        .request_reboot = request_reboot,
        .request_reboot_ctx = &observed_reboot,
        .hidrpc = {
            .on_event = observe_hidrpc,
            .ctx = &observed_hid,
        },
        .get_keyboard_led_state = get_keyboard_led_state,
        .get_key_down_state = get_key_down_state,
        .get_usb_state = get_usb_state,
        .usb_configuration = {
            .get = get_usb_configuration,
            .set_descriptor_config = set_usb_descriptor_config,
            .set_devices = set_usb_devices,
            .set_device_state = set_usb_device_state,
            .set_emulation_enabled = set_usb_emulation_enabled,
            .ctx = &observed_usb_configuration,
        },
        .wake_host = wake_host,
        .send_wol = send_wol,
        .wol_devices = {
            .get = get_wol_devices,
            .set = set_wol_devices,
            .ctx = &observed_wol,
        },
        .keyboard_macros = {
            .get = get_keyboard_macros,
            .set = set_keyboard_macros,
            .ctx = &observed_hid_profiles,
        },
        .jiggler = {
            .get_state = get_jiggler_state,
            .set_state = set_jiggler_state,
            .get_config = get_jiggler_config,
            .set_config = set_jiggler_config,
            .ctx = &observed_hid_profiles,
        },
        .network_ctx = &observed_wol,
        .hid_state_ctx = &observed_hid_state,
        .virtual_media = {
            .get_state = get_virtual_media_state,
            .get_space = get_storage_space,
            .list_files = list_storage_files,
            .check_url = check_mount_url,
            .mount_http = mount_http_image,
            .mount_storage = mount_storage_file,
            .mount_builtin_installer = mount_builtin_installer,
            .unmount = unmount_virtual_media,
            .format_storage = format_storage,
            .delete_file = delete_storage_file,
            .start_upload = start_storage_upload,
            .ctx = &observed_virtual_media,
        },
    };
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getMqttSettings\",\"id\":490}",
        "{\"jsonrpc\":\"2.0\",\"id\":490,\"result\":{\"enabled\":true,\"broker\":\"mqtt.example.test\",\"port\":8883,\"username\":\"operator\",\"password\":\"********\",\"base_topic\":\"lab/jetkvm\",\"use_tls\":true,\"tls_insecure\":false,\"enable_ha_discovery\":true,\"enable_actions\":true,\"debounce_ms\":750}}");
    assert(observed_mqtt.get_settings_calls == 1U);
    const char *mqtt_settings_json =
        "{\"enabled\":false,\"broker\":\"192.168.50.1\",\"port\":1883,"
        "\"username\":\"\",\"password\":\"secret\",\"base_topic\":\"jetkvm\","
        "\"use_tls\":false,\"tls_insecure\":false,"
        "\"enable_ha_discovery\":true,\"enable_actions\":true,"
        "\"debounce_ms\":0}";
    char mqtt_request[768];
    assert(snprintf(
               mqtt_request, sizeof(mqtt_request),
               "{\"jsonrpc\":\"2.0\",\"method\":\"setMqttSettings\","
               "\"params\":{\"settings\":%s},\"id\":491}",
               mqtt_settings_json) > 0);
    expect_rpc_context(
        &context, mqtt_request,
        "{\"jsonrpc\":\"2.0\",\"id\":491,\"result\":null}");
    assert(observed_mqtt.set_settings_calls == 1U);
    assert(!observed_mqtt.settings.enabled);
    assert(strcmp(observed_mqtt.settings.broker, "192.168.50.1") == 0);
    assert(observed_mqtt.settings.port == 1883);
    assert(strcmp(observed_mqtt.settings.password, "secret") == 0);
    assert(strcmp(observed_mqtt.settings.base_topic, "jetkvm") == 0);
    assert(observed_mqtt.settings.enable_ha_discovery);
    assert(observed_mqtt.settings.enable_actions);
    assert(observed_mqtt.settings.debounce_ms == 0);

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getMqttStatus\",\"id\":492}",
        "{\"jsonrpc\":\"2.0\",\"id\":492,\"result\":{\"connected\":false,\"error\":\"connection refused\"}}");
    assert(observed_mqtt.get_status_calls == 1U);
    observed_mqtt.status.connected = true;
    observed_mqtt.status.error[0] = '\0';
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getMqttStatus\",\"params\":{},\"id\":493}",
        "{\"jsonrpc\":\"2.0\",\"id\":493,\"result\":{\"connected\":true}}");

    assert(snprintf(
               mqtt_request, sizeof(mqtt_request),
               "{\"jsonrpc\":\"2.0\",\"method\":\"testMqttConnection\","
               "\"params\":{\"settings\":%s},\"id\":494}",
               mqtt_settings_json) > 0);
    expect_rpc_context(
        &context, mqtt_request,
        "{\"jsonrpc\":\"2.0\",\"id\":494,\"result\":{\"success\":true}}");
    assert(observed_mqtt.test_calls == 1U);
    assert(strcmp(observed_mqtt.settings.broker, "192.168.50.1") == 0);
    observed_mqtt.test_result.success = false;
    strcpy(observed_mqtt.test_result.error, "connection timed out");
    assert(snprintf(
               mqtt_request, sizeof(mqtt_request),
               "{\"jsonrpc\":\"2.0\",\"method\":\"testMqttConnection\","
               "\"params\":{\"settings\":%s},\"id\":495}",
               mqtt_settings_json) > 0);
    expect_rpc_context(
        &context, mqtt_request,
        "{\"jsonrpc\":\"2.0\",\"id\":495,\"result\":{\"success\":false,\"error\":\"connection timed out\"}}");

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setMqttSettings\",\"params\":{\"settings\":{\"enabled\":true}},\"id\":496}",
        "{\"jsonrpc\":\"2.0\",\"id\":496,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"settings must contain the complete MQTT configuration\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"testMqttConnection\",\"params\":{\"settings\":{\"enabled\":false,\"broker\":\"\",\"port\":1883.5,\"username\":\"\",\"password\":\"\",\"base_topic\":\"jetkvm\",\"use_tls\":false,\"tls_insecure\":false,\"enable_ha_discovery\":true,\"enable_actions\":true,\"debounce_ms\":500}},\"id\":497}",
        "{\"jsonrpc\":\"2.0\",\"id\":497,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"settings must contain the complete MQTT configuration\"}}");
    assert(observed_mqtt.set_settings_calls == 1U);
    assert(observed_mqtt.test_calls == 2U);

    observed_mqtt.set_result = JETKVM_MQTT_CONFIG_CHANGE_INVALID;
    assert(snprintf(
               mqtt_request, sizeof(mqtt_request),
               "{\"jsonrpc\":\"2.0\",\"method\":\"setMqttSettings\","
               "\"params\":{\"settings\":%s},\"id\":498}",
               mqtt_settings_json) > 0);
    expect_rpc_context(
        &context, mqtt_request,
        "{\"jsonrpc\":\"2.0\",\"id\":498,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"Invalid MQTT settings\"}}");
    observed_mqtt.set_result = JETKVM_MQTT_CONFIG_CHANGE_ACCEPTED;

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceCapabilities\",\"id\":569}",
        "{\"jsonrpc\":\"2.0\",\"id\":569,\"result\":[\"removable_storage\"]}");

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUpdateStatus\",\"id\":570}",
        "{\"jsonrpc\":\"2.0\",\"id\":570,\"result\":{\"local\":{\"appVersion\":\"0.5.0-p4.1\",\"systemVersion\":\"89abcdef\"},\"remote\":{\"appVersion\":\"0.5.0-p4.2\",\"appUrl\":\"http://192.168.50.1/releases/app.bin\",\"appHash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"systemVersion\":\"\",\"systemUrl\":\"\",\"systemHash\":\"\"},\"systemUpdateAvailable\":false,\"appUpdateAvailable\":true,\"willDisableAutoUpdate\":false}}");
    assert(observed_ota.get_status_calls == 1U);

    observed_ota.status.error[0] = '\0';
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"checkUpdateComponents\",\"params\":{\"params\":{\"components\":{\"app\":\"0.5.0-p4.2\",\"system\":\"\"}},\"includePreRelease\":true},\"id\":571}",
        "{\"jsonrpc\":\"2.0\",\"id\":571,\"result\":{\"local\":{\"appVersion\":\"0.5.0-p4.1\",\"systemVersion\":\"89abcdef\"},\"remote\":{\"appVersion\":\"0.5.0-p4.2\",\"appUrl\":\"http://192.168.50.1/releases/app.bin\",\"appHash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"systemVersion\":\"\",\"systemUrl\":\"\",\"systemHash\":\"\"},\"systemUpdateAvailable\":false,\"appUpdateAvailable\":true,\"willDisableAutoUpdate\":false}}");
    assert(observed_ota.check_calls == 1U);
    assert(strcmp(observed_ota.checked_app, "0.5.0-p4.2") == 0);
    assert(observed_ota.checked_system[0] == '\0');
    assert(observed_ota.checked_prerelease);

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"checkUpdateComponents\",\"params\":{\"params\":{\"components\":123}},\"id\":5711}",
        "{\"jsonrpc\":\"2.0\",\"id\":5711,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"components and includePreRelease expected\"}}");
    assert(observed_ota.check_calls == 1U);

    /* A failed check is an RPC error, not a result with an error field:
     * the UI treats every result as a completed check. */
    strcpy(observed_ota.status.error, "could not reach the update server");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUpdateStatus\",\"id\":5712}",
        "{\"jsonrpc\":\"2.0\",\"id\":5712,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"error checking for updates: could not reach the update server\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"checkUpdateComponents\",\"params\":{\"params\":{\"components\":{\"system\":\"0.0.1\"}}},\"id\":5713}",
        "{\"jsonrpc\":\"2.0\",\"id\":5713,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"failed to check update: could not reach the update server\"}}");
    assert(observed_ota.get_status_calls == 2U);
    assert(observed_ota.check_calls == 2U);
    observed_ota.status.error[0] = '\0';
    observed_ota.check_calls = 1U;

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"tryUpdate\",\"params\":{},\"id\":572}",
        "{\"jsonrpc\":\"2.0\",\"id\":572,\"result\":null}");
    assert(observed_ota.try_calls == 1U);
    assert(observed_ota.tried_app[0] == '\0');
    assert(!observed_ota.tried_prerelease);
    assert(!observed_ota.tried_reset_config);

    /* The frontend's plain tryUpdate carries no includePreRelease; the dev
     * channel setting decides. An explicit value still wins. */
    observed_ota.dev_channel = true;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"tryUpdate\",\"params\":{},\"id\":5721}",
        "{\"jsonrpc\":\"2.0\",\"id\":5721,\"result\":null}");
    assert(observed_ota.try_calls == 2U);
    assert(observed_ota.tried_prerelease);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"checkUpdateComponents\",\"params\":{\"params\":{\"components\":{}}},\"id\":5722}",
        "{\"jsonrpc\":\"2.0\",\"id\":5722,\"result\":{\"local\":{\"appVersion\":\"0.5.0-p4.1\",\"systemVersion\":\"89abcdef\"},\"remote\":{\"appVersion\":\"0.5.0-p4.2\",\"appUrl\":\"http://192.168.50.1/releases/app.bin\",\"appHash\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"systemVersion\":\"\",\"systemUrl\":\"\",\"systemHash\":\"\"},\"systemUpdateAvailable\":false,\"appUpdateAvailable\":true,\"willDisableAutoUpdate\":false}}");
    assert(observed_ota.check_calls == 2U);
    assert(observed_ota.checked_prerelease);
    observed_ota.dev_channel = false;
    observed_ota.try_calls = 1U;
    observed_ota.check_calls = 1U;

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"tryUpdateComponents\",\"params\":{\"params\":{\"components\":{\"app\":\"0.5.0-p4.2\"}},\"includePreRelease\":false,\"resetConfig\":true},\"id\":573}",
        "{\"jsonrpc\":\"2.0\",\"id\":573,\"result\":null}");
    assert(observed_ota.try_calls == 2U);
    assert(strcmp(observed_ota.tried_app, "0.5.0-p4.2") == 0);
    assert(!observed_ota.tried_prerelease);
    assert(observed_ota.tried_reset_config);

    observed_ota.try_ok = false;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"tryUpdate\",\"params\":{},\"id\":574}",
        "{\"jsonrpc\":\"2.0\",\"id\":574,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"an update is already in progress\"}}");
    observed_ota.try_ok = true;
    assert(observed_ota.try_calls == 3U);

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getAutoUpdateState\",\"id\":575}",
        "{\"jsonrpc\":\"2.0\",\"id\":575,\"result\":false}");
    assert(observed_ota.get_auto_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setAutoUpdateState\",\"params\":{\"enabled\":true},\"id\":576}",
        "{\"jsonrpc\":\"2.0\",\"id\":576,\"result\":true}");
    assert(observed_ota.set_auto_calls == 1U);
    assert(observed_ota.set_auto_value);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setAutoUpdateState\",\"params\":{\"enabled\":\"yes\"},\"id\":5761}",
        "{\"jsonrpc\":\"2.0\",\"id\":5761,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"enabled boolean expected\"}}");
    assert(observed_ota.set_auto_calls == 1U);

    observed_ota.get_dev_channel_calls = 0U; /* the update RPCs above read it */
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getDevChannelState\",\"id\":5762}",
        "{\"jsonrpc\":\"2.0\",\"id\":5762,\"result\":false}");
    assert(observed_ota.get_dev_channel_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setDevChannelState\",\"params\":{\"enabled\":true},\"id\":5763}",
        "{\"jsonrpc\":\"2.0\",\"id\":5763,\"result\":true}");
    assert(observed_ota.set_dev_channel_calls == 1U);
    assert(observed_ota.dev_channel);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getDevChannelState\",\"id\":5764}",
        "{\"jsonrpc\":\"2.0\",\"id\":5764,\"result\":true}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setDevChannelState\",\"params\":{\"enabled\":1},\"id\":5765}",
        "{\"jsonrpc\":\"2.0\",\"id\":5765,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"enabled boolean expected\"}}");
    assert(observed_ota.set_dev_channel_calls == 1U);

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"isUpdatePending\",\"id\":577}",
        "{\"jsonrpc\":\"2.0\",\"id\":577,\"result\":false}");
    assert(observed_ota.is_pending_calls == 1U);

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUpdateApiUrl\",\"id\":578}",
        "{\"jsonrpc\":\"2.0\",\"id\":578,\"result\":\"http://192.168.50.1:8765\"}");
    assert(observed_ota.get_url_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUpdateApiUrl\",\"params\":{\"url\":\"http://192.168.50.1:9999\"},\"id\":579}",
        "{\"jsonrpc\":\"2.0\",\"id\":579,\"result\":null}");
    assert(observed_ota.set_url_calls == 1U);
    assert(strcmp(observed_ota.set_url_value, "http://192.168.50.1:9999") == 0);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUpdateApiUrl\",\"params\":{\"url\":123},\"id\":5791}",
        "{\"jsonrpc\":\"2.0\",\"id\":5791,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"url string expected\"}}");
    assert(observed_ota.set_url_calls == 1U);

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getVirtualMediaState\",\"id\":500}",
        "{\"jsonrpc\":\"2.0\",\"id\":500,\"result\":{\"source\":\"Storage\",\"mode\":\"CDROM\",\"filename\":\"installer.iso\",\"size\":734003200}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getStorageState\",\"id\":5001}",
        "{\"jsonrpc\":\"2.0\",\"id\":5001,\"result\":{\"initialized\":true,\"cardPresent\":true,\"cardGeneration\":0,\"filesystemMounted\":true}}");
    observed_virtual_media.state.card_present = false;
    observed_virtual_media.state.card_generation = 1U;
    observed_virtual_media.state.filesystem_mounted = false;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getStorageState\",\"id\":5002}",
        "{\"jsonrpc\":\"2.0\",\"id\":5002,\"result\":{\"initialized\":true,\"cardPresent\":false,\"cardGeneration\":1,\"filesystemMounted\":false}}");
    observed_virtual_media.state.card_present = true;
    observed_virtual_media.state.filesystem_mounted = true;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"formatStorage\",\"params\":{},\"id\":5003}",
        "{\"jsonrpc\":\"2.0\",\"id\":5003,\"result\":{\"bytesUsed\":196608,\"bytesFree\":31452659712}}");
    assert(observed_virtual_media.format_calls == 1U);
    /* Each refusal of the card's format names its reason. */
    const struct {
        jetkvm_control_storage_format_result_t result;
        const char *data;
    } format_refusals[] = {
        { JETKVM_CONTROL_STORAGE_FORMAT_NOT_READY,
          "virtual media storage unavailable" },
        { JETKVM_CONTROL_STORAGE_FORMAT_NO_CARD, "No TF card is inserted." },
        { JETKVM_CONTROL_STORAGE_FORMAT_IMAGE_MOUNTED,
          "An image is mounted. Unmount it, then format the card." },
        { JETKVM_CONTROL_STORAGE_FORMAT_UPLOAD_ACTIVE,
          "A file is being written to the TF card. Format the card after the upload has finished." },
        { JETKVM_CONTROL_STORAGE_FORMAT_BUSY,
          "another virtual media operation is active" },
        { JETKVM_CONTROL_STORAGE_FORMAT_FAILED,
          "failed to format storage as exFAT" },
    };
    for (size_t i = 0; i < sizeof(format_refusals) / sizeof(format_refusals[0]); ++i) {
        char expected[256];
        snprintf(expected, sizeof(expected),
                 "{\"jsonrpc\":\"2.0\",\"id\":5004,\"error\":{\"code\":-32603,"
                 "\"message\":\"Internal error\",\"data\":\"%s\"}}",
                 format_refusals[i].data);
        observed_virtual_media.format_result = format_refusals[i].result;
        expect_rpc_context(
            &context,
            "{\"jsonrpc\":\"2.0\",\"method\":\"formatStorage\",\"id\":5004}",
            expected);
        assert(observed_virtual_media.format_calls == 2U + i);
    }
    observed_virtual_media.format_result = JETKVM_CONTROL_STORAGE_FORMAT_COMPLETED;
    /* A format during a firmware update does not reach the card. */
    observed_ota.update_pending = true;
    const unsigned format_calls_before_update = observed_virtual_media.format_calls;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"formatStorage\",\"id\":5005}",
        "{\"jsonrpc\":\"2.0\",\"id\":5005,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"A firmware update is in progress. Format the card after the update has finished.\"}}");
    assert(observed_virtual_media.format_calls == format_calls_before_update);
    observed_ota.update_pending = false;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getStorageSpace\",\"id\":501}",
        "{\"jsonrpc\":\"2.0\",\"id\":501,\"result\":{\"bytesUsed\":4096,\"bytesFree\":8192}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"listStorageFiles\",\"id\":502}",
        "{\"jsonrpc\":\"2.0\",\"id\":502,\"result\":{\"files\":[{\"filename\":\"installer.iso\",\"size\":734003200,\"createdAt\":\"1970-01-01T00:00:00Z\"}]}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"mountWithStorage\",\"params\":{\"filename\":\"disk.img\",\"mode\":\"Disk\"},\"id\":503}",
        "{\"jsonrpc\":\"2.0\",\"id\":503,\"result\":null}");
    assert(observed_virtual_media.mount_calls == 1U);
    assert(observed_virtual_media.mounted_mode ==
           JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_DISK);
    assert(strcmp(observed_virtual_media.action_filename, "disk.img") == 0);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"checkMountUrl\",\"params\":{\"url\":\"http://example.test/disk.img\"},\"id\":5031}",
        "{\"jsonrpc\":\"2.0\",\"id\":5031,\"result\":{\"usable\":true,\"size\":4194304}}");
    assert(observed_virtual_media.check_url_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"checkMountUrl\",\"params\":{\"url\":\"http://example.test/missing.iso\"},\"id\":50311}",
        "{\"jsonrpc\":\"2.0\",\"id\":50311,\"result\":{\"usable\":false,\"size\":0,\"reason\":\"The URL is not available.\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"mountWithHTTP\",\"params\":{\"url\":\"http://example.test/disk.img\",\"mode\":\"CDROM\"},\"id\":5032}",
        "{\"jsonrpc\":\"2.0\",\"id\":5032,\"result\":null}");
    assert(observed_virtual_media.http_mount_calls == 1U);
    assert(strcmp(observed_virtual_media.action_url,
                  "http://example.test/disk.img") == 0);
    /* A URL that does not answer usably is not a storage problem; the
     * message is upstream's so the shared UI and e2e suite recognise it. */
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"mountWithHTTP\",\"params\":{\"url\":\"http://example.test/missing.iso\",\"mode\":\"CDROM\"},\"id\":50321}",
        "{\"jsonrpc\":\"2.0\",\"id\":50321,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"The URL is not available. Check the image URL and try again.\"}}");
    assert(observed_virtual_media.http_mount_calls == 2U);
    observed_virtual_media.state.source =
        JETKVM_CONTROL_VIRTUAL_MEDIA_SOURCE_HTTP;
    observed_virtual_media.state.filename[0] = '\0';
    strcpy(observed_virtual_media.state.url,
           "http://example.test/disk.img");
    observed_virtual_media.state.size = 4194304U;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getVirtualMediaState\",\"id\":5033}",
        "{\"jsonrpc\":\"2.0\",\"id\":5033,\"result\":{\"source\":\"HTTP\",\"mode\":\"CDROM\",\"url\":\"http://example.test/disk.img\",\"size\":4194304}}");
    /* The built-in installer disc takes no parameters, so it has no mode
     * but CD-ROM, and it does not replace a mounted image. */
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"mountBuiltinInstaller\",\"params\":{\"mode\":\"Disk\"},\"id\":5034}",
        "{\"jsonrpc\":\"2.0\",\"id\":5034,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"method does not accept parameters\"}}");
    assert(observed_virtual_media.builtin_mount_calls == 0U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"mountBuiltinInstaller\",\"id\":5035}",
        "{\"jsonrpc\":\"2.0\",\"id\":5035,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"An image is already mounted. Unmount it first.\"}}");
    assert(observed_virtual_media.builtin_mount_calls == 1U);
    const jetkvm_control_virtual_media_state_t http_state =
        observed_virtual_media.state;
    observed_virtual_media.state.mounted = false;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"mountBuiltinInstaller\",\"params\":{},\"id\":5036}",
        "{\"jsonrpc\":\"2.0\",\"id\":5036,\"result\":null}");
    assert(observed_virtual_media.builtin_mount_calls == 2U);
    observed_virtual_media.state.mounted = true;
    observed_virtual_media.state.source =
        JETKVM_CONTROL_VIRTUAL_MEDIA_SOURCE_BUILTIN;
    observed_virtual_media.state.mode = JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_CDROM;
    strcpy(observed_virtual_media.state.filename, "Install OS Services.iso");
    observed_virtual_media.state.url[0] = '\0';
    observed_virtual_media.state.size = 360448U;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getVirtualMediaState\",\"id\":5037}",
        "{\"jsonrpc\":\"2.0\",\"id\":5037,\"result\":{\"source\":\"Builtin\",\"mode\":\"CDROM\",\"filename\":\"Install OS Services.iso\",\"size\":360448}}");
    observed_virtual_media.state = http_state;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"deleteStorageFile\",\"params\":{\"filename\":\"old.iso\"},\"id\":504}",
        "{\"jsonrpc\":\"2.0\",\"id\":504,\"result\":null}");
    assert(observed_virtual_media.delete_calls == 1U);
    assert(strcmp(observed_virtual_media.action_filename, "old.iso") == 0);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"deleteStorageFile\",\"params\":{\"filename\":\"missing.iso\"},\"id\":5041}",
        "{\"jsonrpc\":\"2.0\",\"id\":5041,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"file does not exist: missing.iso\"}}");
    assert(observed_virtual_media.delete_calls == 2U);
    /* The longest accepted filename is echoed in full. */
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"deleteStorageFile\",\"params\":{\"filename\":\"missing-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.iso\"},\"id\":5042}",
        "{\"jsonrpc\":\"2.0\",\"id\":5042,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"file does not exist: missing-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx.iso\"}}");
    assert(observed_virtual_media.delete_calls == 3U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"startStorageFileUpload\",\"params\":{\"filename\":\"new.iso\",\"size\":1048576},\"id\":5041}",
        "{\"jsonrpc\":\"2.0\",\"id\":5041,\"result\":{\"alreadyUploadedBytes\":65536,\"dataChannel\":\"upload_01234567\"}}");
    assert(observed_virtual_media.upload_calls == 1U);
    assert(observed_virtual_media.upload_size == 1048576U);
    assert(strcmp(observed_virtual_media.upload_sha256,
                  JETKVM_VIRTUAL_MEDIA_UPLOAD_SHA256_UNAVAILABLE) == 0);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"startStorageFileUpload\",\"params\":{\"filename\":\"new.iso\",\"size\":1048576,\"unexpected\":true},\"id\":5043}",
        "{\"jsonrpc\":\"2.0\",\"id\":5043,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"invalid virtual media parameters\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"startStorageFileUpload\",\"params\":{\"filename\":\"new.iso\",\"size\":1048576,\"sha256\":\"not-a-digest\"},\"id\":5042}",
        "{\"jsonrpc\":\"2.0\",\"id\":5042,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"invalid virtual media parameters\"}}");
    assert(observed_virtual_media.upload_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"startStorageFileUpload\",\"params\":{\"filename\":\"new.iso\",\"size\":1048576,\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"},\"id\":505}",
        "{\"jsonrpc\":\"2.0\",\"id\":505,\"result\":{\"alreadyUploadedBytes\":65536,\"dataChannel\":\"upload_01234567\"}}");
    assert(observed_virtual_media.upload_calls == 2U);
    assert(observed_virtual_media.upload_size == 1048576U);
    assert(strcmp(observed_virtual_media.upload_sha256,
                  "0123456789abcdef0123456789abcdef"
                  "0123456789abcdef0123456789abcdef") == 0);
    /* No upload or download to the card starts during a firmware update. */
    observed_ota.update_pending = true;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"startStorageFileUpload\",\"params\":{\"filename\":\"new.iso\",\"size\":1048576},\"id\":5051}",
        "{\"jsonrpc\":\"2.0\",\"id\":5051,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"A firmware update is in progress. Upload the file after the update has finished.\"}}");
    assert(observed_virtual_media.upload_calls == 2U);
    const unsigned download_starts_before = download_starts;
    jetkvm_control_context_t downloads = context;
    downloads.storage_download.start = fake_download_start;
    expect_rpc_context(
        &downloads,
        "{\"jsonrpc\":\"2.0\",\"method\":\"downloadToStorage\",\"params\":{\"url\":\"https://h/os.iso\",\"filename\":\"os.iso\"},\"id\":5052}",
        "{\"jsonrpc\":\"2.0\",\"id\":5052,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"A firmware update is in progress. Upload the file after the update has finished.\"}}");
    assert(download_starts == download_starts_before);
    observed_ota.update_pending = false;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"unmountImage\",\"id\":506}",
        "{\"jsonrpc\":\"2.0\",\"id\":506,\"result\":null}");
    assert(observed_virtual_media.unmount_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getVideoState\",\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"ready\":true,\"streaming\":1,\"width\":1920,\"height\":1080,\"fps\":30}}");
    /* An upload to the TF card holds video off: the reply says so. */
    jetkvm_control_context_t upload_pauses = context;
    upload_pauses.video.streaming = JETKVM_VIDEO_STREAM_INACTIVE;
    upload_pauses.video.upload_pauses_video = true;
    expect_rpc_context(
        &upload_pauses,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getVideoState\",\"id\":1}",
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"ready\":true,\"streaming\":0,\"uploadPausesVideo\":true,\"width\":1920,\"height\":1080,\"fps\":30}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getVideoLogStatus\",\"id\":11}",
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"result\":\"JetKVM ESP32-P4 video diagnostics\\nmode: 1920x1080@30\"}");
    assert(observed_video_log_status.calls == 1U);
    observed_video_log_status.readable = false;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getVideoLogStatus\",\"id\":12}",
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Video debugging information unavailable\"}}");
    assert(observed_video_log_status.calls == 2U);
    observed_video_log_status.readable = true;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getLocalVersion\",\"id\":2}",
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{\"appVersion\":\"1.2.3\",\"systemVersion\":\"esp-idf-6.0.2\"}}");
    /* One firmware image, no separate app: appVersion is left out. */
    jetkvm_control_context_t no_app = context;
    no_app.app_version = "";
    expect_rpc_context(
        &no_app,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getLocalVersion\",\"id\":3}",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"systemVersion\":\"esp-idf-6.0.2\"}}");
    /* The SKU, when the host sets one, lets the UI name the cloud UI version. */
    no_app.sku = "jetkvm-mini-ethernet";
    expect_rpc_context(
        &no_app,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getLocalVersion\",\"id\":4}",
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"sku\":\"jetkvm-mini-ethernet\",\"systemVersion\":\"esp-idf-6.0.2\"}}");
    char version_event[256];
    size_t version_length = 0;
    assert(jetkvm_control_render_local_version_notification(
        "", "0.5.8", "jetkvm-mini-wireless", version_event, sizeof(version_event), &version_length));
    assert(strcmp(version_event,
                  "{\"jsonrpc\":\"2.0\",\"method\":\"localVersion\",\"params\":{\"sku\":\"jetkvm-mini-wireless\",\"systemVersion\":\"0.5.8\"}}") == 0);
    assert(jetkvm_control_render_local_version_notification(
        "1.2.3", "0.5.8", NULL, version_event, sizeof(version_event), &version_length));
    assert(strcmp(version_event,
                  "{\"jsonrpc\":\"2.0\",\"method\":\"localVersion\",\"params\":{\"appVersion\":\"1.2.3\",\"systemVersion\":\"0.5.8\"}}") == 0);
    assert(version_length == strlen(version_event));
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getNetworkState\",\"id\":30}",
        "{\"jsonrpc\":\"2.0\",\"id\":30,\"result\":{\"ipv4_addresses\":[\"192.168.50.2\"],\"ipv6_addresses\":[],\"interface_name\":\"eth0\",\"hostname\":\"p4-kvm\",\"mac_address\":\"02:00:00:00:00:01\",\"up\":true,\"online\":true,\"ipv4_ready\":true,\"ipv6_ready\":false,\"last_updated\":\"2026-07-13T14:00:00Z\",\"ipv4_address\":\"192.168.50.2\",\"dhcp_lease\":{}}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getNetworkSettings\",\"id\":3}",
        "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{\"read_only_fields\":[\"dhcp_client\",\"domain\",\"http_proxy\",\"ipv6_mode\",\"lldp_mode\",\"mdns_mode\",\"tailscale\"],\"hostname\":\"p4-kvm\",\"ipv4_mode\":\"static\",\"dhcp_client\":\"lwip\",\"domain\":\"local\",\"http_proxy\":\"\",\"ipv6_mode\":\"disabled\",\"lldp_mode\":\"disabled\",\"mdns_mode\":\"disabled\",\"time_sync_mode\":\"ntp_only\",\"time_sync_ntp_servers\":[],\"time_sync_disable_fallback\":false,\"ipv4_static\":{\"address\":\"192.168.50.2\",\"netmask\":\"255.255.255.0\",\"gateway\":\"192.168.50.1\",\"dns\":[\"8.8.8.8\",\"1.1.1.1\"]}}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getDeviceID\",\"params\":{},\"id\":31}",
        "{\"jsonrpc\":\"2.0\",\"id\":31,\"result\":\"e8f60a01b2cf\"}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getCloudState\",\"id\":32}",
        "{\"jsonrpc\":\"2.0\",\"id\":32,\"result\":{\"connected\":true,\"url\":\"https://cloud.example.test\",\"appUrl\":\"https://app.example.test\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getAudioConfig\",\"id\":4}",
        "{\"jsonrpc\":\"2.0\",\"id\":4,\"result\":{\"enabled\":false}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"reboot\",\"params\":{\"force\":true},\"id\":400}",
        "{\"jsonrpc\":\"2.0\",\"id\":400,\"result\":null}");
    assert(observed_reboot.calls == 1U);
    assert(observed_reboot.force);
    observed_reboot.result = JETKVM_REBOOT_REQUEST_BUSY;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"reboot\",\"params\":{\"force\":false},\"id\":4001}",
        "{\"jsonrpc\":\"2.0\",\"id\":4001,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Reboot already pending\"}}");
    assert(observed_reboot.calls == 2U);
    assert(!observed_reboot.force);
    observed_reboot.result = JETKVM_REBOOT_REQUEST_UNAVAILABLE;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"reboot\",\"params\":{\"force\":true},\"id\":4002}",
        "{\"jsonrpc\":\"2.0\",\"id\":4002,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Reboot unavailable\"}}");
    assert(observed_reboot.calls == 3U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"reboot\",\"params\":{},\"id\":4003}",
        "{\"jsonrpc\":\"2.0\",\"id\":4003,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"force must be a boolean\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"reboot\",\"params\":{\"force\":1},\"id\":4004}",
        "{\"jsonrpc\":\"2.0\",\"id\":4004,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"force must be a boolean\"}}");
    assert(observed_reboot.calls == 3U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getKeyboardLayout\",\"params\":{},\"id\":401}",
        "{\"jsonrpc\":\"2.0\",\"id\":401,\"result\":\"en-US\"}");
    assert(observed_layout.get_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setKeyboardLayout\",\"params\":{\"layout\":\"fr-FR\"},\"id\":402}",
        "{\"jsonrpc\":\"2.0\",\"id\":402,\"result\":null}");
    assert(observed_layout.set_calls == 1U);
    assert(strcmp(observed_layout.layout, "fr-FR") == 0);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setKeyboardLayout\",\"params\":{\"layout\":\"en_US\"},\"id\":403}",
        "{\"jsonrpc\":\"2.0\",\"id\":403,\"result\":null}");
    assert(strcmp(observed_layout.layout, "en-US") == 0);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setKeyboardLayout\",\"params\":{},\"id\":404}",
        "{\"jsonrpc\":\"2.0\",\"id\":404,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"layout must be a non-empty string\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setKeyboardLayout\",\"params\":{\"layout\":\"invalid!\"},\"id\":405}",
        "{\"jsonrpc\":\"2.0\",\"id\":405,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"Unsupported keyboard layout\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setKeyboardLayout\",\"params\":{\"layout\":\"persist-fail\"},\"id\":406}",
        "{\"jsonrpc\":\"2.0\",\"id\":406,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Failed to save keyboard layout\"}}");
    observed_layout.readable = false;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getKeyboardLayout\",\"id\":407}",
        "{\"jsonrpc\":\"2.0\",\"id\":407,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Keyboard layout unavailable\"}}");
    observed_layout.readable = true;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getKeyboardLedState\",\"params\":{},\"id\":41}",
        "{\"jsonrpc\":\"2.0\",\"id\":41,\"result\":{\"num_lock\":true,\"caps_lock\":false,\"scroll_lock\":true,\"compose\":false,\"kana\":true,\"shift\":true}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getKeyDownState\",\"params\":{},\"id\":42}",
        "{\"jsonrpc\":\"2.0\",\"id\":42,\"result\":{\"modifier\":34,\"keys\":[4,5,0,0,0,0]}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUSBState\",\"params\":{},\"id\":43}",
        "{\"jsonrpc\":\"2.0\",\"id\":43,\"result\":\"configured\"}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUsbDevices\",\"params\":{},\"id\":431}",
        "{\"jsonrpc\":\"2.0\",\"id\":431,\"result\":{\"absolute_mouse\":true,\"relative_mouse\":true,\"keyboard\":true,\"mass_storage\":false,\"serial_console\":false,\"audio\":false}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUsbConfig\",\"id\":432}",
        "{\"jsonrpc\":\"2.0\",\"id\":432,\"result\":{\"vendor_id\":\"0x303a\",\"product_id\":\"0x400c\",\"serial_number\":\"ESP32-P4\",\"manufacturer\":\"JetKVM\",\"product\":\"JetKVM USB Emulation Device\"}}");

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbConfig\",\"params\":{\"usbConfig\":{\"vendor_id\":\"0x303a\",\"product_id\":\"0xABcd\",\"serial_number\":\"1234567890123456789012345678901\",\"manufacturer\":\"\",\"product\":\"JetKVM P4~\"}},\"id\":4321}",
        "{\"jsonrpc\":\"2.0\",\"id\":4321,\"result\":null}");
    assert(observed_usb_configuration.descriptor_change_calls == 1U);
    assert(observed_usb_configuration.last_descriptor_config.vendor_id ==
           0x303aU);
    assert(observed_usb_configuration.last_descriptor_config.product_id ==
           0xabcdU);
    assert(strcmp(
               observed_usb_configuration.last_descriptor_config.serial_number,
               "1234567890123456789012345678901") == 0);
    assert(strcmp(
               observed_usb_configuration.last_descriptor_config.manufacturer,
               "") == 0);
    assert(strcmp(observed_usb_configuration.last_descriptor_config.product,
                  "JetKVM P4~") == 0);

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDevices\",\"params\":{\"devices\":{\"absolute_mouse\":true,\"relative_mouse\":false,\"keyboard\":true,\"mass_storage\":false,\"serial_console\":true,\"audio\":false}},\"id\":4322}",
        "{\"jsonrpc\":\"2.0\",\"id\":4322,\"result\":null}");
    assert(observed_usb_configuration.devices_change_calls == 1U);
    assert(observed_usb_configuration.last_devices.absolute_mouse);
    assert(!observed_usb_configuration.last_devices.relative_mouse);
    assert(observed_usb_configuration.last_devices.keyboard);
    assert(!observed_usb_configuration.last_devices.mass_storage);
    assert(observed_usb_configuration.last_devices.serial_console);
    assert(!observed_usb_configuration.last_devices.audio);

    /* Go's encoding/json binding makes omitted bool fields false. */
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDevices\",\"params\":{\"devices\":{\"keyboard\":true}},\"id\":4323}",
        "{\"jsonrpc\":\"2.0\",\"id\":4323,\"result\":null}");
    assert(observed_usb_configuration.devices_change_calls == 2U);
    assert(!observed_usb_configuration.last_devices.absolute_mouse);
    assert(!observed_usb_configuration.last_devices.relative_mouse);
    assert(observed_usb_configuration.last_devices.keyboard);
    assert(!observed_usb_configuration.last_devices.mass_storage);
    assert(!observed_usb_configuration.last_devices.serial_console);
    assert(!observed_usb_configuration.last_devices.audio);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDevices\",\"params\":{\"devices\":{}},\"id\":4324}",
        "{\"jsonrpc\":\"2.0\",\"id\":4324,\"result\":null}");
    assert(observed_usb_configuration.devices_change_calls == 3U);
    assert(!observed_usb_configuration.last_devices.absolute_mouse);
    assert(!observed_usb_configuration.last_devices.relative_mouse);
    assert(!observed_usb_configuration.last_devices.keyboard);
    assert(!observed_usb_configuration.last_devices.mass_storage);
    assert(!observed_usb_configuration.last_devices.serial_console);
    assert(!observed_usb_configuration.last_devices.audio);

    const struct {
        const char *name;
        jetkvm_usb_device_t device;
    } usb_device_names[] = {
        { "absoluteMouse", JETKVM_USB_DEVICE_ABSOLUTE_MOUSE },
        { "relativeMouse", JETKVM_USB_DEVICE_RELATIVE_MOUSE },
        { "keyboard", JETKVM_USB_DEVICE_KEYBOARD },
        { "massStorage", JETKVM_USB_DEVICE_MASS_STORAGE },
        { "serialConsole", JETKVM_USB_DEVICE_SERIAL_CONSOLE },
        { "audio", JETKVM_USB_DEVICE_AUDIO },
    };
    for (size_t i = 0;
         i < sizeof(usb_device_names) / sizeof(usb_device_names[0]); ++i) {
        char request[192];
        char response[96];
        int id = 4330 + (int)i;
        assert(snprintf(
                   request, sizeof(request),
                   "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDeviceState\",\"params\":{\"device\":\"%s\",\"enabled\":true},\"id\":%d}",
                   usb_device_names[i].name, id) > 0);
        assert(snprintf(response, sizeof(response),
                        "{\"jsonrpc\":\"2.0\",\"id\":%d,\"result\":null}",
                        id) > 0);
        expect_rpc_context(&context, request, response);
        assert(observed_usb_configuration.last_device ==
               usb_device_names[i].device);
        assert(observed_usb_configuration.last_device_enabled);
    }
    assert(observed_usb_configuration.device_change_calls == 6U);

    /* Callback outcomes are explicit; unsupported true values are not
     * silently coerced to false. */
    observed_usb_configuration.descriptor_change_result =
        JETKVM_USB_CONFIGURATION_CHANGE_INVALID;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbConfig\",\"params\":{\"usbConfig\":{\"vendor_id\":\"0x303a\",\"product_id\":\"0x400c\",\"serial_number\":\"P4\",\"manufacturer\":\"JetKVM\",\"product\":\"KVM\"}},\"id\":4340}",
        "{\"jsonrpc\":\"2.0\",\"id\":4340,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"USB configuration change rejected invalid values\"}}");
    observed_usb_configuration.descriptor_change_result =
        JETKVM_USB_CONFIGURATION_CHANGE_COMPLETED;
    observed_usb_configuration.devices_change_result =
        JETKVM_USB_CONFIGURATION_CHANGE_UNSUPPORTED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDevices\",\"params\":{\"devices\":{\"mass_storage\":true}},\"id\":43401}",
        "{\"jsonrpc\":\"2.0\",\"id\":43401,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"USB configuration change is unsupported\"}}");
    assert(observed_usb_configuration.last_devices.mass_storage);
    observed_usb_configuration.devices_change_result =
        JETKVM_USB_CONFIGURATION_CHANGE_COMPLETED;
    observed_usb_configuration.device_change_result =
        JETKVM_USB_CONFIGURATION_CHANGE_UNSUPPORTED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDeviceState\",\"params\":{\"device\":\"audio\",\"enabled\":true},\"id\":434011}",
        "{\"jsonrpc\":\"2.0\",\"id\":434011,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"USB configuration change is unsupported\"}}");
    assert(observed_usb_configuration.last_device ==
           JETKVM_USB_DEVICE_AUDIO);
    assert(observed_usb_configuration.last_device_enabled);
    observed_usb_configuration.device_change_result =
        JETKVM_USB_CONFIGURATION_CHANGE_FAILED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDeviceState\",\"params\":{\"device\":\"audio\",\"enabled\":true},\"id\":43402}",
        "{\"jsonrpc\":\"2.0\",\"id\":43402,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB configuration change failed\"}}");
    assert(observed_usb_configuration.last_device_enabled);
    observed_usb_configuration.device_change_result =
        JETKVM_USB_CONFIGURATION_CHANGE_UNAVAILABLE;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDeviceState\",\"params\":{\"device\":\"audio\",\"enabled\":false},\"id\":43403}",
        "{\"jsonrpc\":\"2.0\",\"id\":43403,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB configuration control unavailable\"}}");
    observed_usb_configuration.device_change_result =
        JETKVM_USB_CONFIGURATION_CHANGE_BUSY;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDeviceState\",\"params\":{\"device\":\"audio\",\"enabled\":false},\"id\":43404}",
        "{\"jsonrpc\":\"2.0\",\"id\":43404,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB configuration change already in progress\"}}");
    observed_usb_configuration.device_change_result =
        JETKVM_USB_CONFIGURATION_CHANGE_COMPLETED;

    unsigned descriptor_calls_before_invalid =
        observed_usb_configuration.descriptor_change_calls;
    unsigned devices_calls_before_invalid =
        observed_usb_configuration.devices_change_calls;
    unsigned device_calls_before_invalid =
        observed_usb_configuration.device_change_calls;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbConfig\",\"params\":{\"usbConfig\":{\"vendor_id\":\"0x0000\",\"product_id\":\"0x400c\",\"serial_number\":\"P4\",\"manufacturer\":\"JetKVM\",\"product\":\"KVM\"}},\"id\":4341}",
        "{\"jsonrpc\":\"2.0\",\"id\":4341,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"usbConfig must contain valid vendor_id, product_id, serial_number, manufacturer, and product fields\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbConfig\",\"params\":{\"usbConfig\":{\"vendor_id\":\"0X303a\",\"product_id\":\"0x400c\",\"serial_number\":\"P4\",\"manufacturer\":\"JetKVM\",\"product\":\"KVM\"}},\"id\":4342}",
        "{\"jsonrpc\":\"2.0\",\"id\":4342,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"usbConfig must contain valid vendor_id, product_id, serial_number, manufacturer, and product fields\"}}");
    const char *invalid_usb_ids[] = { "0x123", "0xgggg" };
    for (size_t i = 0;
         i < sizeof(invalid_usb_ids) / sizeof(invalid_usb_ids[0]); ++i) {
        char request[384];
        char response[320];
        int id = 43420 + (int)i;
        assert(snprintf(
                   request, sizeof(request),
                   "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbConfig\",\"params\":{\"usbConfig\":{\"vendor_id\":\"%s\",\"product_id\":\"0x400c\",\"serial_number\":\"P4\",\"manufacturer\":\"JetKVM\",\"product\":\"KVM\"}},\"id\":%d}",
                   invalid_usb_ids[i], id) > 0);
        assert(snprintf(
                   response, sizeof(response),
                   "{\"jsonrpc\":\"2.0\",\"id\":%d,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"usbConfig must contain valid vendor_id, product_id, serial_number, manufacturer, and product fields\"}}",
                   id) > 0);
        expect_rpc_context(&context, request, response);
    }
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbConfig\",\"params\":{\"usbConfig\":{\"vendor_id\":1234,\"product_id\":\"0x400c\",\"serial_number\":\"P4\",\"manufacturer\":\"JetKVM\",\"product\":\"KVM\"}},\"id\":43422}",
        "{\"jsonrpc\":\"2.0\",\"id\":43422,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"usbConfig must contain valid vendor_id, product_id, serial_number, manufacturer, and product fields\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbConfig\",\"params\":{\"usbConfig\":{\"vendor_id\":\"0x303a\",\"product_id\":\"0x400c\",\"serial_number\":\"12345678901234567890123456789012\",\"manufacturer\":\"JetKVM\",\"product\":\"KVM\"}},\"id\":4343}",
        "{\"jsonrpc\":\"2.0\",\"id\":4343,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"usbConfig must contain valid vendor_id, product_id, serial_number, manufacturer, and product fields\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbConfig\",\"params\":{\"usbConfig\":{\"vendor_id\":\"0x303a\",\"product_id\":\"0x400c\",\"serial_number\":\"P4\\n\",\"manufacturer\":\"JetKVM\",\"product\":\"KVM\"}},\"id\":4344}",
        "{\"jsonrpc\":\"2.0\",\"id\":4344,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"usbConfig must contain valid vendor_id, product_id, serial_number, manufacturer, and product fields\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbConfig\",\"params\":{\"usbConfig\":{\"vendor_id\":\"0x303a\",\"product_id\":\"0x400c\",\"serial_number\":\"P4\",\"manufacturer\":\"J\\u00e9tKVM\",\"product\":\"KVM\",\"extra\":true}},\"id\":4345}",
        "{\"jsonrpc\":\"2.0\",\"id\":4345,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"usbConfig must contain valid vendor_id, product_id, serial_number, manufacturer, and product fields\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbConfig\",\"params\":{\"usbConfig\":{\"vendor_id\":\"0x303a\",\"product_id\":\"0x400c\",\"serial_number\":\"P4\",\"manufacturer\":\"J\\u00e9tKVM\",\"product\":\"KVM\"}},\"id\":43451}",
        "{\"jsonrpc\":\"2.0\",\"id\":43451,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"usbConfig must contain valid vendor_id, product_id, serial_number, manufacturer, and product fields\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbConfig\",\"params\":{\"usbConfig\":{\"vendor_id\":\"0x303a\",\"product_id\":\"0x400c\",\"serial_number\":\"P4\",\"manufacturer\":\"JetKVM\"}},\"id\":4346}",
        "{\"jsonrpc\":\"2.0\",\"id\":4346,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"usbConfig must contain valid vendor_id, product_id, serial_number, manufacturer, and product fields\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDevices\",\"params\":{\"devices\":{\"keyboard\":1}},\"id\":4347}",
        "{\"jsonrpc\":\"2.0\",\"id\":4347,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"devices must contain only boolean USB device fields\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDevices\",\"params\":{\"devices\":{\"keyboard\":true,\"unknown\":false}},\"id\":4348}",
        "{\"jsonrpc\":\"2.0\",\"id\":4348,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"devices must contain only boolean USB device fields\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDevices\",\"params\":{\"devices\":{},\"extra\":false},\"id\":43481}",
        "{\"jsonrpc\":\"2.0\",\"id\":43481,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"devices must contain only boolean USB device fields\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDeviceState\",\"params\":{\"device\":\"absolute_mouse\",\"enabled\":true},\"id\":4349}",
        "{\"jsonrpc\":\"2.0\",\"id\":4349,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"device must be a supported name and enabled must be boolean\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDeviceState\",\"params\":{\"device\":\"keyboard\",\"enabled\":1,\"extra\":false},\"id\":4350}",
        "{\"jsonrpc\":\"2.0\",\"id\":4350,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"device and enabled must be the only parameters\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDeviceState\",\"params\":{\"device\":\"keyboard\",\"enabled\":1},\"id\":43501}",
        "{\"jsonrpc\":\"2.0\",\"id\":43501,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"device must be a supported name and enabled must be boolean\"}}");
    assert(observed_usb_configuration.descriptor_change_calls ==
           descriptor_calls_before_invalid);
    assert(observed_usb_configuration.devices_change_calls ==
           devices_calls_before_invalid);
    assert(observed_usb_configuration.device_change_calls ==
           device_calls_before_invalid);

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUsbEmulationState\",\"params\":null,\"id\":433}",
        "{\"jsonrpc\":\"2.0\",\"id\":433,\"result\":true}");
    observed_usb_configuration.snapshot.emulation_enabled = false;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUsbEmulationState\",\"params\":{},\"id\":434}",
        "{\"jsonrpc\":\"2.0\",\"id\":434,\"result\":false}");
    observed_usb_configuration.snapshot.emulation_enabled = true;

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbEmulationState\",\"params\":{\"enabled\":false},\"id\":4341}",
        "{\"jsonrpc\":\"2.0\",\"id\":4341,\"result\":null}");
    assert(observed_usb_configuration.change_calls == 1U);
    assert(!observed_usb_configuration.last_enabled);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbEmulationState\",\"params\":{\"enabled\":true},\"id\":4342}",
        "{\"jsonrpc\":\"2.0\",\"id\":4342,\"result\":null}");
    assert(observed_usb_configuration.change_calls == 2U);
    assert(observed_usb_configuration.last_enabled);
    observed_usb_configuration.change_result =
        JETKVM_USB_EMULATION_CHANGE_UNAVAILABLE;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbEmulationState\",\"params\":{\"enabled\":false},\"id\":4343}",
        "{\"jsonrpc\":\"2.0\",\"id\":4343,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB emulation control unavailable\"}}");
    assert(observed_usb_configuration.change_calls == 3U);
    observed_usb_configuration.change_result = JETKVM_USB_EMULATION_CHANGE_BUSY;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbEmulationState\",\"params\":{\"enabled\":false},\"id\":4344}",
        "{\"jsonrpc\":\"2.0\",\"id\":4344,\"error\":{\"code\":-32000,\"message\":\"USB emulation control busy\",\"data\":\"Another control operation is in progress; try again\"}}");
    assert(observed_usb_configuration.change_calls == 4U);
    observed_usb_configuration.change_result =
        JETKVM_USB_EMULATION_CHANGE_ACCEPTED;

    unsigned usb_configuration_calls = observed_usb_configuration.calls;
    unsigned usb_change_calls = observed_usb_configuration.change_calls;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUsbDevices\",\"params\":{\"unexpected\":true},\"id\":435}",
        "{\"jsonrpc\":\"2.0\",\"id\":435,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"method does not accept parameters\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUsbConfig\",\"params\":[],\"id\":436}",
        "{\"jsonrpc\":\"2.0\",\"id\":436,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"method does not accept parameters\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUsbEmulationState\",\"params\":false,\"id\":437}",
        "{\"jsonrpc\":\"2.0\",\"id\":437,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"method does not accept parameters\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbEmulationState\",\"params\":{\"enabled\":1},\"id\":4371}",
        "{\"jsonrpc\":\"2.0\",\"id\":4371,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"enabled must be the only parameter and must be boolean\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbEmulationState\",\"params\":{\"enabled\":true,\"extra\":false},\"id\":4372}",
        "{\"jsonrpc\":\"2.0\",\"id\":4372,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"enabled must be the only parameter and must be boolean\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbEmulationState\",\"params\":{},\"id\":4373}",
        "{\"jsonrpc\":\"2.0\",\"id\":4373,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"enabled must be the only parameter and must be boolean\"}}");
    assert(observed_usb_configuration.calls == usb_configuration_calls);
    assert(observed_usb_configuration.change_calls == usb_change_calls);

    observed_usb_configuration.readable = false;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUsbConfig\",\"id\":438}",
        "{\"jsonrpc\":\"2.0\",\"id\":438,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB configuration unavailable\"}}");
    observed_usb_configuration.readable = true;
    observed_hid_state.usb_state = JETKVM_USB_STATE_ATTACHED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUSBState\",\"id\":44}",
        "{\"jsonrpc\":\"2.0\",\"id\":44,\"result\":\"attached\"}");
    observed_hid_state.usb_state = JETKVM_USB_STATE_ADDRESSED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUSBState\",\"id\":45}",
        "{\"jsonrpc\":\"2.0\",\"id\":45,\"result\":\"addressed\"}");
    observed_hid_state.usb_state = JETKVM_USB_STATE_SUSPENDED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUSBState\",\"id\":46}",
        "{\"jsonrpc\":\"2.0\",\"id\":46,\"result\":\"suspended\"}");
    observed_hid_state.usb_state = JETKVM_USB_STATE_NOT_ATTACHED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUSBState\",\"id\":47}",
        "{\"jsonrpc\":\"2.0\",\"id\":47,\"result\":\"not attached\"}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"wakeHost\",\"params\":{},\"id\":48}",
        "{\"jsonrpc\":\"2.0\",\"id\":48,\"result\":null}");
    assert(observed_hid_state.wake_calls == 1U);
    observed_hid_state.wake_result = JETKVM_USB_WAKE_NOOP;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"wakeHost\",\"id\":49}",
        "{\"jsonrpc\":\"2.0\",\"id\":49,\"result\":null}");
    observed_hid_state.wake_result = JETKVM_USB_WAKE_NOT_ENABLED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"wakeHost\",\"id\":50}",
        "{\"jsonrpc\":\"2.0\",\"id\":50,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB remote wakeup was not enabled by the host\"}}");
    observed_hid_state.wake_result = JETKVM_USB_WAKE_BUSY;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"wakeHost\",\"id\":51}",
        "{\"jsonrpc\":\"2.0\",\"id\":51,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB wake request already in progress\"}}");
    observed_hid_state.wake_result = JETKVM_USB_WAKE_FAILED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"wakeHost\",\"id\":52}",
        "{\"jsonrpc\":\"2.0\",\"id\":52,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB remote wakeup failed\"}}");
    observed_hid_state.wake_result = JETKVM_USB_WAKE_UNAVAILABLE;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"wakeHost\",\"id\":53}",
        "{\"jsonrpc\":\"2.0\",\"id\":53,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB wake unavailable\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"sendWOLMagicPacket\",\"params\":{\"macAddress\":\"01:23:45:67:89:ab\"},\"id\":54}",
        "{\"jsonrpc\":\"2.0\",\"id\":54,\"result\":null}");
    assert(observed_wol.calls == 1U);
    const uint8_t expected_wol_mac[] = {1, 0x23, 0x45, 0x67, 0x89, 0xab};
    assert(memcmp(observed_wol.mac, expected_wol_mac,
                  sizeof(expected_wol_mac)) == 0);
    assert(strcmp(observed_wol.broadcast, "") == 0);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"sendWOLMagicPacket\",\"params\":{\"macAddress\":\"aa:bb:cc:dd:ee:ff\",\"broadcastIP\":\"192.168.50.255\"},\"id\":55}",
        "{\"jsonrpc\":\"2.0\",\"id\":55,\"result\":null}");
    assert(observed_wol.calls == 2U);
    assert(strcmp(observed_wol.broadcast, "192.168.50.255") == 0);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"sendWOLMagicPacket\",\"params\":{\"macAddress\":\"invalid\"},\"id\":56}",
        "{\"jsonrpc\":\"2.0\",\"id\":56,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"invalid MAC address or broadcast IP\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getWakeOnLanDevices\",\"params\":{},\"id\":57}",
        "{\"jsonrpc\":\"2.0\",\"id\":57,\"result\":[{\"name\":\"Build host\",\"macAddress\":\"02:00:00:00:00:01\"}]}");
    assert(observed_wol.get_devices_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setWakeOnLanDevices\",\"params\":{\"params\":{\"devices\":[{\"name\":\"Build host\",\"macAddress\":\"02:00:00:00:00:01\"},{\"name\":\"Lab server\",\"macAddress\":\"aa:bb:cc:dd:ee:ff\",\"broadcastIP\":\"192.168.50.255\"}]}},\"id\":58}",
        "{\"jsonrpc\":\"2.0\",\"id\":58,\"result\":null}");
    assert(observed_wol.set_devices_calls == 1U);
    assert(observed_wol.devices.count == 2U);
    assert(strcmp(observed_wol.devices.devices[1].name, "Lab server") == 0);
    assert(strcmp(observed_wol.devices.devices[1].mac_address,
                  "aa:bb:cc:dd:ee:ff") == 0);
    assert(strcmp(observed_wol.devices.devices[1].broadcast_ip,
                  "192.168.50.255") == 0);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getWakeOnLanDevices\",\"id\":59}",
        "{\"jsonrpc\":\"2.0\",\"id\":59,\"result\":[{\"name\":\"Build host\",\"macAddress\":\"02:00:00:00:00:01\"},{\"name\":\"Lab server\",\"macAddress\":\"aa:bb:cc:dd:ee:ff\",\"broadcastIP\":\"192.168.50.255\"}]}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setWakeOnLanDevices\",\"params\":{\"params\":{\"devices\":[{\"name\":\"Bad\",\"macAddress\":\"invalid\"}]}},\"id\":60}",
        "{\"jsonrpc\":\"2.0\",\"id\":60,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"invalid Wake-on-LAN device\"}}");
    assert(observed_wol.set_devices_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getKeyboardMacros\",\"params\":{},\"id\":601}",
        "{\"jsonrpc\":\"2.0\",\"id\":601,\"result\":[]}");
    assert(observed_hid_profiles.get_macros_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setKeyboardMacros\",\"params\":{\"params\":{\"macros\":[{\"id\":\"e2e-f24\",\"name\":\"E2E F24\",\"sortOrder\":0,\"steps\":[{\"keys\":[\"F24\"],\"modifiers\":[\"ShiftLeft\"],\"delay\":1}] }]}},\"id\":602}",
        "{\"jsonrpc\":\"2.0\",\"id\":602,\"result\":null}");
    assert(observed_hid_profiles.set_macros_calls == 1U);
    assert(observed_hid_profiles.macros.count == 1U);
    assert(strcmp(observed_hid_profiles.macros.macros[0].id,
                  "e2e-f24") == 0);
    assert(observed_hid_profiles.macros.macros[0].steps[0].key_ids[0] ==
           71U);
    assert(observed_hid_profiles.macros.macros[0].steps[0]
               .modifier_ids[0] == 2U);
    assert(observed_hid_profiles.macros.macros[0].steps[0].delay_ms ==
           50U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getKeyboardMacros\",\"id\":603}",
        "{\"jsonrpc\":\"2.0\",\"id\":603,\"result\":[{\"id\":\"e2e-f24\",\"name\":\"E2E F24\",\"sortOrder\":0,\"steps\":[{\"keys\":[\"F24\"],\"modifiers\":[\"ShiftLeft\"],\"delay\":50}]}]}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setKeyboardMacros\",\"params\":{\"params\":{\"macros\":[{\"id\":\"bad\",\"name\":\"Bad\",\"steps\":[{\"keys\":[\"NotAKey\"],\"modifiers\":[],\"delay\":100}]}]}},\"id\":604}",
        "{\"jsonrpc\":\"2.0\",\"id\":604,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"invalid keyboard macro step\"}}");
    assert(observed_hid_profiles.set_macros_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getJigglerState\",\"id\":605}",
        "{\"jsonrpc\":\"2.0\",\"id\":605,\"result\":false}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getJigglerConfig\",\"id\":606}",
        "{\"jsonrpc\":\"2.0\",\"id\":606,\"result\":{\"inactivity_limit_seconds\":60,\"jitter_percentage\":25,\"schedule_cron_tab\":\"0 * * * * *\",\"timezone\":\"UTC\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getTimezones\",\"id\":607}",
        "{\"jsonrpc\":\"2.0\",\"id\":607,\"result\":[\"UTC\"]}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setJigglerConfig\",\"params\":{\"jigglerConfig\":{\"inactivity_limit_seconds\":2,\"jitter_percentage\":0,\"schedule_cron_tab\":\"*/2 * * * * *\",\"timezone\":\"UTC\"}},\"id\":608}",
        "{\"jsonrpc\":\"2.0\",\"id\":608,\"result\":null}");
    assert(observed_hid_profiles.set_jiggler_config_calls == 1U);
    assert(strcmp(observed_hid_profiles.jiggler_config.schedule_cron_tab,
                  "*/2 * * * * *") == 0);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setJigglerState\",\"params\":{\"enabled\":true},\"id\":609}",
        "{\"jsonrpc\":\"2.0\",\"id\":609,\"result\":null}");
    assert(observed_hid_profiles.jiggler_enabled);
    observed_hid_profiles.config_change_result =
        JETKVM_JIGGLER_CONFIG_CHANGE_INVALID;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setJigglerConfig\",\"params\":{\"jigglerConfig\":{\"inactivity_limit_seconds\":2,\"jitter_percentage\":0,\"schedule_cron_tab\":\"bad cron\",\"timezone\":\"UTC\"}},\"id\":610}",
        "{\"jsonrpc\":\"2.0\",\"id\":610,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"invalid cron or timezone\"}}");
    observed_hid_profiles.config_change_result =
        JETKVM_JIGGLER_CONFIG_CHANGE_ACCEPTED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getStreamQualityFactor\",\"id\":5}",
        "{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":0.5}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getVideoCodecPreference\",\"id\":6}",
        "{\"jsonrpc\":\"2.0\",\"id\":6,\"result\":\"auto\"}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getEDID\",\"id\":60}",
        "{\"jsonrpc\":\"2.0\",\"id\":60,\"result\":\"" TEST_EDID "\"}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getSupportedVideoCodecs\",\"id\":65}",
        "{\"jsonrpc\":\"2.0\",\"id\":65,\"result\":[\"h264\"]}");
    assert(observed_edid.get_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getEDIDPresets\",\"id\":604}",
        "{\"jsonrpc\":\"2.0\",\"id\":604,\"result\":[{\"name\":\"JetKVM Default\",\"edid\":\"" TEST_EDID "\"}]}");
    observed_edid.readable = false;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getEDIDPresets\",\"id\":605}",
        "{\"jsonrpc\":\"2.0\",\"id\":605,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"EDID presets unavailable\"}}");
    observed_edid.readable = true;

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setEDID\",\"params\":{\"edid\":\"" TEST_EDID "\"},\"id\":601}",
        "{\"jsonrpc\":\"2.0\",\"id\":601,\"result\":null}");
    assert(observed_edid.set_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setEDID\",\"params\":{\"edid\":\"xyz\"},\"id\":602}",
        "{\"jsonrpc\":\"2.0\",\"id\":602,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"edid must contain one or two 128-byte blocks\"}}");
    assert(observed_edid.set_calls == 1U);
    observed_edid.result = JETKVM_EDID_CHANGE_BUSY;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setEDID\",\"params\":{\"edid\":\"" TEST_EDID "\"},\"id\":603}",
        "{\"jsonrpc\":\"2.0\",\"id\":603,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"EDID control is busy\"}}");
    observed_edid.result = JETKVM_EDID_CHANGE_ACCEPTED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getHostDisplayIdleMode\",\"id\":604}",
        "{\"jsonrpc\":\"2.0\",\"id\":604,\"result\":{\"enabled\":false}}");
    assert(observed_host_display_idle.get_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setHostDisplayIdleMode\",\"params\":{\"enabled\":true},\"id\":605}",
        "{\"jsonrpc\":\"2.0\",\"id\":605,\"result\":null}");
    assert(observed_host_display_idle.set_calls == 1U);
    assert(observed_host_display_idle.enabled);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getHostDisplayIdleMode\",\"id\":606}",
        "{\"jsonrpc\":\"2.0\",\"id\":606,\"result\":{\"enabled\":true}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setHostDisplayIdleMode\",\"params\":{\"enabled\":1},\"id\":607}",
        "{\"jsonrpc\":\"2.0\",\"id\":607,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"enabled must be a boolean\"}}");
    assert(observed_host_display_idle.set_calls == 1U);
    observed_host_display_idle.result =
        JETKVM_HOST_DISPLAY_IDLE_CHANGE_FAILED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setHostDisplayIdleMode\",\"params\":{\"enabled\":false},\"id\":608}",
        "{\"jsonrpc\":\"2.0\",\"id\":608,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Failed to save host display idle mode\"}}");
    observed_host_display_idle.result =
        JETKVM_HOST_DISPLAY_IDLE_CHANGE_ACCEPTED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getVideoSleepMode\",\"id\":609}",
        "{\"jsonrpc\":\"2.0\",\"id\":609,\"result\":{\"supported\":true,\"enabled\":false,\"duration\":-1}}"
    );
    assert(observed_video_sleep.get_calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setVideoSleepMode\",\"params\":{\"duration\":90},\"id\":610}",
        "{\"jsonrpc\":\"2.0\",\"id\":610,\"result\":null}"
    );
    assert(observed_video_sleep.set_calls == 1U);
    assert(observed_video_sleep.duration == 90);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setVideoSleepMode\",\"params\":{\"duration\":-20},\"id\":611}",
        "{\"jsonrpc\":\"2.0\",\"id\":611,\"result\":null}"
    );
    assert(observed_video_sleep.set_calls == 2U);
    assert(observed_video_sleep.duration == -1);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setVideoSleepMode\",\"params\":{\"duration\":1.5},\"id\":612}",
        "{\"jsonrpc\":\"2.0\",\"id\":612,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"duration must be an integer number of seconds\"}}"
    );
    assert(observed_video_sleep.set_calls == 2U);
    observed_video_sleep.result = JETKVM_VIDEO_SLEEP_CHANGE_FAILED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setVideoSleepMode\",\"params\":{\"duration\":90},\"id\":613}",
        "{\"jsonrpc\":\"2.0\",\"id\":613,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Failed to save video sleep mode\"}}"
    );
    observed_video_sleep.result = JETKVM_VIDEO_SLEEP_CHANGE_ACCEPTED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setVideoCodecPreference\",\"params\":{\"codec\":\"h264\"},\"id\":61}",
        "{\"jsonrpc\":\"2.0\",\"id\":61,\"result\":null}");
    assert(observed_video_codec.calls == 1U);
    assert(strcmp(observed_video_codec.codec, "h264") == 0);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setVideoCodecPreference\",\"params\":{\"codec\":\"h265\"},\"id\":62}",
        "{\"jsonrpc\":\"2.0\",\"id\":62,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"H.265 is unsupported on ESP32-P4\"}}");
    assert(observed_video_codec.calls == 1U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setVideoCodecPreference\",\"params\":{\"codec\":\"vp9\"},\"id\":63}",
        "{\"jsonrpc\":\"2.0\",\"id\":63,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"codec must be auto, h264, or h265\"}}");
    observed_video_codec.result = JETKVM_VIDEO_CODEC_CHANGE_FAILED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setVideoCodecPreference\",\"params\":{\"codec\":\"auto\"},\"id\":64}",
        "{\"jsonrpc\":\"2.0\",\"id\":64,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Failed to save video codec preference\"}}");
    observed_video_codec.result = JETKVM_VIDEO_CODEC_CHANGE_ACCEPTED;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setVideoStreamPaused\",\"params\":{\"paused\":true},\"id\":65}",
        "{\"jsonrpc\":\"2.0\",\"id\":65,\"result\":null}");
    assert(observed_video_stream_pause.calls == 1U);
    assert(observed_video_stream_pause.paused);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setVideoStreamPaused\",\"params\":{\"paused\":false,\"extra\":true},\"id\":66}",
        "{\"jsonrpc\":\"2.0\",\"id\":66,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"paused must be the only parameter and must be boolean\"}}");
    assert(observed_video_stream_pause.calls == 1U);
    observed_video_stream_pause.available = false;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setVideoStreamPaused\",\"params\":{\"paused\":false},\"id\":67}",
        "{\"jsonrpc\":\"2.0\",\"id\":67,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Video stream pause control unavailable\"}}");
    observed_video_stream_pause.available = true;
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setStreamQualityFactor\",\"params\":{\"factor\":0.5},\"id\":7}",
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":null}");
    assert(observed_factor == 0.5);
    /* 0 is Auto and reaches the adapter; negative and NaN never do. */
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setStreamQualityFactor\",\"params\":{\"factor\":0},\"id\":72}",
        "{\"jsonrpc\":\"2.0\",\"id\":72,\"result\":null}");
    assert(observed_factor == 0.0);
    observed_factor = 0.5;
    jetkvm_control_context_t auto_context = context;
    auto_context.stream_quality_factor = 0.0;
    expect_rpc_context(
        &auto_context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"getStreamQualityFactor\",\"id\":74}",
        "{\"jsonrpc\":\"2.0\",\"id\":74,\"result\":0}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setStreamQualityFactor\",\"params\":{\"factor\":-0.5},\"id\":73}",
        "{\"jsonrpc\":\"2.0\",\"id\":73,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"factor must be 0 (Auto) or a positive finite number\"}}");
    assert(observed_factor == 0.5);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setCloudUrl\",\"params\":{\"apiUrl\":\"https://new-cloud.example\",\"appUrl\":\"https://new-app.example\"},\"id\":71}",
        "{\"jsonrpc\":\"2.0\",\"id\":71,\"result\":null}");
    assert(strcmp(observed_cloud.api_url,
                  "https://new-cloud.example") == 0);
    assert(strcmp(observed_cloud.app_url,
                  "https://new-app.example") == 0);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"keyboardReport\",\"params\":{\"modifier\":2,\"keys\":[4,5,0,0,0,0]},\"id\":72}",
        "{\"jsonrpc\":\"2.0\",\"id\":72,\"result\":null}");
    assert(observed_hid.type == JETKVM_HIDRPC_EVENT_KEYBOARD_REPORT);
    assert(observed_hid.keyboard.modifier == 2U);
    assert(observed_hid.keyboard.keys[0] == 4U);
    assert(observed_hid.keyboard.keys[1] == 5U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"keypressReport\",\"params\":{\"key\":225,\"press\":true},\"id\":73}",
        "{\"jsonrpc\":\"2.0\",\"id\":73,\"result\":null}");
    assert(observed_hid.type == JETKVM_HIDRPC_EVENT_KEYPRESS_REPORT);
    assert(observed_hid.keypress.key == 225U);
    assert(observed_hid.keypress.pressed);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"absMouseReport\",\"params\":{\"x\":32767,\"y\":-2147483648,\"buttons\":5},\"id\":74}",
        "{\"jsonrpc\":\"2.0\",\"id\":74,\"result\":null}");
    assert(observed_hid.type == JETKVM_HIDRPC_EVENT_ABS_MOUSE_REPORT);
    assert(observed_hid.abs_mouse.x == 32767);
    assert(observed_hid.abs_mouse.y == INT32_MIN);
    assert(observed_hid.abs_mouse.buttons == 5U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"relMouseReport\",\"params\":{\"dx\":-128,\"dy\":127,\"buttons\":31},\"id\":75}",
        "{\"jsonrpc\":\"2.0\",\"id\":75,\"result\":null}");
    assert(observed_hid.type == JETKVM_HIDRPC_EVENT_REL_MOUSE_REPORT);
    assert(observed_hid.rel_mouse.dx == INT8_MIN);
    assert(observed_hid.rel_mouse.dy == INT8_MAX);
    assert(observed_hid.rel_mouse.buttons == 31U);
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"wheelReport\",\"params\":{\"wheelY\":-127,\"wheelX\":127},\"id\":76}",
        "{\"jsonrpc\":\"2.0\",\"id\":76,\"result\":null}");
    assert(observed_hid.type == JETKVM_HIDRPC_EVENT_WHEEL_REPORT);
    assert(observed_hid.wheel.wheel_y == -127);
    assert(observed_hid.wheel.wheel_x == 127);

    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"keyboardReport\",\"params\":{\"modifier\":256,\"keys\":[4,0,0,0,0,0]},\"id\":80}",
        "{\"jsonrpc\":\"2.0\",\"id\":80,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"modifier must be uint8 and keys must contain 6 uint8 values\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"keyboardReport\",\"params\":{\"modifier\":0,\"keys\":[4,0,0,0,0]},\"id\":81}",
        "{\"jsonrpc\":\"2.0\",\"id\":81,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"modifier must be uint8 and keys must contain 6 uint8 values\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"keyboardReport\",\"params\":{\"modifier\":0,\"keys\":[4,0,0,0,0,300]},\"id\":82}",
        "{\"jsonrpc\":\"2.0\",\"id\":82,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"modifier must be uint8 and keys must contain 6 uint8 values\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"keypressReport\",\"params\":{\"key\":4,\"press\":1},\"id\":83}",
        "{\"jsonrpc\":\"2.0\",\"id\":83,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"key must be uint8 and press must be boolean\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"absMouseReport\",\"params\":{\"x\":2147483648,\"y\":0,\"buttons\":0},\"id\":84}",
        "{\"jsonrpc\":\"2.0\",\"id\":84,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"x/y must be int32 and buttons must be uint8\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"relMouseReport\",\"params\":{\"dx\":128,\"dy\":0,\"buttons\":0},\"id\":85}",
        "{\"jsonrpc\":\"2.0\",\"id\":85,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"dx/dy must be int8 and buttons must be uint8\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"wheelReport\",\"params\":{\"wheelY\":-129,\"wheelX\":0},\"id\":86}",
        "{\"jsonrpc\":\"2.0\",\"id\":86,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"wheelY/wheelX must be int8\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"wheelReport\",\"params\":{\"wheelY\":1.5,\"wheelX\":0},\"id\":87}",
        "{\"jsonrpc\":\"2.0\",\"id\":87,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"wheelY/wheelX must be int8\"}}");
    expect_rpc_context(
        &context,
        "{\"jsonrpc\":\"2.0\",\"method\":\"setStreamQualityFactor\",\"params\":{\"factor\":0.1},\"id\":8}",
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"Unsupported stream quality factor\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"setStreamQualityFactor\",\"params\":{\"factor\":1},\"id\":9}",
        "{\"jsonrpc\":\"2.0\",\"id\":9,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Stream quality control unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"setVideoCodecPreference\",\"params\":{\"codec\":\"auto\"},\"id\":91}",
        "{\"jsonrpc\":\"2.0\",\"id\":91,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Video codec preference unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"keyboardReport\",\"id\":10}",
        "{\"jsonrpc\":\"2.0\",\"id\":10,\"error\":{\"code\":-32602,\"message\":\"Invalid params\",\"data\":\"modifier must be uint8 and keys must contain 6 uint8 values\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"getKeyboardLedState\",\"id\":11}",
        "{\"jsonrpc\":\"2.0\",\"id\":11,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Keyboard LED state unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"getKeyboardLayout\",\"id\":12}",
        "{\"jsonrpc\":\"2.0\",\"id\":12,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Keyboard layout unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"wakeHost\",\"id\":121}",
        "{\"jsonrpc\":\"2.0\",\"id\":121,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB wake unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUsbDevices\",\"id\":1211}",
        "{\"jsonrpc\":\"2.0\",\"id\":1211,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB configuration unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUsbConfig\",\"id\":1212}",
        "{\"jsonrpc\":\"2.0\",\"id\":1212,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB configuration unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"getUsbEmulationState\",\"id\":1213}",
        "{\"jsonrpc\":\"2.0\",\"id\":1213,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB configuration unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbEmulationState\",\"params\":{\"enabled\":true},\"id\":1214}",
        "{\"jsonrpc\":\"2.0\",\"id\":1214,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB emulation control unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbConfig\",\"params\":{\"usbConfig\":{\"vendor_id\":\"0x303a\",\"product_id\":\"0x400c\",\"serial_number\":\"P4\",\"manufacturer\":\"JetKVM\",\"product\":\"KVM\"}},\"id\":1215}",
        "{\"jsonrpc\":\"2.0\",\"id\":1215,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB configuration control unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDevices\",\"params\":{\"devices\":{\"keyboard\":true}},\"id\":1216}",
        "{\"jsonrpc\":\"2.0\",\"id\":1216,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB configuration control unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"setUsbDeviceState\",\"params\":{\"device\":\"keyboard\",\"enabled\":true},\"id\":1217}",
        "{\"jsonrpc\":\"2.0\",\"id\":1217,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"USB configuration control unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"reboot\",\"params\":{\"force\":true},\"id\":122}",
        "{\"jsonrpc\":\"2.0\",\"id\":122,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Reboot unavailable\"}}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"getVideoLogStatus\",\"id\":123}",
        "{\"jsonrpc\":\"2.0\",\"id\":123,\"error\":{\"code\":-32603,\"message\":\"Internal error\",\"data\":\"Video debugging information unavailable\"}}");

    char notification_output[32];
    size_t output_size = 99;
    const char *notification = "{\"jsonrpc\":\"2.0\",\"method\":\"ping\"}";
    assert(jetkvm_control_handle_rpc(
        (const uint8_t *)notification, strlen(notification), notification_output,
        sizeof(notification_output), &output_size) ==
        JETKVM_CONTROL_NO_RESPONSE);
    assert(output_size == 0);

    char tiny_output[8];
    output_size = 99;
    assert(jetkvm_control_handle_rpc(
        (const uint8_t *)"not-json", strlen("not-json"), tiny_output,
        sizeof(tiny_output), &output_size) == JETKVM_CONTROL_OUTPUT_TOO_SMALL);
    assert(output_size == 0);

    /* Replies print straight into the buffer when cJSON has a byte to
     * spare and through a copy when they only just fit; both give the same
     * bytes, and the smallest buffer that holds a reply is unchanged. */
    static const char pong_request[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":1}";
    static const char pong[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":\"pong\"}";
    for (size_t capacity = sizeof(pong) - 1U; capacity <= sizeof(pong) + 1U;
         ++capacity) {
        char exact[sizeof(pong) + 1U];
        output_size = 99;
        jetkvm_control_result_t rendered = jetkvm_control_handle_rpc(
            (const uint8_t *)pong_request, strlen(pong_request), exact,
            capacity, &output_size);
        if (capacity < sizeof(pong)) {
            assert(rendered == JETKVM_CONTROL_OUTPUT_TOO_SMALL);
            assert(output_size == 0);
        } else {
            assert(rendered == JETKVM_CONTROL_RESPONSE_READY);
            assert(output_size == strlen(pong) && strcmp(exact, pong) == 0);
        }
    }

    /* Application composition can register the persistent provider once
     * before WebRTC starts; per-request callbacks still take precedence. */
    const jetkvm_keyboard_layout_context_t default_layout = {
        .get = get_keyboard_layout,
        .set = set_keyboard_layout,
        .ctx = &observed_layout,
    };
    jetkvm_control_set_default_keyboard_layout_context(&default_layout);
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"getKeyboardLayout\",\"id\":13}",
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"result\":\"en-US\"}");
    expect_rpc(
        "{\"jsonrpc\":\"2.0\",\"method\":\"setKeyboardLayout\",\"params\":{\"layout\":\"de-DE\"},\"id\":14}",
        "{\"jsonrpc\":\"2.0\",\"id\":14,\"result\":null}");
    assert(strcmp(observed_layout.layout, "de-DE") == 0);
    jetkvm_control_set_default_keyboard_layout_context(NULL);

    test_local_display();
    puts("JetKVM control protocol vectors: OK");
    return 0;
}
