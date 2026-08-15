/**
 * @file pk_underglow_internal.h
 * @brief Internal headers and global variable sharing for the ZMK pk_underglow module.
 *
 * This file is responsible for exposing internal state, timer loops, and Zephyr queue objects
 * across the different C files that make up the pk_underglow subsystem.
 * It prevents circular dependencies and encapsulates internal functions that should not be
 * directly exposed to the broader ZMK application layer.
 */

#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zmk/behavior.h>
#include <zmk/endpoints.h>
#include <zmk/pk_underglow.h>

#if !DT_HAS_CHOSEN(zmk_underglow)
#error "A zmk,underglow chosen node must be declared"
#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

void zmk_pk_underglow_set_layer(uint8_t layer);
int zmk_pk_underglow_save_state(void);
struct zmk_behavior_binding;
int zmk_pk_underglow_apply_rgbmap(const struct zmk_behavior_binding *bindings, size_t rgbmap_len, uint8_t layer);

#if CONFIG_ZMK_PK_UNDERGLOW_WHITE_SATURATION != -1
#define WHITE_SATURATION CONFIG_ZMK_PK_UNDERGLOW_WHITE_SATURATION
#elif DT_HAS_COMPAT_STATUS_OKAY(zmk_pk_underglow_layer) &&                                                             \
    DT_NODE_HAS_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), white_saturation)
#define WHITE_SATURATION DT_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), white_saturation)
#else
#define WHITE_SATURATION 0
#endif

#ifndef CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS
#define CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS 5
#endif

#define PK_UG_MATRIX_COLS DT_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), columns)
#define PK_UG_MATRIX_ROWS DT_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), rows)

#define PK_UG_POWER_STABILIZATION_MS                                                                                   \
    DT_PROP_OR(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), power_stabilization_ms, 20)
#define PK_UG_SYNC_DELAY_MS DT_PROP_OR(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), sync_delay_ms, 500)
#define PK_UG_SYNC_RETRY_MS DT_PROP_OR(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), sync_retry_ms, 1000)


#if defined(CONFIG_ZMK_PK_UNDERGLOW_FPS) && CONFIG_ZMK_PK_UNDERGLOW_FPS > 0
#define PK_UG_FPS CONFIG_ZMK_PK_UNDERGLOW_FPS
#else
#define PK_UG_FPS DT_PROP_OR(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), fps, 20)
#endif

#define PK_UG_FRAME_DURATION (1000 / PK_UG_FPS)

#define MAX_UNDERGLOW_EFFECTS 32

typedef void (*pk_underglow_render_t)(void);
typedef void (*pk_underglow_select_t)(void);
typedef void (*pk_underglow_pos_changed_t)(uint8_t row, uint8_t col);

struct pk_underglow_effect_ops {
    const char *name;
    pk_underglow_render_t render;
    pk_underglow_select_t select;
    pk_underglow_pos_changed_t pos_changed;
    bool is_layer_indicator;
};


extern const struct pk_underglow_effect_ops pk_underglow_effects[];
extern const int pk_underglow_effects_count;

struct pk_underglow_state {
    struct zmk_led_hsb colors[ZMK_ENDPOINT_COUNT];
    uint8_t current_effects[ZMK_ENDPOINT_COUNT];
    uint8_t effect_speeds[MAX_UNDERGLOW_EFFECTS];
    uint16_t animation_step;
};


struct pk_underglow_runtime_state {
    bool on;
    bool layer_enabled;
};


// Global State
extern struct pk_underglow_state state;
extern struct pk_underglow_runtime_state runtime_state;
extern uint8_t active_profile_index;
extern struct led_rgb pixels[STRIP_NUM_PIXELS];
extern uint16_t global_rainbow_hue;
extern uint16_t pixel_base_hues[STRIP_NUM_PIXELS];
extern int center_row;
extern int center_col;
extern const struct device *led_strip;
extern struct k_timer underglow_tick;

#if IS_ENABLED(CONFIG_SETTINGS) && (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
extern struct k_work_delayable underglow_save_work;
#endif

// Utility functions
struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb);
struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb);
struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb);
int rgb_pixel_lookup(int idx);

// Effect Specific API
void zmk_pk_underglow_effect_ripple_trigger(uint8_t row, uint8_t col);
void zmk_pk_underglow_effect_rainbow_ripple_trigger(uint8_t row, uint8_t col);
void zmk_pk_underglow_effect_fireworks(void);
