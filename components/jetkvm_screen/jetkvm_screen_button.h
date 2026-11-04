#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Debounce and hold timing for one push button. It takes level samples with
 * their time and has no GPIO or LVGL dependency, so host tests drive it.
 */

#define JETKVM_SCREEN_BUTTON_DEBOUNCE_SAMPLES 2U
/* A hold this long accepts the highlighted entry. */
#define JETKVM_SCREEN_BUTTON_ACCEPT_HOLD_MS 700U
/* Reset Config needs the hold of the JetKVM reset screen. */
#define JETKVM_SCREEN_BUTTON_RESET_HOLD_MS 10000U
/* The screen loop samples a pressed button every 10 ms. A longer gap means
 * the task did not run, and the pin was not watched in that time. */
#define JETKVM_SCREEN_BUTTON_MAX_GAP_MS 50U

typedef enum {
    JETKVM_SCREEN_BUTTON_NONE,
    JETKVM_SCREEN_BUTTON_PRESSED,
    JETKVM_SCREEN_BUTTON_RELEASED,
} jetkvm_screen_button_edge_t;

typedef struct {
    bool raw_pressed;
    bool stable_pressed;
    uint8_t samples;
    uint32_t sample_ms;
    uint32_t held_since_ms;
} jetkvm_screen_button_t;

void jetkvm_screen_button_init(jetkvm_screen_button_t *button, bool pressed, uint32_t now_ms);

/* Takes the level sampled at `now_ms` and returns the debounced edge, if any.
 * `changed` says the pin changed level after the previous sample. */
jetkvm_screen_button_edge_t jetkvm_screen_button_sample(jetkvm_screen_button_t *button,
                                                        bool pressed, bool changed,
                                                        uint32_t now_ms);

/* How long the button has read pressed on every sample up to the last one,
 * with no level change between them and no gap longer than
 * JETKVM_SCREEN_BUTTON_MAX_GAP_MS; 0 when released. */
uint32_t jetkvm_screen_button_held_ms(const jetkvm_screen_button_t *button);

#ifdef __cplusplus
}
#endif
