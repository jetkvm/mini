#include "keyboard_rollover.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void expect_report(const jetkvm_keyboard_usage_state_t *state,
                          const uint8_t expected[6])
{
    uint8_t report[6];
    jetkvm_keyboard_usage_build_report(state, report);
    assert(memcmp(report, expected, sizeof(report)) == 0);
}

int main(void)
{
    jetkvm_keyboard_usage_state_t state;
    jetkvm_keyboard_usage_clear(&state);

    for (uint8_t usage = 4; usage <= 9; ++usage) {
        assert(jetkvm_keyboard_usage_update(&state, usage, true));
    }
    const uint8_t six_keys[6] = {4, 5, 6, 7, 8, 9};
    expect_report(&state, six_keys);

    assert(!jetkvm_keyboard_usage_update(&state, 7, true));
    assert(jetkvm_keyboard_usage_update(&state, 10, true));
    const uint8_t rollover[6] = {1, 1, 1, 1, 1, 1};
    expect_report(&state, rollover);

    assert(jetkvm_keyboard_usage_update(&state, 6, false));
    const uint8_t recovered[6] = {4, 5, 7, 8, 9, 10};
    expect_report(&state, recovered);
    assert(!jetkvm_keyboard_usage_update(&state, 6, false));

    const uint8_t direct[6] = {30, 4, 0, 0, 0, 0};
    jetkvm_keyboard_usage_from_report(&state, direct);
    const uint8_t normalized[6] = {4, 30, 0, 0, 0, 0};
    expect_report(&state, normalized);

    jetkvm_keyboard_usage_from_report(&state, rollover);
    const uint8_t empty[6] = {0, 0, 0, 0, 0, 0};
    expect_report(&state, empty);

    /* Modifier usages never consume one of the six ordinary-key slots. */
    jetkvm_keyboard_usage_from_report(&state, direct);
    assert(jetkvm_keyboard_usage_update(&state, 0xe1, true));
    expect_report(&state, normalized);

    return 0;
}
