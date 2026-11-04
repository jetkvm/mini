#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "jetkvm_screen_button.h"

/* The screen loop samples a held button every 10 ms. */
#define TICK_MS 10U

static jetkvm_screen_button_t released_button(void)
{
    jetkvm_screen_button_t button;
    jetkvm_screen_button_init(&button, false, 0);
    return button;
}

/* Samples `pressed` every TICK_MS from `from_ms` up to and including `to_ms`
 * and returns the longest hold seen. */
static uint32_t sample_run(jetkvm_screen_button_t *button, bool pressed, uint32_t from_ms,
                           uint32_t to_ms)
{
    uint32_t longest = 0;
    for (uint32_t now = from_ms; now <= to_ms; now += TICK_MS) {
        (void)jetkvm_screen_button_sample(button, pressed, false, now);
        uint32_t held = jetkvm_screen_button_held_ms(button);
        longest = held > longest ? held : longest;
    }
    return longest;
}

static void test_debounce_needs_two_samples(void)
{
    jetkvm_screen_button_t button = released_button();
    assert(jetkvm_screen_button_sample(&button, true, false, 10) == JETKVM_SCREEN_BUTTON_NONE);
    assert(jetkvm_screen_button_sample(&button, false, false, 20) == JETKVM_SCREEN_BUTTON_NONE);
    assert(jetkvm_screen_button_sample(&button, true, false, 30) == JETKVM_SCREEN_BUTTON_NONE);
    assert(jetkvm_screen_button_sample(&button, true, false, 40) == JETKVM_SCREEN_BUTTON_PRESSED);
    assert(jetkvm_screen_button_sample(&button, false, false, 50) == JETKVM_SCREEN_BUTTON_NONE);
    assert(jetkvm_screen_button_sample(&button, false, false, 60) == JETKVM_SCREEN_BUTTON_RELEASED);
    assert(jetkvm_screen_button_held_ms(&button) == 0U);
}

static void test_steady_hold_reaches_the_accept_hold(void)
{
    jetkvm_screen_button_t button = released_button();
    assert(sample_run(&button, true, 10, 700) < JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS);
    assert(sample_run(&button, true, 710, 720) >= JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS);
}

/* A short low passes the debounce, the screen task does not run for 4 s, and
 * its next sample reads the pin released. */
static void test_stall_after_a_glitch_is_not_a_hold(void)
{
    jetkvm_screen_button_t button = released_button();
    assert(jetkvm_screen_button_sample(&button, true, false, 10) == JETKVM_SCREEN_BUTTON_NONE);
    assert(jetkvm_screen_button_sample(&button, true, false, 20) == JETKVM_SCREEN_BUTTON_PRESSED);
    (void)jetkvm_screen_button_sample(&button, false, false, 4020);
    assert(jetkvm_screen_button_held_ms(&button) < JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS);
}

/* Time the task did not sample does not count, also when the pin reads low
 * on both sides of the stall. */
static void test_stall_inside_a_hold_restarts_the_hold(void)
{
    jetkvm_screen_button_t button = released_button();
    (void)sample_run(&button, true, 10, 300);
    assert(sample_run(&button, true, 4300, 4300 + JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS - TICK_MS) <
           JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS);
    assert(sample_run(&button, true, 4300 + JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS,
                      4300 + JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS) >=
           JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS);
}

/* Lows with single high samples between them stay one debounced press but
 * are not one hold. */
static void test_high_sample_restarts_the_hold(void)
{
    jetkvm_screen_button_t button = released_button();
    uint32_t longest = 0;
    for (uint32_t now = 10; now <= 2000; now += 200) {
        uint32_t held = sample_run(&button, true, now, now + 180);
        longest = held > longest ? held : longest;
        (void)jetkvm_screen_button_sample(&button, false, false, now + 190);
        assert(button.stable_pressed);
    }
    assert(longest < JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS);
}

/* A pin can toggle thousands of times a second while it reads low at every
 * sample. */
static void test_level_changes_between_samples_restart_the_hold(void)
{
    jetkvm_screen_button_t button = released_button();
    for (uint32_t now = 10; now <= 2000; now += TICK_MS) {
        (void)jetkvm_screen_button_sample(&button, true, true, now);
        assert(jetkvm_screen_button_held_ms(&button) == 0U);
    }
    assert(button.stable_pressed);
    /* The pin settles: a hold counts from the sample that saw the last change. */
    assert(sample_run(&button, true, 2010, 2000 + JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS - TICK_MS) <
           JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS);
    assert(sample_run(&button, true, 2000 + JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS,
                      2000 + JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS) >=
           JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS);
}

static void test_gaps_up_to_the_limit_count(void)
{
    jetkvm_screen_button_t button = released_button();
    uint32_t now = 10;
    while (jetkvm_screen_button_held_ms(&button) < JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS &&
           now < 2000) {
        (void)jetkvm_screen_button_sample(&button, true, false, now);
        now += JETKVM_SCREEN_BUTTON_MAX_GAP_MS;
    }
    assert(jetkvm_screen_button_held_ms(&button) >= JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS);
}

int main(void)
{
    test_debounce_needs_two_samples();
    test_steady_hold_reaches_the_accept_hold();
    test_stall_after_a_glitch_is_not_a_hold();
    test_stall_inside_a_hold_restarts_the_hold();
    test_high_sample_restarts_the_hold();
    test_level_changes_between_samples_restart_the_hold();
    test_gaps_up_to_the_limit_count();
    puts("screen button tests passed");
    return 0;
}
