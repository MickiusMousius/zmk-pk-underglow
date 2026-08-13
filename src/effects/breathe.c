/**
 * Breathe Effect
 * 
 * Visual Description:
 * A smooth, pulsing animation where the entire keyboard gracefully fades in and out.
 * The brightness oscillates between a minimum and maximum threshold to simulate a slow, rhythmic "breathing" motion.
 */
#include "../pk_underglow_internal.h"
#include <stdlib.h>

void zmk_pk_underglow_effect_breathe(void) {
    struct zmk_led_hsb hsb = state.colors[active_profile_index];
    hsb.b = abs(state.animation_step - 1200) / 12;
    struct led_rgb rgb = hsb_to_rgb(hsb_scale_zero_max(hsb));

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }

    state.animation_step += state.effect_speeds[state.current_effects[active_profile_index]] * 13;

    if (state.animation_step > 2400) {
        state.animation_step = 0;
    }
}
