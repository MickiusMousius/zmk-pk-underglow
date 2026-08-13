#include "pk_underglow_internal.h"
#include <zephyr/kernel.h>
#include <zmk/pk_underglow.h>

int center_row = 0;
int center_col = 0;

struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
    hsb.b = CONFIG_ZMK_PK_UNDERGLOW_BRT_MIN +
            (CONFIG_ZMK_PK_UNDERGLOW_BRT_MAX - CONFIG_ZMK_PK_UNDERGLOW_BRT_MIN) * hsb.b / BRT_MAX;
    return hsb;
}

struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb) {
    hsb.b = hsb.b * CONFIG_ZMK_PK_UNDERGLOW_BRT_MAX / BRT_MAX;
    return hsb;
}

/**
 * Fast integer-math alternative to floating point HSV-to-RGB conversion.
 * Avoids any FPU division for maximum performance during LED frame rendering.
 * Scales saturation and brightness accurately using bit-shifting.
 */
struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    uint8_t r = 0, g = 0, b = 0;

    // Scale S and V to 0-255
    uint32_t s = (hsb.s * 255) / SAT_MAX;
    uint32_t v = (hsb.b * 255) / BRT_MAX;

    if (s == 0) {
        r = g = b = v;
    } else {
        uint32_t h = (hsb.h * 255) / HUE_MAX;
        uint32_t region = h / 43;
        uint32_t remainder = (h - (region * 43)) * 6;

        uint32_t p = (v * (255 - s)) >> 8;
        uint32_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
        uint32_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

        switch (region) {
            case 0: r = v; g = t; b = p; break;
            case 1: r = q; g = v; b = p; break;
            case 2: r = p; g = v; b = t; break;
            case 3: r = p; g = q; b = v; break;
            case 4: r = t; g = p; b = v; break;
            default: r = v; g = p; b = q; break;
        }
    }

    return (struct led_rgb){ .r = r, .g = g, .b = b };
}
