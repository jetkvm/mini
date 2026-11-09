/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: MIT
 */

#include "jetkvm_hid_input.h"

#include <limits.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jetkvm_timing_log.h"

#define BRIDGE_TASK_NAME "jetkvm_hid"
#define BRIDGE_TASK_STACK_SIZE 4096U
#define BRIDGE_TASK_PRIORITY 7U
#define AUTO_RELEASE_SLOTS JETKVM_USB_HID_KEY_COUNT
#define RELEASE_IMMEDIATE_ATTEMPTS 2U
#define RELEASE_RETRY_INTERVAL_MS 100U
#define MACRO_QUEUE_LENGTH 1U
#define USB_TRANSITION_TIMEOUT_MS 1000U

_Static_assert(JETKVM_USB_HID_DESCRIPTOR_STRING_CAPACITY ==
                   JETKVM_USB_DESCRIPTOR_STRING_CAPACITY,
               "USB snapshot string capacities must match");
_Static_assert(JETKVM_HID_KEEPALIVE_BASE_EXTENSION_US >=
                   JETKVM_HID_KEEPALIVE_MAX_LATENESS_US,
               "keepalive extension must cover the jitter budget");

void jetkvm_hid_keepalive_policy_reset(
    jetkvm_hid_keepalive_policy_state_t *state)
{
    if (state != NULL) {
        memset(state, 0, sizeof(*state));
    }
}

jetkvm_hid_keepalive_decision_t jetkvm_hid_keepalive_policy_evaluate(
    jetkvm_hid_keepalive_policy_state_t *state,
    uint32_t generation,
    uint32_t epoch,
    int64_t received_at_us,
    int64_t evaluated_at_us)
{
    jetkvm_hid_keepalive_decision_t decision = {
        .result = JETKVM_HID_KEEPALIVE_INVALID_ARGUMENT,
    };
    if (state == NULL || generation == 0U || received_at_us < 0 ||
        evaluated_at_us < received_at_us ||
        received_at_us >
            INT64_MAX - JETKVM_HID_KEEPALIVE_BASE_EXTENSION_US) {
        return decision;
    }

    if (evaluated_at_us - received_at_us >
        JETKVM_HID_KEEPALIVE_MAX_STALENESS_US) {
        decision.result = JETKVM_HID_KEEPALIVE_REJECTED_STALE;
        return decision;
    }

    if (!state->initialized || state->generation != generation ||
        state->epoch != epoch) {
        jetkvm_hid_keepalive_policy_reset(state);
        state->generation = generation;
        state->epoch = epoch;
    } else {
        /* A keepalive can be inserted at the queue front. Never let an older
         * arrival processed later move the policy clock backwards. */
        if (received_at_us <= state->last_arrival_us) {
            decision.result = JETKVM_HID_KEEPALIVE_REJECTED_LATE;
            return decision;
        }
        if (received_at_us - state->last_timer_reset_us >
            JETKVM_HID_KEEPALIVE_MAX_STALENESS_US) {
            decision.result = JETKVM_HID_KEEPALIVE_REJECTED_STALE;
            return decision;
        }
    }

    int64_t extension_us = JETKVM_HID_KEEPALIVE_BASE_EXTENSION_US;
    if (state->initialized) {
        const int64_t interval_us =
            received_at_us - state->last_arrival_us;
        const int64_t lateness_us =
            interval_us - JETKVM_HID_KEEPALIVE_EXPECTED_INTERVAL_US;
        if (lateness_us > JETKVM_HID_KEEPALIVE_MAX_LATENESS_US) {
            decision.result = JETKVM_HID_KEEPALIVE_REJECTED_LATE;
            return decision;
        }
        if (lateness_us > 0) {
            extension_us -= lateness_us;
            decision.result = JETKVM_HID_KEEPALIVE_JITTER_ADJUSTED;
        } else {
            decision.result = JETKVM_HID_KEEPALIVE_ACCEPTED;
        }
    } else {
        decision.result = JETKVM_HID_KEEPALIVE_ACCEPTED;
    }

    decision.extension_us = extension_us;
    decision.deadline_us = received_at_us + extension_us;
    state->initialized = true;
    state->generation = generation;
    state->epoch = epoch;
    state->last_arrival_us = received_at_us;
    state->last_timer_reset_us = received_at_us;
    return decision;
}

typedef enum {
    COMMAND_KEYBOARD,
    COMMAND_KEYPRESS,
    COMMAND_ABSOLUTE_MOUSE,
    COMMAND_RELATIVE_MOUSE,
    COMMAND_WHEEL,
    COMMAND_KEEPALIVE,
    COMMAND_RELEASE_ALL,
    COMMAND_PAUSE,
    COMMAND_RESUME,
    COMMAND_SHUTDOWN,
} command_type_t;

typedef struct {
    command_type_t type;
    bool local;
    uint32_t generation;
    uint32_t epoch;
    uint32_t barrier_token;
    int64_t received_at_us;
    union {
        jetkvm_hidrpc_keyboard_report_t keyboard;
        jetkvm_hidrpc_keypress_report_t keypress;
        jetkvm_hidrpc_abs_mouse_report_t absolute_mouse;
        jetkvm_hidrpc_rel_mouse_report_t relative_mouse;
        jetkvm_hidrpc_wheel_report_t wheel;
    } data;
} input_command_t;

typedef struct {
    uint32_t generation;
    uint32_t epoch;
    uint32_t action_sequence;
    bool is_paste;
    uint32_t step_count;
    jetkvm_hidrpc_macro_step_t steps[JETKVM_HIDRPC_MACRO_MAX_STEPS];
} macro_command_t;

typedef struct {
    bool active;
    uint32_t next_step;
    int64_t next_step_at_us;
    macro_command_t command;
} macro_runtime_t;

typedef struct {
    uint8_t usage;
    uint32_t generation;
    uint32_t epoch;
    int64_t deadline_us;
} automatic_release_t;

typedef struct {
    bool initialized;
    bool initializing;
    bool stopping;
    bool usb_available;
    bool worker_paused;
    bool accepting_input;
    uint32_t active_generation;
    uint32_t active_epoch;
    uint8_t keyboard_leds;
    uint8_t submitted_absolute_buttons;
    uint8_t submitted_relative_buttons;
    bool overflow_release;
    uint32_t macro_action_sequence;
    uint32_t macro_cancel_sequence;
    bool macro_active;
    bool macro_is_paste;
    uint32_t macro_step_index;
    uint32_t macro_step_count;
    uint64_t macro_state_revision;
    uint32_t keyboard_revision;
    int64_t last_user_input_us;
    TaskHandle_t worker_task;
    QueueHandle_t queue;
    QueueHandle_t macro_queue;
    SemaphoreHandle_t worker_done;
    SemaphoreHandle_t pause_done;
    SemaphoreHandle_t resume_done;
    esp_err_t pause_result;
    esp_err_t resume_result;
    uint32_t barrier_sequence;
    uint32_t pause_ack_token;
    uint32_t resume_ack_token;
    SemaphoreHandle_t admin_mutex;
    SemaphoreHandle_t macro_mutex;
    jetkvm_hid_input_stats_t stats;
} bridge_context_t;

static portMUX_TYPE s_state_lock = portMUX_INITIALIZER_UNLOCKED;
static bridge_context_t s_ctx;
static StaticQueue_t s_queue_control;
static uint8_t s_queue_storage[JETKVM_HID_INPUT_QUEUE_LENGTH *
                               sizeof(input_command_t)];
static StaticQueue_t s_macro_queue_control;
static uint8_t s_macro_queue_storage[MACRO_QUEUE_LENGTH *
                                     sizeof(macro_command_t)];
static macro_command_t s_incoming_macro;
static macro_command_t s_discarded_macro;
static StaticSemaphore_t s_worker_done_storage;
static StaticSemaphore_t s_pause_done_storage;
static StaticSemaphore_t s_resume_done_storage;
static StaticSemaphore_t s_admin_mutex_storage;
static StaticSemaphore_t s_macro_mutex_storage;

static TickType_t milliseconds_to_ticks_ceil(uint32_t milliseconds)
{
    TickType_t ticks = pdMS_TO_TICKS(milliseconds);
    if (ticks == 0U) {
        ticks = 1U;
    }
    return ticks;
}

static TickType_t deadline_remaining_ticks(int64_t deadline_us)
{
    int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    uint64_t remaining_ms = ((uint64_t)remaining_us + 999U) / 1000U;
    return milliseconds_to_ticks_ceil(
        remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_ms);
}

static uint32_t next_macro_action_sequence_locked(void)
{
    s_ctx.macro_action_sequence++;
    if (s_ctx.macro_action_sequence == 0U) {
        s_ctx.macro_action_sequence++;
    }
    return s_ctx.macro_action_sequence;
}

static void notify_worker(void)
{
    TaskHandle_t worker;
    portENTER_CRITICAL(&s_state_lock);
    worker = s_ctx.worker_task;
    portEXIT_CRITICAL(&s_state_lock);
    if (worker != NULL) {
        xTaskNotifyGive(worker);
    }
}

static void publish_macro_state(const macro_runtime_t *runtime)
{
    portENTER_CRITICAL(&s_state_lock);
    if (runtime != NULL && runtime->active) {
        s_ctx.macro_active = true;
        s_ctx.macro_is_paste = runtime->command.is_paste;
        s_ctx.macro_step_index = runtime->next_step;
        s_ctx.macro_step_count = runtime->command.step_count;
    } else {
        s_ctx.macro_active = false;
        s_ctx.macro_step_index = 0U;
        s_ctx.macro_step_count = 0U;
    }
    s_ctx.macro_state_revision++;
    portEXIT_CRITICAL(&s_state_lock);
}

static uint32_t macro_cancel_sequence(void)
{
    uint32_t sequence;
    portENTER_CRITICAL(&s_state_lock);
    sequence = s_ctx.macro_cancel_sequence;
    portEXIT_CRITICAL(&s_state_lock);
    return sequence;
}

static void record_usb_result(esp_err_t error)
{
    if (error == ESP_OK) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.stats.usb_errors++;
    portEXIT_CRITICAL(&s_state_lock);
}

/* Keyboard write-timeout streak, mirroring upstream usbgadget/recovery.go:
 * HidWriteTimeoutEscalationThreshold = 3, HidWriteRecoveryRetryInterval = 30 s.
 * Only the HID worker touches these. */
#define HID_WRITE_TIMEOUT_ESCALATION_THRESHOLD 3U
#define HID_WRITE_RECOVERY_RETRY_INTERVAL_US (INT64_C(30) * 1000000)
/* Upstream usbgadget hidWriteTimeout. */
#define HID_WRITE_DEADLINE_US (INT64_C(100) * 1000)
static jetkvm_hid_write_recovery_fn s_write_recovery_fn;
static void *s_write_recovery_ctx;
static jetkvm_hid_keyboard_changed_fn s_keyboard_changed_fn;
static void *s_keyboard_changed_ctx;
static unsigned s_keyboard_timeout_streak;
static int64_t s_last_write_recovery_us;

esp_err_t jetkvm_hid_input_set_write_recovery_handler(
    jetkvm_hid_write_recovery_fn handler, void *ctx)
{
    s_write_recovery_fn = handler;
    s_write_recovery_ctx = ctx;
    return ESP_OK;
}

esp_err_t jetkvm_hid_input_set_keyboard_changed_handler(
    jetkvm_hid_keyboard_changed_fn handler, void *ctx)
{
    s_keyboard_changed_fn = handler;
    s_keyboard_changed_ctx = ctx;
    return ESP_OK;
}

/* The keyboard state at the last revision; only the HID worker uses it. */
static jetkvm_usb_hid_keyboard_state_t s_revision_keyboard;

/* Tell the consumer the host accepted a keyboard change; runs on the HID worker.
 * The revision lets a consumer that reads the state later see a change that
 * was undone before it read, such as a short press and its release. A report
 * that leaves the keys as they were, such as an auto-repeated key down, does
 * not advance it. */
static void note_keyboard_changed(void)
{
    jetkvm_usb_hid_keyboard_state_t keyboard;
    if (jetkvm_usb_hid_get_keyboard_state(&keyboard) == ESP_OK &&
        memcmp(&keyboard, &s_revision_keyboard, sizeof(keyboard)) != 0) {
        s_revision_keyboard = keyboard;
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.keyboard_revision++;
        portEXIT_CRITICAL(&s_state_lock);
    }
    if (s_keyboard_changed_fn) {
        s_keyboard_changed_fn(s_keyboard_changed_ctx);
    }
}

static void note_keyboard_write_result(int type, esp_err_t error)
{
    if (type != COMMAND_KEYBOARD && type != COMMAND_KEYPRESS) {
        return;
    }
    if (error != ESP_ERR_TIMEOUT && error != ESP_ERR_NOT_FINISHED) {
        s_keyboard_timeout_streak = 0;
        return;
    }
    jetkvm_usb_hid_state_t usb = {0};
    if (jetkvm_usb_hid_get_state(&usb) != ESP_OK || !usb.initialized ||
        !usb.mounted || usb.suspended) {
        /* Not configured: the host is gone or asleep, nothing to recover. */
        s_keyboard_timeout_streak = 0;
        return;
    }
    if (jetkvm_usb_hid_bus_events_waiting()) {
        /* The report may have waited for this device and not for the host:
         * TinyUSB's task, which is below this worker and shares its core
         * with other work, has not handled what the bus sent, and the
         * completion of the report before this one can be among it. Such a
         * timeout is no sign of a host that stopped reading, so it does not
         * count. It does not end the streak either: only a report that the
         * host took does. */
        return;
    }
    if (++s_keyboard_timeout_streak < HID_WRITE_TIMEOUT_ESCALATION_THRESHOLD) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    if (s_last_write_recovery_us != 0 &&
        now_us - s_last_write_recovery_us < HID_WRITE_RECOVERY_RETRY_INTERVAL_US) {
        return;
    }
    s_last_write_recovery_us = now_us;
    const unsigned streak = s_keyboard_timeout_streak;
    s_keyboard_timeout_streak = 0;
    if (s_write_recovery_fn) {
        s_write_recovery_fn(streak, s_write_recovery_ctx);
    }
}

static void record_user_input_now(void)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.last_user_input_us = now_us;
    portEXIT_CRITICAL(&s_state_lock);
}

static bool generation_is_active(uint32_t generation, uint32_t epoch)
{
    bool active;
    portENTER_CRITICAL(&s_state_lock);
    active = s_ctx.initialized && !s_ctx.stopping &&
             s_ctx.accepting_input && generation != 0U &&
             generation == s_ctx.active_generation &&
             epoch == s_ctx.active_epoch;
    portEXIT_CRITICAL(&s_state_lock);
    return active;
}

static bool is_modifier_usage(uint8_t usage)
{
    return usage >= 0xe0U && usage <= 0xe7U;
}

static void clear_automatic_releases(
    automatic_release_t releases[],
    jetkvm_hid_keepalive_policy_state_t *keepalive)
{
    memset(releases, 0,
           sizeof(automatic_release_t) * AUTO_RELEASE_SLOTS);
    jetkvm_hid_keepalive_policy_reset(keepalive);
}

static void remove_automatic_release(automatic_release_t releases[],
                                     uint8_t usage)
{
    for (size_t i = 0; i < AUTO_RELEASE_SLOTS; ++i) {
        if (releases[i].usage == usage) {
            memset(&releases[i], 0, sizeof(releases[i]));
        }
    }
}

static void arm_automatic_release(automatic_release_t releases[],
                                  uint8_t usage, uint32_t generation,
                                  uint32_t epoch, int64_t now_us)
{
    size_t free_index = AUTO_RELEASE_SLOTS;
    for (size_t i = 0; i < AUTO_RELEASE_SLOTS; ++i) {
        if (releases[i].usage == usage) {
            free_index = i;
            break;
        }
        if (releases[i].usage == 0U && free_index == AUTO_RELEASE_SLOTS) {
            free_index = i;
        }
    }
    if (free_index == AUTO_RELEASE_SLOTS) {
        return;
    }
    releases[free_index] = (automatic_release_t) {
        .usage = usage,
        .generation = generation,
        .epoch = epoch,
        .deadline_us = now_us +
                       (int64_t)JETKVM_HID_INPUT_KEYPRESS_RELEASE_MS * 1000,
    };
}

static void extend_automatic_releases(automatic_release_t releases[],
                                      uint32_t generation, uint32_t epoch,
                                      int64_t deadline_us)
{
    for (size_t i = 0; i < AUTO_RELEASE_SLOTS; ++i) {
        if (releases[i].usage != 0U &&
            releases[i].generation == generation &&
            releases[i].epoch == epoch) {
            releases[i].deadline_us = deadline_us;
        }
    }
}

static TickType_t next_automatic_release_wait(
    const automatic_release_t releases[], int64_t now_us)
{
    int64_t nearest = INT64_MAX;
    for (size_t i = 0; i < AUTO_RELEASE_SLOTS; ++i) {
        if (releases[i].usage != 0U && releases[i].deadline_us < nearest) {
            nearest = releases[i].deadline_us;
        }
    }
    if (nearest == INT64_MAX) {
        return portMAX_DELAY;
    }
    if (nearest <= now_us) {
        return 0;
    }

    const uint64_t remaining_ms =
        (uint64_t)(nearest - now_us + 999) / 1000U;
    TickType_t wait = pdMS_TO_TICKS(remaining_ms);
    return wait == 0 ? 1 : wait;
}

static bool execute_release_all(
    automatic_release_t releases[],
    jetkvm_hid_keepalive_policy_state_t *keepalive)
{
    clear_automatic_releases(releases, keepalive);
    esp_err_t error = ESP_FAIL;
    bool complete = false;
    jetkvm_usb_hid_state_t usb = {0};
    for (uint32_t attempt = 0; attempt < RELEASE_IMMEDIATE_ATTEMPTS;
         ++attempt) {
        error = jetkvm_usb_hid_release_all();
        if (error == ESP_OK) {
            note_keyboard_write_result(COMMAND_KEYBOARD, ESP_OK);
            note_keyboard_changed();
            complete = true;
            break;
        }
        bool have_usb = jetkvm_usb_hid_get_state(&usb) == ESP_OK;
        if (have_usb && (!usb.initialized || !usb.mounted)) {
            /* A detached host cannot retain input, and TinyUSB has already
             * cleared the low-level shadow in its detach callback. */
            complete = true;
            break;
        }
        if (!have_usb || usb.suspended ||
            attempt + 1U >= RELEASE_IMMEDIATE_ATTEMPTS) {
            break;
        }
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.release_retries++;
        portEXIT_CRITICAL(&s_state_lock);
        /* Explicitly yield one tick to the lower-priority TinyUSB task. */
        vTaskDelay(1);
    }
    if (!complete) {
        record_usb_result(error);
        /* A wedged host HID driver shows up here first: the release barrier
         * blocks every later key command, so the keyboard write streak must
         * count these failures too (upstream times out the same report). */
        note_keyboard_write_result(COMMAND_KEYBOARD, error);
    }
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.stats.release_pending = !complete;
    if (complete) {
        s_ctx.stats.releases_completed++;
    } else {
        s_ctx.stats.release_failures++;
    }
    portEXIT_CRITICAL(&s_state_lock);
    return !complete;
}

static bool process_expired_releases(
    automatic_release_t releases[],
    jetkvm_hid_keepalive_policy_state_t *keepalive,
    int64_t now_us)
{
    for (size_t i = 0; i < AUTO_RELEASE_SLOTS; ++i) {
        automatic_release_t release = releases[i];
        if (release.usage == 0U || release.deadline_us > now_us) {
            continue;
        }
        memset(&releases[i], 0, sizeof(releases[i]));
        if (!generation_is_active(release.generation, release.epoch)) {
            continue;
        }

        esp_err_t error = jetkvm_usb_hid_keypress(release.usage, false);
        record_usb_result(error);
        note_keyboard_write_result(COMMAND_KEYPRESS, error);
        jetkvm_hid_keepalive_policy_reset(keepalive);
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.automatic_key_releases++;
        portEXIT_CRITICAL(&s_state_lock);
        if (error == ESP_OK) {
            note_keyboard_changed();
        }
        if (error != ESP_OK) {
            return execute_release_all(releases, keepalive);
        }
    }
    return false;
}

static void count_processed(command_type_t type)
{
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.stats.processed++;
    switch (type) {
    case COMMAND_KEYBOARD:
        s_ctx.stats.keyboard_reports++;
        break;
    case COMMAND_KEYPRESS:
        s_ctx.stats.keypress_reports++;
        break;
    case COMMAND_ABSOLUTE_MOUSE:
        s_ctx.stats.absolute_mouse_reports++;
        break;
    case COMMAND_RELATIVE_MOUSE:
        s_ctx.stats.relative_mouse_reports++;
        break;
    case COMMAND_WHEEL:
        s_ctx.stats.wheel_reports++;
        break;
    case COMMAND_KEEPALIVE:
        s_ctx.stats.keepalives++;
        break;
    case COMMAND_RELEASE_ALL:
    case COMMAND_PAUSE:
    case COMMAND_RESUME:
    case COMMAND_SHUTDOWN:
        break;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static void record_keepalive_decision(
    jetkvm_hid_keepalive_result_t result)
{
    portENTER_CRITICAL(&s_state_lock);
    switch (result) {
    case JETKVM_HID_KEEPALIVE_JITTER_ADJUSTED:
        s_ctx.stats.keepalives_jitter_adjusted++;
        break;
    case JETKVM_HID_KEEPALIVE_REJECTED_LATE:
        s_ctx.stats.keepalives_rejected_late++;
        break;
    case JETKVM_HID_KEEPALIVE_REJECTED_STALE:
        s_ctx.stats.keepalives_rejected_stale++;
        break;
    case JETKVM_HID_KEEPALIVE_ACCEPTED:
        break;
    case JETKVM_HID_KEEPALIVE_INVALID_ARGUMENT:
        s_ctx.stats.invalid_events++;
        break;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t submit_usb_command(const input_command_t *command)
{
    switch (command->type) {
    case COMMAND_KEYBOARD:
        return jetkvm_usb_hid_keyboard_report(
            command->data.keyboard.modifier, command->data.keyboard.keys);
    case COMMAND_KEYPRESS:
        return jetkvm_usb_hid_keypress(command->data.keypress.key,
                                       command->data.keypress.pressed);
    case COMMAND_ABSOLUTE_MOUSE:
        return jetkvm_usb_hid_absolute_mouse_report(
            command->data.absolute_mouse.x,
            command->data.absolute_mouse.y,
            command->data.absolute_mouse.buttons);
    case COMMAND_RELATIVE_MOUSE:
        return jetkvm_usb_hid_relative_mouse_report(
            command->data.relative_mouse.dx,
            command->data.relative_mouse.dy,
            command->data.relative_mouse.buttons, 0, 0);
    case COMMAND_WHEEL:
        return jetkvm_usb_hid_wheel_report(
            command->data.wheel.wheel_y, command->data.wheel.wheel_x);
    case COMMAND_KEEPALIVE:
    case COMMAND_RELEASE_ALL:
    case COMMAND_PAUSE:
    case COMMAND_RESUME:
    case COMMAND_SHUTDOWN:
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t submit_usb_command_with_retry(
    const input_command_t *command, bool require_active_generation)
{
    esp_err_t error = submit_usb_command(command);
    bool retryable_type = command->type != COMMAND_WHEEL;
    bool may_retry = !require_active_generation ||
                     generation_is_active(command->generation,
                                          command->epoch);
    /* Upstream writes keyboard reports with a 100 ms deadline (hidWriteTimeout)
     * so a burst that outruns the host's interrupt polling is queued, not
     * dropped, and only a host that stops polling for the whole deadline
     * counts as a write timeout.  Other reports keep the single quick retry. */
    const bool keyboard = command->type == COMMAND_KEYBOARD ||
                          command->type == COMMAND_KEYPRESS;
    const int64_t deadline_us = esp_timer_get_time() + HID_WRITE_DEADLINE_US;
    while (retryable_type &&
           (error == ESP_ERR_TIMEOUT || error == ESP_ERR_NOT_FINISHED) &&
           may_retry) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.usb_report_retries++;
        portEXIT_CRITICAL(&s_state_lock);
        vTaskDelay(1);
        if (require_active_generation &&
            !generation_is_active(command->generation, command->epoch)) {
            break;
        }
        error = submit_usb_command(command);
        if (!keyboard || esp_timer_get_time() >= deadline_us) {
            break;
        }
    }
    return error;
}

static void finish_macro(macro_runtime_t *runtime, bool completed,
                         bool failed, bool replaced)
{
    if (!runtime->active) {
        return;
    }
    portENTER_CRITICAL(&s_state_lock);
    if (failed) {
        s_ctx.stats.macro_failed++;
    } else if (completed) {
        s_ctx.stats.macro_completed++;
    } else {
        s_ctx.stats.macro_cancelled++;
    }
    if (replaced) {
        s_ctx.stats.macro_replaced++;
    }
    s_ctx.macro_is_paste = runtime->command.is_paste;
    portEXIT_CRITICAL(&s_state_lock);
    runtime->active = false;
    publish_macro_state(NULL);
}

static bool cancel_active_macro(macro_runtime_t *runtime, bool replaced,
                                automatic_release_t releases[],
                                jetkvm_hid_keepalive_policy_state_t *keepalive)
{
    if (!runtime->active) {
        return false;
    }

    input_command_t clear = {
        .type = COMMAND_KEYBOARD,
        .generation = runtime->command.generation,
        .epoch = runtime->command.epoch,
    };
    esp_err_t error = submit_usb_command_with_retry(&clear, false);
    note_keyboard_write_result(COMMAND_KEYBOARD, error);
    if (error == ESP_OK) {
        note_keyboard_changed();
        clear_automatic_releases(releases, keepalive);
    }
    record_usb_result(error);
    count_processed(COMMAND_KEYBOARD);
    finish_macro(runtime, false, false, replaced);
    return error == ESP_OK ? false :
                             execute_release_all(releases, keepalive);
}

static bool macro_is_due(const macro_runtime_t *runtime, int64_t now_us)
{
    return runtime->active && runtime->next_step_at_us <= now_us;
}

static TickType_t next_macro_wait(const macro_runtime_t *runtime,
                                  int64_t now_us)
{
    if (!runtime->active) {
        return portMAX_DELAY;
    }
    if (runtime->next_step_at_us <= now_us) {
        return 0;
    }
    uint64_t remaining_ms =
        (uint64_t)(runtime->next_step_at_us - now_us + 999) / 1000U;
    TickType_t wait = pdMS_TO_TICKS(remaining_ms);
    return wait == 0 ? 1 : wait;
}

static bool process_macro_due(macro_runtime_t *runtime,
                              automatic_release_t releases[],
                              jetkvm_hid_keepalive_policy_state_t *keepalive,
                              int64_t now_us)
{
    if (!macro_is_due(runtime, now_us)) {
        return false;
    }
    if (!generation_is_active(runtime->command.generation,
                              runtime->command.epoch) ||
        macro_cancel_sequence() > runtime->command.action_sequence) {
        return cancel_active_macro(runtime, false, releases, keepalive);
    }
    if (runtime->next_step >= runtime->command.step_count) {
        finish_macro(runtime, true, false, false);
        return false;
    }

    const jetkvm_hidrpc_macro_step_t *step =
        &runtime->command.steps[runtime->next_step];
    input_command_t command = {
        .type = COMMAND_KEYBOARD,
        .generation = runtime->command.generation,
        .epoch = runtime->command.epoch,
        .data.keyboard.modifier = step->modifier,
    };
    memcpy(command.data.keyboard.keys, step->keys,
           sizeof(command.data.keyboard.keys));
    esp_err_t error = submit_usb_command_with_retry(&command, true);
    record_usb_result(error);
    note_keyboard_write_result(COMMAND_KEYBOARD, error);
    count_processed(COMMAND_KEYBOARD);
    if (error != ESP_OK) {
        finish_macro(runtime, false, true, false);
        return execute_release_all(releases, keepalive);
    }
    record_user_input_now();
    note_keyboard_changed();

    clear_automatic_releases(releases, keepalive);
    runtime->next_step++;
    runtime->next_step_at_us = esp_timer_get_time() +
        (int64_t)step->delay_ms * 1000;
    publish_macro_state(runtime);
    return false;
}

static void process_command(const input_command_t *command,
                            automatic_release_t releases[],
                            jetkvm_hid_keepalive_policy_state_t *keepalive)
{
    if (!command->local &&
        !generation_is_active(command->generation, command->epoch)) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.stale_generation_dropped++;
        portEXIT_CRITICAL(&s_state_lock);
        return;
    }

    if (command->type == COMMAND_KEEPALIVE) {
        jetkvm_hid_keepalive_decision_t decision =
            jetkvm_hid_keepalive_policy_evaluate(
                keepalive, command->generation, command->epoch,
                command->received_at_us, esp_timer_get_time());
        count_processed(command->type);
        record_keepalive_decision(decision.result);
        if (decision.result == JETKVM_HID_KEEPALIVE_ACCEPTED ||
            decision.result == JETKVM_HID_KEEPALIVE_JITTER_ADJUSTED) {
            /* This is deliberately an arrival-time deadline. If the worker
             * was delayed, the remaining extension shrinks or is already
             * expired instead of becoming worker-now + 100 ms. */
            extend_automatic_releases(releases, command->generation,
                                      command->epoch,
                                      decision.deadline_us);
        }
        return;
    }
    if (command->type == COMMAND_RELEASE_ALL ||
        command->type == COMMAND_PAUSE ||
        command->type == COMMAND_RESUME ||
        command->type == COMMAND_SHUTDOWN) {
        return;
    }

    esp_err_t error = submit_usb_command_with_retry(command, !command->local);

    if (error == ESP_OK && (command->type == COMMAND_KEYBOARD ||
                            command->type == COMMAND_KEYPRESS)) {
        const int64_t now_us = esp_timer_get_time();
        const int64_t latency_us = now_us - command->received_at_us;
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.keyboard_write_last_us = (uint32_t)latency_us;
        if ((uint32_t)latency_us > s_ctx.stats.keyboard_write_max_us) {
            s_ctx.stats.keyboard_write_max_us = (uint32_t)latency_us;
        }
        if (latency_us > 20000) {
            s_ctx.stats.keyboard_write_slow++;
        }
        portEXIT_CRITICAL(&s_state_lock);
        jetkvm_timing_log_key(
            command->type == COMMAND_KEYBOARD ? command->data.keyboard.keys[0] :
            command->data.keypress.pressed ? command->data.keypress.key : 0U,
            now_us);
        note_keyboard_changed();
    }
    if (error == ESP_OK && command->type == COMMAND_KEYBOARD) {
        clear_automatic_releases(releases, keepalive);
    } else if (error == ESP_OK && command->type == COMMAND_KEYPRESS &&
               !is_modifier_usage(command->data.keypress.key)) {
        /* Match Go's keys-down callback: a key-state change starts a fresh
         * keepalive timing baseline while retaining other held keys. */
        jetkvm_hid_keepalive_policy_reset(keepalive);
        if (command->data.keypress.pressed) {
            arm_automatic_release(releases,
                                  command->data.keypress.key,
                                  command->generation,
                                  command->epoch,
                                  esp_timer_get_time());
        } else {
            remove_automatic_release(releases,
                                     command->data.keypress.key);
        }
    } else if (error == ESP_OK && command->type == COMMAND_KEYPRESS) {
        jetkvm_hid_keepalive_policy_reset(keepalive);
    }

    note_keyboard_write_result(command->type, error);
    record_usb_result(error);
    count_processed(command->type);
    if (error == ESP_OK) {
        record_user_input_now();
        if (command->local && command->type == COMMAND_RELATIVE_MOUSE) {
            portENTER_CRITICAL(&s_state_lock);
            s_ctx.stats.local_relative_mouse_reports++;
            portEXIT_CRITICAL(&s_state_lock);
        }
    }
}

static void count_unstarted_macro_cancelled(bool replaced, bool failed)
{
    portENTER_CRITICAL(&s_state_lock);
    if (failed) {
        s_ctx.stats.macro_failed++;
    } else {
        s_ctx.stats.macro_cancelled++;
    }
    if (replaced) {
        s_ctx.stats.macro_replaced++;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static bool accept_incoming_macro(macro_runtime_t *runtime,
                                  automatic_release_t releases[],
                                  jetkvm_hid_keepalive_policy_state_t *keepalive)
{
    if (s_incoming_macro.action_sequence <= macro_cancel_sequence() ||
        !generation_is_active(s_incoming_macro.generation,
                              s_incoming_macro.epoch)) {
        count_unstarted_macro_cancelled(false, false);
        return false;
    }
    if (runtime->active &&
        s_incoming_macro.action_sequence <=
            runtime->command.action_sequence) {
        count_unstarted_macro_cancelled(false, false);
        return false;
    }

    if (runtime->active &&
        cancel_active_macro(runtime, true, releases, keepalive)) {
        count_unstarted_macro_cancelled(false, true);
        return true;
    }

    runtime->command = s_incoming_macro;
    runtime->active = true;
    runtime->next_step = 0U;
    runtime->next_step_at_us = esp_timer_get_time();
    publish_macro_state(runtime);
    return false;
}

static void bridge_worker(void *ctx)
{
    (void)ctx;
    automatic_release_t releases[AUTO_RELEASE_SLOTS] = {0};
    jetkvm_hid_keepalive_policy_state_t keepalive = {0};
    macro_runtime_t macro = {0};
    bool release_pending = false;
    bool worker_paused = false;
    int64_t release_retry_at_us = 0;

    for (;;) {
        if (worker_paused) {
            input_command_t paused_command = {0};
            if (xQueueReceive(s_ctx.queue, &paused_command,
                              portMAX_DELAY) != pdTRUE) {
                continue;
            }
            if (paused_command.type == COMMAND_SHUTDOWN) {
                break;
            }
            if (paused_command.type == COMMAND_PAUSE) {
                portENTER_CRITICAL(&s_state_lock);
                s_ctx.worker_paused = true;
                s_ctx.pause_result = ESP_OK;
                s_ctx.pause_ack_token = paused_command.barrier_token;
                portEXIT_CRITICAL(&s_state_lock);
                xSemaphoreGive(s_ctx.pause_done);
                continue;
            }
            if (paused_command.type == COMMAND_RESUME) {
                bool resume_release_pending =
                    execute_release_all(releases, &keepalive);
                portENTER_CRITICAL(&s_state_lock);
                s_ctx.resume_result = resume_release_pending ? ESP_FAIL :
                                                              ESP_OK;
                s_ctx.resume_ack_token = paused_command.barrier_token;
                if (!resume_release_pending) {
                    s_ctx.worker_paused = false;
                }
                portEXIT_CRITICAL(&s_state_lock);
                if (!resume_release_pending) {
                    release_pending = false;
                    release_retry_at_us = 0;
                    worker_paused = false;
                }
                xSemaphoreGive(s_ctx.resume_done);
            }
            /* No other command may touch USB while the pause barrier is
             * closed. Generation changes update state without queueing. */
            continue;
        }

        portENTER_CRITICAL(&s_state_lock);
        bool overflow_release = s_ctx.overflow_release;
        s_ctx.overflow_release = false;
        portEXIT_CRITICAL(&s_state_lock);
        if (overflow_release) {
            release_pending = execute_release_all(releases, &keepalive);
            release_retry_at_us = 0;
        }
        int64_t now_us = esp_timer_get_time();
        if (macro.active &&
            (macro_cancel_sequence() > macro.command.action_sequence ||
             !generation_is_active(macro.command.generation,
                                   macro.command.epoch))) {
            release_pending = cancel_active_macro(
                &macro, false, releases, &keepalive);
            release_retry_at_us = release_pending ?
                esp_timer_get_time() +
                    (int64_t)RELEASE_RETRY_INTERVAL_MS * 1000 :
                0;
            now_us = esp_timer_get_time();
        }
        if (release_pending && now_us >= release_retry_at_us) {
            release_pending = execute_release_all(releases, &keepalive);
            release_retry_at_us = release_pending ?
                esp_timer_get_time() +
                    (int64_t)RELEASE_RETRY_INTERVAL_MS * 1000 :
                0;
            now_us = esp_timer_get_time();
        }
        if (!release_pending) {
            release_pending = process_expired_releases(
                releases, &keepalive, now_us);
            if (release_pending) {
                release_retry_at_us = esp_timer_get_time() +
                    (int64_t)RELEASE_RETRY_INTERVAL_MS * 1000;
            }
        }

        input_command_t command = {0};
        if (xQueueReceive(s_ctx.queue, &command, 0) == pdTRUE) {
            if (command.type == COMMAND_SHUTDOWN) {
                if (macro.active) {
                    (void)cancel_active_macro(
                        &macro, false, releases, &keepalive);
                }
                (void)execute_release_all(releases, &keepalive);
                break;
            }
            if (command.type == COMMAND_PAUSE) {
                if (macro.active) {
                    (void)cancel_active_macro(
                        &macro, false, releases, &keepalive);
                }
                (void)execute_release_all(releases, &keepalive);
                clear_automatic_releases(releases, &keepalive);
                release_pending = false;
                release_retry_at_us = 0;
                worker_paused = true;
                portENTER_CRITICAL(&s_state_lock);
                s_ctx.worker_paused = true;
                s_ctx.pause_result = ESP_OK;
                s_ctx.pause_ack_token = command.barrier_token;
                portEXIT_CRITICAL(&s_state_lock);
                xSemaphoreGive(s_ctx.pause_done);
                continue;
            }
            if (command.type == COMMAND_RESUME) {
                bool resume_release_pending =
                    execute_release_all(releases, &keepalive);
                release_pending = resume_release_pending;
                portENTER_CRITICAL(&s_state_lock);
                s_ctx.resume_result = resume_release_pending ? ESP_FAIL :
                                                              ESP_OK;
                s_ctx.resume_ack_token = command.barrier_token;
                s_ctx.worker_paused = resume_release_pending;
                portEXIT_CRITICAL(&s_state_lock);
                xSemaphoreGive(s_ctx.resume_done);
                if (resume_release_pending) {
                    worker_paused = true;
                }
                continue;
            }
            if (command.type == COMMAND_RELEASE_ALL) {
                release_pending = execute_release_all(releases, &keepalive);
                release_retry_at_us = release_pending ?
                    esp_timer_get_time() +
                        (int64_t)RELEASE_RETRY_INTERVAL_MS * 1000 :
                    0;
            } else {
                if (release_pending) {
                    /* A resumed host must observe the old all-up state before
                     * any new generation is allowed to press a key/button. */
                    release_pending =
                        execute_release_all(releases, &keepalive);
                    release_retry_at_us = release_pending ?
                        esp_timer_get_time() +
                            (int64_t)RELEASE_RETRY_INTERVAL_MS * 1000 :
                        0;
                }
                if (release_pending) {
                    portENTER_CRITICAL(&s_state_lock);
                    s_ctx.stats.release_blocked_dropped++;
                    portEXIT_CRITICAL(&s_state_lock);
                } else {
                    process_command(&command, releases, &keepalive);
                }
            }
        }

        if (!release_pending &&
            uxQueueMessagesWaiting(s_ctx.macro_queue) != 0U) {
            xSemaphoreTake(s_ctx.macro_mutex, portMAX_DELAY);
            BaseType_t received = xQueueReceive(
                s_ctx.macro_queue, &s_incoming_macro, 0);
            xSemaphoreGive(s_ctx.macro_mutex);
            if (received == pdTRUE) {
                release_pending = accept_incoming_macro(
                    &macro, releases, &keepalive);
                release_retry_at_us = release_pending ?
                    esp_timer_get_time() +
                        (int64_t)RELEASE_RETRY_INTERVAL_MS * 1000 :
                    0;
            }
        }

        now_us = esp_timer_get_time();
        if (!release_pending && macro_is_due(&macro, now_us)) {
            release_pending = process_macro_due(
                &macro, releases, &keepalive, now_us);
            release_retry_at_us = release_pending ?
                esp_timer_get_time() +
                    (int64_t)RELEASE_RETRY_INTERVAL_MS * 1000 :
                0;
            continue;
        }

        TickType_t wait = next_automatic_release_wait(releases, now_us);
        TickType_t macro_wait = next_macro_wait(&macro, now_us);
        if (macro_wait < wait) {
            wait = macro_wait;
        }
        if (release_pending) {
            uint64_t remaining_ms = release_retry_at_us > now_us ?
                (uint64_t)(release_retry_at_us - now_us + 999) / 1000U :
                1U;
            wait = pdMS_TO_TICKS(remaining_ms);
            if (wait == 0) {
                wait = 1;
            }
        }
        (void)ulTaskNotifyTake(pdFALSE, wait);
    }

    portENTER_CRITICAL(&s_state_lock);
    s_ctx.worker_task = NULL;
    s_ctx.worker_paused = false;
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_ctx.worker_done);
    vTaskDelete(NULL);
}

static void led_callback(uint8_t leds, void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&s_state_lock);
    if (s_ctx.usb_available) {
        s_ctx.keyboard_leds = leds;
        s_ctx.stats.led_updates++;
    }
    portEXIT_CRITICAL(&s_state_lock);
}

static esp_err_t priority_flush_command(command_type_t type)
{
    input_command_t command = {.type = type};
    if (xQueueReset(s_ctx.queue) != pdPASS ||
        xQueueSendToFront(s_ctx.queue, &command, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    notify_worker();
    return ESP_OK;
}

static esp_err_t worker_usb_barrier(command_type_t type, int64_t deadline_us)
{
    SemaphoreHandle_t done = type == COMMAND_PAUSE ? s_ctx.pause_done :
                                                     s_ctx.resume_done;
    if (done == NULL || (type != COMMAND_PAUSE && type != COMMAND_RESUME)) {
        return ESP_ERR_INVALID_ARG;
    }
    while (xSemaphoreTake(done, 0) == pdTRUE) {
    }
    uint32_t token;
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.barrier_sequence++;
    if (s_ctx.barrier_sequence == 0U) {
        s_ctx.barrier_sequence++;
    }
    token = s_ctx.barrier_sequence;
    if (type == COMMAND_PAUSE) {
        s_ctx.pause_result = ESP_ERR_INVALID_STATE;
        s_ctx.pause_ack_token = 0U;
    } else {
        s_ctx.resume_result = ESP_ERR_INVALID_STATE;
        s_ctx.resume_ack_token = 0U;
    }
    portEXIT_CRITICAL(&s_state_lock);

    input_command_t command = {
        .type = type,
        .barrier_token = token,
    };
    if (xQueueReset(s_ctx.queue) != pdPASS ||
        xQueueSendToFront(s_ctx.queue, &command, 0) != pdTRUE) {
        return ESP_FAIL;
    }
    notify_worker();

    for (;;) {
        TickType_t wait = deadline_remaining_ticks(deadline_us);
        if (wait == 0) {
            return ESP_ERR_TIMEOUT;
        }
        if (xSemaphoreTake(done, wait) != pdTRUE) {
            return ESP_ERR_TIMEOUT;
        }
        portENTER_CRITICAL(&s_state_lock);
        uint32_t ack_token = type == COMMAND_PAUSE ?
            s_ctx.pause_ack_token : s_ctx.resume_ack_token;
        esp_err_t error = type == COMMAND_PAUSE ? s_ctx.pause_result :
                                                   s_ctx.resume_result;
        portEXIT_CRITICAL(&s_state_lock);
        if (ack_token == token) {
            return error;
        }
    }
}

static void discard_pending_macro(void)
{
    if (s_ctx.macro_queue == NULL || s_ctx.macro_mutex == NULL) {
        return;
    }
    xSemaphoreTake(s_ctx.macro_mutex, portMAX_DELAY);
    BaseType_t received = xQueueReceive(
        s_ctx.macro_queue, &s_discarded_macro, 0);
    xSemaphoreGive(s_ctx.macro_mutex);
    if (received == pdTRUE) {
        count_unstarted_macro_cancelled(false, false);
    }
}

static esp_err_t discard_pending_macro_until(int64_t deadline_us)
{
    if (s_ctx.macro_queue == NULL || s_ctx.macro_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    TickType_t wait = deadline_remaining_ticks(deadline_us);
    if (wait == 0 || xSemaphoreTake(s_ctx.macro_mutex, wait) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    BaseType_t received = xQueueReceive(
        s_ctx.macro_queue, &s_discarded_macro, 0);
    xSemaphoreGive(s_ctx.macro_mutex);
    if (received == pdTRUE) {
        count_unstarted_macro_cancelled(false, false);
    }
    return ESP_OK;
}

esp_err_t jetkvm_hid_input_init(void)
{
    jetkvm_usb_hid_state_t usb = {0};
    esp_err_t error = jetkvm_usb_hid_get_state(&usb);
    if (error != ESP_OK || !usb.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_state_lock);
    if (s_ctx.initialized || s_ctx.initializing || s_ctx.stopping) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.initializing = true;
    portEXIT_CRITICAL(&s_state_lock);

    if (s_ctx.queue == NULL) {
        s_ctx.queue = xQueueCreateStatic(
            JETKVM_HID_INPUT_QUEUE_LENGTH, sizeof(input_command_t),
            s_queue_storage, &s_queue_control);
        s_ctx.macro_queue = xQueueCreateStatic(
            MACRO_QUEUE_LENGTH, sizeof(macro_command_t),
            s_macro_queue_storage, &s_macro_queue_control);
        s_ctx.worker_done =
            xSemaphoreCreateBinaryStatic(&s_worker_done_storage);
        s_ctx.pause_done =
            xSemaphoreCreateBinaryStatic(&s_pause_done_storage);
        s_ctx.resume_done =
            xSemaphoreCreateBinaryStatic(&s_resume_done_storage);
        s_ctx.admin_mutex =
            xSemaphoreCreateMutexStatic(&s_admin_mutex_storage);
        s_ctx.macro_mutex =
            xSemaphoreCreateMutexStatic(&s_macro_mutex_storage);
    }
    if (s_ctx.queue == NULL || s_ctx.macro_queue == NULL ||
        s_ctx.worker_done == NULL || s_ctx.pause_done == NULL ||
        s_ctx.resume_done == NULL ||
        s_ctx.admin_mutex == NULL || s_ctx.macro_mutex == NULL) {
        error = ESP_ERR_NO_MEM;
        goto fail;
    }

    xQueueReset(s_ctx.queue);
    xQueueReset(s_ctx.macro_queue);
    while (xSemaphoreTake(s_ctx.worker_done, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(s_ctx.pause_done, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(s_ctx.resume_done, 0) == pdTRUE) {
    }
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.pause_result = ESP_ERR_INVALID_STATE;
    s_ctx.resume_result = ESP_ERR_INVALID_STATE;
    portEXIT_CRITICAL(&s_state_lock);
    error = jetkvm_usb_hid_set_led_callback(led_callback, NULL);
    if (error != ESP_OK) {
        goto fail;
    }

    TaskHandle_t worker = NULL;
    if (xTaskCreatePinnedToCore(bridge_worker, BRIDGE_TASK_NAME,
                                BRIDGE_TASK_STACK_SIZE, NULL,
                                BRIDGE_TASK_PRIORITY, &worker, 0) != pdPASS) {
        jetkvm_usb_hid_set_led_callback(NULL, NULL);
        error = ESP_ERR_NO_MEM;
        goto fail;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_ctx.worker_task = worker;
    s_ctx.initialized = true;
    s_ctx.initializing = false;
    s_ctx.stopping = false;
    s_ctx.usb_available = true;
    s_ctx.worker_paused = false;
    s_ctx.accepting_input = false;
    s_ctx.active_generation = 0U;
    s_ctx.active_epoch = 1U;
    s_ctx.submitted_absolute_buttons = 0U;
    s_ctx.submitted_relative_buttons = 0U;
    memset(&s_ctx.stats, 0, sizeof(s_ctx.stats));
    s_ctx.macro_action_sequence = 0U;
    s_ctx.macro_cancel_sequence = 0U;
    s_ctx.macro_active = false;
    s_ctx.macro_is_paste = false;
    s_ctx.macro_step_index = 0U;
    s_ctx.macro_step_count = 0U;
    s_ctx.macro_state_revision = 0U;
    s_ctx.last_user_input_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;

fail:
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.initializing = false;
    portEXIT_CRITICAL(&s_state_lock);
    return error;
}

esp_err_t jetkvm_hid_input_deinit(void)
{
    if (s_ctx.admin_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctx.admin_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_state_lock);
    if (!s_ctx.initialized || s_ctx.stopping) {
        portEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(s_ctx.admin_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    s_ctx.stopping = true;
    s_ctx.usb_available = false;
    s_ctx.accepting_input = false;
    s_ctx.active_generation = 0U;
    s_ctx.active_epoch++;
    s_ctx.submitted_absolute_buttons = 0U;
    s_ctx.submitted_relative_buttons = 0U;
    s_ctx.macro_cancel_sequence = next_macro_action_sequence_locked();
    s_ctx.stats.release_requests++;
    portEXIT_CRITICAL(&s_state_lock);

    discard_pending_macro();
    esp_err_t error = priority_flush_command(COMMAND_SHUTDOWN);
    xSemaphoreGive(s_ctx.admin_mutex);
    if (error != ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stopping = false;
        portEXIT_CRITICAL(&s_state_lock);
        return error;
    }

    xSemaphoreTake(s_ctx.worker_done, portMAX_DELAY);
    jetkvm_usb_hid_set_led_callback(NULL, NULL);
    xQueueReset(s_ctx.queue);
    xQueueReset(s_ctx.macro_queue);
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.initialized = false;
    s_ctx.stopping = false;
    s_ctx.usb_available = false;
    s_ctx.worker_paused = false;
    s_ctx.keyboard_leds = 0U;
    s_ctx.macro_active = false;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

esp_err_t jetkvm_hid_input_set_usb_available(bool available)
{
    if (s_ctx.admin_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const int64_t deadline_us = esp_timer_get_time() +
        (int64_t)USB_TRANSITION_TIMEOUT_MS * 1000;
    if (available) {
        jetkvm_usb_hid_state_t usb = {0};
        if (jetkvm_usb_hid_get_state(&usb) != ESP_OK || !usb.initialized) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    const TickType_t timeout = deadline_remaining_ticks(deadline_us);
    if (timeout == 0) {
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(s_ctx.admin_mutex, timeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_state_lock);
    if (!s_ctx.initialized || s_ctx.stopping) {
        portEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(s_ctx.admin_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t generation = s_ctx.active_generation;
    const bool already_stable = available ?
        (s_ctx.usb_available && !s_ctx.worker_paused &&
         (generation == 0U || s_ctx.accepting_input)) :
        (!s_ctx.usb_available && s_ctx.worker_paused);
    if (already_stable) {
        portEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(s_ctx.admin_mutex);
        return ESP_OK;
    }
    /* Keep the public gate closed until the worker has crossed the matching
     * pause/resume barrier. The active generation itself is deliberately
     * preserved and may still be replaced by a peer after this call. */
    s_ctx.usb_available = false;
    s_ctx.accepting_input = false;
    s_ctx.active_epoch++;
    s_ctx.submitted_absolute_buttons = 0U;
    s_ctx.submitted_relative_buttons = 0U;
    s_ctx.macro_cancel_sequence = next_macro_action_sequence_locked();
    s_ctx.stats.release_requests++;
    portEXIT_CRITICAL(&s_state_lock);

    esp_err_t error = discard_pending_macro_until(deadline_us);
    if (error != ESP_OK) {
        xSemaphoreGive(s_ctx.admin_mutex);
        return error;
    }
    if (available) {
        esp_err_t callback_error =
            jetkvm_usb_hid_set_led_callback(led_callback, NULL);
        if (callback_error != ESP_OK) {
            xSemaphoreGive(s_ctx.admin_mutex);
            return callback_error;
        }
    }
    error = worker_usb_barrier(
        available ? COMMAND_RESUME : COMMAND_PAUSE, deadline_us);
    portENTER_CRITICAL(&s_state_lock);
    const bool barrier_state_matches = available ? !s_ctx.worker_paused :
                                                   s_ctx.worker_paused;
    if (error == ESP_OK && !barrier_state_matches) {
        error = ESP_ERR_INVALID_STATE;
    }
    if (available && error == ESP_OK && s_ctx.initialized &&
        !s_ctx.stopping && !s_ctx.worker_paused) {
        s_ctx.usb_available = true;
        if (generation != 0U && s_ctx.active_generation == generation) {
            s_ctx.accepting_input = true;
        }
    } else {
        s_ctx.usb_available = false;
        s_ctx.accepting_input = false;
    }
    if (!available && error == ESP_OK) {
        /* The device is about to detach; there is no meaningful retry once
         * the worker has proven it will no longer touch the USB core. */
        s_ctx.stats.release_pending = false;
        s_ctx.keyboard_leds = 0U;
        s_ctx.stats.led_updates++;
    }
    portEXIT_CRITICAL(&s_state_lock);

    if (!available && error == ESP_OK) {
        (void)jetkvm_usb_hid_set_led_callback(NULL, NULL);
    } else if (available && error == ESP_OK) {
        portENTER_CRITICAL(&s_state_lock);
        uint64_t led_revision = s_ctx.stats.led_updates;
        portEXIT_CRITICAL(&s_state_lock);
        jetkvm_usb_hid_state_t usb = {0};
        if (jetkvm_usb_hid_get_state(&usb) == ESP_OK) {
            portENTER_CRITICAL(&s_state_lock);
            if (s_ctx.usb_available &&
                s_ctx.stats.led_updates == led_revision &&
                s_ctx.keyboard_leds != usb.keyboard_leds) {
                s_ctx.keyboard_leds = usb.keyboard_leds;
                s_ctx.stats.led_updates++;
            }
            portEXIT_CRITICAL(&s_state_lock);
        }
    }
    xSemaphoreGive(s_ctx.admin_mutex);
    return error;
}

esp_err_t jetkvm_hid_input_set_active_generation(uint32_t generation)
{
    if (generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.admin_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctx.admin_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_state_lock);
    if (!s_ctx.initialized || s_ctx.stopping) {
        portEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(s_ctx.admin_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.accepting_input && s_ctx.active_generation == generation) {
        portEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(s_ctx.admin_mutex);
        return ESP_OK;
    }
    s_ctx.accepting_input = false;
    s_ctx.active_generation = generation;
    s_ctx.active_epoch++;
    s_ctx.submitted_absolute_buttons = 0U;
    s_ctx.submitted_relative_buttons = 0U;
    s_ctx.macro_cancel_sequence = next_macro_action_sequence_locked();
    s_ctx.stats.generation_changes++;
    s_ctx.stats.release_requests++;
    const bool usb_available = s_ctx.usb_available;
    portEXIT_CRITICAL(&s_state_lock);

    discard_pending_macro();
    if (!usb_available) {
        xSemaphoreGive(s_ctx.admin_mutex);
        return ESP_OK;
    }
    esp_err_t error = priority_flush_command(COMMAND_RELEASE_ALL);
    portENTER_CRITICAL(&s_state_lock);
    if (error == ESP_OK && s_ctx.initialized && !s_ctx.stopping &&
        s_ctx.usb_available &&
        s_ctx.active_generation == generation) {
        s_ctx.accepting_input = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_ctx.admin_mutex);
    return error;
}

esp_err_t jetkvm_hid_input_invalidate_generation(uint32_t generation)
{
    if (generation == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.admin_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctx.admin_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_state_lock);
    if (!s_ctx.initialized || s_ctx.stopping) {
        portEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(s_ctx.admin_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ctx.active_generation != generation) {
        portEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(s_ctx.admin_mutex);
        return ESP_OK;
    }
    s_ctx.accepting_input = false;
    s_ctx.active_generation = 0U;
    s_ctx.active_epoch++;
    s_ctx.submitted_absolute_buttons = 0U;
    s_ctx.submitted_relative_buttons = 0U;
    s_ctx.macro_cancel_sequence = next_macro_action_sequence_locked();
    s_ctx.stats.generation_changes++;
    s_ctx.stats.release_requests++;
    const bool usb_available = s_ctx.usb_available;
    portEXIT_CRITICAL(&s_state_lock);

    discard_pending_macro();
    esp_err_t error = usb_available ?
        priority_flush_command(COMMAND_RELEASE_ALL) : ESP_OK;
    xSemaphoreGive(s_ctx.admin_mutex);
    return error;
}

esp_err_t jetkvm_hid_input_release_all(void)
{
    if (s_ctx.admin_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_ctx.admin_mutex, portMAX_DELAY);
    portENTER_CRITICAL(&s_state_lock);
    if (!s_ctx.initialized || s_ctx.stopping) {
        portEXIT_CRITICAL(&s_state_lock);
        xSemaphoreGive(s_ctx.admin_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t generation = s_ctx.active_generation;
    s_ctx.accepting_input = false;
    s_ctx.active_epoch++;
    s_ctx.submitted_absolute_buttons = 0U;
    s_ctx.submitted_relative_buttons = 0U;
    s_ctx.macro_cancel_sequence = next_macro_action_sequence_locked();
    s_ctx.stats.release_requests++;
    const bool usb_available = s_ctx.usb_available;
    portEXIT_CRITICAL(&s_state_lock);

    discard_pending_macro();
    esp_err_t error = usb_available ?
        priority_flush_command(COMMAND_RELEASE_ALL) : ESP_OK;
    portENTER_CRITICAL(&s_state_lock);
    if (error == ESP_OK && s_ctx.initialized && !s_ctx.stopping &&
        s_ctx.usb_available &&
        generation != 0U && s_ctx.active_generation == generation) {
        s_ctx.accepting_input = true;
    }
    portEXIT_CRITICAL(&s_state_lock);
    xSemaphoreGive(s_ctx.admin_mutex);
    return error;
}

static bool copy_event_to_command(const jetkvm_hidrpc_event_t *event,
                                  input_command_t *command)
{
    switch (event->type) {
    case JETKVM_HIDRPC_EVENT_KEYBOARD_REPORT:
        command->type = COMMAND_KEYBOARD;
        command->data.keyboard = event->data.keyboard;
        return true;
    case JETKVM_HIDRPC_EVENT_KEYPRESS_REPORT:
        if (event->data.keypress.key == 0U) {
            return false;
        }
        command->type = COMMAND_KEYPRESS;
        command->data.keypress = event->data.keypress;
        return true;
    case JETKVM_HIDRPC_EVENT_ABS_MOUSE_REPORT:
        command->type = COMMAND_ABSOLUTE_MOUSE;
        command->data.absolute_mouse = event->data.abs_mouse;
        return true;
    case JETKVM_HIDRPC_EVENT_REL_MOUSE_REPORT:
        command->type = COMMAND_RELATIVE_MOUSE;
        command->data.relative_mouse = event->data.rel_mouse;
        return true;
    case JETKVM_HIDRPC_EVENT_WHEEL_REPORT:
        command->type = COMMAND_WHEEL;
        command->data.wheel = event->data.wheel;
        return true;
    case JETKVM_HIDRPC_EVENT_KEYPRESS_KEEPALIVE_REPORT:
        command->type = COMMAND_KEEPALIVE;
        return true;
    case JETKVM_HIDRPC_EVENT_KEYBOARD_MACRO_REPORT:
    case JETKVM_HIDRPC_EVENT_CANCEL_KEYBOARD_MACRO_REPORT:
        return false;
    }
    return false;
}

static bool copy_macro_event(const jetkvm_hidrpc_event_t *event,
                             macro_command_t *macro)
{
    if (event->type != JETKVM_HIDRPC_EVENT_KEYBOARD_MACRO_REPORT ||
        event->data.macro.step_count > JETKVM_HIDRPC_MACRO_MAX_STEPS) {
        return false;
    }
    memset(macro, 0, sizeof(*macro));
    /* Match Go rpcExecuteKeyboardMacro: every remotely executed macro is
     * surfaced as paste-in-progress, irrespective of the currently unused
     * wire IsPaste bit (the JetKVM frontend sends that bit as false). */
    macro->is_paste = true;
    macro->step_count = event->data.macro.step_count;
    for (uint32_t i = 0; i < macro->step_count; ++i) {
        if (!jetkvm_hidrpc_macro_step_at(&event->data.macro, i,
                                         &macro->steps[i])) {
            return false;
        }
    }
    return true;
}

static jetkvm_hid_input_submit_result_t submit_macro_event(
    uint32_t generation, const jetkvm_hidrpc_event_t *event)
{
    macro_command_t macro;
    if (!copy_macro_event(event, &macro)) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.submitted++;
        s_ctx.stats.macro_rejected++;
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_HID_INPUT_SUBMIT_INVALID_ARGUMENT;
    }

    QueueHandle_t queue;
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.stats.submitted++;
    if (!s_ctx.initialized || s_ctx.stopping || !s_ctx.accepting_input) {
        s_ctx.stats.macro_rejected++;
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_HID_INPUT_SUBMIT_NOT_READY;
    }
    if (generation != s_ctx.active_generation) {
        s_ctx.stats.stale_generation_dropped++;
        s_ctx.stats.macro_rejected++;
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_HID_INPUT_SUBMIT_STALE_GENERATION;
    }
    macro.generation = generation;
    macro.epoch = s_ctx.active_epoch;
    macro.action_sequence = next_macro_action_sequence_locked();
    queue = s_ctx.macro_queue;
    portEXIT_CRITICAL(&s_state_lock);

    /* Zero-tick acquisition preserves the WebRTC callback boundary while
     * making pending replacement accounting atomic with worker receive. */
    if (xSemaphoreTake(s_ctx.macro_mutex, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.macro_rejected++;
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_HID_INPUT_SUBMIT_BACKPRESSURE;
    }
    bool replaced_pending = uxQueueMessagesWaiting(queue) != 0U;
    BaseType_t queued = xQueueOverwrite(queue, &macro);
    xSemaphoreGive(s_ctx.macro_mutex);
    if (queued != pdPASS) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.macro_rejected++;
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_HID_INPUT_SUBMIT_QUEUE_FULL;
    }

    portENTER_CRITICAL(&s_state_lock);
    s_ctx.stats.enqueued++;
    s_ctx.stats.macro_accepted++;
    if (replaced_pending) {
        s_ctx.stats.macro_cancelled++;
        s_ctx.stats.macro_replaced++;
    }
    portEXIT_CRITICAL(&s_state_lock);
    notify_worker();
    return JETKVM_HID_INPUT_SUBMIT_ACCEPTED;
}

static jetkvm_hid_input_submit_result_t submit_macro_cancel(
    uint32_t generation)
{
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.stats.submitted++;
    if (!s_ctx.initialized || s_ctx.stopping || !s_ctx.accepting_input) {
        s_ctx.stats.macro_rejected++;
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_HID_INPUT_SUBMIT_NOT_READY;
    }
    if (generation != s_ctx.active_generation) {
        s_ctx.stats.stale_generation_dropped++;
        s_ctx.stats.macro_rejected++;
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_HID_INPUT_SUBMIT_STALE_GENERATION;
    }
    s_ctx.macro_cancel_sequence = next_macro_action_sequence_locked();
    s_ctx.stats.enqueued++;
    s_ctx.stats.macro_cancel_requests++;
    portEXIT_CRITICAL(&s_state_lock);
    notify_worker();
    return JETKVM_HID_INPUT_SUBMIT_ACCEPTED;
}

static jetkvm_hid_input_submit_result_t submit_event(
    uint32_t generation, const jetkvm_hidrpc_event_t *event, bool movement_only)
{
    /* Capture before validation, locks or queue inspection. Worker scheduling
     * must never change the semantic arrival time of a keepalive. */
    const int64_t received_at_us = esp_timer_get_time();
    if (event == NULL || generation == 0U) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.invalid_events++;
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_HID_INPUT_SUBMIT_INVALID_ARGUMENT;
    }

    if (movement_only && event->type != JETKVM_HIDRPC_EVENT_ABS_MOUSE_REPORT &&
        event->type != JETKVM_HIDRPC_EVENT_REL_MOUSE_REPORT) {
        return JETKVM_HID_INPUT_SUBMIT_NOT_SUPPORTED;
    }
    if (event->type == JETKVM_HIDRPC_EVENT_KEYBOARD_MACRO_REPORT) {
        return submit_macro_event(generation, event);
    }
    if (event->type == JETKVM_HIDRPC_EVENT_CANCEL_KEYBOARD_MACRO_REPORT) {
        return submit_macro_cancel(generation);
    }

    input_command_t command = {
        .generation = generation,
        .received_at_us = received_at_us,
    };
    if (!copy_event_to_command(event, &command)) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.invalid_events++;
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_HID_INPUT_SUBMIT_INVALID_ARGUMENT;
    }

    bool pure_absolute_move = false;
    bool mouse_state_change = false;
    QueueHandle_t queue;
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.stats.submitted++;
    if (!s_ctx.initialized || s_ctx.stopping || !s_ctx.accepting_input) {
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_HID_INPUT_SUBMIT_NOT_READY;
    }
    if (generation != s_ctx.active_generation) {
        s_ctx.stats.stale_generation_dropped++;
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_HID_INPUT_SUBMIT_STALE_GENERATION;
    }
    command.epoch = s_ctx.active_epoch;
    /* Partially reliable movement can arrive after a reliable button release.
     * Only the reliable channel owns button transitions, within this epoch. */
    if (movement_only) {
        if (command.type == COMMAND_ABSOLUTE_MOUSE)
            command.data.absolute_mouse.buttons = s_ctx.submitted_absolute_buttons;
        else
            command.data.relative_mouse.buttons = s_ctx.submitted_relative_buttons;
    }
    bool cancels_macro = command.type == COMMAND_KEYBOARD ||
                         command.type == COMMAND_KEYPRESS;
    if (cancels_macro) {
        s_ctx.macro_cancel_sequence = next_macro_action_sequence_locked();
    }
    if (command.type == COMMAND_ABSOLUTE_MOUSE) {
        pure_absolute_move = command.data.absolute_mouse.buttons ==
                             s_ctx.submitted_absolute_buttons;
    }
    mouse_state_change = !movement_only &&
        ((command.type == COMMAND_ABSOLUTE_MOUSE && !pure_absolute_move) ||
         (command.type == COMMAND_RELATIVE_MOUSE &&
          command.data.relative_mouse.buttons != s_ctx.submitted_relative_buttons));
    queue = s_ctx.queue;
    portEXIT_CRITICAL(&s_state_lock);

    UBaseType_t depth = uxQueueMessagesWaiting(queue);
    if (pure_absolute_move &&
        depth >= JETKVM_HID_INPUT_QUEUE_LENGTH -
                     JETKVM_HID_INPUT_RESERVED_STATE_SLOTS) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.absolute_move_dropped++;
        s_ctx.stats.absolute_move_backpressure_dropped++;
        portEXIT_CRITICAL(&s_state_lock);
        return JETKVM_HID_INPUT_SUBMIT_BACKPRESSURE;
    }

    /* The peer/data-channel thread must never wait behind USB work. */
    BaseType_t queued = command.type == COMMAND_KEEPALIVE
                            ? xQueueSendToFront(queue, &command, 0)
                            : xQueueSendToBack(queue, &command, 0);
    if (queued != pdTRUE) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.queue_full_dropped++;
        if (mouse_state_change && generation == s_ctx.active_generation &&
            command.epoch == s_ctx.active_epoch) {
            /* A reliable release must not leave the cached button pressed.
             * Invalidate queued input and request an out-of-band release; this
             * remains nonblocking even when every queue slot is occupied. */
            s_ctx.active_epoch++;
            s_ctx.submitted_absolute_buttons = 0;
            s_ctx.submitted_relative_buttons = 0;
            s_ctx.overflow_release = true;
            s_ctx.stats.release_requests++;
        }
        if (pure_absolute_move) {
            s_ctx.stats.absolute_move_dropped++;
        }
        portEXIT_CRITICAL(&s_state_lock);
        if (cancels_macro || mouse_state_change) {
            notify_worker();
        }
        return JETKVM_HID_INPUT_SUBMIT_QUEUE_FULL;
    }

    depth = uxQueueMessagesWaiting(queue);
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.stats.enqueued++;
    if (command.type == COMMAND_ABSOLUTE_MOUSE &&
        s_ctx.initialized && !s_ctx.stopping && s_ctx.accepting_input &&
        command.generation == s_ctx.active_generation &&
        command.epoch == s_ctx.active_epoch) {
        s_ctx.submitted_absolute_buttons =
            command.data.absolute_mouse.buttons;
    }
    if (command.type == COMMAND_RELATIVE_MOUSE &&
        s_ctx.initialized && !s_ctx.stopping && s_ctx.accepting_input &&
        command.generation == s_ctx.active_generation &&
        command.epoch == s_ctx.active_epoch) {
        s_ctx.submitted_relative_buttons = command.data.relative_mouse.buttons;
    }
    if (depth > s_ctx.stats.queue_high_watermark) {
        s_ctx.stats.queue_high_watermark = depth;
    }
    portEXIT_CRITICAL(&s_state_lock);
    notify_worker();
    return JETKVM_HID_INPUT_SUBMIT_ACCEPTED;
}

jetkvm_hid_input_submit_result_t jetkvm_hid_input_submit_event(
    uint32_t generation, const jetkvm_hidrpc_event_t *event)
{
    return submit_event(generation, event, false);
}

jetkvm_hid_input_submit_result_t jetkvm_hid_input_submit_movement(
    uint32_t generation, const jetkvm_hidrpc_event_t *event)
{
    return submit_event(generation, event, true);
}

esp_err_t jetkvm_hid_input_submit_local_relative_mouse(int8_t dx, int8_t dy)
{
    input_command_t command = {
        .type = COMMAND_RELATIVE_MOUSE,
        .local = true,
        .received_at_us = esp_timer_get_time(),
        .data.relative_mouse = {
            .dx = dx,
            .dy = dy,
            .buttons = 0U,
        },
    };
    QueueHandle_t queue = NULL;
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.stats.submitted++;
    if (!s_ctx.initialized || s_ctx.stopping || !s_ctx.usb_available ||
        s_ctx.worker_paused) {
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_INVALID_STATE;
    }
    command.epoch = s_ctx.active_epoch;
    queue = s_ctx.queue;
    portEXIT_CRITICAL(&s_state_lock);

    if (xQueueSendToBack(queue, &command, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_state_lock);
        s_ctx.stats.queue_full_dropped++;
        portEXIT_CRITICAL(&s_state_lock);
        return ESP_ERR_TIMEOUT;
    }
    UBaseType_t depth = uxQueueMessagesWaiting(queue);
    portENTER_CRITICAL(&s_state_lock);
    s_ctx.stats.enqueued++;
    if (depth > s_ctx.stats.queue_high_watermark) {
        s_ctx.stats.queue_high_watermark = depth;
    }
    portEXIT_CRITICAL(&s_state_lock);
    notify_worker();
    return ESP_OK;
}

int64_t jetkvm_hid_input_last_user_input_us(void)
{
    int64_t last_user_input_us;
    portENTER_CRITICAL(&s_state_lock);
    last_user_input_us = s_ctx.last_user_input_us;
    portEXIT_CRITICAL(&s_state_lock);
    return last_user_input_us;
}

esp_err_t jetkvm_hid_input_get_snapshot(
    jetkvm_hid_input_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    portENTER_CRITICAL(&s_state_lock);
    snapshot->initialized = s_ctx.initialized;
    snapshot->usb_available = s_ctx.usb_available;
    snapshot->accepting_input = s_ctx.accepting_input;
    snapshot->active_generation = s_ctx.active_generation;
    snapshot->keyboard_leds = s_ctx.keyboard_leds;
    snapshot->macro_active = s_ctx.macro_active;
    snapshot->macro_is_paste = s_ctx.macro_is_paste;
    snapshot->macro_step_index = s_ctx.macro_step_index;
    snapshot->macro_step_count = s_ctx.macro_step_count;
    snapshot->macro_state_revision = s_ctx.macro_state_revision;
    snapshot->keyboard_revision = s_ctx.keyboard_revision;
    snapshot->last_user_input_us = s_ctx.last_user_input_us;
    portEXIT_CRITICAL(&s_state_lock);

    esp_err_t error = jetkvm_usb_hid_get_state(&snapshot->usb);
    if (error != ESP_OK) {
        return error;
    }
    if (snapshot->usb_available) {
        snapshot->keyboard_leds = snapshot->usb.keyboard_leds;
    }
    return jetkvm_usb_hid_get_keyboard_state(&snapshot->keyboard);
}

esp_err_t jetkvm_hid_input_get_stats(jetkvm_hid_input_stats_t *stats)
{
    if (stats == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_state_lock);
    *stats = s_ctx.stats;
    portEXIT_CRITICAL(&s_state_lock);
    return ESP_OK;
}

bool jetkvm_hid_input_get_keyboard_led_state(
    jetkvm_keyboard_led_state_t *state, void *ctx)
{
    (void)ctx;
    if (state == NULL) {
        return false;
    }
    jetkvm_hid_input_snapshot_t snapshot;
    if (jetkvm_hid_input_get_snapshot(&snapshot) != ESP_OK ||
        !snapshot.initialized) {
        return false;
    }
    *state = (jetkvm_keyboard_led_state_t) {
        .num_lock = (snapshot.keyboard_leds & 0x01U) != 0U,
        .caps_lock = (snapshot.keyboard_leds & 0x02U) != 0U,
        .scroll_lock = (snapshot.keyboard_leds & 0x04U) != 0U,
        .compose = (snapshot.keyboard_leds & 0x08U) != 0U,
        .kana = (snapshot.keyboard_leds & 0x10U) != 0U,
        .shift = (snapshot.keyboard.modifier & (0x02U | 0x20U)) != 0U,
    };
    return true;
}

bool jetkvm_hid_input_get_key_down_state(
    jetkvm_hidrpc_keyboard_report_t *state, void *ctx)
{
    (void)ctx;
    if (state == NULL) {
        return false;
    }
    jetkvm_hid_input_snapshot_t snapshot;
    if (jetkvm_hid_input_get_snapshot(&snapshot) != ESP_OK ||
        !snapshot.initialized) {
        return false;
    }
    state->modifier = snapshot.keyboard.modifier;
    memcpy(state->keys, snapshot.keyboard.keycodes, sizeof(state->keys));
    return true;
}

bool jetkvm_hid_input_get_usb_state(jetkvm_usb_state_t *state, void *ctx)
{
    (void)ctx;
    if (state == NULL) {
        return false;
    }
    jetkvm_hid_input_snapshot_t snapshot;
    if (jetkvm_hid_input_get_snapshot(&snapshot) != ESP_OK) {
        return false;
    }
    if (!snapshot.usb.initialized || !snapshot.usb.connected) {
        *state = JETKVM_USB_STATE_NOT_ATTACHED;
    } else if (snapshot.usb.suspended) {
        *state = JETKVM_USB_STATE_SUSPENDED;
    } else if (snapshot.usb.mounted) {
        *state = JETKVM_USB_STATE_CONFIGURED;
    } else {
        /* tud_connected() becomes true on the first setup packet. TinyUSB
         * keeps its SET_ADDRESS bit private, so claiming ADDRESSED here would
         * overstate what the public API proves. ATTACHED is the conservative
         * lower-bound state until configuration completes. */
        *state = JETKVM_USB_STATE_ATTACHED;
    }
    return true;
}

bool jetkvm_hid_input_get_usb_configuration(
    jetkvm_usb_configuration_snapshot_t *snapshot, void *ctx)
{
    (void)ctx;
    if (snapshot == NULL) {
        return false;
    }

    jetkvm_usb_hid_configuration_snapshot_t usb = {0};
    if (jetkvm_usb_hid_get_configuration_snapshot(&usb) != ESP_OK) {
        return false;
    }
    *snapshot = (jetkvm_usb_configuration_snapshot_t) {
        .vendor_id = usb.vendor_id,
        .product_id = usb.product_id,
        .absolute_mouse = usb.absolute_mouse,
        .relative_mouse = usb.relative_mouse,
        .keyboard = usb.keyboard,
        .mass_storage = usb.mass_storage,
        .serial_console = usb.serial_console,
        .audio = usb.audio,
        .emulation_enabled = usb.emulation_enabled,
    };
    memcpy(snapshot->serial_number, usb.serial_number,
           sizeof(snapshot->serial_number));
    memcpy(snapshot->manufacturer, usb.manufacturer,
           sizeof(snapshot->manufacturer));
    memcpy(snapshot->product, usb.product, sizeof(snapshot->product));
    return true;
}

jetkvm_usb_wake_result_t jetkvm_hid_input_wake_host(void *ctx)
{
    (void)ctx;
    bool bridge_initialized;
    portENTER_CRITICAL(&s_state_lock);
    bridge_initialized = s_ctx.initialized;
    portEXIT_CRITICAL(&s_state_lock);

    jetkvm_usb_hid_state_t usb = {0};
    if (jetkvm_usb_hid_get_state(&usb) != ESP_OK || !usb.initialized) {
        /* Once the bridge has initialized, an absent core is the normal
         * result of setUsbEmulationState(false), not a missing capability.
         * Match Go JetKVM: waking a deliberately detached host is a no-op. */
        return bridge_initialized ? JETKVM_USB_WAKE_NOOP :
                                    JETKVM_USB_WAKE_UNAVAILABLE;
    }

    /* Match JetKVM's no-op semantics for an absent or already-active host.
     * Crucially, do not call the core in these states: wake_attempts then
     * measures real suspended-bus attempts only. */
    if (!usb.connected || !usb.mounted || !usb.suspended) {
        return JETKVM_USB_WAKE_NOOP;
    }
    if (!usb.remote_wakeup_enabled) {
        return JETKVM_USB_WAKE_NOT_ENABLED;
    }

    esp_err_t error = jetkvm_usb_hid_remote_wakeup();
    if (error == ESP_OK) {
        return JETKVM_USB_WAKE_SUCCEEDED;
    }
    if (error == ESP_ERR_TIMEOUT) {
        return JETKVM_USB_WAKE_BUSY;
    }
    if (error != ESP_ERR_INVALID_STATE) {
        return JETKVM_USB_WAKE_FAILED;
    }

    /* Resolve a legitimate resume/detach race as a no-op. If the bus is
     * still suspended, INVALID_STATE means host permission disappeared or
     * the driver rejected the request and must remain visible as an error. */
    jetkvm_usb_hid_state_t after = {0};
    if (jetkvm_usb_hid_get_state(&after) != ESP_OK || !after.initialized) {
        return JETKVM_USB_WAKE_UNAVAILABLE;
    }
    if (!after.connected || !after.mounted || !after.suspended) {
        return JETKVM_USB_WAKE_NOOP;
    }
    return after.remote_wakeup_enabled ? JETKVM_USB_WAKE_FAILED :
                                         JETKVM_USB_WAKE_NOT_ENABLED;
}
