/**
 * Spectrum Effect
 * 
 * Visual Description:
 * A smooth transition through the entire color spectrum. 
 * The entire keyboard changes color simultaneously, slowly shifting from red to green to blue and back again.
 */
#include "../pk_underglow_internal.h"

void zmk_pk_underglow_effect_spectrum(void) {
    struct zmk_led_hsb hsb = state.colors[active_profile_index];
    hsb.h = state.animation_step;
    struct led_rgb rgb = hsb_to_rgb(hsb_scale_min_max(hsb));

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }

    state.animation_step += state.effect_speeds[state.current_effects[active_profile_index]];
    state.animation_step = state.animation_step % HUE_MAX;
}
