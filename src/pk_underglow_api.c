/**
 * @file pk_underglow_api.c
 * @brief Public API and state mutators for the ZMK pk_underglow module.
 *
 * This file is responsible for housing the high-level API routines that are
 * invoked by ZMK keymap behaviors. It includes functions to:
 * - Toggle power on and off.
 * - Change the active effect, hue, saturation, and brightness.
 * - Manage transient layer indicator effects.
 * - Defer operations that require state saves to the background.
 *
 * Persistent State (NVS Flash Writes):
 * To prevent degrading the flash memory with excessive writes during rapid visual 
 * adjustments (e.g. holding down a hue cycle button), this API schedules a delayed 
 * Zephyr work item. This delay explicitly respects the global ZMK settings debounce 
 * configuration (CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE). Each repeated call reschedules 
 * the timer (debounces). Only after the timer expires is a SAVE_SETTINGS task pushed 
 * to the background queue to execute the actual flash write.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/pk_underglow.h>
#include <zmk/pk_underglow_layer.h>
#include <zmk/pk_underglow_queue.h>

#include "pk_underglow_internal.h"

LOG_MODULE_DECLARE(zmk_pk_underglow, CONFIG_ZMK_PK_UNDERGLOW_LOG_LEVEL);

/* ==========================================================================
 * PUBLIC API & STATE MUTATORS
 * ========================================================================== */

int zmk_pk_underglow_save_state(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    pk_ug_queue_push_sync(pk_underglow_top_layer());
#endif
#if IS_ENABLED(CONFIG_SETTINGS) && (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
    int ret = k_work_reschedule(&underglow_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}


int zmk_pk_underglow_get_state(bool *on_off) {
    if (!led_strip)
        return -ENODEV;

    *on_off = runtime_state.on;
    return 0;
}


int zmk_pk_underglow_on(void) {
    // Catch silent endpoint desyncs that happen during a cold boot/deep sleep wake
    pk_underglow_check_active_profile();

    runtime_state.on = true;
    
    // CRITICAL: We must push POWER_ON before calling zmk_pk_underglow_set_layer().
    // If set_layer() successfully applies a layer map, it will push RENDER_FRAME.
    // If we pushed POWER_ON *after* set_layer(), the background queue's deduplication 
    // logic would delete the initial POWER_ON (pushed by transient_on during set_layer)
    // and place our new POWER_ON at the end of the queue. This would cause RENDER_FRAME 
    // to execute before the LEDs actually have physical power, resulting in dead LEDs.
    pk_ug_queue_push_power(PK_UG_TASK_POWER_ON);

    if (pk_underglow_effects[state.current_effects[active_profile_index]].is_layer_indicator) {
        runtime_state.layer_enabled = true;
        zmk_pk_underglow_set_layer(pk_underglow_top_layer());
    } else {
        state.animation_step = 0;
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(PK_UG_FRAME_DURATION));
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    pk_ug_queue_push_sync(pk_underglow_top_layer());
#endif
    return 0;
}


int zmk_pk_underglow_transient_on(void) {
    if (!led_strip)
        return -ENODEV;

    state.animation_step = 0;

    pk_ug_queue_push_power(PK_UG_TASK_POWER_ON);
    k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(PK_UG_FRAME_DURATION));

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    pk_ug_queue_push_sync(pk_underglow_top_layer());
#endif

    return 0;
}


int zmk_pk_underglow_off(void) {
    runtime_state.on = false;
    runtime_state.layer_enabled = false;

    pk_ug_queue_push_power(PK_UG_TASK_POWER_OFF);
    k_timer_stop(&underglow_tick);
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    pk_ug_queue_push_sync(pk_underglow_top_layer());
#endif
    return 0;
}


int zmk_pk_underglow_transient_off(void) {
    if (!led_strip)
        return -ENODEV;

    if (!zmk_pk_underglow_is_on())
        return 0;

    pk_ug_queue_push_power(PK_UG_TASK_POWER_OFF);
    k_timer_stop(&underglow_tick);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    pk_ug_queue_push_sync(pk_underglow_top_layer());
#endif

    return 0;
}


int zmk_pk_underglow_calc_effect(int direction) {
    return (state.current_effects[active_profile_index] + pk_underglow_effects_count + direction) %
           pk_underglow_effects_count;
}


int zmk_pk_underglow_select_effect(int effect) {
    if (!led_strip)
        return -ENODEV;

    if (effect < 0 || effect >= pk_underglow_effects_count) {
        return -EINVAL;
    }

    state.current_effects[active_profile_index] = effect;
    state.animation_step = 0;

    if (pk_underglow_effects[effect].select) {
        pk_underglow_effects[effect].select();
    }

    runtime_state.layer_enabled = pk_underglow_effects[effect].is_layer_indicator;

    LOG_INF("Selected effect: %d, layer_enabled: %d, state.on: %d", effect, runtime_state.layer_enabled,
            runtime_state.on);

    if (runtime_state.layer_enabled) {
        zmk_pk_underglow_set_layer(pk_underglow_top_layer());
    } else if (runtime_state.on) {
        LOG_INF("Restarting animation timer for effect %d", effect);
        zmk_pk_underglow_transient_on();
    }
    return zmk_pk_underglow_save_state();
}


int zmk_pk_underglow_cycle_effect(int direction) {
    return zmk_pk_underglow_select_effect(zmk_pk_underglow_calc_effect(direction));
}


int zmk_pk_underglow_toggle(void) { return runtime_state.on ? zmk_pk_underglow_off() : zmk_pk_underglow_on(); }

void zmk_pk_underglow_set_layer(uint8_t layer) {
    LOG_INF("Setting pk underglow layer: %d. layer_enabled: %d, state.on: %d", layer, runtime_state.layer_enabled,
            runtime_state.on);
    if (!runtime_state.layer_enabled || !runtime_state.on)
        return;

    const struct zmk_behavior_binding *rgbmap = pk_underglow_get_bindings(layer);
    if (rgbmap != NULL && zmk_pk_underglow_apply_rgbmap(rgbmap, ZMK_KEYMAP_LEN, layer)) {
        if (!zmk_pk_underglow_is_on()) {
            zmk_pk_underglow_transient_on();
        }

        k_timer_stop(&underglow_tick);
        state.animation_step = 0;
        int fade_delay = zmk_rgbmap_fade_delay(layer);
        bool animated = zmk_rgbmap_is_animated(layer);
        if (fade_delay > 0) {
            k_timer_start(&underglow_tick, K_SECONDS(fade_delay), K_MSEC(PK_UG_FRAME_DURATION));
        } else if (animated || fade_delay == 0) {
            k_timer_start(&underglow_tick, K_MSEC(PK_UG_FRAME_DURATION), K_MSEC(PK_UG_FRAME_DURATION));
        }

        pk_ug_queue_push(PK_UG_TASK_RENDER_FRAME);
    } else {
        zmk_pk_underglow_transient_off();
    }
}


int zmk_pk_underglow_set_hsb(struct zmk_led_hsb color) {
    if (color.h > HUE_MAX || color.s > SAT_MAX || color.b > BRT_MAX) {
        return -ENOTSUP;
    }

    state.colors[active_profile_index] = color;

    if (runtime_state.layer_enabled) {
        zmk_pk_underglow_set_layer(pk_underglow_top_layer());
    }

    return zmk_pk_underglow_save_state();
}


struct zmk_led_hsb zmk_pk_underglow_calc_hue(int direction) {
    struct zmk_led_hsb color = state.colors[active_profile_index];

    color.h += HUE_MAX + (direction * CONFIG_ZMK_PK_UNDERGLOW_HUE_STEP);
    color.h %= HUE_MAX;

    return color;
}


struct zmk_led_hsb zmk_pk_underglow_calc_sat(int direction) {
    struct zmk_led_hsb color = state.colors[active_profile_index];

    int s = color.s + (direction * CONFIG_ZMK_PK_UNDERGLOW_SAT_STEP);
    color.s = CLAMP(s, 0, SAT_MAX);

    return color;
}


struct zmk_led_hsb zmk_pk_underglow_calc_brt(int direction) {
    struct zmk_led_hsb color = state.colors[active_profile_index];

    int b = color.b + (direction * CONFIG_ZMK_PK_UNDERGLOW_BRT_STEP);
    color.b = CLAMP(b, 0, BRT_MAX);

    return color;
}


int zmk_pk_underglow_change_hue(int direction) {
    if (!led_strip)
        return -ENODEV;
    return zmk_pk_underglow_set_hsb(zmk_pk_underglow_calc_hue(direction));
}


int zmk_pk_underglow_change_sat(int direction) {
    if (!led_strip)
        return -ENODEV;
    return zmk_pk_underglow_set_hsb(zmk_pk_underglow_calc_sat(direction));
}


int zmk_pk_underglow_change_brt(int direction) {
    if (!led_strip)
        return -ENODEV;
    return zmk_pk_underglow_set_hsb(zmk_pk_underglow_calc_brt(direction));
}


int zmk_pk_underglow_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;

    int speed = state.effect_speeds[state.current_effects[active_profile_index]] + direction;
    state.effect_speeds[state.current_effects[active_profile_index]] = CLAMP(speed, 1, 5);

    return zmk_pk_underglow_save_state();
}


struct zmk_led_hsb zmk_pk_underglow_get_color(void) { return state.colors[active_profile_index]; }

const char *zmk_pk_underglow_get_effect_name(void) {
    return pk_underglow_effects[state.current_effects[active_profile_index]].name;
}


uint8_t zmk_pk_underglow_get_speed(void) { return state.effect_speeds[state.current_effects[active_profile_index]]; }

int zmk_pk_underglow_hsb_to_hex(struct zmk_led_hsb hsb) {
    struct led_rgb rgb = hsb_to_rgb(hsb);
    return (rgb.r << 16) | (rgb.g << 8) | rgb.b;
}
