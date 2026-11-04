#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "fake_runtime.h"
#include "jetkvm_hid_input.h"

static void sleep_ms(unsigned milliseconds)
{
    struct timespec delay = {
        .tv_sec = milliseconds / 1000U,
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    nanosleep(&delay, NULL);
}

static bool wait_for_reports(size_t count, unsigned timeout_ms)
{
    for (unsigned elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        if (fake_usb_report_count() >= count) {
            return true;
        }
        sleep_ms(1U);
    }
    return false;
}

static jetkvm_hid_input_snapshot_t snapshot(void)
{
    jetkvm_hid_input_snapshot_t value;
    assert(jetkvm_hid_input_get_snapshot(&value) == ESP_OK);
    return value;
}

static bool wait_for_macro_active(bool active, unsigned timeout_ms)
{
    for (unsigned elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        if (snapshot().macro_active == active) {
            return true;
        }
        sleep_ms(1U);
    }
    return false;
}

static void encode_step(uint8_t *wire, uint8_t modifier, uint8_t key,
                        uint16_t delay_ms)
{
    memset(wire, 0, JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE);
    wire[0] = modifier;
    wire[1] = key;
    wire[7] = (uint8_t)(delay_ms >> 8);
    wire[8] = (uint8_t)delay_ms;
}

static jetkvm_hid_input_submit_result_t submit_macro(
    uint32_t generation, bool is_paste, const uint8_t *wire,
    uint32_t step_count)
{
    jetkvm_hidrpc_event_t event = {
        .type = JETKVM_HIDRPC_EVENT_KEYBOARD_MACRO_REPORT,
        .data.macro = {
            .is_paste = is_paste,
            .step_count = step_count,
            .encoded_steps = wire,
            .encoded_steps_size =
                (size_t)step_count * JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE,
        },
    };
    return jetkvm_hid_input_submit_event(generation, &event);
}

static jetkvm_hid_keepalive_decision_t evaluate_keepalive(
    jetkvm_hid_keepalive_policy_state_t *state, uint32_t generation,
    uint32_t epoch, int64_t received_at_us,
    jetkvm_hid_keepalive_result_t expected)
{
    jetkvm_hid_keepalive_decision_t decision =
        jetkvm_hid_keepalive_policy_evaluate(
            state, generation, epoch, received_at_us, received_at_us);
    assert(decision.result == expected);
    return decision;
}

static void test_keepalive_policy_thresholds(void)
{
    const int64_t start = 1000000;
    jetkvm_hid_keepalive_policy_state_t state = {0};

    jetkvm_hid_keepalive_decision_t decision = evaluate_keepalive(
        &state, 1U, 7U, start, JETKVM_HID_KEEPALIVE_ACCEPTED);
    assert(decision.extension_us ==
           JETKVM_HID_KEEPALIVE_BASE_EXTENSION_US);
    assert(decision.deadline_us ==
           start + JETKVM_HID_KEEPALIVE_BASE_EXTENSION_US);

    decision = evaluate_keepalive(
        &state, 1U, 7U,
        start + JETKVM_HID_KEEPALIVE_EXPECTED_INTERVAL_US,
        JETKVM_HID_KEEPALIVE_ACCEPTED);
    assert(decision.extension_us ==
           JETKVM_HID_KEEPALIVE_BASE_EXTENSION_US);

    jetkvm_hid_keepalive_policy_reset(&state);
    (void)evaluate_keepalive(
        &state, 1U, 7U, start, JETKVM_HID_KEEPALIVE_ACCEPTED);
    decision = evaluate_keepalive(
        &state, 1U, 7U,
        start + JETKVM_HID_KEEPALIVE_EXPECTED_INTERVAL_US + 1,
        JETKVM_HID_KEEPALIVE_JITTER_ADJUSTED);
    assert(decision.extension_us ==
           JETKVM_HID_KEEPALIVE_BASE_EXTENSION_US - 1);

    jetkvm_hid_keepalive_policy_reset(&state);
    (void)evaluate_keepalive(
        &state, 1U, 7U, start, JETKVM_HID_KEEPALIVE_ACCEPTED);
    decision = evaluate_keepalive(
        &state, 1U, 7U,
        start + JETKVM_HID_KEEPALIVE_EXPECTED_INTERVAL_US +
            JETKVM_HID_KEEPALIVE_MAX_LATENESS_US,
        JETKVM_HID_KEEPALIVE_JITTER_ADJUSTED);
    assert(decision.extension_us ==
           JETKVM_HID_KEEPALIVE_BASE_EXTENSION_US -
               JETKVM_HID_KEEPALIVE_MAX_LATENESS_US);

    jetkvm_hid_keepalive_policy_state_t before = state;
    decision = evaluate_keepalive(
        &state, 1U, 7U,
        state.last_arrival_us +
            JETKVM_HID_KEEPALIVE_EXPECTED_INTERVAL_US +
            JETKVM_HID_KEEPALIVE_MAX_LATENESS_US + 1,
        JETKVM_HID_KEEPALIVE_REJECTED_LATE);
    assert(decision.extension_us == 0 && decision.deadline_us == 0);
    assert(memcmp(&state, &before, sizeof(state)) == 0);

    jetkvm_hid_keepalive_policy_reset(&state);
    (void)evaluate_keepalive(
        &state, 1U, 7U, start, JETKVM_HID_KEEPALIVE_ACCEPTED);
    before = state;
    (void)evaluate_keepalive(
        &state, 1U, 7U,
        start + JETKVM_HID_KEEPALIVE_MAX_STALENESS_US + 1,
        JETKVM_HID_KEEPALIVE_REJECTED_STALE);
    assert(memcmp(&state, &before, sizeof(state)) == 0);

    /* Go uses a strict greater-than staleness test. The exact 225 ms edge is
     * therefore classified by the tighter lateness guard, not stale. */
    (void)evaluate_keepalive(
        &state, 1U, 7U,
        start + JETKVM_HID_KEEPALIVE_MAX_STALENESS_US,
        JETKVM_HID_KEEPALIVE_REJECTED_LATE);
    assert(memcmp(&state, &before, sizeof(state)) == 0);
}

static void test_keepalive_arrival_deadline_and_isolation(void)
{
    const int64_t start = 2000000;
    jetkvm_hid_keepalive_policy_state_t state = {0};
    (void)evaluate_keepalive(
        &state, 3U, 11U, start, JETKVM_HID_KEEPALIVE_ACCEPTED);

    /* 25 ms late: the extension shrinks by the lateness. */
    const int64_t received_at = start + 75000;
    const int64_t expected_extension =
        JETKVM_HID_KEEPALIVE_BASE_EXTENSION_US - 25000;
    const int64_t worker_before_deadline =
        received_at + expected_extension - 10000;
    jetkvm_hid_keepalive_decision_t decision =
        jetkvm_hid_keepalive_policy_evaluate(
            &state, 3U, 11U, received_at, worker_before_deadline);
    assert(decision.result == JETKVM_HID_KEEPALIVE_JITTER_ADJUSTED);
    assert(decision.extension_us == expected_extension);
    assert(decision.deadline_us == received_at + expected_extension);

    /* Simulated worker queue delay cannot move this fixed arrival deadline. */
    const int64_t worker_after_deadline = decision.deadline_us + 10000;
    assert(decision.deadline_us - worker_before_deadline == 10000);
    assert(decision.deadline_us < worker_after_deadline);
    assert(decision.deadline_us !=
           worker_before_deadline +
               JETKVM_HID_KEEPALIVE_BASE_EXTENSION_US);

    /* A packet that has itself sat in the worker queue past the staleness
     * budget cannot resurrect a release timer, even as a first tick. */
    jetkvm_hid_keepalive_policy_state_t queued = {0};
    decision = jetkvm_hid_keepalive_policy_evaluate(
        &queued, 8U, 1U, start,
        start + JETKVM_HID_KEEPALIVE_MAX_STALENESS_US + 1);
    assert(decision.result == JETKVM_HID_KEEPALIVE_REJECTED_STALE);
    assert(!queued.initialized);

    /* A new generation or flush epoch receives a fresh first-tick baseline,
     * even when it starts well beyond the previous staleness window. */
    decision = evaluate_keepalive(
        &state, 4U, 11U, start + 1000000,
        JETKVM_HID_KEEPALIVE_ACCEPTED);
    assert(decision.extension_us ==
           JETKVM_HID_KEEPALIVE_BASE_EXTENSION_US);
    assert(state.generation == 4U && state.epoch == 11U);
    decision = evaluate_keepalive(
        &state, 4U, 12U, start + 2000000,
        JETKVM_HID_KEEPALIVE_ACCEPTED);
    assert(state.generation == 4U && state.epoch == 12U);

    /* Queue-front insertion can expose an older packet after a newer one.
     * It must be rejected without rolling policy state backwards. */
    jetkvm_hid_keepalive_policy_state_t before = state;
    (void)evaluate_keepalive(
        &state, 4U, 12U, state.last_arrival_us - 1,
        JETKVM_HID_KEEPALIVE_REJECTED_LATE);
    assert(memcmp(&state, &before, sizeof(state)) == 0);

    assert(jetkvm_hid_keepalive_policy_evaluate(
               NULL, 4U, 12U, start, start).result ==
           JETKVM_HID_KEEPALIVE_INVALID_ARGUMENT);
    assert(jetkvm_hid_keepalive_policy_evaluate(
               &state, 0U, 12U, start, start).result ==
           JETKVM_HID_KEEPALIVE_INVALID_ARGUMENT);
    assert(jetkvm_hid_keepalive_policy_evaluate(
               &state, 4U, 12U, start, start - 1).result ==
           JETKVM_HID_KEEPALIVE_INVALID_ARGUMENT);
}

static void test_completion(uint64_t *last_revision)
{
    uint8_t wire[2U * JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE];
    encode_step(wire, 0x02U, 0x04U, 5U);
    encode_step(wire + JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE, 0U, 0U, 5U);
    fake_usb_reset_log();
    assert(submit_macro(1U, false, wire, 2U) ==
           JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
    assert(wait_for_reports(2U, 100U));
    assert(wait_for_macro_active(false, 100U));
    fake_keyboard_report_t first = fake_usb_report_at(0U);
    fake_keyboard_report_t second = fake_usb_report_at(1U);
    assert(first.modifier == 0x02U && first.keys[0] == 0x04U);
    assert(second.modifier == 0U && second.keys[0] == 0U);

    jetkvm_hid_input_stats_t stats;
    assert(jetkvm_hid_input_get_stats(&stats) == ESP_OK);
    assert(stats.macro_accepted == 1U);
    assert(stats.macro_completed == 1U);
    jetkvm_hid_input_snapshot_t state = snapshot();
    assert(state.macro_state_revision > *last_revision);
    *last_revision = state.macro_state_revision;
}

static void test_cancel_during_delay(uint64_t *last_revision)
{
    uint8_t wire[JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE];
    encode_step(wire, 0U, 0x05U, 500U);
    fake_usb_reset_log();
    assert(submit_macro(1U, true, wire, 1U) ==
           JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
    assert(wait_for_reports(1U, 100U));
    assert(wait_for_macro_active(true, 100U));
    int64_t cancel_at = esp_timer_get_time();
    jetkvm_hidrpc_event_t cancel = {
        .type = JETKVM_HIDRPC_EVENT_CANCEL_KEYBOARD_MACRO_REPORT,
    };
    assert(jetkvm_hid_input_submit_event(1U, &cancel) ==
           JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
    assert(wait_for_macro_active(false, 100U));
    assert(wait_for_reports(2U, 100U));
    fake_keyboard_report_t clear = fake_usb_report_at(1U);
    assert(clear.modifier == 0U && clear.keys[0] == 0U);
    assert(clear.timestamp_us - cancel_at < 100000);

    jetkvm_hid_input_stats_t stats;
    assert(jetkvm_hid_input_get_stats(&stats) == ESP_OK);
    assert(stats.macro_cancel_requests >= 1U);
    assert(stats.macro_cancelled >= 1U);
    jetkvm_hid_input_snapshot_t state = snapshot();
    assert(!state.macro_active && state.macro_is_paste);
    assert(state.macro_state_revision > *last_revision);
    *last_revision = state.macro_state_revision;
}

static void test_replacement_has_zero_barrier(uint64_t *last_revision)
{
    uint8_t old_wire[JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE];
    uint8_t new_wire[2U * JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE];
    encode_step(old_wire, 0U, 0x06U, 500U);
    encode_step(new_wire, 0U, 0x07U, 5U);
    encode_step(new_wire + JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE,
                0U, 0U, 5U);
    fake_usb_reset_log();
    assert(submit_macro(1U, false, old_wire, 1U) ==
           JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
    assert(wait_for_reports(1U, 100U));
    assert(snapshot().macro_is_paste);
    assert(submit_macro(1U, false, new_wire, 2U) ==
           JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
    assert(wait_for_reports(4U, 150U));
    assert(wait_for_macro_active(false, 100U));
    assert(fake_usb_report_at(0U).keys[0] == 0x06U);
    assert(fake_usb_report_at(1U).keys[0] == 0U);
    assert(fake_usb_report_at(2U).keys[0] == 0x07U);

    jetkvm_hid_input_stats_t stats;
    assert(jetkvm_hid_input_get_stats(&stats) == ESP_OK);
    assert(stats.macro_replaced >= 1U);
    jetkvm_hid_input_snapshot_t state = snapshot();
    assert(state.macro_state_revision > *last_revision);
    *last_revision = state.macro_state_revision;
}

static void test_normal_key_up_not_delayed(void)
{
    uint8_t wire[JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE];
    encode_step(wire, 0U, 0x08U, 500U);
    fake_usb_reset_log();
    assert(submit_macro(1U, false, wire, 1U) ==
           JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
    assert(wait_for_reports(1U, 100U));

    jetkvm_hidrpc_event_t key_up = {
        .type = JETKVM_HIDRPC_EVENT_KEYPRESS_REPORT,
        .data.keypress = {.key = 0x08U, .pressed = false},
    };
    int64_t submitted_at = esp_timer_get_time();
    assert(jetkvm_hid_input_submit_event(1U, &key_up) ==
           JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
    assert(wait_for_macro_active(false, 100U));
    assert(wait_for_reports(3U, 100U));
    fake_keyboard_report_t final = fake_usb_report_at(2U);
    assert(final.keys[0] == 0U);
    assert(final.timestamp_us - submitted_at < 100000);
}

static void submit_key(uint8_t key, bool pressed)
{
    jetkvm_hidrpc_event_t event = {
        .type = JETKVM_HIDRPC_EVENT_KEYPRESS_REPORT,
        .data.keypress = {.key = key, .pressed = pressed},
    };
    assert(jetkvm_hid_input_submit_event(1U, &event) ==
           JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
}

static bool wait_for_keyboard_revision(uint32_t revision, unsigned timeout_ms)
{
    for (unsigned elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        if (snapshot().keyboard_revision == revision) {
            return true;
        }
        sleep_ms(1U);
    }
    return false;
}

/* A press and its release advance the keyboard revision twice, so a reader
 * that polls after both still sees a change; auto-repeated key downs leave
 * the keys as they were and do not advance it. */
static void test_keyboard_revision_counts_changes(void)
{
    const uint32_t start = snapshot().keyboard_revision;
    submit_key(0x73U, true);
    assert(wait_for_keyboard_revision(start + 1U, 100U));
    submit_key(0x73U, true);
    submit_key(0x73U, true);
    submit_key(0x73U, false);
    assert(wait_for_keyboard_revision(start + 2U, 100U));
    sleep_ms(20U);
    assert(snapshot().keyboard_revision == start + 2U);
    assert(snapshot().keyboard.keycodes[0] == 0U);
}

static void test_generation_change_cancels_and_releases(void)
{
    uint8_t wire[2U * JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE];
    encode_step(wire, 0U, 0x09U, 500U);
    encode_step(wire + JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE,
                0U, 0x0aU, 1U);
    fake_usb_reset_log();
    assert(submit_macro(1U, false, wire, 2U) ==
           JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
    assert(wait_for_reports(1U, 100U));
    int64_t changed_at = esp_timer_get_time();
    assert(jetkvm_hid_input_set_active_generation(2U) == ESP_OK);
    assert(esp_timer_get_time() - changed_at < 100000);
    assert(wait_for_macro_active(false, 100U));
    for (unsigned elapsed = 0; elapsed < 100U &&
                               fake_usb_release_all_count() == 0U;
         ++elapsed) {
        sleep_ms(1U);
    }
    assert(fake_usb_release_all_count() >= 1U);
    sleep_ms(550U);
    size_t reports = fake_usb_report_count();
    for (size_t i = 0; i < reports; ++i) {
        assert(fake_usb_report_at(i).keys[0] != 0x0aU);
    }
}

static void test_fixed_limit_rejection(void)
{
    uint8_t wire[(JETKVM_HIDRPC_MACRO_MAX_STEPS + 1U) *
                 JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE] = { 0 };
    jetkvm_hid_input_stats_t before;
    assert(jetkvm_hid_input_get_stats(&before) == ESP_OK);
    assert(submit_macro(2U, false, wire,
                        JETKVM_HIDRPC_MACRO_MAX_STEPS + 1U) ==
           JETKVM_HID_INPUT_SUBMIT_INVALID_ARGUMENT);
    jetkvm_hid_input_stats_t after;
    assert(jetkvm_hid_input_get_stats(&after) == ESP_OK);
    assert(after.macro_rejected == before.macro_rejected + 1U);
}

static void test_usb_pause_preserves_latest_generation(void)
{
    jetkvm_hid_input_snapshot_t before = snapshot();
    assert(before.initialized);
    assert(before.usb_available);
    assert(before.active_generation == 2U);
    assert(before.accepting_input);
    fake_usb_emit_led(0x02U);
    assert(snapshot().keyboard_leds == 0x02U);

    assert(jetkvm_hid_input_set_usb_available(false) == ESP_OK);
    jetkvm_hid_input_snapshot_t paused = snapshot();
    assert(paused.initialized);
    assert(!paused.usb_available);
    assert(!paused.accepting_input);
    assert(paused.active_generation == 2U);
    assert(paused.keyboard_leds == 0U);
    fake_usb_emit_led(0x04U);
    assert(snapshot().keyboard_leds == 0U);

    jetkvm_hidrpc_event_t key = {
        .type = JETKVM_HIDRPC_EVENT_KEYPRESS_REPORT,
        .data.keypress = {.key = 0x04U, .pressed = true},
    };
    assert(jetkvm_hid_input_submit_event(2U, &key) ==
           JETKVM_HID_INPUT_SUBMIT_NOT_READY);

    /* A peer replacement while target USB is off updates the retained truth
     * without accidentally reopening report acceptance. */
    unsigned releases_while_paused = fake_usb_release_all_count();
    assert(jetkvm_hid_input_set_active_generation(3U) == ESP_OK);
    sleep_ms(20U);
    assert(fake_usb_release_all_count() == releases_while_paused);
    paused = snapshot();
    assert(paused.active_generation == 3U);
    assert(!paused.usb_available);
    assert(!paused.accepting_input);

    assert(jetkvm_hid_input_set_usb_available(true) == ESP_OK);
    jetkvm_hid_input_snapshot_t resumed = snapshot();
    assert(resumed.usb_available);
    assert(resumed.accepting_input);
    assert(resumed.active_generation == 3U);
    fake_usb_emit_led(0x01U);
    assert(snapshot().keyboard_leds == 0x01U);
    assert(jetkvm_hid_input_submit_event(2U, &key) ==
           JETKVM_HID_INPUT_SUBMIT_STALE_GENERATION);
    assert(jetkvm_hid_input_submit_event(3U, &key) ==
           JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
}

static bool wait_for_local_mouse_reports(uint64_t count, unsigned timeout_ms)
{
    for (unsigned elapsed = 0U; elapsed < timeout_ms; ++elapsed) {
        jetkvm_hid_input_stats_t stats;
        assert(jetkvm_hid_input_get_stats(&stats) == ESP_OK);
        if (stats.local_relative_mouse_reports >= count) {
            return true;
        }
        sleep_ms(1U);
    }
    return false;
}

static void test_local_mouse_without_live_generation(void)
{
    jetkvm_hid_input_stats_t before;
    assert(jetkvm_hid_input_get_stats(&before) == ESP_OK);
    int64_t previous_input_us = jetkvm_hid_input_last_user_input_us();
    assert(jetkvm_hid_input_submit_local_relative_mouse(2, -3) == ESP_OK);
    assert(wait_for_local_mouse_reports(
        before.local_relative_mouse_reports + 1U, 100U));
    assert(jetkvm_hid_input_last_user_input_us() >= previous_input_us);

    /* Persisted automation must keep working after the browser generation is
     * gone; it still shares the worker's release-all barrier. */
    assert(jetkvm_hid_input_invalidate_generation(3U) == ESP_OK);
    assert(!snapshot().accepting_input);
    assert(jetkvm_hid_input_submit_local_relative_mouse(-1, 1) == ESP_OK);
    assert(wait_for_local_mouse_reports(
        before.local_relative_mouse_reports + 2U, 100U));
}

static jetkvm_usb_hid_state_t usb_state(bool initialized, bool connected,
                                         bool mounted, bool suspended,
                                         bool wake_enabled)
{
    return (jetkvm_usb_hid_state_t) {
        .initialized = initialized,
        .connected = connected,
        .mounted = mounted,
        .suspended = suspended,
        .remote_wakeup_enabled = wake_enabled,
        .link_high_speed = connected,
    };
}

static bool wait_for_macro_failures(uint64_t count, unsigned timeout_ms)
{
    for (unsigned elapsed = 0; elapsed < timeout_ms; ++elapsed) {
        jetkvm_hid_input_stats_t stats;
        assert(jetkvm_hid_input_get_stats(&stats) == ESP_OK);
        if (stats.macro_failed >= count) {
            return true;
        }
        sleep_ms(1U);
    }
    return false;
}

static void test_detached_macro_fails_without_false_completion(
    uint64_t *last_revision)
{
    uint8_t wire[JETKVM_HIDRPC_MACRO_STEP_WIRE_SIZE];
    encode_step(wire, 0U, 0x04U, 20U);
    jetkvm_hid_input_stats_t before;
    assert(jetkvm_hid_input_get_stats(&before) == ESP_OK);

    fake_usb_set_state(usb_state(true, false, false, false, false));
    fake_usb_reset_log();
    assert(submit_macro(2U, false, wire, 1U) ==
           JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
    assert(wait_for_macro_failures(before.macro_failed + 1U, 100U));
    assert(wait_for_macro_active(false, 100U));

    jetkvm_hid_input_stats_t after;
    assert(jetkvm_hid_input_get_stats(&after) == ESP_OK);
    assert(after.macro_accepted == before.macro_accepted + 1U);
    assert(after.macro_failed == before.macro_failed + 1U);
    assert(after.macro_completed == before.macro_completed);
    assert(fake_usb_report_count() == 0U);
    jetkvm_hid_input_snapshot_t state = snapshot();
    assert(!state.macro_active && state.macro_is_paste);
    assert(state.macro_state_revision > *last_revision);
    *last_revision = state.macro_state_revision;

    fake_usb_set_state(usb_state(true, true, true, false, false));
}

static void expect_usb_state(jetkvm_usb_state_t expected)
{
    jetkvm_usb_state_t actual = JETKVM_USB_STATE_ADDRESSED;
    assert(jetkvm_hid_input_get_usb_state(&actual, NULL));
    assert(actual == expected);
}

static void expect_usb_configuration(bool emulation_enabled)
{
    jetkvm_usb_configuration_snapshot_t snapshot = {0};
    assert(jetkvm_hid_input_get_usb_configuration(&snapshot, NULL));
    assert(snapshot.vendor_id == 0x303a);
    assert(snapshot.product_id == 0x400c);
    assert(strcmp(snapshot.serial_number, "ESP32-P4") == 0);
    assert(strcmp(snapshot.manufacturer, "JetKVM") == 0);
    assert(strcmp(snapshot.product, "JetKVM USB Emulation Device") == 0);
    assert(snapshot.keyboard);
    assert(snapshot.absolute_mouse);
    assert(snapshot.relative_mouse);
    assert(!snapshot.mass_storage);
    assert(!snapshot.serial_console);
    assert(!snapshot.audio);
    assert(snapshot.emulation_enabled == emulation_enabled);
}

static void test_usb_state_and_wake_adapter(void)
{
    unsigned attempts = fake_usb_wake_call_count();

    fake_usb_set_state(usb_state(false, false, false, false, false));
    expect_usb_state(JETKVM_USB_STATE_NOT_ATTACHED);
    expect_usb_configuration(false);
    assert(!jetkvm_hid_input_get_usb_configuration(NULL, NULL));
    /* The input bridge remains initialized across a user-requested target
     * USB shutdown, so wake is supported but detached: a compatible no-op. */
    assert(jetkvm_hid_input_wake_host(NULL) == JETKVM_USB_WAKE_NOOP);
    assert(fake_usb_wake_call_count() == attempts);

    fake_usb_set_state(usb_state(true, false, false, false, false));
    expect_usb_state(JETKVM_USB_STATE_NOT_ATTACHED);
    expect_usb_configuration(true);
    assert(jetkvm_hid_input_wake_host(NULL) == JETKVM_USB_WAKE_NOOP);
    assert(fake_usb_wake_call_count() == attempts);

    fake_usb_set_state(usb_state(true, true, false, false, false));
    expect_usb_state(JETKVM_USB_STATE_ATTACHED);
    assert(jetkvm_hid_input_wake_host(NULL) == JETKVM_USB_WAKE_NOOP);
    assert(fake_usb_wake_call_count() == attempts);

    fake_usb_set_state(usb_state(true, true, true, false, false));
    expect_usb_state(JETKVM_USB_STATE_CONFIGURED);
    assert(jetkvm_hid_input_wake_host(NULL) == JETKVM_USB_WAKE_NOOP);
    assert(fake_usb_wake_call_count() == attempts);

    fake_usb_set_state(usb_state(true, true, true, true, false));
    expect_usb_state(JETKVM_USB_STATE_SUSPENDED);
    assert(jetkvm_hid_input_wake_host(NULL) ==
           JETKVM_USB_WAKE_NOT_ENABLED);
    assert(fake_usb_wake_call_count() == attempts);

    fake_usb_set_state(usb_state(true, true, true, true, true));
    fake_usb_set_wake_result(ESP_OK);
    assert(jetkvm_hid_input_wake_host(NULL) ==
           JETKVM_USB_WAKE_SUCCEEDED);
    assert(fake_usb_wake_call_count() == ++attempts);

    fake_usb_set_wake_result(ESP_ERR_TIMEOUT);
    assert(jetkvm_hid_input_wake_host(NULL) == JETKVM_USB_WAKE_BUSY);
    assert(fake_usb_wake_call_count() == ++attempts);

    fake_usb_set_wake_result(ESP_FAIL);
    assert(jetkvm_hid_input_wake_host(NULL) == JETKVM_USB_WAKE_FAILED);
    assert(fake_usb_wake_call_count() == ++attempts);

    fake_usb_set_wake_result(ESP_OK);
    fake_usb_set_state(usb_state(true, true, true, false, false));
}

static void test_unreliable_movement_buttons(void)
{
    for (unsigned relative = 0; relative < 2; ++relative) {
        size_t start = fake_usb_mouse_count();
        jetkvm_hidrpc_event_t event = {.type = relative ?
            JETKVM_HIDRPC_EVENT_REL_MOUSE_REPORT : JETKVM_HIDRPC_EVENT_ABS_MOUSE_REPORT};
        if (relative) event.data.rel_mouse.buttons = 1;
        else event.data.abs_mouse.buttons = 1;
        assert(jetkvm_hid_input_submit_event(1, &event) == JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
        /* Old unpressed movement cannot interrupt the reliable drag. */
        if (relative) event.data.rel_mouse.buttons = 0;
        else event.data.abs_mouse.buttons = 0;
        assert(jetkvm_hid_input_submit_movement(1, &event) == JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
        assert(jetkvm_hid_input_submit_event(1, &event) == JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
        /* Delayed pressed movement cannot undo the reliable release. */
        if (relative) event.data.rel_mouse.buttons = 1;
        else event.data.abs_mouse.buttons = 1;
        assert(jetkvm_hid_input_submit_movement(1, &event) == JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
        for (unsigned i = 0; i < 200 && fake_usb_mouse_count() < start + 4; ++i) sleep_ms(1);
        assert(fake_usb_mouse_count() == start + 4);
        assert(fake_usb_mouse_at(start).buttons == 1);
        assert(fake_usb_mouse_at(start + 1).buttons == 1);
        assert(fake_usb_mouse_at(start + 2).buttons == 0);
        assert(fake_usb_mouse_at(start + 3).buttons == 0);
        assert(jetkvm_hid_input_submit_movement(2, &event) == JETKVM_HID_INPUT_SUBMIT_STALE_GENERATION);
    }
    size_t count = fake_usb_mouse_count();
    jetkvm_hidrpc_event_t mouse = {.type = JETKVM_HIDRPC_EVENT_ABS_MOUSE_REPORT,
        .data.abs_mouse = {.buttons = 1}};
    assert(jetkvm_hid_input_submit_event(1, &mouse) == JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
    for (unsigned i = 0; i < 200 && fake_usb_mouse_count() < count + 1; ++i) sleep_ms(1);
    assert(fake_usb_mouse_at(count).buttons == 1);
    unsigned releases = fake_usb_release_all_count();
    fake_queue_fail_next_send();
    mouse.data.abs_mouse.buttons = 0;
    assert(jetkvm_hid_input_submit_event(1, &mouse) == JETKVM_HID_INPUT_SUBMIT_QUEUE_FULL);
    for (unsigned i = 0; i < 200 && fake_usb_release_all_count() == releases; ++i) sleep_ms(1);
    assert(fake_usb_release_all_count() > releases);
    mouse.data.abs_mouse.buttons = 1;
    assert(jetkvm_hid_input_submit_movement(1, &mouse) == JETKVM_HID_INPUT_SUBMIT_ACCEPTED);
    for (unsigned i = 0; i < 200 && fake_usb_mouse_count() < count + 2; ++i) sleep_ms(1);
    assert(fake_usb_mouse_at(count + 1).buttons == 0);
    jetkvm_hidrpc_event_t key = {.type = JETKVM_HIDRPC_EVENT_KEYBOARD_REPORT};
    assert(jetkvm_hid_input_submit_movement(1, &key) == JETKVM_HID_INPUT_SUBMIT_NOT_SUPPORTED);
}

static atomic_uint s_recoveries;
static atomic_uint s_recovery_timeouts;

static void count_recovery(unsigned consecutive_timeouts, void *ctx)
{
    (void)ctx;
    atomic_store(&s_recovery_timeouts, consecutive_timeouts);
    atomic_fetch_add(&s_recoveries, 1U);
}

/* Presses a key that the fake host does not take, and waits until the worker
 * has given the report up, which takes its 100 ms deadline. */
static void press_key_that_times_out(void)
{
    jetkvm_hid_input_stats_t before;
    assert(jetkvm_hid_input_get_stats(&before) == ESP_OK);
    submit_key(0x04U, true);
    for (unsigned elapsed = 0; elapsed < 1000U; ++elapsed) {
        jetkvm_hid_input_stats_t now;
        assert(jetkvm_hid_input_get_stats(&now) == ESP_OK);
        if (now.usb_errors > before.usb_errors) {
            return;
        }
        sleep_ms(1U);
    }
    assert(!"the worker did not give the report up");
}

/* The recovery is for a host that stops reading the keyboard while USB stays
 * configured. Three reports in a row that time out show that, but only when
 * TinyUSB's task has handled every event of the bus: while it has not, a
 * report can wait for this device. */
static void test_write_recovery_counts_timeouts_of_the_host_only(void)
{
    assert(jetkvm_hid_input_set_write_recovery_handler(count_recovery, NULL) ==
           ESP_OK);
    fake_usb_set_state(usb_state(true, true, true, false, false));

    /* TinyUSB's task is behind: no number of timeouts is a recovery. */
    fake_usb_set_bus_events_waiting(true);
    fake_usb_set_keypress_result(ESP_ERR_TIMEOUT);
    for (unsigned i = 0; i < 5U; ++i) {
        press_key_that_times_out();
    }
    assert(atomic_load(&s_recoveries) == 0U);

    /* It has caught up and the host still takes nothing: two timeouts are
     * not enough, and a report that the host takes ends the streak. */
    fake_usb_set_bus_events_waiting(false);
    press_key_that_times_out();
    press_key_that_times_out();
    fake_usb_set_keypress_result(ESP_OK);
    size_t reports = fake_usb_report_count();
    submit_key(0x04U, false);
    assert(wait_for_reports(reports + 1U, 100U));
    fake_usb_set_keypress_result(ESP_ERR_TIMEOUT);
    press_key_that_times_out();
    press_key_that_times_out();
    assert(atomic_load(&s_recoveries) == 0U);

    /* A timeout while TinyUSB's task is behind neither counts nor ends the
     * streak; the third timeout of the host asks for the recovery. */
    fake_usb_set_bus_events_waiting(true);
    press_key_that_times_out();
    assert(atomic_load(&s_recoveries) == 0U);
    fake_usb_set_bus_events_waiting(false);
    press_key_that_times_out();
    assert(atomic_load(&s_recoveries) == 1U);
    assert(atomic_load(&s_recovery_timeouts) == 3U);

    fake_usb_set_keypress_result(ESP_OK);
    reports = fake_usb_report_count();
    submit_key(0x04U, false);
    assert(wait_for_reports(reports + 1U, 100U));
    assert(jetkvm_hid_input_set_write_recovery_handler(NULL, NULL) == ESP_OK);
}

int main(void)
{
    test_keepalive_policy_thresholds();
    test_keepalive_arrival_deadline_and_isolation();

    assert(jetkvm_hid_input_init() == ESP_OK);
    assert(jetkvm_hid_input_set_active_generation(1U) == ESP_OK);
    for (unsigned elapsed = 0; elapsed < 100U &&
                               fake_usb_release_all_count() == 0U;
         ++elapsed) {
        sleep_ms(1U);
    }
    assert(fake_usb_release_all_count() >= 1U);

    test_unreliable_movement_buttons();
    uint64_t revision = snapshot().macro_state_revision;
    test_completion(&revision);
    test_cancel_during_delay(&revision);
    test_replacement_has_zero_barrier(&revision);
    test_normal_key_up_not_delayed();
    test_keyboard_revision_counts_changes();
    test_write_recovery_counts_timeouts_of_the_host_only();
    test_generation_change_cancels_and_releases();
    test_fixed_limit_rejection();
    test_detached_macro_fails_without_false_completion(&revision);
    test_usb_state_and_wake_adapter();
    test_usb_pause_preserves_latest_generation();
    test_local_mouse_without_live_generation();

    assert(jetkvm_hid_input_deinit() == ESP_OK);
    puts("JetKVM HID macro bridge: OK");
    return 0;
}
