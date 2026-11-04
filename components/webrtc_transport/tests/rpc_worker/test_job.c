#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rpc_worker.h"
#include "esp_heap_caps.h"
#include "jetkvm_cloud_registration.h"
#include "jetkvm_control.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_usb_service.h"
#include "jetkvm_device_state.h"

static void (*pending_worker)(void *);
static void *pending_arg;
static int deletes, prepared, published, resets;
static bool reset_fails;
static unsigned reset_dispatches;
void jetkvm_factory_reset_dispatch_complete(void) { ++reset_dispatches; }
const char *jetkvm_factory_reset_request(void *ctx) { (void)ctx; ++resets; return reset_fails ? "failed" : NULL; }
static int usb_calls;
static bool usb_fails;
jetkvm_usb_service_configuration_result_t jetkvm_usb_service_set_enabled(bool enabled)
{
    assert(!enabled);
    ++usb_calls;
    return usb_fails ? JETKVM_USB_SERVICE_CONFIGURATION_FAILED : JETKVM_USB_SERVICE_CONFIGURATION_COMPLETED;
}
static bool create_fails;
static const char *last_task;
static esp_err_t delete_error;
static jetkvm_control_result_t rpc_result = JETKVM_CONTROL_RESPONSE_READY;
int xTaskCreateWithCaps(void (*fn)(void *), const char *name, uint32_t stack,
                        void *arg, int priority, void *handle, unsigned caps)
{
    (void)stack; (void)priority; (void)handle;
    /* Requests that write flash run on an internal-RAM stack. */
    bool writes_flash = !strcmp(name, "jk_reset_rpc") || !strcmp(name, "jk_cloud_rpc");
    assert(caps == ((writes_flash ? MALLOC_CAP_INTERNAL : MALLOC_CAP_SPIRAM) | MALLOC_CAP_8BIT));
    last_task = name;
    if (create_fails) return 0;
    assert(!pending_worker);
    pending_worker = fn;
    pending_arg = arg;
    return 1;
}
void vTaskDeleteWithCaps(void *handle) { (void)handle; }
esp_err_t jetkvm_device_state_cloud_snapshot(jetkvm_cloud_config_t *config, uint32_t *generation)
{
    jetkvm_cloud_config_defaults(config);
    assert(jetkvm_cloud_config_set_registration(config, "token", "aud:sub") == ESP_OK);
    *generation = 42;
    return ESP_OK;
}
esp_err_t jetkvm_device_state_get_id(char *id, size_t capacity)
{
    assert(capacity >= 13);
    strcpy(id, "80f1b2d631d9");
    return ESP_OK;
}
esp_err_t jetkvm_cloud_registration_delete_remote(const jetkvm_cloud_config_t *config, const char *id)
{
    assert(strcmp(config->cloud_token, "token") == 0);
    assert(strcmp(id, "80f1b2d631d9") == 0);
    ++deletes;
    return delete_error;
}
esp_err_t jetkvm_device_state_cloud_prepare_deregistration(uint32_t generation, const char *url)
{
    assert(generation == 42 && strcmp(url, JETKVM_DEFAULT_CLOUD_URL) == 0);
    ++prepared;
    return ESP_OK;
}
void jetkvm_device_state_cloud_finish_deregistration(void) { ++published; }
jetkvm_control_result_t jetkvm_control_handle_rpc_with_context(
    const jetkvm_control_context_t *context, const uint8_t *input, size_t length,
    char *output, size_t capacity, size_t *written)
{
    (void)length;
    const char *error;
    if (strstr((const char *)input, "setUsbEmulationState")) {
        error = context->usb_configuration.set_emulation_enabled(false, NULL) ==
            JETKVM_USB_EMULATION_CHANGE_ACCEPTED ? NULL : "failed";
    } else if (strstr((const char *)input, "factoryReset")) {
        error = context->factory_reset(context->factory_reset_ctx);
    } else error = context->deregister_device(context->deregister_device_ctx);
    assert(capacity > 3);
    strcpy(output, error ? "err" : "ok");
    *written = strlen(output);
    return rpc_result;
}
/* This binary exercises the control slot; the media adapters must not run. */
static void media_adapter_called(void) { assert(!"media adapter"); abort(); }
bool rpc_check_virtual_media_url_adapter(const char *url,
    jetkvm_control_virtual_media_url_info_t *info, void *ctx)
{ (void)url; (void)info; (void)ctx; media_adapter_called(); return false; }
jetkvm_virtual_media_change_result_t rpc_mount_virtual_media_http_adapter(
    const char *url, jetkvm_control_virtual_media_mode_t mode, void *ctx)
{ (void)url; (void)mode; (void)ctx; media_adapter_called(); return 0; }
jetkvm_virtual_media_change_result_t rpc_mount_virtual_media_storage_adapter(
    const char *filename, jetkvm_control_virtual_media_mode_t mode, void *ctx)
{ (void)filename; (void)mode; (void)ctx; media_adapter_called(); return 0; }
jetkvm_virtual_media_change_result_t
rpc_mount_virtual_media_builtin_installer_adapter(void *ctx)
{ (void)ctx; media_adapter_called(); return 0; }
jetkvm_virtual_media_change_result_t rpc_unmount_virtual_media_adapter(void *ctx)
{ (void)ctx; media_adapter_called(); return 0; }
jetkvm_control_storage_format_result_t rpc_format_virtual_media_storage_adapter(
    jetkvm_control_storage_space_t *space, void *ctx)
{ (void)space; (void)ctx; media_adapter_called(); return 0; }
jetkvm_virtual_media_change_result_t rpc_start_virtual_media_upload_adapter(
    const char *filename, uint64_t size, const char *sha256,
    jetkvm_control_storage_upload_t *output, void *ctx)
{ (void)filename; (void)size; (void)sha256; (void)output; (void)ctx; media_adapter_called(); return 0; }
jetkvm_usb_configuration_change_result_t rpc_set_usb_descriptor_config(
    const jetkvm_usb_descriptor_config_t *config, void *ctx)
{ (void)config; (void)ctx; media_adapter_called(); return 0; }
jetkvm_usb_configuration_change_result_t rpc_set_usb_devices_adapter(
    const jetkvm_usb_devices_t *devices, void *ctx)
{ (void)devices; (void)ctx; media_adapter_called(); return 0; }
jetkvm_usb_configuration_change_result_t rpc_set_usb_device_state_adapter(
    jetkvm_usb_device_t device, bool enabled, void *ctx)
{ (void)device; (void)enabled; (void)ctx; media_adapter_called(); return 0; }
jetkvm_mqtt_context_t jetkvm_control_default_mqtt_context(void)
{
    return (jetkvm_mqtt_context_t){ 0 };
}
jetkvm_ota_context_t jetkvm_control_default_ota_context(void)
{
    return (jetkvm_ota_context_t){ 0 };
}
jetkvm_guest_clipboard_context_t jetkvm_control_default_guest_clipboard_context(void)
{
    return (jetkvm_guest_clipboard_context_t){ 0 };
}
jetkvm_guest_file_context_t jetkvm_control_default_guest_file_context(void)
{
    return (jetkvm_guest_file_context_t){ 0 };
}
bool jetkvm_access_lock_epoch(uint32_t epoch) { return epoch == 1; }
void jetkvm_access_unlock(void) {}
void rpc_worker_notify_done(void) {}
static const rpc_worker_origin_t origin = {
    .session_id = 7, .generation = 9, .stream_id = 11, .access_epoch = 1,
};
static bool control_busy(void)
{
    jetkvm_control_context_t peer = {0};
    rpc_worker_peer_fallbacks(&peer);
    return peer.usb_configuration.set_emulation_enabled(false, NULL) ==
        JETKVM_USB_EMULATION_CHANGE_BUSY;
}
static bool enqueue(void)
{
    static const char input[] = "{\"id\":1,\"method\":\"deregisterDevice\"}";
    return rpc_worker_defer((const uint8_t *)input, strlen(input), &origin);
}
static void complete(void)
{
    assert(pending_worker);
    void (*fn)(void *) = pending_worker;
    pending_worker = NULL;
    fn(pending_arg);
}
int main(void)
{
    rpc_worker_reply_t reply;
    assert(!control_busy());
    assert(enqueue());
    /* Deregistration clears the cloud configuration in NVS. */
    assert(!strcmp(last_task, "jk_cloud_rpc"));
    assert(control_busy());
    assert(!enqueue()); /* bounded admission */
    assert(!rpc_worker_take_reply(&reply));
    complete();
    assert(prepared == 1 && published == 0);
    assert(rpc_worker_take_reply(&reply));
    assert(!control_busy());
    assert(reply.session_id == 7 && reply.generation == 9 && reply.stream_id == 11);
    assert(reply.publish_deregistration && published == 0);
    jetkvm_device_state_cloud_finish_deregistration(); /* after send attempt */
    rpc_worker_cancel_replies();
    assert(published == 1);

    /* A callback can enqueue after stop's first cancellation. Peer exit must
     * cancel again so completion cannot leave unpublished state orphaned. */
    rpc_worker_cancel_replies();
    assert(enqueue());
    rpc_worker_cancel_replies(); /* peer exit */
    complete();
    assert(prepared == 2 && published == 2);
    assert(!rpc_worker_take_reply(&reply));

    assert(enqueue());
    complete();
    rpc_worker_cancel_replies(); /* completion before peer exit */
    assert(prepared == 3 && published == 3);
    assert(!rpc_worker_take_reply(&reply));

    rpc_result = JETKVM_CONTROL_NO_RESPONSE;
    assert(enqueue());
    complete();
    assert(!rpc_worker_take_reply(&reply));
    assert(prepared == 4 && published == 4); /* JSON-RPC notification */
    rpc_result = JETKVM_CONTROL_RESPONSE_READY;
    create_fails = true;
    assert(!enqueue());
    assert(!control_busy());
    create_fails = false;
    delete_error = ESP_FAIL;
    assert(enqueue());
    complete();
    assert(rpc_worker_take_reply(&reply));
    assert(!reply.publish_deregistration && prepared == 4 && published == 4);
    assert(deletes == 5);
    const char reset[] = "{\"id\":1,\"method\":\"factoryReset\"}";
    for (unsigned i = 0; i < 2; ++i) {
        reset_fails = i != 0;
        assert(rpc_worker_defer((const uint8_t *)reset, strlen(reset), &origin));
        const char competing_usb[] = "{\"method\":\"setUsbEmulationState\"}";
        assert(!rpc_worker_defer((const uint8_t *)competing_usb, strlen(competing_usb), &origin));
        assert(resets == (int)i && !rpc_worker_take_reply(&reply));
        complete();
        assert(rpc_worker_take_reply(&reply));
        assert(strcmp(reply.json, reset_fails ? "err" : "ok") == 0);
        assert(reply.will_reboot == !reset_fails);
        assert(reset_dispatches == (i ? 1U : 0U));
        if (reply.will_reboot) jetkvm_factory_reset_dispatch_complete();
        assert(!reply.publish_deregistration);
    }
    reset_fails = false;
    rpc_result = JETKVM_CONTROL_NO_RESPONSE;
    assert(rpc_worker_defer((const uint8_t *)reset, strlen(reset), &origin));
    complete();
    assert(!rpc_worker_take_reply(&reply) && reset_dispatches == 2);
    rpc_result = JETKVM_CONTROL_RESPONSE_READY;
    assert(rpc_worker_defer((const uint8_t *)reset, strlen(reset), &origin));
    rpc_worker_cancel_replies();complete();
    assert(reset_dispatches == 3);
    puts("Cloud/factory reset deferred completion, failure and session tests passed");
    const char usb_request[] = "{\"id\":2,\"method\":\"setUsbEmulationState\",\"params\":{\"enabled\":false}}";
    for (int failure = 0; failure < 2; ++failure) {
        usb_fails = failure;
        assert(rpc_worker_defer((const uint8_t *)usb_request, strlen(usb_request), &origin));
        assert(!rpc_worker_defer((const uint8_t *)reset, strlen(reset), &origin));
        assert(usb_calls == failure); /* no blocking driver work on the peer */
        assert(!rpc_worker_take_reply(&reply));
        complete();
        assert(rpc_worker_take_reply(&reply));
        assert(!strcmp(reply.json, failure ? "err" : "ok"));
        assert(!reply.publish_deregistration && published == 4);
    }
    assert(control_arena_blocks == 0);
    puts("Cloud/USB worker completion, failure and session lifecycle tests passed");
}
