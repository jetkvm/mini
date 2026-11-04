#include "diagnostics.h"
#include "diagnostics_zip.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "jetkvm_device_state.h"
#include "jetkvm_hid_input.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_network_settings.h"
#include "jetkvm_usb_service.h"
#include "webrtc_transport.h"
#include <string.h>

/* Explicit fields only: never dump NVS, configuration blobs, request URLs,
 * raw serial logs, TLS material or cloud credentials into a support bundle. */
static cJSON *collect(void)
{
    cJSON *root = cJSON_CreateObject();
    webrtc_transport_stats_t *video = jetkvm_memory_policy_control_alloc(sizeof(*video));
    jetkvm_usb_service_snapshot_t usb = {0};
    jetkvm_network_state_t network = {0};
    char device_id[JETKVM_DEVICE_ID_CAPACITY] = {0};
    if (!root || !video || webrtc_transport_get_stats_detailed(video) != ESP_OK ||
        jetkvm_usb_service_get_snapshot(&usb) != ESP_OK ||
        jetkvm_device_state_get_id(device_id, sizeof(device_id)) != ESP_OK) goto fail;
    jetkvm_network_snapshot(&network);
    const esp_app_desc_t *app = esp_app_get_description();
    char elf_sha[65];
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(app->app_elf_sha256); ++i) {
        elf_sha[2 * i] = hex[app->app_elf_sha256[i] >> 4];
        elf_sha[2 * i + 1] = hex[app->app_elf_sha256[i] & 15];
    }
    elf_sha[64] = '\0';
#define STR(object, key, value) do { if (!cJSON_AddStringToObject(object, key, value)) goto fail; } while (0)
#define NUM(object, key, value) do { if (!cJSON_AddNumberToObject(object, key, value)) goto fail; } while (0)
#define BOOL(object, key, value) do { if (!cJSON_AddBoolToObject(object, key, value)) goto fail; } while (0)
#define OBJ(name) cJSON *name = cJSON_AddObjectToObject(root, #name); if (!name) goto fail
    NUM(root, "schemaVersion", 1);
    STR(root, "deviceId", device_id);
    NUM(root, "uptimeSeconds", esp_timer_get_time() / 1000000);
    NUM(root, "resetReason", esp_reset_reason());
    OBJ(firmware);
    STR(firmware, "project", app->project_name); STR(firmware, "version", app->version);
    STR(firmware, "idfVersion", app->idf_ver); STR(firmware, "elfSha256", elf_sha);
    STR(firmware, "buildDate", app->date); STR(firmware, "buildTime", app->time);
    OBJ(net);
    STR(net, "interface", network.interface_name); STR(net, "hostname", network.hostname);
    STR(net, "mac", network.mac_address); STR(net, "ipv4", network.ipv4_address);
    STR(net, "netmask", network.netmask); STR(net, "gateway", network.gateway);
    STR(net, "dnsMain", network.dns_main); STR(net, "dnsBackup", network.dns_backup);
    STR(net, "dnsFallback", network.dns_fallback);
    BOOL(net, "up", network.up); BOOL(net, "online", network.online); BOOL(net, "dhcp", network.dhcp);
    OBJ(usbState);
    BOOL(usbState, "enabled", usb.driver_enabled); BOOL(usbState, "hidReady", usb.hid_input_ready);
    BOOL(usbState, "transitioning", usb.transitioning || usb.configuration_transitioning);
    NUM(usbState, "lastError", usb.last_error); NUM(usbState, "lastConfigurationError", usb.last_configuration_error);
    NUM(usbState, "transitionFailures", usb.transition_failures);
    NUM(usbState, "configurationFailures", usb.configuration_transaction_failures);
    NUM(usbState, "rollbackFailures", usb.configuration_rollback_failures);
    OBJ(stream);
    BOOL(stream, "ready", video->mode_valid); BOOL(stream, "connected", video->peer_connected);
    NUM(stream, "width", video->width); NUM(stream, "height", video->height); NUM(stream, "fps", video->fps);
    NUM(stream, "framesSent", video->access_units_sent); NUM(stream, "framesReplaced", video->access_units_replaced);
    NUM(stream, "sendErrors", video->access_units_send_errors);
    NUM(stream, "rtpPackets", video->rtp_packets_sent);
    OBJ(memory);
    NUM(memory, "internalFree", heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    NUM(memory, "internalMinimum", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    NUM(memory, "psramFree", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    NUM(memory, "psramMinimum", heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
    STR(root, "logAvailability", "Serial history and crash dumps are not persisted by this firmware. This bundle contains current state and cumulative error counters.");
    jetkvm_memory_policy_control_free(video);
    return root;
fail:
    jetkvm_memory_policy_control_free(video);
    cJSON_Delete(root);
    return NULL;
#undef STR
#undef NUM
#undef BOOL
#undef OBJ
}

static bool send_chunk(const void *data, size_t size, void *ctx)
{ return httpd_resp_send_chunk(ctx, data, size) == ESP_OK; }

esp_err_t diagnostics_handler(httpd_req_t *req)
{
    cJSON *root = collect();
    char *json = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    if (!json) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Diagnostics collection failed");
    static const char readme[] =
        "JetKVM Mini diagnostics\n\n"
        "diagnostics.json contains firmware identity, device ID, network state, USB, video,\n"
        "memory and cumulative error counters. Uptime and reset reason refer to this boot.\n"
        "IP/MAC addresses and the hostname are included for network troubleshooting.\n"
        "Serial history and crash dumps are not persisted and cannot be collected.\n"
        "Passwords, keys, tokens, raw logs and configuration blobs are excluded.\n";
    const diagnostics_zip_entry_t entries[] = {
        {"diagnostics.json", json, strlen(json)},
        {"README.txt", readme, sizeof(readme) - 1},
    };
    httpd_resp_set_type(req, "application/zip");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=jetkvm-mini-diagnostics.zip");
    bool sent = diagnostics_zip_write(entries, sizeof(entries) / sizeof(entries[0]), send_chunk, req);
    cJSON_free(json);
    /* A failed transfer must remain incomplete, never become a valid empty ZIP. */
    return sent ? httpd_resp_send_chunk(req, NULL, 0) : ESP_FAIL;
}
