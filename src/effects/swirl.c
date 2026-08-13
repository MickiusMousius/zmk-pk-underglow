/**
 * Swirl Effect
 *
 * Visual Description:
 * A rolling wave of colors that flows continuously across the keyboard.
 * Creates a dynamic gradient where the hue shifts based on the physical position of the LEDs, simulating a flowing
 * river of rainbow light.
 */
#include "../pk_underglow_internal.h"

void zmk_pk_underglow_effect_swirl(void) {
    struct zmk_led_hsb base_hsb = state.colors[active_profile_index];
    const uint16_t hue_step = HUE_MAX / STRIP_NUM_PIXELS;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = base_hsb;
        hsb.h = (hue_step * i + state.animation_step) % HUE_MAX;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.effect_speeds[state.current_effects[active_profile_index]] * 3;
    state.animation_step = state.animation_step % HUE_MAX;
}
