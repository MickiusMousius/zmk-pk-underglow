/**
 * White Effect
 *
 * Visual Description:
 * A specialized static effect that sets all LEDs to maximum brightness white.
 * Bypasses hue and saturation processing for pure, bright illumination.
 */
#include "../pk_underglow_internal.h"

void zmk_pk_underglow_effect_white(void) {
    struct zmk_led_hsb hsb = state.colors[active_profile_index];
    hsb.s = WHITE_SATURATION;
    struct led_rgb rgb = hsb_to_rgb(hsb_scale_min_max(hsb));

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }
}
