/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

/* JSON-RPC control-protocol adapters for WOL, keyboard macros, jiggler,
 * cloud URLs, USB configuration, virtual media, video quality/codec, and the
 * network snapshot.  The only transport state they may touch is documented
 * per function. */

#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "jetkvm_control.h"
#include "jetkvm_device_state.h"
#include "jetkvm_hid_profiles.h"
#include "jetkvm_settings.h"
#include "jetkvm_usb_service.h"
#include "jetkvm_video_quality.h"
#include "jetkvm_virtual_media.h"
#include "jetkvm_wol.h"

#include "webrtc_transport_ctx_internal.h"
#include "webrtc_rpc_adapters_internal.h"

static const char *TAG = "webrtc_transport";

bool rpc_send_wol(const uint8_t mac[6], const char *broadcast_ip,
                         void *ctx)
{
    (void)ctx;
    uint8_t packet[6U + 16U * 6U];
    memset(packet, 0xff, 6U);
    for (size_t i = 0U; i < 16U; ++i) {
        memcpy(packet + 6U + i * 6U, mac, 6U);
    }
    const char *target_ip = broadcast_ip != NULL && broadcast_ip[0] != '\0' ?
        broadcast_ip : "255.255.255.255";
    struct sockaddr_in target = {
        .sin_family = AF_INET,
        .sin_port = htons(9U),
    };
    if (inet_pton(AF_INET, target_ip, &target.sin_addr) != 1) {
        return false;
    }
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) {
        return false;
    }
    int enabled = 1;
    bool sent = setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST,
                           &enabled, sizeof(enabled)) == 0 &&
        sendto(socket_fd, packet, sizeof(packet), 0,
               (const struct sockaddr *)&target, sizeof(target)) ==
            (ssize_t)sizeof(packet);
    close(socket_fd);
    return sent;
}

bool rpc_get_wol_devices_adapter(
    jetkvm_control_wol_device_list_t *output, void *ctx)
{
    (void)ctx;
    if (output == NULL) {
        return false;
    }
    jetkvm_wol_device_list_t stored = {0};
    if (jetkvm_wol_get_devices(&stored) != ESP_OK ||
        stored.count > JETKVM_CONTROL_WOL_MAX_DEVICES) {
        return false;
    }
    memset(output, 0, sizeof(*output));
    output->count = stored.count;
    for (size_t i = 0U; i < stored.count; ++i) {
        memcpy(output->devices[i].name, stored.devices[i].name,
               sizeof(output->devices[i].name));
        memcpy(output->devices[i].mac_address,
               stored.devices[i].mac_address,
               sizeof(output->devices[i].mac_address));
        memcpy(output->devices[i].broadcast_ip,
               stored.devices[i].broadcast_ip,
               sizeof(output->devices[i].broadcast_ip));
    }
    return true;
}

bool rpc_set_wol_devices_adapter(
    const jetkvm_control_wol_device_list_t *input, void *ctx)
{
    (void)ctx;
    if (input == NULL || input->count > JETKVM_WOL_MAX_DEVICES) {
        return false;
    }
    jetkvm_wol_device_list_t stored = {.count = input->count};
    for (size_t i = 0U; i < input->count; ++i) {
        memcpy(stored.devices[i].name, input->devices[i].name,
               sizeof(stored.devices[i].name));
        memcpy(stored.devices[i].mac_address,
               input->devices[i].mac_address,
               sizeof(stored.devices[i].mac_address));
        memcpy(stored.devices[i].broadcast_ip,
               input->devices[i].broadcast_ip,
               sizeof(stored.devices[i].broadcast_ip));
    }
    return jetkvm_wol_set_devices(&stored) == ESP_OK;
}

bool rpc_get_keyboard_macros_adapter(
    jetkvm_control_keyboard_macro_list_t *output, void *ctx)
{
    (void)ctx;
    return output != NULL &&
           jetkvm_hid_profiles_get_macros(output) == ESP_OK;
}

bool rpc_set_keyboard_macros_adapter(
    const jetkvm_control_keyboard_macro_list_t *input, void *ctx)
{
    (void)ctx;
    return input != NULL &&
           jetkvm_hid_profiles_set_macros(input) == ESP_OK;
}

bool rpc_get_jiggler_state_adapter(bool *enabled, void *ctx)
{
    (void)ctx;
    return enabled != NULL &&
           jetkvm_hid_profiles_get_jiggler_state(enabled) == ESP_OK;
}

bool rpc_set_jiggler_state_adapter(bool enabled, void *ctx)
{
    (void)ctx;
    return jetkvm_hid_profiles_set_jiggler_state(enabled) == ESP_OK;
}

bool rpc_get_jiggler_config_adapter(
    jetkvm_control_jiggler_config_t *config, void *ctx)
{
    (void)ctx;
    return config != NULL &&
           jetkvm_hid_profiles_get_jiggler_config(config) == ESP_OK;
}

jetkvm_jiggler_config_change_result_t
rpc_set_jiggler_config_adapter(
    const jetkvm_control_jiggler_config_t *config, void *ctx)
{
    (void)ctx;
    esp_err_t error = jetkvm_hid_profiles_set_jiggler_config(config);
    if (error == ESP_OK) {
        return JETKVM_JIGGLER_CONFIG_CHANGE_ACCEPTED;
    }
    return error == ESP_ERR_INVALID_ARG ?
               JETKVM_JIGGLER_CONFIG_CHANGE_INVALID :
               JETKVM_JIGGLER_CONFIG_CHANGE_FAILED;
}

jetkvm_cloud_config_change_result_t rpc_set_cloud_urls(
    const char *cloud_url, const char *cloud_app_url, void *ctx)
{
    (void)ctx;
    esp_err_t err = jetkvm_device_state_cloud_set_urls(
        cloud_url, cloud_app_url, NULL);
    if (err == ESP_OK) {
        return JETKVM_CLOUD_CONFIG_CHANGE_ACCEPTED;
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        return JETKVM_CLOUD_CONFIG_CHANGE_INVALID;
    }
    return JETKVM_CLOUD_CONFIG_CHANGE_FAILED;
}

bool rpc_get_usb_configuration(
    jetkvm_usb_configuration_snapshot_t *snapshot, void *ctx)
{
    (void)ctx;
    if (snapshot == NULL) {
        return false;
    }
    jetkvm_usb_config_t configuration;
    if (jetkvm_usb_service_get_configuration(&configuration) != ESP_OK) {
        return false;
    }
    jetkvm_usb_service_snapshot_t service = {0};
    if (jetkvm_usb_service_get_snapshot(&service) != ESP_OK ||
        !service.initialized) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->vendor_id = configuration.vendor_id;
    snapshot->product_id = configuration.product_id;
    memcpy(snapshot->serial_number, configuration.serial_number,
           sizeof(configuration.serial_number));
    memcpy(snapshot->manufacturer, configuration.manufacturer,
           sizeof(configuration.manufacturer));
    memcpy(snapshot->product, configuration.product,
           sizeof(configuration.product));
    snapshot->absolute_mouse = configuration.absolute_mouse;
    snapshot->relative_mouse = configuration.relative_mouse;
    snapshot->keyboard = configuration.keyboard;
    snapshot->mass_storage = configuration.mass_storage;
    snapshot->serial_console = configuration.serial_console;
    snapshot->audio = configuration.audio;
    snapshot->emulation_enabled = service.driver_enabled;
    return true;
}

static jetkvm_virtual_media_change_result_t virtual_media_result(esp_err_t error)
{
    switch (error) {
    case ESP_OK:
        return JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
    case ESP_ERR_INVALID_ARG:
    case ESP_ERR_INVALID_SIZE:
        return JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID;
    case ESP_ERR_NOT_FOUND:
        return JETKVM_VIRTUAL_MEDIA_CHANGE_NOT_FOUND;
    case ESP_ERR_INVALID_STATE:
        return JETKVM_VIRTUAL_MEDIA_CHANGE_BUSY;
    case ESP_ERR_NOT_SUPPORTED:
        return JETKVM_VIRTUAL_MEDIA_CHANGE_UNAVAILABLE;
    default:
        return JETKVM_VIRTUAL_MEDIA_CHANGE_FAILED;
    }
}

bool rpc_get_virtual_media_state_adapter(
    jetkvm_control_virtual_media_state_t *output, void *ctx)
{
    (void)ctx;
    jetkvm_virtual_media_state_t state;
    if (output == NULL || jetkvm_virtual_media_get_state(&state) != ESP_OK) {
        return false;
    }
    memset(output, 0, sizeof(*output));
    output->initialized = state.initialized;
    output->card_present = state.card_present;
    output->card_generation = state.card_generation;
    output->filesystem_mounted = state.filesystem_mounted;
    output->mounted = state.image_mounted;
    output->source =
        state.source == JETKVM_VIRTUAL_MEDIA_SOURCE_HTTP ?
            JETKVM_CONTROL_VIRTUAL_MEDIA_SOURCE_HTTP :
        state.source == JETKVM_VIRTUAL_MEDIA_SOURCE_BUILTIN ?
            JETKVM_CONTROL_VIRTUAL_MEDIA_SOURCE_BUILTIN :
            JETKVM_CONTROL_VIRTUAL_MEDIA_SOURCE_STORAGE;
    output->mode = state.mode == JETKVM_VIRTUAL_MEDIA_MODE_CDROM ?
        JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_CDROM :
        JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_DISK;
    output->size = state.size;
    memcpy(output->filename, state.filename, sizeof(output->filename));
    memcpy(output->url, state.url, sizeof(output->url));
    return true;
}

bool rpc_get_virtual_media_space_adapter(
    jetkvm_control_storage_space_t *output, void *ctx)
{
    (void)ctx;
    jetkvm_virtual_media_space_t space;
    if (output == NULL || jetkvm_virtual_media_get_space(&space) != ESP_OK) {
        return false;
    }
    output->bytes_used = space.bytes_used;
    output->bytes_free = space.bytes_free;
    return true;
}

bool rpc_list_virtual_media_files_adapter(
    jetkvm_visit_storage_file_fn visit, void *visit_ctx, void *ctx)
{
    transport_ctx_t *transport = ctx;
    if (transport == NULL || transport->rpc_scratch == NULL || visit == NULL) {
        return false;
    }
    /* The response buffer is still untouched while the control dispatcher is
     * building its cJSON tree.  Reuse it for the FatFs snapshot, then let the
     * dispatcher overwrite it with the final serialized response. */
    jetkvm_virtual_media_file_list_t *files =
        (jetkvm_virtual_media_file_list_t *)transport->rpc_scratch;
    esp_err_t error = jetkvm_virtual_media_list_files(files);
    if (error != ESP_OK) {
        return false;
    }
    for (size_t i = 0U; i < files->count; ++i) {
        jetkvm_control_storage_file_t file = {
            .size = files->files[i].size,
            .created_at = (int64_t)files->files[i].created_at,
        };
        memcpy(file.filename, files->files[i].filename,
               sizeof(file.filename));
        if (!visit(&file, visit_ctx)) {
            return false;
        }
    }
    return true;
}

jetkvm_control_storage_format_result_t
rpc_format_virtual_media_storage_adapter(jetkvm_control_storage_space_t *space,
                                         void *ctx)
{
    (void)ctx;
    jetkvm_virtual_media_space_t formatted = {0};
    switch (jetkvm_virtual_media_format_exfat(&formatted)) {
    case JETKVM_VIRTUAL_MEDIA_FORMAT_COMPLETED:
        space->bytes_used = formatted.bytes_used;
        space->bytes_free = formatted.bytes_free;
        return JETKVM_CONTROL_STORAGE_FORMAT_COMPLETED;
    case JETKVM_VIRTUAL_MEDIA_FORMAT_NOT_READY:
        return JETKVM_CONTROL_STORAGE_FORMAT_NOT_READY;
    case JETKVM_VIRTUAL_MEDIA_FORMAT_NO_CARD:
        return JETKVM_CONTROL_STORAGE_FORMAT_NO_CARD;
    case JETKVM_VIRTUAL_MEDIA_FORMAT_IMAGE_MOUNTED:
        return JETKVM_CONTROL_STORAGE_FORMAT_IMAGE_MOUNTED;
    case JETKVM_VIRTUAL_MEDIA_FORMAT_UPLOAD_ACTIVE:
        return JETKVM_CONTROL_STORAGE_FORMAT_UPLOAD_ACTIVE;
    default:
        return JETKVM_CONTROL_STORAGE_FORMAT_FAILED;
    }
}

/* The host gets the mounted image as a new medium of its CD-ROM drive, or
 * as a drive that it enumerates again. A mount that the host cannot be given
 * is taken back. */
static jetkvm_virtual_media_change_result_t
present_mounted_medium(jetkvm_control_virtual_media_mode_t mode)
{
    jetkvm_usb_service_configuration_result_t presented =
        jetkvm_usb_service_present_medium(
            mode == JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_CDROM);
    if (presented != JETKVM_USB_SERVICE_CONFIGURATION_COMPLETED) {
        (void)jetkvm_virtual_media_unmount();
        return presented == JETKVM_USB_SERVICE_CONFIGURATION_BUSY ?
            JETKVM_VIRTUAL_MEDIA_CHANGE_BUSY :
            JETKVM_VIRTUAL_MEDIA_CHANGE_FAILED;
    }
    return JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
}

jetkvm_virtual_media_change_result_t
rpc_mount_virtual_media_storage_adapter(
    const char *filename, jetkvm_control_virtual_media_mode_t mode, void *ctx)
{
    (void)ctx;
    esp_err_t error = jetkvm_virtual_media_mount(
        filename, mode == JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_CDROM ?
                      JETKVM_VIRTUAL_MEDIA_MODE_CDROM :
                      JETKVM_VIRTUAL_MEDIA_MODE_DISK);
    if (error != ESP_OK) {
        return virtual_media_result(error);
    }
    return present_mounted_medium(mode);
}

bool webrtc_transport_mount_storage_image(const char *filename,
                                          bool cdrom_image)
{
    return rpc_mount_virtual_media_storage_adapter(
               filename, cdrom_image ?
                             JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_CDROM :
                             JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_DISK,
               NULL) == JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
}

bool rpc_check_virtual_media_url_adapter(
    const char *url, jetkvm_control_virtual_media_url_info_t *output, void *ctx)
{
    (void)ctx;
    if (url == NULL || output == NULL) {
        return false;
    }
    jetkvm_virtual_media_url_info_t info;
    if (jetkvm_virtual_media_check_url(url, &info) != ESP_OK) {
        return false;
    }
    memset(output, 0, sizeof(*output));
    output->usable = info.usable;
    output->size = info.size;
    memcpy(output->reason, info.reason, sizeof(output->reason));
    return true;
}

jetkvm_virtual_media_change_result_t
rpc_mount_virtual_media_http_adapter(
    const char *url, jetkvm_control_virtual_media_mode_t mode, void *ctx)
{
    (void)ctx;
    esp_err_t error = jetkvm_virtual_media_mount_http(
        url, mode == JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_CDROM ?
                 JETKVM_VIRTUAL_MEDIA_MODE_CDROM :
                 JETKVM_VIRTUAL_MEDIA_MODE_DISK);
    if (error == ESP_ERR_INVALID_RESPONSE) {
        /* Only the HTTP mount means "the server answered but not usably"
         * by this code; formatStorage uses it for a non-exFAT result. */
        return JETKVM_VIRTUAL_MEDIA_CHANGE_URL_UNAVAILABLE;
    }
    if (error != ESP_OK) {
        return virtual_media_result(error);
    }
    return present_mounted_medium(mode);
}

jetkvm_virtual_media_change_result_t
rpc_mount_virtual_media_builtin_installer_adapter(void *ctx)
{
    (void)ctx;
    esp_err_t error = jetkvm_virtual_media_mount_builtin_installer();
    if (error == ESP_ERR_INVALID_STATE) {
        return JETKVM_VIRTUAL_MEDIA_CHANGE_ALREADY_MOUNTED;
    }
    if (error != ESP_OK) {
        return virtual_media_result(error);
    }
    return present_mounted_medium(JETKVM_CONTROL_VIRTUAL_MEDIA_MODE_CDROM);
}

jetkvm_virtual_media_change_result_t
rpc_unmount_virtual_media_adapter(void *ctx)
{
    (void)ctx;
    return virtual_media_result(jetkvm_virtual_media_unmount());
}

jetkvm_virtual_media_change_result_t
rpc_delete_virtual_media_file_adapter(const char *filename, void *ctx)
{
    (void)ctx;
    return virtual_media_result(jetkvm_virtual_media_delete_file(filename));
}

jetkvm_virtual_media_change_result_t
rpc_start_virtual_media_upload_adapter(
    const char *filename, uint64_t size, const char *sha256,
    jetkvm_control_storage_upload_t *output, void *ctx)
{
    (void)ctx;
    jetkvm_virtual_media_upload_t upload;
    if (output == NULL) {
        return JETKVM_VIRTUAL_MEDIA_CHANGE_INVALID;
    }
    esp_err_t error = jetkvm_virtual_media_start_upload(
        filename, size, sha256, &upload);
    if (error == ESP_OK) {
        /* Go parity: this RPC creates only the resumable virtual-media
         * session.  The opaque upload id is consumed by /storage/upload; it
         * is deliberately independent of the WebRTC/RPC channel lifetime. */
        memset(output, 0, sizeof(*output));
        output->already_uploaded_bytes = upload.already_uploaded_bytes;
        memcpy(output->data_channel, upload.upload_id,
               sizeof(output->data_channel));
    }
    return virtual_media_result(error);
}

static jetkvm_usb_configuration_change_result_t
rpc_usb_configuration_result(
    jetkvm_usb_service_configuration_result_t result)
{
    switch (result) {
    case JETKVM_USB_SERVICE_CONFIGURATION_COMPLETED:
        return JETKVM_USB_CONFIGURATION_CHANGE_COMPLETED;
    case JETKVM_USB_SERVICE_CONFIGURATION_INVALID:
        return JETKVM_USB_CONFIGURATION_CHANGE_INVALID;
    case JETKVM_USB_SERVICE_CONFIGURATION_UNSUPPORTED:
        return JETKVM_USB_CONFIGURATION_CHANGE_UNSUPPORTED;
    case JETKVM_USB_SERVICE_CONFIGURATION_UNAVAILABLE:
        return JETKVM_USB_CONFIGURATION_CHANGE_UNAVAILABLE;
    case JETKVM_USB_SERVICE_CONFIGURATION_BUSY:
        return JETKVM_USB_CONFIGURATION_CHANGE_BUSY;
    case JETKVM_USB_SERVICE_CONFIGURATION_FAILED:
    default:
        return JETKVM_USB_CONFIGURATION_CHANGE_FAILED;
    }
}

static jetkvm_usb_configuration_change_result_t
rpc_apply_usb_configuration(const jetkvm_usb_config_t *configuration)
{
    return rpc_usb_configuration_result(
        jetkvm_usb_service_apply_configuration(configuration));
}


jetkvm_usb_configuration_change_result_t
rpc_set_usb_descriptor_config(
    const jetkvm_usb_descriptor_config_t *descriptor, void *ctx)
{
    (void)ctx;
    jetkvm_usb_config_t configuration;
    if (descriptor == NULL ||
        jetkvm_usb_service_get_configuration(&configuration) != ESP_OK) {
        return descriptor == NULL ? JETKVM_USB_CONFIGURATION_CHANGE_INVALID :
                                    JETKVM_USB_CONFIGURATION_CHANGE_UNAVAILABLE;
    }
    esp_err_t error = jetkvm_usb_config_set_identity(
        &configuration, descriptor->vendor_id, descriptor->product_id,
        descriptor->serial_number, descriptor->manufacturer,
        descriptor->product);
    if (error != ESP_OK) {
        return JETKVM_USB_CONFIGURATION_CHANGE_INVALID;
    }
    return rpc_apply_usb_configuration(&configuration);
}

jetkvm_usb_configuration_change_result_t rpc_set_usb_devices_adapter(
    const jetkvm_usb_devices_t *devices, void *ctx)
{
    (void)ctx;
    jetkvm_usb_config_t configuration;
    if (devices == NULL ||
        jetkvm_usb_service_get_configuration(&configuration) != ESP_OK) {
        return devices == NULL ? JETKVM_USB_CONFIGURATION_CHANGE_INVALID :
                                 JETKVM_USB_CONFIGURATION_CHANGE_UNAVAILABLE;
    }
    esp_err_t error = jetkvm_usb_config_set_devices(
        &configuration, devices->keyboard, devices->absolute_mouse,
        devices->relative_mouse, devices->mass_storage,
        devices->serial_console, devices->audio);
    if (error == ESP_ERR_NOT_SUPPORTED) {
        return JETKVM_USB_CONFIGURATION_CHANGE_UNSUPPORTED;
    }
    if (error != ESP_OK) {
        return JETKVM_USB_CONFIGURATION_CHANGE_INVALID;
    }
    return rpc_apply_usb_configuration(&configuration);
}

jetkvm_usb_configuration_change_result_t
rpc_set_usb_device_state_adapter(jetkvm_usb_device_t device, bool enabled,
                         void *ctx)
{
    (void)ctx;
    jetkvm_usb_config_t configuration;
    if (jetkvm_usb_service_get_configuration(&configuration) != ESP_OK) {
        return JETKVM_USB_CONFIGURATION_CHANGE_UNAVAILABLE;
    }
    switch (device) {
    case JETKVM_USB_DEVICE_ABSOLUTE_MOUSE:
        configuration.absolute_mouse = enabled;
        break;
    case JETKVM_USB_DEVICE_RELATIVE_MOUSE:
        configuration.relative_mouse = enabled;
        break;
    case JETKVM_USB_DEVICE_KEYBOARD:
        configuration.keyboard = enabled;
        break;
    case JETKVM_USB_DEVICE_MASS_STORAGE:
        configuration.mass_storage = enabled;
        break;
    case JETKVM_USB_DEVICE_SERIAL_CONSOLE:
        configuration.serial_console = enabled;
        break;
    case JETKVM_USB_DEVICE_AUDIO:
        configuration.audio = enabled;
        break;
    default:
        return JETKVM_USB_CONFIGURATION_CHANGE_INVALID;
    }
    esp_err_t error = jetkvm_usb_config_set_devices(
        &configuration, configuration.keyboard,
        configuration.absolute_mouse, configuration.relative_mouse,
        configuration.mass_storage, configuration.serial_console,
        configuration.audio);
    if (error == ESP_ERR_NOT_SUPPORTED) {
        return JETKVM_USB_CONFIGURATION_CHANGE_UNSUPPORTED;
    }
    if (error != ESP_OK) {
        return JETKVM_USB_CONFIGURATION_CHANGE_INVALID;
    }
    return rpc_apply_usb_configuration(&configuration);
}

jetkvm_quality_change_result_t rpc_queue_video_quality_change(
    double factor, void *ctx)
{
    (void)ctx;
    esp_err_t error = jetkvm_settings_set_video_quality(factor);
    if (error == ESP_ERR_INVALID_ARG || error == ESP_ERR_INVALID_SIZE) {
        return JETKVM_QUALITY_CHANGE_INVALID;
    }
    if (error != ESP_OK) {
        return JETKVM_QUALITY_CHANGE_UNAVAILABLE;
    }
    double persisted_factor = 1.0;
    if (jetkvm_settings_get_video_quality(&persisted_factor) != ESP_OK ||
        !jetkvm_video_quality_set(persisted_factor)) {
        return JETKVM_QUALITY_CHANGE_UNAVAILABLE;
    }
    return JETKVM_QUALITY_CHANGE_ACCEPTED;
}

bool rpc_set_video_stream_paused_adapter(bool paused, void *ctx)
{
    (void)ctx;
    return webrtc_transport_set_video_paused(paused) == ESP_OK;
}

bool rpc_get_video_log_status_adapter(
    char *output, size_t output_capacity, void *arg)
{
    if (!output || output_capacity == 0U || !arg) {
        return false;
    }
    webrtc_transport_stats_t stats = { 0 };
    if (webrtc_transport_get_stats_detailed(&stats) != ESP_OK) {
        return false;
    }
    jetkvm_memory_policy_snapshot_t memory = { 0 };
    jetkvm_memory_policy_get_snapshot(&memory);
    uint32_t quality_permille = jetkvm_video_quality_get_permille();
    int written = snprintf(
        output, output_capacity,
        "JetKVM ESP32-P4 video diagnostics\n"
        "mode: ready=%s %ux%u@%u\n"
        "quality-factor: %" PRIu32 ".%03" PRIu32 "\n"
        "transport: started=%s session=%s peer=%s state=%s "
        "sync-wait=%s generation=%" PRIu32 "\n"
        "access-units: offered=%" PRIu64 " enqueued=%" PRIu64
        " sent=%" PRIu64 " replaced=%" PRIu64 " oversize=%" PRIu64
        " no-session=%" PRIu64 " mode-mismatch=%" PRIu64
        " queue-full=%" PRIu64 " send-errors=%" PRIu64
        " would-block=%" PRIu64 "\n"
        "rtp: packets=%" PRIu64 " bytes=%" PRIu64 " markers=%" PRIu64
        " pts-regressions=%" PRIu64 "\n"
        "sync-idr: requests=%" PRIu64 " issued=%" PRIu64
        " committed=%" PRIu64 " successes=%" PRIu64
        " failures=%" PRIu64 "\n"
        "data-channels: rpc=%s hidrpc=%s messages-rx=%" PRIu64
        " messages-tx=%" PRIu64 " protocol-errors=%" PRIu64 "\n"
        "heap-internal: free=%" PRIu32 " minimum=%" PRIu32
        " largest=%" PRIu32 "\n"
        "heap-psram: free=%" PRIu32 " minimum=%" PRIu32
        " largest=%" PRIu32 "\n"
        "memory-policy: reservations=%" PRIu32
        " admission-denials=%" PRIu32 " rebuild-active=%s"
        " rebuild-begin-denials=%" PRIu32 " rebuild-failures=%" PRIu32
        " json-failures=%" PRIu32,
        stats.mode_valid ? "true" : "false",
        (unsigned)stats.width, (unsigned)stats.height, (unsigned)stats.fps,
        quality_permille / 1000U, quality_permille % 1000U,
        stats.started ? "true" : "false",
        stats.browser_session_active ? "true" : "false",
        stats.peer_connected ? "true" : "false",
        webrtc_transport_peer_state_name(stats.peer_state),
        stats.waiting_for_sync_frame ? "true" : "false", stats.generation,
        stats.access_units_offered, stats.access_units_enqueued,
        stats.access_units_sent, stats.access_units_replaced,
        stats.access_units_oversize, stats.access_units_no_session,
        stats.access_units_mode_mismatch, stats.access_units_queue_full,
        stats.access_units_send_errors, stats.access_units_would_block,
        stats.rtp_packets_sent, stats.rtp_bytes_sent,
        stats.rtp_marker_packets, stats.pts_regressions,
        stats.sync_idr_requests, stats.sync_idr_issued,
        stats.sync_idr_committed, stats.sync_idr_successes,
        stats.sync_idr_failures,
        stats.rpc_data_channel_open ? "true" : "false",
        stats.hidrpc_data_channel_open ? "true" : "false",
        stats.data_channel_messages_rx, stats.data_channel_messages_tx,
        stats.data_channel_protocol_errors,
        memory.internal_free, memory.internal_minimum,
        memory.internal_largest, memory.psram_free, memory.psram_minimum,
        memory.psram_largest, memory.active_reservations,
        memory.admission_denials,
        memory.video_rebuild_active ? "true" : "false",
        memory.video_rebuild_begin_denials, memory.video_rebuild_failures,
        memory.json_allocation_failures);
    return written >= 0 && (size_t)written < output_capacity;
}

jetkvm_video_codec_change_result_t
rpc_set_video_codec_preference(const char *codec, void *ctx)
{
    (void)ctx;
    esp_err_t error = jetkvm_settings_set_video_codec_preference(codec);
    if (error == ESP_ERR_INVALID_ARG || error == ESP_ERR_INVALID_SIZE) {
        return JETKVM_VIDEO_CODEC_CHANGE_INVALID;
    }
    return error == ESP_OK ? JETKVM_VIDEO_CODEC_CHANGE_ACCEPTED :
                             JETKVM_VIDEO_CODEC_CHANGE_FAILED;
}
