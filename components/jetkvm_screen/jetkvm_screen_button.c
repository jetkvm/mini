#include "jetkvm_screen_button.h"

void jetkvm_screen_button_init(jetkvm_screen_button_t *button, bool pressed, uint32_t now_ms)
{
    button->raw_pressed = pressed;
    button->stable_pressed = pressed;
    button->samples = JETKVM_SCREEN_BUTTON_DEBOUNCE_SAMPLES;
    button->sample_ms = now_ms;
    button->held_since_ms = now_ms;
}

jetkvm_screen_button_edge_t jetkvm_screen_button_sample(jetkvm_screen_button_t *button,
                                                        bool pressed, bool changed,
                                                        uint32_t now_ms)
{
    /* A released sample, a level change between samples or a sampling gap
     * restarts the hold at this sample. */
    if (!pressed || changed || !button->raw_pressed ||
        now_ms - button->sample_ms > JETKVM_SCREEN_BUTTON_MAX_GAP_MS) {
        button->held_since_ms = now_ms;
    }
    button->sample_ms = now_ms;
    if (pressed != button->raw_pressed) {
        button->raw_pressed = pressed;
        button->samples = 1;
    } else if (button->samples < JETKVM_SCREEN_BUTTON_DEBOUNCE_SAMPLES) {
        button->samples++;
    }
    if (button->samples < JETKVM_SCREEN_BUTTON_DEBOUNCE_SAMPLES ||
        button->stable_pressed == button->raw_pressed) {
        return JETKVM_SCREEN_BUTTON_NONE;
    }
    button->stable_pressed = button->raw_pressed;
    return button->stable_pressed ? JETKVM_SCREEN_BUTTON_PRESSED : JETKVM_SCREEN_BUTTON_RELEASED;
}

uint32_t jetkvm_screen_button_held_ms(const jetkvm_screen_button_t *button)
{
    return button->stable_pressed && button->raw_pressed
               ? button->sample_ms - button->held_since_ms
               : 0U;
}
