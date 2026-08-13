#pragma once

#include <zmk/endpoints.h>
#include <zmk/behavior.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zmk/pk_underglow.h>

#if !DT_HAS_CHOSEN(zmk_underglow)
#error "A zmk,underglow chosen node must be declared"
#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_pk_underglow_layer) && IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_LAYER)
#define UNDERGLOW_LAYER_ENABLED 1
void zmk_pk_underglow_set_layer(uint8_t layer, bool wakeup);
int zmk_pk_underglow_transient_off(void);
struct zmk_behavior_binding;
int zmk_pk_underglow_apply_rgbmap(const struct zmk_behavior_binding *bindings, size_t rgbmap_len, uint8_t layer);
#endif

#if CONFIG_ZMK_PK_UNDERGLOW_WHITE_SATURATION != -1
    #define WHITE_SATURATION CONFIG_ZMK_PK_UNDERGLOW_WHITE_SATURATION
#elif DT_HAS_COMPAT_STATUS_OKAY(zmk_pk_underglow_layer) && DT_NODE_HAS_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), white_saturation)
    #define WHITE_SATURATION DT_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), white_saturation)
#else
    #define WHITE_SATURATION 0
#endif

#ifndef CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS
#define CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS 5
#endif

enum pk_underglow_effect {
    UNDERGLOW_EFFECT_WHITE,
    UNDERGLOW_EFFECT_SOLID,
    UNDERGLOW_EFFECT_BREATHE,
    UNDERGLOW_EFFECT_SPECTRUM,
    UNDERGLOW_EFFECT_SWIRL,
    UNDERGLOW_EFFECT_PINWHEEL,
    UNDERGLOW_EFFECT_RIPPLE,
    UNDERGLOW_EFFECT_RAINBOW_RIPPLE,
    UNDERGLOW_EFFECT_TWINKLE,
    UNDERGLOW_EFFECT_RAINBOW_TWINKLE,
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    UNDERGLOW_EFFECT_LAYER_INDICATORS,
#endif
    UNDERGLOW_EFFECT_NUMBER // Used to track number of underglow effects
};

struct pk_underglow_state {
    struct zmk_led_hsb colors[ZMK_ENDPOINT_COUNT];
    uint8_t current_effects[ZMK_ENDPOINT_COUNT];
    uint8_t effect_speeds[UNDERGLOW_EFFECT_NUMBER];
    uint16_t animation_step;
    bool on;
    bool layer_enabled;
};

// Global State
extern struct pk_underglow_state state;
extern uint8_t active_profile_index;
extern struct led_rgb pixels[STRIP_NUM_PIXELS];
extern uint16_t global_rainbow_hue;
extern uint16_t pixel_base_hues[STRIP_NUM_PIXELS];
extern int center_row;
extern int center_col;

// Utility functions
struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb);
struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb);
struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb);
int rgb_pixel_lookup(int idx);

// Effect functions
void zmk_pk_underglow_effect_solid(void);
void zmk_pk_underglow_effect_white(void);
void zmk_pk_underglow_effect_breathe(void);
void zmk_pk_underglow_effect_spectrum(void);
void zmk_pk_underglow_effect_swirl(void);

void zmk_pk_underglow_effect_ripple_trigger(uint8_t row, uint8_t col);
void zmk_pk_underglow_effect_twinkle_reset(void);
void zmk_pk_underglow_effect_ripple(void);
void zmk_pk_underglow_effect_rainbow_ripple(void);
void zmk_pk_underglow_effect_twinkle(void);
void zmk_pk_underglow_effect_rainbow_twinkle(void);
void zmk_pk_underglow_effect_pinwheel(void);
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
void zmk_pk_underglow_effect_layer(void);
#endif
