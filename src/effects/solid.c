/**
 * Solid Effect
 *
 * Visual Description:
 * A static, uniform color applied across the entire keyboard.
 * Provides a clean and consistent backlight without any animation.
 */
#include "../pk_underglow_internal.h"

void zmk_pk_underglow_effect_solid(void) {
    struct led_rgb rgb = hsb_to_rgb(hsb_scale_min_max(state.colors[active_profile_index]));
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }
}
