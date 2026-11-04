/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_usb_service.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jetkvm_hid_input.h"
#include "jetkvm_usb_hid.h"

/* With a 4,096-byte stack, the task's peak use leaves 352 to 360 bytes free
 * (taskStacks[] in /webrtc/stats?details=1); 6,144 bytes leave about
 * 2,400. The stack stays internal: the task saves the USB configuration to
 * NVS, and a flash call from a task with its stack in PSRAM asserts. */
#define USB_OWNER_TASK_STACK_SIZE 6144U
#define USB_OWNER_TASK_PRIORITY   4U
#define USB_OWNER_TASK_CORE       0
#define USB_OWNER_START_TIMEOUT_MS 3000U
#define USB_CONFIGURATION_TIMEOUT_MS 6000U
#define USB_REENUMERATION_DETACH_MS 250U

typedef struct {
    jetkvm_usb_service_snapshot_t state;
    bool starting;
    uint32_t desired_revision;
    esp_err_t initial_attempt_error;
    SemaphoreHandle_t initial_attempt_done;
    StaticSemaphore_t initial_attempt_done_storage;
    SemaphoreHandle_t configuration_call_mutex;
    StaticSemaphore_t configuration_call_mutex_storage;
    SemaphoreHandle_t configuration_done;
    StaticSemaphore_t configuration_done_storage;
    TaskHandle_t owner_task;
    jetkvm_usb_service_state_callback_t callback;
    void *callback_ctx;
    jetkvm_usb_config_t configuration;
    jetkvm_usb_config_store_state_t configuration_store;
    jetkvm_usb_config_t requested_configuration;
    jetkvm_usb_service_configuration_result_t configuration_result;
    uint32_t configuration_request_revision;
    uint32_t configuration_completed_revision;
    uint32_t configuration_cancel_revision;
    bool configuration_force_reenumeration;
    int configuration_emulation_enabled;
} usb_service_context_t;

static const char *TAG = "jetkvm_usb_service";
static usb_service_context_t s_ctx;
static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;

static esp_err_t disable_usb_stack(void);

static TickType_t milliseconds_to_ticks_ceil(uint32_t milliseconds)
{
    uint64_t ticks =
        ((uint64_t)milliseconds * (uint64_t)configTICK_RATE_HZ + 999U) /
        1000U;
    if (ticks == 0U) {
        ticks = 1U;
    }
    return ticks > (uint64_t)portMAX_DELAY ? portMAX_DELAY :
                                             (TickType_t)ticks;
}

static void publish_state(void)
{
    jetkvm_usb_service_snapshot_t snapshot;
    jetkvm_usb_service_state_callback_t callback;
    void *callback_ctx;

    portENTER_CRITICAL(&s_state_lock);
    snapshot = s_ctx.state;
    callback = s_ctx.callback;
    callback_ctx = s_ctx.callback_ctx;
    portEXIT_CRITICAL(&s_state_lock);

    if (callback != NULL) {
        callback(&snapshot, callback_ctx);
    }
}

static bool usb_driver_is_enabled(void)
{
    jetkvm_usb_hid_state_t usb = {0};
    return jetkvm_usb_hid_get_state(&usb) == ESP_OK && usb.initialized;
}

static bool hid_input_is_initialized(void)
{
    jetkvm_hid_input_snapshot_t input = {0};
    return jetkvm_hid_input_get_snapshot(&input) == ESP_OK &&
           input.initialized;
}

static bool hid_input_usb_is_available(void)
{
    jetkvm_hid_input_snapshot_t input = {0};
    return jetkvm_hid_input_get_snapshot(&input) == ESP_OK &&
           input.initialized && input.usb_available;
}

static esp_err_t select_descriptor_configuration(
    const jetkvm_usb_config_t *configuration)
{
    const jetkvm_usb_hid_descriptor_configuration_t descriptor = {
        .vendor_id = configuration->vendor_id,
        .product_id = configuration->product_id,
        .serial_number = configuration->serial_number,
        .manufacturer = configuration->manufacturer,
        .product = configuration->product,
        .absolute_mouse = configuration->absolute_mouse,
        .relative_mouse = configuration->relative_mouse,
        .keyboard = configuration->keyboard,
        .mass_storage = configuration->mass_storage,
        .audio = configuration->audio,
#if CONFIG_JETKVM_OS_SERVICES
        /* The interface is always present; the OS Services component
         * claims it. */
        .os_services = true,
#endif
    };
    return jetkvm_usb_hid_set_descriptor_configuration(&descriptor);
}

static esp_err_t enable_usb_stack(bool *hid_ready)
{
    *hid_ready = false;
    if (!usb_driver_is_enabled()) {
        esp_err_t error = jetkvm_usb_hid_init();
        if (error != ESP_OK) {
            return error;
        }
    }

    esp_err_t error = hid_input_is_initialized() ?
        jetkvm_hid_input_set_usb_available(true) :
        jetkvm_hid_input_init();
    if (error == ESP_OK) {
        *hid_ready = true;
        return ESP_OK;
    }

    /* Do not expose a driver-only half-start when the input bridge fails. */
    esp_err_t cleanup_error = disable_usb_stack();
    if (cleanup_error != ESP_OK) {
        ESP_LOGE(TAG, "target USB enable rollback failed: %s",
                 esp_err_to_name(cleanup_error));
    }
    return error;
}

static esp_err_t disable_usb_stack(void)
{
    if (hid_input_is_initialized()) {
        esp_err_t error = jetkvm_hid_input_set_usb_available(false);
        if (error != ESP_OK) {
            if (usb_driver_is_enabled()) {
                (void)jetkvm_hid_input_set_usb_available(true);
            }
            return error;
        }
    }

    esp_err_t error = jetkvm_usb_hid_deinit();
    const bool driver_enabled = usb_driver_is_enabled();
    if (!driver_enabled) {
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            /* release_all is best effort immediately before a successful
             * detach. Once the target controller is actually stopped there
             * is no host-visible held state left to preserve, so lifecycle
             * and descriptor transactions may continue. The HID component
             * retains the release error in its own stats/last_error. */
            ESP_LOGW(TAG, "target USB detached after release warning: %s",
                     esp_err_to_name(error));
        }
        return ESP_OK;
    }
    if (error != ESP_OK && hid_input_is_initialized()) {
        (void)jetkvm_hid_input_set_usb_available(true);
    }
    return error;
}

static esp_err_t restore_configuration(
    const jetkvm_usb_config_t *configuration, bool restore_enabled)
{
    if (usb_driver_is_enabled() || hid_input_usb_is_available()) {
        esp_err_t error = disable_usb_stack();
        if (error != ESP_OK) {
            return error;
        }
    }
    esp_err_t error = select_descriptor_configuration(configuration);
    if (error != ESP_OK) {
        return error;
    }
    if (restore_enabled) {
        bool hid_ready = false;
        error = enable_usb_stack(&hid_ready);
    }
    return error;
}

static void complete_configuration_request(
    jetkvm_usb_service_configuration_result_t result,
    esp_err_t error, bool completed, bool rolled_back,
    bool rollback_failed, uint32_t request_revision)
{
    const bool driver_enabled = usb_driver_is_enabled();
    const bool hid_ready = driver_enabled && hid_input_usb_is_available();
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.configuration_result = result;
    s_ctx.configuration_completed_revision = request_revision;
    s_ctx.state.configuration_pending = false;
    s_ctx.state.configuration_transitioning = false;
    s_ctx.state.last_configuration_error = error;
    if (s_ctx.configuration_emulation_enabled >= 0) s_ctx.state.desired_enabled = driver_enabled;
    if (completed) {
        s_ctx.state.configuration_transactions_completed++;
    } else {
        s_ctx.state.configuration_transaction_failures++;
    }
    if (rolled_back) {
        s_ctx.state.configuration_rollbacks++;
    }
    if (rollback_failed) {
        s_ctx.state.configuration_rollback_failures++;
    }
    s_ctx.state.driver_enabled = driver_enabled;
    s_ctx.state.hid_input_ready = hid_ready;
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_ctx.configuration_done);
    publish_state();
}

static bool complete_configuration_success(
    const jetkvm_usb_config_t *configuration,
    const jetkvm_usb_config_store_state_t *store,
    uint32_t request_revision)
{
    const bool driver_enabled = usb_driver_is_enabled();
    const bool hid_ready = driver_enabled && hid_input_usb_is_available();
    portENTER_CRITICAL(&s_state_lock);
    if (s_ctx.configuration_cancel_revision == request_revision) {
        portEXIT_CRITICAL(&s_state_lock);
        return false;
    }
    s_ctx.configuration = *configuration;
    s_ctx.configuration_store = *store;
    s_ctx.configuration_result =
        JETKVM_USB_SERVICE_CONFIGURATION_COMPLETED;
    s_ctx.configuration_completed_revision = request_revision;
    s_ctx.state.configuration_pending = false;
    s_ctx.state.configuration_transitioning = false;
    s_ctx.state.configuration_generation = store->generation;
    s_ctx.state.configuration_store_degraded = store->degraded;
    if (s_ctx.configuration_emulation_enabled >= 0) {
        s_ctx.state.desired_enabled = driver_enabled;
    }
    s_ctx.state.last_configuration_error = ESP_OK;
    s_ctx.state.configuration_transactions_completed++;
    s_ctx.state.driver_enabled = driver_enabled;
    s_ctx.state.hid_input_ready = hid_ready;
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_ctx.configuration_done);
    publish_state();
    return true;
}

static bool configuration_request_cancelled(uint32_t request_revision)
{
    portENTER_CRITICAL(&s_state_lock);
    bool cancelled =
        s_ctx.configuration_cancel_revision == request_revision;
    portEXIT_CRITICAL(&s_state_lock);
    return cancelled;
}

static void apply_emulation_request(const jetkvm_usb_config_t *previous,
    const jetkvm_usb_config_store_state_t *store, uint32_t revision, bool enabled)
{
    const bool was_enabled = usb_driver_is_enabled();
    const bool was_hid_ready = hid_input_usb_is_available();
    bool hid_ready = false;
    esp_err_t error = enabled ? enable_usb_stack(&hid_ready) : disable_usb_stack();
    if (error == ESP_OK && complete_configuration_success(previous, store, revision)) return;
    bool hardware_changed = was_enabled != usb_driver_is_enabled() ||
        was_hid_ready != hid_input_usb_is_available();
    bool rollback_failed = hardware_changed &&
        restore_configuration(previous, was_enabled) != ESP_OK;
    complete_configuration_request(JETKVM_USB_SERVICE_CONFIGURATION_FAILED,
        error == ESP_OK ? ESP_ERR_TIMEOUT : error, false, hardware_changed, rollback_failed, revision);
}

static void apply_pending_configuration(void)
{
    jetkvm_usb_config_t candidate;
    jetkvm_usb_config_t previous;
    jetkvm_usb_config_store_state_t previous_store;
    uint32_t request_revision;
    bool force_reenumeration;
    int emulation_enabled;

    portENTER_CRITICAL(&s_state_lock);
    if (!s_ctx.state.configuration_pending) {
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }
    candidate = s_ctx.requested_configuration;
    previous = s_ctx.configuration;
    previous_store = s_ctx.configuration_store;
    request_revision = s_ctx.configuration_request_revision;
    force_reenumeration = s_ctx.configuration_force_reenumeration;
    emulation_enabled = s_ctx.configuration_emulation_enabled;
    s_ctx.state.configuration_transitioning = true;
    portEXIT_CRITICAL(&s_state_lock);
    publish_state();

    if (configuration_request_cancelled(request_revision)) {
        complete_configuration_request(
            JETKVM_USB_SERVICE_CONFIGURATION_FAILED, ESP_ERR_TIMEOUT,
            false, false, false, request_revision);
        return;
    }

    if (emulation_enabled >= 0) {
        apply_emulation_request(&previous, &previous_store, request_revision, emulation_enabled != 0);
        return;
    }

    const bool configuration_changed =
        !jetkvm_usb_config_equal(&candidate, &previous);
    if (!configuration_changed && !force_reenumeration) {
        if (!complete_configuration_success(
                &previous, &previous_store, request_revision)) {
            complete_configuration_request(
                JETKVM_USB_SERVICE_CONFIGURATION_FAILED, ESP_ERR_TIMEOUT,
                false, false, false, request_revision);
        }
        return;
    }

    const bool was_enabled = usb_driver_is_enabled();
    bool hardware_changed = false;
    esp_err_t error = ESP_OK;
    if (was_enabled) {
        error = disable_usb_stack();
        if (error != ESP_OK) {
            complete_configuration_request(
                JETKVM_USB_SERVICE_CONFIGURATION_FAILED, error,
                false, false, false, request_revision);
            return;
        }
        hardware_changed = true;
        /* Keep the pull-up absent long enough for the target host to observe
         * a real disconnect.  An immediate uninstall/install can be treated
         * as a transport reset by Linux, which preserves the previous MSC
         * INQUIRY type when switching between Disk and CDROM. */
        vTaskDelay(milliseconds_to_ticks_ceil(USB_REENUMERATION_DETACH_MS));
    }

    if (configuration_request_cancelled(request_revision)) {
        error = ESP_ERR_TIMEOUT;
    }

    if (error == ESP_OK) {
        error = select_descriptor_configuration(&candidate);
    }
    if (error == ESP_OK) {
        hardware_changed = true;
    }
    if (error == ESP_OK && was_enabled) {
        bool hid_ready = false;
        error = enable_usb_stack(&hid_ready);
    }

    jetkvm_usb_config_store_state_t candidate_store = previous_store;
    bool persistence_attempted = false;
    if (error == ESP_OK && configuration_changed &&
        !configuration_request_cancelled(request_revision)) {
        persistence_attempted = true;
        error = jetkvm_usb_config_save(&candidate, &candidate_store);
    } else if (error == ESP_OK &&
               configuration_request_cancelled(request_revision)) {
        error = ESP_ERR_TIMEOUT;
    }

    if (error == ESP_OK) {
        if (complete_configuration_success(
                &candidate, &candidate_store, request_revision)) {
            if (force_reenumeration && !configuration_changed) {
                ESP_LOGI(TAG, "target USB safely re-enumerated");
            } else {
                ESP_LOGI(TAG, "target USB configuration committed");
            }
            return;
        }
        error = ESP_ERR_TIMEOUT;
    }

    bool rollback_failed = false;
    if (hardware_changed) {
        esp_err_t rollback_error = restore_configuration(&previous,
                                                         was_enabled);
        rollback_failed = rollback_error != ESP_OK;
        if (rollback_failed) {
            ESP_LOGE(TAG, "target USB hardware rollback failed: %s",
                     esp_err_to_name(rollback_error));
        }
    }

    /* A save can fail after its inactive-slot commit but before verification.
     * Overwrite that same slot with the previous snapshot so a reboot cannot
     * select a candidate which this transaction rejected. */
    if (persistence_attempted) {
        jetkvm_usb_config_store_state_t repaired_store = previous_store;
        esp_err_t repair_error = jetkvm_usb_config_save(
            &previous, &repaired_store);
        if (repair_error != ESP_OK) {
            rollback_failed = true;
            ESP_LOGE(TAG, "target USB persistence rollback failed: %s",
                     esp_err_to_name(repair_error));
        } else {
            portENTER_CRITICAL(&s_state_lock);
            s_ctx.configuration_store = repaired_store;
            s_ctx.state.configuration_generation = repaired_store.generation;
            s_ctx.state.configuration_store_degraded = repaired_store.degraded;
            portEXIT_CRITICAL(&s_state_lock);
        }
    }

    complete_configuration_request(
        JETKVM_USB_SERVICE_CONFIGURATION_FAILED, error, false,
        hardware_changed, rollback_failed, request_revision);
    ESP_LOGW(TAG, "target USB configuration rejected and restored: %s",
             esp_err_to_name(error));
}

static bool begin_latest_transition(bool *enabled,
                                    uint32_t *lifecycle_generation,
                                    uint32_t *desired_revision)
{
    portENTER_CRITICAL(&s_state_lock);
    if (!s_ctx.state.initialized) {
        portEXIT_CRITICAL(&s_state_lock);
        return false;
    }
    *enabled = s_ctx.state.desired_enabled;
    *desired_revision = s_ctx.desired_revision;
    s_ctx.state.transitioning = true;
    s_ctx.state.command_pending = false;
    s_ctx.state.lifecycle_generation++;
    *lifecycle_generation = s_ctx.state.lifecycle_generation;
    portEXIT_CRITICAL(&s_state_lock);
    publish_state();
    return true;
}

static esp_err_t apply_latest_enabled(void)
{
    bool enabled;
    uint32_t lifecycle_generation;
    uint32_t desired_revision;
    if (!begin_latest_transition(&enabled, &lifecycle_generation,
                                 &desired_revision)) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t error = ESP_OK;
    bool hid_ready = false;
    if (enabled) {
        if (!usb_driver_is_enabled() || !hid_input_usb_is_available()) {
            error = enable_usb_stack(&hid_ready);
        } else {
            hid_ready = true;
        }
    } else {
        error = disable_usb_stack();
    }

    const bool driver_enabled = usb_driver_is_enabled();
    hid_ready = driver_enabled && hid_input_usb_is_available();

    portENTER_CRITICAL(&s_state_lock);
    s_ctx.state.driver_enabled = driver_enabled;
    s_ctx.state.hid_input_ready = hid_ready;
    s_ctx.state.transitioning = false;
    s_ctx.state.last_error = error;
    if (error == ESP_OK) {
        s_ctx.state.transitions_completed++;
    } else {
        /* A failed asynchronous transition must not leave the protocol's
         * desired state permanently disagreeing with a usable stack. */
        if (s_ctx.desired_revision == desired_revision) {
            s_ctx.state.desired_enabled = hid_ready;
        }
        s_ctx.state.transition_failures++;
    }
    portEXIT_CRITICAL(&s_state_lock);
    publish_state();

    if (error == ESP_OK) {
        ESP_LOGI(TAG, "target USB %s generation=%" PRIu32,
                 driver_enabled ? "enabled" : "disabled",
                 lifecycle_generation);
    } else {
        ESP_LOGW(TAG,
                 "target USB transition enabled=%d driver=%d hid=%d failed: %s",
                 enabled, driver_enabled, hid_ready, esp_err_to_name(error));
    }
    return error;
}

static void usb_owner_task(void *arg)
{
    (void)arg;
    /* The creator publishes owner_task and initialized before opening this
     * gate, so the first callback can never expose a half-started owner. */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    esp_err_t initial_error = apply_latest_enabled();
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.initial_attempt_error = initial_error;
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_ctx.initial_attempt_done);

    for (;;) {
        for (;;) {
            bool configuration_pending;
            bool command_pending;
            portENTER_CRITICAL(&s_state_lock);
            configuration_pending = s_ctx.state.configuration_pending;
            command_pending = s_ctx.state.command_pending;
            portEXIT_CRITICAL(&s_state_lock);
            if (configuration_pending) {
                apply_pending_configuration();
                continue;
            }
            if (command_pending) {
                (void)apply_latest_enabled();
                continue;
            }
            break;
        }
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

esp_err_t jetkvm_usb_service_start(
    jetkvm_usb_service_state_callback_t callback, void *callback_ctx)
{
    portENTER_CRITICAL(&s_state_lock);
    if (s_ctx.starting || s_ctx.state.initialized ||
        s_ctx.owner_task != NULL) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.starting = true;
    portEXIT_CRITICAL(&s_state_lock);

    jetkvm_usb_config_t configuration;
    jetkvm_usb_config_store_state_t configuration_store;
    esp_err_t configuration_load_error = jetkvm_usb_config_load(
        &configuration, &configuration_store);
    if (configuration_load_error != ESP_OK) {
        ESP_LOGW(TAG, "USB configuration load failed; using safe defaults: %s",
                 esp_err_to_name(configuration_load_error));
    }
    esp_err_t descriptor_error =
        select_descriptor_configuration(&configuration);
    if (descriptor_error != ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.starting = false;
        portEXIT_CRITICAL(&s_state_lock);
        return descriptor_error;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_ctx.callback = callback;
    s_ctx.callback_ctx = callback_ctx;
    s_ctx.state.desired_enabled = true;
    s_ctx.desired_revision = 1U;
    s_ctx.state.last_error = ESP_OK;
    s_ctx.configuration = configuration;
    s_ctx.configuration_store = configuration_store;
    s_ctx.state.configuration_ready = true;
    s_ctx.state.configuration_generation = configuration_store.generation;
    s_ctx.state.configuration_store_degraded = configuration_store.degraded;
    s_ctx.state.configuration_load_error = configuration_load_error;
    s_ctx.state.last_configuration_error = ESP_OK;
    portEXIT_CRITICAL(&s_state_lock);

    SemaphoreHandle_t initial_attempt_done = xSemaphoreCreateBinaryStatic(
        &s_ctx.initial_attempt_done_storage);
    SemaphoreHandle_t configuration_call_mutex = xSemaphoreCreateMutexStatic(
        &s_ctx.configuration_call_mutex_storage);
    SemaphoreHandle_t configuration_done = xSemaphoreCreateBinaryStatic(
        &s_ctx.configuration_done_storage);
    if (initial_attempt_done == NULL || configuration_call_mutex == NULL ||
        configuration_done == NULL) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.starting = false;
        s_ctx.callback = NULL;
        s_ctx.callback_ctx = NULL;
        s_ctx.state = (jetkvm_usb_service_snapshot_t){0};
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t owner_task = NULL;
    if (xTaskCreatePinnedToCore(
            usb_owner_task, "usb_owner", USB_OWNER_TASK_STACK_SIZE, NULL,
            USB_OWNER_TASK_PRIORITY, &owner_task, USB_OWNER_TASK_CORE) !=
        pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.callback = NULL;
        s_ctx.callback_ctx = NULL;
        s_ctx.state = (jetkvm_usb_service_snapshot_t){0};
        s_ctx.starting = false;
        s_ctx.desired_revision = 0U;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_ctx.initial_attempt_done = initial_attempt_done;
    s_ctx.configuration_call_mutex = configuration_call_mutex;
    s_ctx.configuration_done = configuration_done;
    s_ctx.owner_task = owner_task;
    s_ctx.state.initialized = true;
    s_ctx.starting = false;
    portEXIT_CRITICAL(&s_state_lock);
    xTaskNotifyGive(owner_task);

    if (xSemaphoreTake(
            initial_attempt_done,
            milliseconds_to_ticks_ceil(USB_OWNER_START_TIMEOUT_MS)) !=
        pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_state_lock);
    esp_err_t initial_error = s_ctx.initial_attempt_error;
    portEXIT_CRITICAL(&s_state_lock);
    return initial_error;
}

jetkvm_usb_service_request_result_t
jetkvm_usb_service_request_enabled(bool enabled)
{
    bool replaces_pending;
    TaskHandle_t owner_task;

    portENTER_CRITICAL(&s_state_lock);
    if (!s_ctx.state.initialized || s_ctx.owner_task == NULL ||
        s_ctx.state.configuration_pending || s_ctx.state.configuration_transitioning) {
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_USB_SERVICE_REQUEST_UNAVAILABLE;
    }
    owner_task = s_ctx.owner_task;
    replaces_pending = s_ctx.state.command_pending;
    s_ctx.state.desired_enabled = enabled;
    s_ctx.desired_revision++;
    s_ctx.state.command_pending = true;
    s_ctx.state.commands_accepted++;
    if (replaces_pending) {
        s_ctx.state.commands_replaced++;
    }
    portEXIT_CRITICAL(&s_state_lock);

    xTaskNotifyGive(owner_task);
    return JETKVM_USB_SERVICE_REQUEST_ACCEPTED;
}

esp_err_t jetkvm_usb_service_get_snapshot(
    jetkvm_usb_service_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    *snapshot = s_ctx.state;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t jetkvm_usb_service_get_configuration(
    jetkvm_usb_config_t *configuration)
{
    if (configuration == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    if (!s_ctx.state.initialized || !s_ctx.state.configuration_ready) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    *configuration = s_ctx.configuration;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

static jetkvm_usb_service_configuration_result_t
apply_configuration_request(const jetkvm_usb_config_t *configuration,
                            bool force_reenumeration, int emulation_enabled)
{
    if (!force_reenumeration && emulation_enabled < 0 && configuration == NULL) {
        return JETKVM_USB_SERVICE_CONFIGURATION_INVALID;
    }
    if (!force_reenumeration && emulation_enabled < 0 &&
        configuration->serial_console) {
        return JETKVM_USB_SERVICE_CONFIGURATION_UNSUPPORTED;
    }
    if (!force_reenumeration && emulation_enabled < 0 && !jetkvm_usb_config_validate(configuration)) {
        return JETKVM_USB_SERVICE_CONFIGURATION_INVALID;
    }
    SemaphoreHandle_t configuration_call_mutex;
    SemaphoreHandle_t configuration_done;
    portENTER_CRITICAL(&s_state_lock);
    if (!s_ctx.state.initialized || !s_ctx.state.configuration_ready ||
        s_ctx.owner_task == NULL || s_ctx.configuration_call_mutex == NULL ||
        s_ctx.configuration_done == NULL) {
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_USB_SERVICE_CONFIGURATION_UNAVAILABLE;
    }
    configuration_call_mutex = s_ctx.configuration_call_mutex;
    configuration_done = s_ctx.configuration_done;
    portEXIT_CRITICAL(&s_state_lock);
    if (xSemaphoreTake(configuration_call_mutex, 0U) != pdTRUE) {
        return JETKVM_USB_SERVICE_CONFIGURATION_BUSY;
    }

    (void)xSemaphoreTake(configuration_done, 0U);
    TaskHandle_t owner_task;
    uint32_t request_revision;
    portENTER_CRITICAL(&s_state_lock);
    if (!s_ctx.state.initialized || !s_ctx.state.configuration_ready ||
        s_ctx.owner_task == NULL) {
        portEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(configuration_call_mutex);
        return JETKVM_USB_SERVICE_CONFIGURATION_UNAVAILABLE;
    }
    if (s_ctx.state.configuration_pending || s_ctx.state.command_pending ||
        s_ctx.state.transitioning || s_ctx.state.configuration_transitioning) {
        portEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(configuration_call_mutex);
        return JETKVM_USB_SERVICE_CONFIGURATION_BUSY;
    }
    s_ctx.requested_configuration = (force_reenumeration || emulation_enabled >= 0) ?
        s_ctx.configuration : *configuration;
    s_ctx.configuration_force_reenumeration = force_reenumeration;
    s_ctx.configuration_emulation_enabled = emulation_enabled;
    s_ctx.configuration_request_revision++;
    if (s_ctx.configuration_request_revision == 0U) {
        s_ctx.configuration_request_revision = 1U;
    }
    request_revision = s_ctx.configuration_request_revision;
    s_ctx.configuration_cancel_revision = 0U;
    s_ctx.state.configuration_pending = true;
    s_ctx.state.configuration_requests++;
    owner_task = s_ctx.owner_task;
    portEXIT_CRITICAL(&s_state_lock);
    xTaskNotifyGive(owner_task);

    const TickType_t timeout =
        milliseconds_to_ticks_ceil(USB_CONFIGURATION_TIMEOUT_MS);
    const TickType_t wait_started = xTaskGetTickCount();
    TickType_t remaining = timeout;
    bool cancellation_requested = false;
    for (;;) {
        if (xSemaphoreTake(configuration_done, remaining) !=
            pdTRUE) {
            portENTER_CRITICAL(&s_state_lock);
            bool request_completed =
                s_ctx.configuration_completed_revision == request_revision;
            jetkvm_usb_service_configuration_result_t completed_result =
                s_ctx.configuration_result;
            if (!request_completed) {
                s_ctx.configuration_cancel_revision = request_revision;
                s_ctx.state.configuration_wait_timeouts++;
            }
            portEXIT_CRITICAL(&s_state_lock);
            if (request_completed) {
                xSemaphoreGive(configuration_call_mutex);
                return completed_result;
            }
            cancellation_requested = true;
            xTaskNotifyGive(owner_task);
            remaining = portMAX_DELAY;
            continue;
        }
        portENTER_CRITICAL(&s_state_lock);
        bool request_completed =
            s_ctx.configuration_completed_revision == request_revision;
        portEXIT_CRITICAL(&s_state_lock);
        if (request_completed) {
            break;
        }
        if (cancellation_requested) {
            remaining = portMAX_DELAY;
            continue;
        }
        const TickType_t elapsed = xTaskGetTickCount() - wait_started;
        if (elapsed >= timeout) {
            portENTER_CRITICAL(&s_state_lock);
            s_ctx.configuration_cancel_revision = request_revision;
            s_ctx.state.configuration_wait_timeouts++;
            portEXIT_CRITICAL(&s_state_lock);
            cancellation_requested = true;
            xTaskNotifyGive(owner_task);
            remaining = portMAX_DELAY;
            continue;
        }
        remaining = timeout - elapsed;
    }
    portENTER_CRITICAL(&s_state_lock);
    jetkvm_usb_service_configuration_result_t result =
        s_ctx.configuration_result;
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(configuration_call_mutex);
    return result;
}

jetkvm_usb_service_configuration_result_t
jetkvm_usb_service_apply_configuration(
    const jetkvm_usb_config_t *configuration)
{
    return apply_configuration_request(configuration, false, -1);
}

jetkvm_usb_service_configuration_result_t
jetkvm_usb_service_reenumerate(void)
{
    return apply_configuration_request(NULL, true, -1);
}

jetkvm_usb_service_configuration_result_t
jetkvm_usb_service_present_medium(bool cdrom_image)
{
    if (cdrom_image && jetkvm_usb_hid_cdrom_drive_takes_medium()) {
        return JETKVM_USB_SERVICE_CONFIGURATION_COMPLETED;
    }
    return jetkvm_usb_service_reenumerate();
}

jetkvm_usb_service_configuration_result_t
jetkvm_usb_service_set_enabled(bool enabled)
{
    return apply_configuration_request(NULL, false, enabled ? 1 : 0);
}
