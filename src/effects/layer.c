/**
 * Layer Effect
 *
 * Visual Description:
 * Highlights specific keys based on the currently active ZMK layer.
 * Allows the user to easily identify which functional layer is active (e.g. Navigation, Numpad, Symbols)
 * by applying custom colors to key groupings, while the rest of the board remains dim or off.
 */
#include "../pk_underglow_internal.h"
#include <drivers/behavior.h>
#include <zmk/matrix.h>
#include <zmk/pk_underglow_layer.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/sys/util.h>
#include <zmk/keymap.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk_pk_underglow, CONFIG_ZMK_PK_UNDERGLOW_LOG_LEVEL);

static struct led_rgb hex_to_rgb(uint8_t r, uint8_t g, uint8_t b) {
    struct zmk_led_hsb hsb = state.colors[active_profile_index];
    return (struct led_rgb){.r = (hsb.b * r) / 0xff, .g = (hsb.b * g) / 0xff, .b = (hsb.b * b) / 0xff};
}


int zmk_pk_underglow_apply_rgbmap(const struct zmk_behavior_binding *bindings, size_t rgbmap_len, uint8_t layer) {
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
            struct zmk_behavior_binding_event event = {.position = midx, .layer = layer, .timestamp = uptime};

            int color = api->binding_pressed((struct zmk_behavior_binding *)&bindings[midx], event);

            if (color > 0) {
                pixels[i] = hex_to_rgb((color & 0xFF0000) >> 16, (color & 0xFF00) >> 8, color & 0xFF);
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
        pixels[i].r -= state.effect_speeds[state.current_effects[active_profile_index]] < pixels[i].r
                           ? state.effect_speeds[state.current_effects[active_profile_index]]
                           : pixels[i].r;
        pixels[i].g -= state.effect_speeds[state.current_effects[active_profile_index]] < pixels[i].g
                           ? state.effect_speeds[state.current_effects[active_profile_index]]
                           : pixels[i].g;
        pixels[i].b -= state.effect_speeds[state.current_effects[active_profile_index]] < pixels[i].b
                           ? state.effect_speeds[state.current_effects[active_profile_index]]
                           : pixels[i].b;
        if (pixels[i].r || pixels[i].g || pixels[i].b) {
            active = true;
        }
    }
    state.animation_step += state.effect_speeds[state.current_effects[active_profile_index]];

    if (state.animation_step > 255 || !active) {
        zmk_pk_underglow_transient_off();
    }
}


#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
uint8_t pk_underglow_peripheral_synced_layer = 0;
#endif

#define DT_DRV_COMPAT zmk_pk_underglow_layer
#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define LAYER_ID(node) DT_PROP(node, layer_id)
#define FADE_DELAY(node) DT_PROP(node, fade_delay)

#define TRANSFORMED_RGB_LAYER(node)                                                                                    \
    {                                                                                                                  \
        COND_CODE_1(DT_NODE_HAS_PROP(node, bindings),                                                                  \
                    (LISTIFY(DT_PROP_LEN(node, bindings), ZMK_RGBMAP_EXTRACT_BINDING, (, ), node)), ())                \
    }

#define RGBMAP_VAR(_name, _opts)                                                                                       \
    static _opts struct zmk_behavior_binding _name[ZMK_RGBMAP_LAYERS_LEN][ZMK_KEYMAP_LEN] = {                          \
        DT_INST_FOREACH_CHILD_STATUS_OKAY_SEP(0, TRANSFORMED_RGB_LAYER, (, ))};

RGBMAP_VAR(zmk_rgbmap, COND_CODE_1(IS_ENABLED(CONFIG_ZMK_KEYMAP_SETTINGS_STORAGE), (), (const)))

const int pixel_lookup_table[] = DT_INST_PROP(0, pixel_lookup);

#define LAYER_IDS_PTR(node) (const int[]) DT_PROP(node, layer_id)
#define LAYER_IDS_LEN_MACRO(node) DT_PROP_LEN(node, layer_id)
#define LAYER_ANIMATED(node) DT_PROP(node, animated)

static const int *zmk_rgbmap_ids[ZMK_RGBMAP_LAYERS_LEN] = {DT_INST_FOREACH_CHILD_SEP(0, LAYER_IDS_PTR, (, ))};
static const size_t zmk_rgbmap_ids_lens[ZMK_RGBMAP_LAYERS_LEN] = {
    DT_INST_FOREACH_CHILD_SEP(0, LAYER_IDS_LEN_MACRO, (, ))};
static int zmk_rgbmap_fds[ZMK_RGBMAP_LAYERS_LEN] = {DT_INST_FOREACH_CHILD_SEP(0, FADE_DELAY, (, ))};
static bool zmk_rgbmap_anis[ZMK_RGBMAP_LAYERS_LEN] = {DT_INST_FOREACH_CHILD_SEP(0, LAYER_ANIMATED, (, ))};

int rgb_pixel_lookup(int idx) { return pixel_lookup_table[idx]; };

int zmk_rgbmap_id(uint8_t layer) {
    for (uint8_t i = 0; i < ZMK_RGBMAP_LAYERS_LEN; i++) {
        for (size_t j = 0; j < zmk_rgbmap_ids_lens[i]; j++) {
            if (zmk_rgbmap_ids[i][j] == layer) {
                return i;
            }
        }
    }
    return -1;
}


int zmk_rgbmap_fade_delay(uint8_t layer) { return zmk_rgbmap_fds[zmk_rgbmap_id(layer)]; }

bool zmk_rgbmap_is_animated(uint8_t layer) { return zmk_rgbmap_anis[zmk_rgbmap_id(layer)]; }

const struct zmk_behavior_binding *pk_underglow_get_bindings(uint8_t layer) {
    int rgblayer = zmk_rgbmap_id(layer);
    if (rgblayer == -1) {
        return NULL;
    } else {
        return zmk_rgbmap[rgblayer];
    }
}


uint8_t pk_underglow_top_layer(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    return zmk_keymap_highest_layer_active();
#else
    return pk_underglow_peripheral_synced_layer;
#endif
}

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
void zmk_pk_underglow_set_peripheral_layer(uint8_t layer) {
    pk_underglow_peripheral_synced_layer = layer;
    zmk_pk_underglow_set_layer(layer);
}
#endif

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */