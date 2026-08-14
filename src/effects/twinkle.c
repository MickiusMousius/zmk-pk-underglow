/**
 * Twinkle Effect
 *
 * Visual Description:
 * Simulates a starry night sky. Individual LEDs randomly light up (twinkle) and then slowly fade back into a dimmer
 * background state. Creates a subtle, sparkling appearance across the keyboard. Can be configured to use a single hue
 * or a rainbow of colors.
 */
#include "../pk_underglow_internal.h"
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

struct pk_twinkle {
    uint8_t led_index;
    uint32_t start_time;
    uint16_t duration;
    uint16_t hue;
    bool active;
};

#ifndef CONFIG_ZMK_PK_UNDERGLOW_TWINKLE_MAX
#define CONFIG_ZMK_PK_UNDERGLOW_TWINKLE_MAX 5
#endif

static struct pk_twinkle twinkles[CONFIG_ZMK_PK_UNDERGLOW_TWINKLE_MAX];

/**
 * Calculates the sine wave mapping for the twinkle effect.
 * Assumes a max animation lifespan mapped to 32 steps.
 *
 * Generation Script:
 * ```python
 * import math
 * vals = [round((math.sin((i / 31.0) * math.pi) ** 2) * 100) for i in range(32)]
 * print(vals)
 * ```
 */
static const uint8_t twinkle_sin_lut[32] = {0,   1,  3,  6,  10, 15, 22, 31, 41, 53, 65, 77, 87, 94, 98, 100,
                                            100, 98, 94, 87, 77, 65, 53, 41, 31, 22, 15, 10, 6,  3,  1,  0};

static inline uint8_t pk_get_twinkle_sin(uint8_t step) {
    if (step > 31)
        step = 31;
    return twinkle_sin_lut[step];
}

void zmk_pk_underglow_effect_twinkle_reset(void) {
    for (int i = 0; i < CONFIG_ZMK_PK_UNDERGLOW_TWINKLE_MAX; i++) {
        twinkles[i].active = false;
    }
}

static void process_twinkles(bool is_rainbow) {
    uint32_t now = k_uptime_get_32();
    struct zmk_led_hsb base_hsb = state.colors[active_profile_index];

    if (is_rainbow) {
        global_rainbow_hue =
            (global_rainbow_hue + state.effect_speeds[state.current_effects[active_profile_index]]) % HUE_MAX;
        base_hsb.h = global_rainbow_hue;
    }

    base_hsb.b = (base_hsb.b * CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS) / 100;
    struct led_rgb base_rgb = hsb_to_rgb(hsb_scale_min_max(base_hsb));

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = base_rgb;
    }

    for (int i = 0; i < CONFIG_ZMK_PK_UNDERGLOW_TWINKLE_MAX; i++) {
        if (!twinkles[i].active) {
            if ((sys_rand32_get() % 100) < 5) {
                // Randomly start a new twinkle
                // But don't do it all at once; only start if random chance allows, to stagger them
                twinkles[i].led_index = sys_rand32_get() % STRIP_NUM_PIXELS;
                twinkles[i].start_time = now;
                // 2000ms / speed + random offset (doubled from original)
                twinkles[i].duration = (4000 / state.effect_speeds[state.current_effects[active_profile_index]]) +
                                       (sys_rand32_get() % 2000);
                twinkles[i].hue = is_rainbow ? global_rainbow_hue : state.colors[active_profile_index].h;
                twinkles[i].active = true;
            }
            continue;
        }

        uint32_t elapsed = now - twinkles[i].start_time;
        if (elapsed >= twinkles[i].duration) {
            twinkles[i].active = false;
            continue;
        }

        uint32_t lut_index = (elapsed * 32) / twinkles[i].duration;
        if (lut_index > 31)
            lut_index = 31;

        uint8_t added_b = pk_get_twinkle_sin(lut_index);
        uint8_t twinkle_b = CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS +
                            (added_b * (100 - CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS)) / 100;

        // If the added brightness is essentially 0, it means the star is at the ambient level.
        // It should assume the background color (which we painted in the loop above).
        if (added_b == 0 && is_rainbow) {
            continue;
        }

        // Scale to global brightness
        twinkle_b = (uint8_t)((state.colors[active_profile_index].b * twinkle_b) / 100);

        struct zmk_led_hsb pixel_hsb = state.colors[active_profile_index];
        pixel_hsb.h = twinkles[i].hue;
        pixel_hsb.b = twinkle_b;

        pixels[twinkles[i].led_index] = hsb_to_rgb(hsb_scale_min_max(pixel_hsb));
    }
}

void zmk_pk_underglow_effect_twinkle(void) { process_twinkles(false); }

void zmk_pk_underglow_effect_rainbow_twinkle(void) { process_twinkles(true); }

void zmk_pk_underglow_effect_twinkle_select(void) {
  zmk_pk_underglow_effect_twinkle_reset();
}

void zmk_pk_underglow_effect_rainbow_twinkle_select(void) {
  global_rainbow_hue = state.colors[active_profile_index].h;
  for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
    pixel_base_hues[i] = state.colors[active_profile_index].h;
  }
  zmk_pk_underglow_effect_twinkle_reset();
}
