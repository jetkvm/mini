/* Media-slot tests with the real JSON-RPC dispatcher. The blocking adapters
 * are fakes that count calls; a deferred reply must equal, byte for byte, the
 * reply the peer task built when it ran the same adapter itself. The guest,
 * MQTT and update queues' tests (test_guest.c, test_mqtt.c, test_update.c)
 * share this binary and its task fakes. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "jetkvm_cloud_registration.h"
#include "jetkvm_memory_policy.h"
#include "jetkvm_device_state.h"
#include "jetkvm_usb_service.h"
#include "rpc_worker.h"
#include "webrtc_rpc_adapters_internal.h"

typedef struct {
    void (*fn)(void *);
    void *arg;
    const char *name;
} pending_task_t;
static pending_task_t tasks[4];
static unsigned notifications, usb_emulation_calls;
static uint32_t access_epoch = 5;
static int access_depth;

int xTaskCreateWithCaps(void (*fn)(void *), const char *name, uint32_t stack,
                        void *arg, int priority, void *handle, unsigned caps)
{
    (void)priority; (void)handle;
    assert(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    assert(stack >= 8192);
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); ++i) {
        if (!tasks[i].fn) {
            tasks[i] = (pending_task_t){fn, arg, name};
            return 1;
        }
    }
    assert(!"too many workers");
    return 0;
}
void vTaskDeleteWithCaps(void *handle) { (void)handle; }
bool worker_pending(const char *name);
bool worker_pending(const char *name)
{
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); ++i) {
        if (tasks[i].fn && !strcmp(tasks[i].name, name)) return true;
    }
    return false;
}
void run_worker(const char *name);
void run_worker(const char *name)
{
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); ++i) {
        if (tasks[i].fn && !strcmp(tasks[i].name, name)) {
            pending_task_t task = tasks[i];
            tasks[i].fn = NULL;
            task.fn(task.arg);
            return;
        }
    }
    assert(!"worker not started");
}

bool jetkvm_access_lock_epoch(uint32_t epoch)
{
    if (epoch != access_epoch) return false;
    ++access_depth;
    return true;
}
void jetkvm_access_unlock(void) { assert(access_depth-- > 0); }
void rpc_worker_notify_done(void) { ++notifications; }

/* Control-slot collaborators; only setUsbEmulationState reaches them here. */
const char *jetkvm_factory_reset_request(void *ctx) { (void)ctx; return "unused"; }
void jetkvm_factory_reset_dispatch_complete(void) {}
esp_err_t jetkvm_device_state_cloud_snapshot(jetkvm_cloud_config_t *config, uint32_t *generation)
{ (void)config; (void)generation; return ESP_FAIL; }
esp_err_t jetkvm_device_state_get_id(char *id, size_t capacity) { (void)id; (void)capacity; return ESP_FAIL; }
esp_err_t jetkvm_cloud_registration_delete_remote(const jetkvm_cloud_config_t *config, const char *id)
{ (void)config; (void)id; return ESP_FAIL; }
esp_err_t jetkvm_device_state_cloud_prepare_deregistration(uint32_t generation, const char *url)
{ (void)generation; (void)url; return ESP_FAIL; }
void jetkvm_device_state_cloud_finish_deregistration(void) {}
jetkvm_usb_service_configuration_result_t jetkvm_usb_service_set_enabled(bool enabled)
{
    (void)enabled;
    ++usb_emulation_calls;
    return JETKVM_USB_SERVICE_CONFIGURATION_COMPLETED;
}

/* Stand-ins for the adapters that mount and re-enumerate, apply a USB
 * configuration, or start an upload on the TF card. Every call is one
 * blocking USB, HTTP or card operation. */
static unsigned blocking_calls;
static char calls[16];
static size_t last_url_length;
static void called(char operation)
{
    ++blocking_calls;
    size_t length = strlen(calls);
    if (length + 1 < sizeof(calls)) {
        calls[length] = operation;
        calls[length + 1] = '\0';
    }
}
static jetkvm_virtual_media_change_result_t media_result;
static jetkvm_usb_configuration_change_result_t usb_result;
jetkvm_virtual_media_change_result_t rpc_mount_virtual_media_http_adapter(
    const char *url, jetkvm_control_virtual_media_mode_t mode, void *ctx)
{
    (void)mode; (void)ctx;
    called('M');
    last_url_length = strlen(url);
    return media_result;
}
jetkvm_virtual_media_change_result_t rpc_mount_virtual_media_storage_adapter(
    const char *filename, jetkvm_control_virtual_media_mode_t mode, void *ctx)
{
    (void)filename; (void)mode; (void)ctx;
    called('S');
    return media_result;
}
jetkvm_virtual_media_change_result_t
rpc_mount_virtual_media_builtin_installer_adapter(void *ctx)
{ (void)ctx; called('B'); return media_result; }
jetkvm_virtual_media_change_result_t rpc_unmount_virtual_media_adapter(void *ctx)
{ (void)ctx; called('U'); return media_result; }
jetkvm_control_storage_format_result_t rpc_format_virtual_media_storage_adapter(
    jetkvm_control_storage_space_t *space, void *ctx)
{
    (void)space; (void)ctx;
    ++blocking_calls;
    return media_result == JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED ?
        JETKVM_CONTROL_STORAGE_FORMAT_COMPLETED : JETKVM_CONTROL_STORAGE_FORMAT_FAILED;
}
jetkvm_virtual_media_change_result_t rpc_start_virtual_media_upload_adapter(
    const char *filename, uint64_t size, const char *sha256,
    jetkvm_control_storage_upload_t *output, void *ctx)
{
    (void)filename; (void)sha256; (void)ctx;
    called('P');
    if (media_result == JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED) {
        memset(output, 0, sizeof(*output));
        output->already_uploaded_bytes = size / 2U;
        snprintf(output->data_channel, sizeof(output->data_channel), "upload_test");
    }
    return media_result;
}
bool rpc_check_virtual_media_url_adapter(const char *url,
    jetkvm_control_virtual_media_url_info_t *info, void *ctx)
{
    (void)url; (void)ctx;
    ++blocking_calls;
    memset(info, 0, sizeof(*info));
    info->usable = true;
    info->size = 4096;
    return true;
}
jetkvm_usb_configuration_change_result_t rpc_set_usb_descriptor_config(
    const jetkvm_usb_descriptor_config_t *config, void *ctx)
{ (void)config; (void)ctx; ++blocking_calls; return usb_result; }
jetkvm_usb_configuration_change_result_t rpc_set_usb_devices_adapter(
    const jetkvm_usb_devices_t *devices, void *ctx)
{ (void)devices; (void)ctx; ++blocking_calls; return usb_result; }
jetkvm_usb_configuration_change_result_t rpc_set_usb_device_state_adapter(
    jetkvm_usb_device_t device, bool enabled, void *ctx)
{ (void)device; (void)enabled; (void)ctx; ++blocking_calls; return usb_result; }

static const rpc_worker_origin_t origin = {
    .session_id = 7, .generation = 9, .stream_id = 11, .access_epoch = 5,
};
static const char mount[] =
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"mountWithHTTP\","
    "\"params\":{\"url\":\"http://192.0.2.1/a.iso\",\"mode\":\"CDROM\"}}";
static const char devices[] =
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"setUsbDevices\",\"params\":{\"devices\":"
    "{\"keyboard\":true,\"absolute_mouse\":true,\"relative_mouse\":true,"
    "\"mass_storage\":false,\"serial_console\":false,\"audio\":false}}}";
static const char start_upload[] =
    "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"startStorageFileUpload\","
    "\"params\":{\"filename\":\"a.iso\",\"size\":4096}}";
static const char emulation[] =
    "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"setUsbEmulationState\",\"params\":{\"enabled\":false}}";

static bool defer(const char *request, const rpc_worker_origin_t *from)
{
    return rpc_worker_defer((const uint8_t *)request, strlen(request), from);
}

/* The reply as the peer task rendered it: the same adapters, called inline. */
static void direct_reply(const char *request, char *json, size_t capacity)
{
    const jetkvm_control_context_t context = {
        .usb_configuration = {
            .set_descriptor_config = rpc_set_usb_descriptor_config,
            .set_devices = rpc_set_usb_devices_adapter,
            .set_device_state = rpc_set_usb_device_state_adapter,
        },
        .virtual_media = {
            .check_url = rpc_check_virtual_media_url_adapter,
            .mount_http = rpc_mount_virtual_media_http_adapter,
            .mount_storage = rpc_mount_virtual_media_storage_adapter,
            .mount_builtin_installer = rpc_mount_virtual_media_builtin_installer_adapter,
            .unmount = rpc_unmount_virtual_media_adapter,
            .format_storage = rpc_format_virtual_media_storage_adapter,
            .start_upload = rpc_start_virtual_media_upload_adapter,
        },
    };
    size_t length = 0;
    assert(jetkvm_control_handle_rpc_with_context(&context, (const uint8_t *)request,
        strlen(request), json, capacity, &length) == JETKVM_CONTROL_RESPONSE_READY);
    json[length] = '\0';
}

/* What the peer task sends when rpc_worker_defer() refuses a request. */
static void peer_reply(const char *request, char *json, size_t capacity)
{
    jetkvm_control_context_t context = {0};
    rpc_worker_peer_fallbacks(&context);
    size_t length = 0;
    assert(jetkvm_control_handle_rpc_with_context(&context, (const uint8_t *)request,
        strlen(request), json, capacity, &length) == JETKVM_CONTROL_RESPONSE_READY);
    json[length] = '\0';
}

static void expect_reply(const char *expected)
{
    rpc_worker_reply_t reply;
    assert(rpc_worker_take_reply(&reply));
    assert(reply.session_id == 7 && reply.generation == 9 && reply.stream_id == 11);
    assert(reply.length == strlen(expected) && !memcmp(reply.json, expected, reply.length));
}

static void test_mount_reply_after_worker(void)
{
    const jetkvm_virtual_media_change_result_t outcomes[] = {
        JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED, JETKVM_VIRTUAL_MEDIA_CHANGE_BUSY,
        JETKVM_VIRTUAL_MEDIA_CHANGE_FAILED, JETKVM_VIRTUAL_MEDIA_CHANGE_URL_UNAVAILABLE,
    };
    for (size_t i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); ++i) {
        media_result = outcomes[i];
        blocking_calls = 0;
        unsigned notified = notifications;
        rpc_worker_reply_t reply;
        assert(defer(mount, &origin));
        assert(blocking_calls == 0 && !rpc_worker_take_reply(&reply));
        run_worker("jk_media_rpc");
        assert(blocking_calls == 1 && notifications == notified + 1);
        char expected[512];
        direct_reply(mount, expected, sizeof(expected));
        expect_reply(expected);
        assert(!rpc_worker_take_reply(&reply));
    }
    puts("mount replies after the worker, same JSON as the inline adapter: OK");
}

static void test_builtin_installer_mount_runs_on_the_worker(void)
{
    static const char builtin[] =
        "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"mountBuiltinInstaller\"}";
    const jetkvm_virtual_media_change_result_t outcomes[] = {
        JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED, JETKVM_VIRTUAL_MEDIA_CHANGE_ALREADY_MOUNTED,
    };
    for (size_t i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); ++i) {
        media_result = outcomes[i];
        calls[0] = '\0';
        rpc_worker_reply_t reply;
        assert(defer(builtin, &origin));
        assert(!rpc_worker_take_reply(&reply));
        run_worker("jk_media_rpc");
        assert(!strcmp(calls, "B"));
        char expected[512];
        direct_reply(builtin, expected, sizeof(expected));
        expect_reply(expected);
    }
    char refused[512];
    peer_reply(builtin, refused, sizeof(refused));
    assert(strstr(refused, "\"error\""));
    puts("mountBuiltinInstaller runs on the media worker: OK");
}

/* A request with its own id, so a test can tell replies apart. */
static void request_with_id(char *out, size_t capacity, const char *method, int id)
{
    if (!strcmp(method, "unmountImage")) {
        snprintf(out, capacity, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"unmountImage\"}", id);
    } else if (!strcmp(method, "mountWithStorage")) {
        snprintf(out, capacity, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"mountWithStorage\","
                 "\"params\":{\"filename\":\"b.img\",\"mode\":\"Disk\"}}", id);
    } else {
        snprintf(out, capacity, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"mountWithHTTP\","
                 "\"params\":{\"url\":\"http://192.0.2.1/a.iso\",\"mode\":\"CDROM\"}}", id);
    }
}

static int reply_id(void)
{
    rpc_worker_reply_t reply;
    assert(rpc_worker_take_reply(&reply));
    const char *id = strstr(reply.json, "\"id\":");
    assert(id);
    return atoi(id + 5);
}

static void test_full_queue_replies_busy(void)
{
    media_result = JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
    usb_result = JETKVM_USB_CONFIGURATION_CHANGE_COMPLETED;
    blocking_calls = 0;
    char request[256];
    for (int id = 1; id <= 4; ++id) {
        request_with_id(request, sizeof(request), "mountWithHTTP", id);
        assert(defer(request, &origin));
    }
    char busy_mount[512], busy_devices[512], busy_format[512], expected[512];
    assert(!defer(mount, &origin));
    peer_reply(mount, busy_mount, sizeof(busy_mount));
    assert(!defer(devices, &origin));
    peer_reply(devices, busy_devices, sizeof(busy_devices));
    const char *format = "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"formatStorage\"}";
    assert(!defer(format, &origin));
    peer_reply(format, busy_format, sizeof(busy_format));
    assert(blocking_calls == 0);
    assert(strstr(busy_format, "another virtual media operation is active"));

    /* Same bytes as when the USB owner itself reported BUSY. */
    media_result = JETKVM_VIRTUAL_MEDIA_CHANGE_BUSY;
    direct_reply(mount, expected, sizeof(expected));
    assert(!strcmp(busy_mount, expected));
    assert(strstr(busy_mount, "another virtual media operation is active"));
    usb_result = JETKVM_USB_CONFIGURATION_CHANGE_BUSY;
    direct_reply(devices, expected, sizeof(expected));
    assert(!strcmp(busy_devices, expected));
    assert(strstr(busy_devices, "USB configuration change already in progress"));

    media_result = JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
    blocking_calls = 0;
    run_worker("jk_media_rpc");
    assert(blocking_calls == 4);
    for (int id = 1; id <= 4; ++id) assert(reply_id() == id);
    /* The queue is empty again: a refused request now reads as unavailable. */
    peer_reply(mount, busy_mount, sizeof(busy_mount));
    assert(strstr(busy_mount, "virtual media storage unavailable"));
    puts("full media queue replies BUSY without a USB operation: OK");
}

static void test_swap_runs_in_order(void)
{
    /* tools/integration/tests/mounted_image.test.mjs sends both at once so
     * that the host never reads an empty drive between them. */
    char unmount[128], remount[256];
    request_with_id(unmount, sizeof(unmount), "unmountImage", 21);
    request_with_id(remount, sizeof(remount), "mountWithStorage", 22);
    media_result = JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
    calls[0] = '\0';
    assert(defer(unmount, &origin));
    assert(defer(remount, &origin));
    run_worker("jk_media_rpc");
    assert(!strcmp(calls, "US"));
    assert(reply_id() == 21);
    assert(reply_id() == 22);
    puts("unmount and mount sent together run and reply in order: OK");
}

/* The start of an upload reads and writes the card. */
static void test_upload_start_runs_on_worker(void)
{
    const jetkvm_virtual_media_change_result_t outcomes[] = {
        JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED, JETKVM_VIRTUAL_MEDIA_CHANGE_BUSY,
        JETKVM_VIRTUAL_MEDIA_CHANGE_NOT_FOUND, JETKVM_VIRTUAL_MEDIA_CHANGE_FAILED,
    };
    char expected[512];
    rpc_worker_reply_t reply;
    for (size_t i = 0; i < sizeof(outcomes) / sizeof(outcomes[0]); ++i) {
        media_result = outcomes[i];
        blocking_calls = 0;
        assert(defer(start_upload, &origin));
        assert(blocking_calls == 0 && !rpc_worker_take_reply(&reply));
        run_worker("jk_media_rpc");
        assert(blocking_calls == 1);
        direct_reply(start_upload, expected, sizeof(expected));
        expect_reply(expected);
    }
    media_result = JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
    direct_reply(start_upload, expected, sizeof(expected));
    assert(strstr(expected, "\"dataChannel\":\"upload_test\"") &&
           strstr(expected, "\"alreadyUploadedBytes\":2048"));

    /* A start that follows an unmount runs after it. */
    char unmount[128];
    request_with_id(unmount, sizeof(unmount), "unmountImage", 14);
    calls[0] = '\0';
    assert(defer(unmount, &origin));
    assert(defer(start_upload, &origin));
    run_worker("jk_media_rpc");
    assert(!strcmp(calls, "UP"));
    assert(reply_id() == 14);
    assert(reply_id() == 12);

    /* With the queue full, the peer task answers and does not touch the card. */
    char request[256], busy[512];
    for (int id = 1; id <= 4; ++id) {
        request_with_id(request, sizeof(request), "mountWithHTTP", id);
        assert(defer(request, &origin));
    }
    blocking_calls = 0;
    assert(!defer(start_upload, &origin));
    peer_reply(start_upload, busy, sizeof(busy));
    assert(blocking_calls == 0);
    media_result = JETKVM_VIRTUAL_MEDIA_CHANGE_BUSY;
    direct_reply(start_upload, expected, sizeof(expected));
    media_result = JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
    assert(!strcmp(busy, expected));
    blocking_calls = 0;
    run_worker("jk_media_rpc");
    assert(blocking_calls == 4);
    for (int id = 1; id <= 4; ++id) assert(reply_id() == id);

    /* The delete of a file stays with the peer task. */
    static const char delete_file[] =
        "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"deleteStorageFile\","
        "\"params\":{\"filename\":\"a.iso.incomplete\"}}";
    assert(!defer(delete_file, &origin));
    puts("upload start runs on the media worker, in order; a full queue answers BUSY: OK");
}

static void test_cancel_before_completion(void)
{
    rpc_worker_reply_t reply;
    char request[256];
    unsigned notified = notifications;
    blocking_calls = 0;
    request_with_id(request, sizeof(request), "mountWithHTTP", 31);
    assert(defer(request, &origin));
    rpc_worker_cancel_replies(); /* transport stop before the worker ran */
    request_with_id(request, sizeof(request), "mountWithHTTP", 32);
    assert(defer(request, &origin)); /* the next session's request */
    run_worker("jk_media_rpc");
    /* Both admitted operations ran; only the next session gets a reply. */
    assert(blocking_calls == 2 && notifications == notified + 1);
    assert(reply_id() == 32);
    assert(!rpc_worker_take_reply(&reply));

    assert(defer(mount, &origin));
    run_worker("jk_media_rpc");
    rpc_worker_cancel_replies(); /* transport stop between completion and send */
    assert(!rpc_worker_take_reply(&reply));
    assert(defer(mount, &origin));
    run_worker("jk_media_rpc");
    assert(rpc_worker_take_reply(&reply));
    puts("cancelled media jobs send nothing and free their place: OK");
}

static void test_long_url(void)
{
    enum { URL_LENGTH = 2000 };
    char url[URL_LENGTH + 1];
    memcpy(url, "http://192.0.2.1/", 17);
    memset(url + 17, 'a', URL_LENGTH - 17);
    url[URL_LENGTH] = '\0';
    char request[URL_LENGTH + 128];
    snprintf(request, sizeof(request),
             "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"mountWithHTTP\","
             "\"params\":{\"url\":\"%s\",\"mode\":\"Disk\"}}", url);
    media_result = JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
    last_url_length = 0;
    assert(defer(request, &origin));
    run_worker("jk_media_rpc");
    assert(last_url_length == URL_LENGTH);
    char expected[256];
    direct_reply(request, expected, sizeof(expected));
    rpc_worker_reply_t reply;
    assert(rpc_worker_take_reply(&reply));
    assert(reply.length == strlen(expected) && !memcmp(reply.json, expected, reply.length));
    puts("2000-character mount URL reaches the worker intact: OK");
}

static void test_expired_access_epoch(void)
{
    rpc_worker_reply_t reply;
    blocking_calls = 0;
    assert(defer(mount, &origin));
    access_epoch = 6; /* password changed after admission */
    run_worker("jk_media_rpc");
    assert(blocking_calls == 0 && !rpc_worker_take_reply(&reply));
    assert(defer(emulation, &origin));
    run_worker("jk_control");
    assert(usb_emulation_calls == 0 && !rpc_worker_take_reply(&reply));

    rpc_worker_origin_t cloud = origin;
    cloud.cloud_authority = true; /* Cloud sessions are not local-password sessions */
    assert(defer(mount, &cloud));
    run_worker("jk_media_rpc");
    assert(blocking_calls == 1 && rpc_worker_take_reply(&reply));
    access_epoch = 5;
    assert(access_depth == 0);
    puts("a revoked access epoch stops a deferred request: OK");
}

static void test_control_during_media(void)
{
    char expected[512];
    rpc_worker_reply_t reply;
    media_result = JETKVM_VIRTUAL_MEDIA_CHANGE_COMPLETED;
    assert(defer(mount, &origin));
    assert(defer(emulation, &origin));
    run_worker("jk_control");
    assert(usb_emulation_calls == 1);
    assert(rpc_worker_take_reply(&reply));
    assert(strstr(reply.json, "\"id\":5"));
    assert(!rpc_worker_take_reply(&reply)); /* the mount has not run */
    run_worker("jk_media_rpc");
    direct_reply(mount, expected, sizeof(expected));
    expect_reply(expected);
    puts("a control job completes while a media job is in flight: OK");
}

static void test_other_methods_stay_on_peer(void)
{
    static const char state[] = "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"getVirtualMediaState\"}";
    assert(!defer(state, &origin));
    static const char notification[] =
        "{\"jsonrpc\":\"2.0\",\"method\":\"unmountImage\"}";
    rpc_worker_reply_t reply;
    assert(defer(notification, &origin));
    run_worker("jk_media_rpc");
    assert(!rpc_worker_take_reply(&reply));
    puts("non-blocking methods stay on the peer; notifications get no reply: OK");
}

void test_guest_queue(void); /* test_guest.c */
void test_mqtt_queue(void); /* test_mqtt.c */
void test_update_queue(void); /* test_update.c */

int main(void)
{
    test_mount_reply_after_worker();
    test_builtin_installer_mount_runs_on_the_worker();
    test_full_queue_replies_busy();
    test_swap_runs_in_order();
    test_upload_start_runs_on_worker();
    test_cancel_before_completion();
    test_long_url();
    test_expired_access_epoch();
    test_control_during_media();
    test_other_methods_stay_on_peer();
    test_guest_queue();
    test_mqtt_queue();
    test_update_queue();
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); ++i) assert(!tasks[i].fn);
    assert(control_arena_blocks == 0);
}
