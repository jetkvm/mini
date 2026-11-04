#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "device_contract.h"

static cJSON *parse_contract(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    assert(root != NULL);
    assert(cJSON_IsObject(root));
    return root;
}

static bool json_bool(cJSON *root, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    assert(cJSON_IsBool(item));
    return cJSON_IsTrue(item);
}

int main(void)
{
    char status_json[512];
    for (unsigned setup = 0; setup < 2; ++setup) {
        assert(p4_device_status_json(setup, false, NULL, "jetkvm-mini-ethernet", status_json,
                                     sizeof(status_json)));
        cJSON *status = parse_contract(status_json);
        assert(json_bool(status, "isSetup") == (bool)setup);
        assert(strcmp(cJSON_GetObjectItem(status, "sku")->valuestring, "jetkvm-mini-ethernet") == 0);
        assert(!json_bool(status, "factoryResetPending"));
        assert(!cJSON_GetObjectItem(status, "factoryResetError"));
        cJSON_Delete(status);
    }
    assert(p4_device_status_json(true, true, "TF cleanup failed", "jetkvm-mini-wireless",
                                 status_json, sizeof(status_json)));
    cJSON *status = parse_contract(status_json);
    assert(json_bool(status, "factoryResetPending"));
    assert(strcmp(cJSON_GetObjectItem(status, "factoryResetError")->valuestring, "TF cleanup failed") == 0);
    cJSON_Delete(status);
    assert(!p4_device_status_json(false, false, NULL, "jetkvm-mini-wireless", status_json, 4));
    assert(!p4_device_status_json(false, false, NULL, NULL, status_json, sizeof(status_json)));

    char identity[96];
    assert(p4_device_identity_json("e8f60a01b2cf", false, identity,
                                   sizeof(identity)));
    cJSON *device = parse_contract(identity);
    assert(strcmp(cJSON_GetObjectItem(device, "authMode")->valuestring,
                  "noPassword") == 0);
    assert(strcmp(cJSON_GetObjectItem(device, "deviceId")->valuestring,
                  "e8f60a01b2cf") == 0);
    cJSON_Delete(device);
    assert(!p4_device_identity_json("bad\"id", false, identity, sizeof(identity)));
    assert(!p4_device_identity_json("e8f60a01b2cf", false, identity, 8U));

    assert(p4_device_identity_json("e8f60a01b2cf", true, identity, sizeof(identity)));
    device = parse_contract(identity);
    assert(strcmp(cJSON_GetObjectItem(device, "authMode")->valuestring, "password") == 0);
    cJSON_Delete(device);

    char cloud_state[640];
    assert(p4_cloud_state_json(
        false, "https://api.jetkvm.com", "https://app.jetkvm.com",
        cloud_state, sizeof(cloud_state)));
    cJSON *cloud = parse_contract(cloud_state);
    assert(!json_bool(cloud, "connected"));
    assert(strcmp(cJSON_GetObjectItem(cloud, "url")->valuestring,
                  "https://api.jetkvm.com") == 0);
    assert(strcmp(cJSON_GetObjectItem(cloud, "appUrl")->valuestring,
                  "https://app.jetkvm.com") == 0);
    cJSON_Delete(cloud);
    assert(p4_cloud_state_json(true, "https://cloud.example/\"quoted",
                               "https://app.example", cloud_state,
                               sizeof(cloud_state)));
    cloud = parse_contract(cloud_state);
    assert(json_bool(cloud, "connected"));
    assert(strcmp(cJSON_GetObjectItem(cloud, "url")->valuestring,
                  "https://cloud.example/\"quoted") == 0);
    cJSON_Delete(cloud);

    cJSON *capabilities = parse_contract(p4_device_capabilities_json());
    assert(strcmp(cJSON_GetObjectItem(capabilities, "signaling")->valuestring,
                  "p4-http-offer-v1") == 0);
    assert(json_bool(capabilities, "rpc"));
    cJSON *video = cJSON_GetObjectItem(capabilities, "video");
    assert(cJSON_IsObject(video));
    assert(json_bool(video, "h264"));
    assert(!json_bool(video, "h265"));
    assert(!json_bool(video, "audio"));
    assert(json_bool(video, "qualityControl"));
    assert(!json_bool(video, "edidControl"));
    assert(!json_bool(video, "hostDisplayIdleControl"));
    assert(!json_bool(video, "sleepControl"));
    assert(!json_bool(video, "customEdid"));
    assert(json_bool(capabilities, "cloud"));
    cJSON *access = cJSON_GetObjectItemCaseSensitive(capabilities, "access");
    assert(cJSON_IsObject(access));
    assert(!json_bool(access, "tls"));
    assert(!json_bool(access, "localAuth"));
    assert(!json_bool(access, "cloudDeregister"));
    cJSON *modes = cJSON_GetObjectItem(video, "modes");
    assert(cJSON_GetArraySize(modes) == 6);
    cJSON *mode_1080 = cJSON_GetArrayItem(modes, 0);
    cJSON *mode_720_30 = cJSON_GetArrayItem(modes, 1);
    cJSON *mode_720_60 = cJSON_GetArrayItem(modes, 2);
    assert(cJSON_GetObjectItem(mode_1080, "width")->valueint == 1920);
    assert(cJSON_GetObjectItem(mode_1080, "height")->valueint == 1080);
    assert(cJSON_GetObjectItem(mode_1080, "fps")->valueint == 30);
    assert(cJSON_GetObjectItem(mode_720_30, "width")->valueint == 1280);
    assert(cJSON_GetObjectItem(mode_720_30, "height")->valueint == 720);
    assert(cJSON_GetObjectItem(mode_720_30, "fps")->valueint == 30);
    assert(cJSON_GetObjectItem(mode_720_60, "width")->valueint == 1280);
    assert(cJSON_GetObjectItem(mode_720_60, "height")->valueint == 720);
    assert(cJSON_GetObjectItem(mode_720_60, "fps")->valueint == 60);
    const int lower_modes[][2] = {{1024, 768}, {1280, 800}, {1280, 1024}};
    for (int i = 0; i < 3; ++i) {
        cJSON *mode = cJSON_GetArrayItem(modes, i + 3);
        assert(cJSON_GetObjectItem(mode, "width")->valueint == lower_modes[i][0]);
        assert(cJSON_GetObjectItem(mode, "height")->valueint == lower_modes[i][1]);
        assert(cJSON_GetObjectItem(mode, "fps")->valueint == 30);
    }
    const char *disabled[] = {
        "hid", "keyboardLayout", "hidMacros", "paste", "mouseJiggler",
        "usbWake", "virtualMediaHttp",
        "hidUnreliable", "terminal", "serial",
        "extensions", "ota", "networkSettings", "mqtt",
        "wakeOnLan",
        "usbConfiguration",
        "reboot",
    };
    for (size_t i = 0; i < sizeof(disabled) / sizeof(disabled[0]); ++i) {
        assert(!json_bool(capabilities, disabled[i]));
    }
    assert(json_bool(capabilities, "virtualMedia"));
    cJSON_Delete(capabilities);

    assert(!p4_device_hid_available());
    assert(!p4_device_keyboard_layout_available());
    assert(!p4_device_usb_wake_available());
    assert(!p4_device_usb_configuration_available());
    assert(!p4_device_virtual_media_http_available());
    assert(!p4_device_reboot_available());
    assert(!p4_device_wake_on_lan_available());
    assert(!p4_device_edid_control_available());
    assert(!p4_device_host_display_idle_control_available());
    assert(!p4_device_video_sleep_control_available());
    p4_device_set_edid_control_available(true);
    capabilities = parse_contract(p4_device_capabilities_json());
    video = cJSON_GetObjectItem(capabilities, "video");
    assert(json_bool(video, "edidControl"));
    assert(!json_bool(video, "hostDisplayIdleControl"));
    assert(!json_bool(video, "customEdid"));
    cJSON_Delete(capabilities);
    p4_device_set_edid_control_available(false);
    p4_device_set_host_display_idle_control_available(true);
    capabilities = parse_contract(p4_device_capabilities_json());
    video = cJSON_GetObjectItem(capabilities, "video");
    assert(!json_bool(video, "edidControl"));
    assert(json_bool(video, "hostDisplayIdleControl"));
    assert(!json_bool(video, "customEdid"));
    cJSON_Delete(capabilities);
    p4_device_set_host_display_idle_control_available(false);
    p4_device_set_video_sleep_control_available(true);
    capabilities = parse_contract(p4_device_capabilities_json());
    video = cJSON_GetObjectItem(capabilities, "video");
    assert(json_bool(video, "sleepControl"));
    assert(!json_bool(video, "customEdid"));
    cJSON_Delete(capabilities);
    p4_device_set_video_sleep_control_available(false);
    p4_device_set_reboot_available(true);
    capabilities = parse_contract(p4_device_capabilities_json());
    assert(json_bool(capabilities, "reboot"));
    assert(!json_bool(capabilities, "ota"));
    cJSON_Delete(capabilities);
    char capabilities_copy[P4_DEVICE_CAPABILITIES_JSON_CAPACITY];
    assert(p4_device_capabilities_json_copy(
        capabilities_copy, sizeof(capabilities_copy)));
    capabilities = parse_contract(capabilities_copy);
    assert(json_bool(capabilities, "reboot"));
    cJSON_Delete(capabilities);
    assert(!p4_device_capabilities_json_copy(capabilities_copy, 8U));
    p4_device_set_reboot_available(false);

    p4_device_set_wake_on_lan_available(true);
    capabilities = parse_contract(p4_device_capabilities_json());
    assert(json_bool(capabilities, "wakeOnLan"));
    assert(!json_bool(capabilities, "networkSettings"));
    cJSON_Delete(capabilities);
    p4_device_set_wake_on_lan_available(false);

    p4_device_set_virtual_media_http_available(true);
    capabilities = parse_contract(p4_device_capabilities_json());
    assert(json_bool(capabilities, "virtualMediaHttp"));
    cJSON_Delete(capabilities);
    p4_device_set_virtual_media_http_available(false);

    p4_device_set_mqtt_available(true);
    capabilities = parse_contract(p4_device_capabilities_json());
    assert(json_bool(capabilities, "mqtt"));
    cJSON_Delete(capabilities);
    p4_device_set_mqtt_available(false);

    p4_device_set_ota_available(true);
    capabilities = parse_contract(p4_device_capabilities_json());
    assert(json_bool(capabilities, "ota"));
    assert(!json_bool(capabilities, "networkSettings"));
    cJSON_Delete(capabilities);
    p4_device_set_ota_available(false);
    p4_device_set_usb_wake_available(true);
    capabilities = parse_contract(p4_device_capabilities_json());
    assert(!json_bool(capabilities, "hid"));
    assert(!json_bool(capabilities, "keyboardLayout"));
    assert(json_bool(capabilities, "usbWake"));
    cJSON_Delete(capabilities);
    p4_device_set_usb_wake_available(false);
    p4_device_set_reboot_available(false);

    p4_device_set_usb_configuration_available(true);
    capabilities = parse_contract(p4_device_capabilities_json());
    assert(json_bool(capabilities, "usbConfiguration"));
    cJSON_Delete(capabilities);
    p4_device_set_usb_configuration_available(false);

    p4_device_set_hid_available(true);
    capabilities = parse_contract(p4_device_capabilities_json());
    assert(json_bool(capabilities, "hid"));
    assert(!json_bool(capabilities, "keyboardLayout"));
    assert(!json_bool(capabilities, "hidMacros"));
    assert(json_bool(capabilities, "paste"));
    assert(!json_bool(capabilities, "usbWake"));
    assert(!json_bool(capabilities, "hidUnreliable"));
    cJSON_Delete(capabilities);

    p4_device_set_usb_wake_available(true);
    assert(p4_device_usb_wake_available());
    capabilities = parse_contract(p4_device_capabilities_json());
    assert(json_bool(capabilities, "hid"));
    assert(json_bool(capabilities, "usbWake"));
    cJSON_Delete(capabilities);

    p4_device_set_keyboard_layout_available(true);
    assert(p4_device_keyboard_layout_available());
    capabilities = parse_contract(p4_device_capabilities_json());
    assert(json_bool(capabilities, "hid"));
    assert(json_bool(capabilities, "keyboardLayout"));
    assert(!json_bool(capabilities, "hidMacros"));
    assert(json_bool(capabilities, "paste"));
    assert(json_bool(capabilities, "usbWake"));
    cJSON_Delete(capabilities);

    p4_device_set_hid_macros_available(true);
    p4_device_set_mouse_jiggler_available(true);
    assert(p4_device_hid_macros_available());
    assert(p4_device_mouse_jiggler_available());
    capabilities = parse_contract(p4_device_capabilities_json());
    assert(json_bool(capabilities, "hidMacros"));
    assert(json_bool(capabilities, "mouseJiggler"));
    cJSON_Delete(capabilities);

    p4_device_set_hid_available(false);
    capabilities = parse_contract(p4_device_capabilities_json());
    assert(!json_bool(capabilities, "hid"));
    assert(!json_bool(capabilities, "paste"));
    assert(json_bool(capabilities, "keyboardLayout"));
    assert(json_bool(capabilities, "usbWake"));
    cJSON_Delete(capabilities);
    p4_device_set_keyboard_layout_available(false);
    p4_device_set_hid_macros_available(false);
    p4_device_set_mouse_jiggler_available(false);
    p4_device_set_hid_available(false);
    p4_device_set_usb_wake_available(false);
    assert(!p4_device_hid_available());
    assert(!p4_device_keyboard_layout_available());
    assert(!p4_device_hid_macros_available());
    assert(!p4_device_mouse_jiggler_available());
    assert(!p4_device_usb_wake_available());
    assert(!p4_device_usb_configuration_available());
    assert(!p4_device_reboot_available());
    assert(!p4_device_wake_on_lan_available());
    assert(!p4_device_edid_control_available());
    assert(!p4_device_host_display_idle_control_available());
    assert(!p4_device_video_sleep_control_available());
    puts("P4 device API contracts: OK");
    return 0;
}
