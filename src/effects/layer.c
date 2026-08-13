/**
 * Layer Effect
 * 
 * Visual Description:
 * Highlights specific keys based on the currently active ZMK layer.
 * Allows the user to easily identify which functional layer is active (e.g. Navigation, Numpad, Symbols)
 * by applying custom colors to key groupings, while the rest of the board remains dim or off.
 */
#include "../pk_underglow_internal.h"
#include <zmk/pk_underglow_layer.h>
#include <zmk/matrix.h>
#include <drivers/behavior.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_pk_underglow, CONFIG_ZMK_PK_UNDERGLOW_LOG_LEVEL);


static struct led_rgb hex_to_rgb(uint8_t r, uint8_t g, uint8_t b) {
    struct zmk_led_hsb hsb = state.colors[active_profile_index];
    return (struct led_rgb){
        .r = (hsb.b * r) / 0xff,
        .g = (hsb.b * g) / 0xff,
        .b = (hsb.b * b) / 0xff
    };
}

int zmk_pk_underglow_apply_rgbmap(const struct zmk_behavior_binding *bindings,
                                          size_t rgbmap_len, uint8_t layer) {
    int rc = 0;
    uint64_t uptime = k_uptime_get();
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t midx = rgb_pixel_lookup(i);
        if (midx >= ZMK_KEYMAP_LEN) {
            LOG_DBG("out of range");
        } else {
            const struct device *dev = zmk_behavior_get_binding(bindings[midx].behavior_dev);

            if (dev == NULL) {
                continue;
            }

            const struct behavior_driver_api *api = (const struct behavior_driver_api *)dev->api;

            if (api->binding_pressed == NULL) {
                continue;
            }
            struct zmk_behavior_binding_event event = {
                .position = midx, .layer = layer, .timestamp = uptime};

            int color = api->binding_pressed((struct zmk_behavior_binding *)&bindings[midx], event);

            if (color > 0) {
                pixels[i] =
                    hex_to_rgb((color & 0xFF0000) >> 16, (color & 0xFF00) >> 8, color & 0xFF);
                rc = 1;
            } else {
                pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
            }
        }
    }
    return rc;
}

void zmk_pk_underglow_effect_layer(void) {
    uint8_t top_layer = pk_underglow_top_layer();
    if (zmk_rgbmap_is_animated(top_layer)) {
        const struct zmk_behavior_binding *rgbmap = pk_underglow_get_bindings(top_layer);
        if (rgbmap != NULL) {
            zmk_pk_underglow_apply_rgbmap(rgbmap, ZMK_KEYMAP_LEN, top_layer);
        }
        return;
    }

    bool active = false;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i].r -= state.effect_speeds[state.current_effects[active_profile_index]] < pixels[i].r ? state.effect_speeds[state.current_effects[active_profile_index]] : pixels[i].r;
        pixels[i].g -= state.effect_speeds[state.current_effects[active_profile_index]] < pixels[i].g ? state.effect_speeds[state.current_effects[active_profile_index]] : pixels[i].g;
        pixels[i].b -= state.effect_speeds[state.current_effects[active_profile_index]] < pixels[i].b ? state.effect_speeds[state.current_effects[active_profile_index]] : pixels[i].b;
        if (pixels[i].r || pixels[i].g || pixels[i].b) {
            active = true;
        }
    }
    state.animation_step += state.effect_speeds[state.current_effects[active_profile_index]];

    if (state.animation_step > 255 || !active) {
        zmk_pk_underglow_transient_off();
    }
}

