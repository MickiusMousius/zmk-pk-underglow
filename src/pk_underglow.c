/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/pm/device.h>

#include <math.h>
#include <stdlib.h>
#include <zephyr/random/random.h>

#include <zephyr/logging/log.h>

#include <zephyr/drivers/led_strip.h>
#include <zephyr/drivers/gpio.h>
#include <drivers/ext_power.h>
#include <drivers/behavior.h>

#include <zmk/pk_underglow.h>
#include <zmk/pk_underglow_layer.h>

#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/matrix.h>
#include <zmk/hid_indicators.h>
#include <zmk/usb.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/underglow_color_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/events/underglow_color_changed.h>

#include <zmk/workqueue.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/pk_split_sync.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/events/split_peripheral_status_changed.h>
static bool is_central_connected = false;
#endif

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_underglow)

#error "A zmk,underglow chosen node must be declared"

#endif

#define STRIP_CHOSEN DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_CHOSEN, chain_length)

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_pk_underglow_layer) && IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_LAYER)
#define UNDERGLOW_LAYER_ENABLED 1
static void zmk_pk_underglow_set_layer(uint8_t layer, bool wakeup);
#endif

#if CONFIG_ZMK_PK_UNDERGLOW_WHITE_SATURATION != -1
    #define WHITE_SATURATION CONFIG_ZMK_PK_UNDERGLOW_WHITE_SATURATION
#elif DT_HAS_COMPAT_STATUS_OKAY(zmk_pk_underglow_layer) && DT_NODE_HAS_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), white_saturation)
    #define WHITE_SATURATION DT_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), white_saturation)
#else
    #define WHITE_SATURATION 0
#endif



BUILD_ASSERT(CONFIG_ZMK_PK_UNDERGLOW_BRT_MIN <= CONFIG_ZMK_PK_UNDERGLOW_BRT_MAX,
             "ERROR: RGB underglow maximum brightness is less than minimum brightness");

enum pk_underglow_effect {
    UNDERGLOW_EFFECT_SOLID,
    UNDERGLOW_EFFECT_BREATHE,
    UNDERGLOW_EFFECT_SPECTRUM,
    UNDERGLOW_EFFECT_SWIRL,
    UNDERGLOW_EFFECT_WHITE,
    UNDERGLOW_EFFECT_RIPPLE,
    UNDERGLOW_EFFECT_RAINBOW_RIPPLE,
    UNDERGLOW_EFFECT_TWINKLE,
    UNDERGLOW_EFFECT_RAINBOW_TWINKLE,
    UNDERGLOW_EFFECT_PINWHEEL,
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    UNDERGLOW_EFFECT_LAYER_INDICATORS,
    UNDERGLOW_EFFECT_LAYER_SPECTRUM,
#endif
    UNDERGLOW_EFFECT_NUMBER // Used to track number of underglow effects
};

struct pk_underglow_state {
    struct zmk_led_hsb color;
    uint8_t effect_speeds[UNDERGLOW_EFFECT_NUMBER];
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
    bool layer_enabled;
};

struct pk_ripple {
    uint8_t row;
    uint8_t col;
    uint32_t start_time;
    uint16_t hue;
    bool active;
};

#define MAX_RIPPLES 4
static struct pk_ripple ripples[MAX_RIPPLES];
static uint8_t ripple_idx = 0;

static uint16_t global_rainbow_hue = 0;
static uint16_t pixel_base_hues[STRIP_NUM_PIXELS];

#ifndef CONFIG_ZMK_PK_UNDERGLOW_TWINKLE_MAX
#define CONFIG_ZMK_PK_UNDERGLOW_TWINKLE_MAX 5
#endif

#ifndef CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS
#define CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS 5
#endif

static float center_row = 0.0f;
static float center_col = 0.0f;

struct pk_twinkle {
    uint8_t led_index;
    uint32_t start_time;
    uint16_t duration;
    uint16_t hue;
    bool active;
};

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
static const uint8_t twinkle_sin_lut[32] = {
    0, 1, 3, 6, 10, 15, 22, 31,
    41, 53, 65, 77, 87, 94, 98, 100,
    100, 98, 94, 87, 77, 65, 53, 41,
    31, 22, 15, 10, 6, 3, 1, 0
};

static inline uint8_t pk_get_twinkle_sin(uint8_t step) {
    if (step > 31) step = 31;
    return twinkle_sin_lut[step];
}

/**
 * Calculates the Euclidean distance between two points on the key matrix.
 * Assumes a maximum matrix size of 22x7 keys.
 * 
 * Generation Script:
 * ```python
 * import math
 * print("{")
 * for r in range(8):
 *     row = [round(math.sqrt(r**2 + c**2) * 100) for c in range(23)]
 *     print("    {" + ", ".join(map(str, row)) + "},")
 * print("}")
 * ```
 */
static const uint16_t ripple_distance_lut[8][23] = {
    {0, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 1100, 1200, 1300, 1400, 1500, 1600, 1700, 1800, 1900, 2000, 2100, 2200},
    {100, 141, 224, 316, 412, 510, 608, 707, 806, 906, 1005, 1105, 1204, 1304, 1404, 1503, 1603, 1703, 1803, 1903, 2002, 2102, 2202},
    {200, 224, 283, 361, 447, 539, 632, 728, 825, 922, 1020, 1118, 1217, 1315, 1414, 1513, 1612, 1712, 1811, 1910, 2010, 2110, 2209},
    {300, 316, 361, 424, 500, 583, 671, 762, 854, 949, 1044, 1140, 1237, 1334, 1432, 1530, 1628, 1726, 1825, 1924, 2022, 2121, 2220},
    {400, 412, 447, 500, 566, 640, 721, 806, 894, 985, 1077, 1170, 1265, 1360, 1456, 1552, 1649, 1746, 1844, 1942, 2040, 2138, 2236},
    {500, 510, 539, 583, 640, 707, 781, 860, 943, 1030, 1118, 1208, 1300, 1393, 1487, 1581, 1676, 1772, 1868, 1965, 2062, 2159, 2256},
    {600, 608, 632, 671, 721, 781, 849, 922, 1000, 1082, 1166, 1253, 1342, 1432, 1523, 1616, 1709, 1803, 1897, 1992, 2088, 2184, 2280},
    {700, 707, 728, 762, 806, 860, 922, 990, 1063, 1140, 1221, 1304, 1389, 1476, 1565, 1655, 1746, 1838, 1931, 2025, 2119, 2214, 2309},
};

static inline float pk_get_ripple_distance(int dr, int dc) {
    int abs_r = abs(dr);
    int abs_c = abs(dc);
    if (abs_r > 7 || abs_c > 22) return 99.0f;
    return ripple_distance_lut[abs_r][abs_c] / 100.0f;
}

/**
 * Calculates the hue angle (0-360) for a given point relative to a center.
 * Assumes a maximum matrix size of 22x7 keys.
 * 
 * Generation Script:
 * ```python
 * import math
 * print("{")
 * for dy in range(-7, 8):
 *     row = [str(int(((math.atan2(dy, dx) + math.pi) / (2.0 * math.pi)) * 360) % 360) for dx in range(-22, 23)]
 *     print("    {" + ", ".join(row) + "},")
 * print("}")
 * ```
 */
static const uint16_t pinwheel_angle_lut[15][45] = {
    {17, 18, 19, 20, 21, 22, 23, 25, 26, 28, 30, 32, 34, 37, 41, 45, 49, 54, 60, 66, 74, 81, 90, 98, 105, 113, 119, 125, 130, 135, 138, 142, 145, 147, 149, 151, 153, 154, 156, 157, 158, 159, 160, 161, 162},
    {15, 15, 16, 17, 18, 19, 20, 21, 23, 24, 26, 28, 30, 33, 36, 40, 45, 50, 56, 63, 71, 80, 90, 99, 108, 116, 123, 129, 135, 139, 143, 146, 149, 151, 153, 155, 156, 158, 159, 160, 161, 162, 163, 164, 164},
    {12, 13, 14, 14, 15, 16, 17, 18, 19, 21, 22, 24, 26, 29, 32, 35, 39, 45, 51, 59, 68, 78, 90, 101, 111, 120, 128, 135, 140, 144, 147, 150, 153, 155, 157, 158, 160, 161, 162, 163, 164, 165, 165, 166, 167},
    {10, 10, 11, 11, 12, 13, 14, 14, 15, 17, 18, 19, 21, 23, 26, 29, 33, 38, 45, 53, 63, 75, 90, 104, 116, 126, 135, 141, 146, 150, 153, 156, 158, 160, 161, 162, 164, 165, 165, 166, 167, 168, 168, 169, 169},
    {7, 8, 8, 8, 9, 10, 10, 11, 12, 12, 14, 15, 16, 18, 20, 23, 26, 30, 36, 45, 56, 71, 90, 108, 123, 135, 143, 149, 153, 156, 159, 161, 163, 164, 165, 167, 167, 168, 169, 169, 170, 171, 171, 171, 172},
    {5, 5, 5, 6, 6, 6, 7, 7, 8, 8, 9, 10, 11, 12, 14, 15, 18, 21, 26, 33, 45, 63, 90, 116, 135, 146, 153, 158, 161, 164, 165, 167, 168, 169, 170, 171, 171, 172, 172, 173, 173, 173, 174, 174, 174},
    {2, 2, 2, 3, 3, 3, 3, 3, 4, 4, 4, 5, 5, 6, 7, 8, 9, 11, 14, 18, 26, 45, 90, 135, 153, 161, 165, 168, 170, 171, 172, 173, 174, 174, 175, 175, 175, 176, 176, 176, 176, 176, 177, 177, 177},
    {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180, 180},
    {357, 357, 357, 356, 356, 356, 356, 356, 355, 355, 355, 354, 354, 353, 352, 351, 350, 348, 345, 341, 333, 315, 270, 225, 206, 198, 194, 191, 189, 188, 187, 186, 185, 185, 184, 184, 184, 183, 183, 183, 183, 183, 182, 182, 182},
    {354, 354, 354, 353, 353, 353, 352, 352, 351, 351, 350, 349, 348, 347, 345, 344, 341, 338, 333, 326, 315, 296, 270, 243, 225, 213, 206, 201, 198, 195, 194, 192, 191, 190, 189, 188, 188, 187, 187, 186, 186, 186, 185, 185, 185},
    {352, 351, 351, 351, 350, 349, 349, 348, 347, 347, 345, 344, 343, 341, 339, 336, 333, 329, 323, 315, 303, 288, 270, 251, 236, 225, 216, 210, 206, 203, 200, 198, 196, 195, 194, 192, 192, 191, 190, 190, 189, 188, 188, 188, 187},
    {349, 349, 348, 348, 347, 346, 345, 345, 344, 342, 341, 340, 338, 336, 333, 330, 326, 321, 315, 306, 296, 284, 270, 255, 243, 233, 225, 218, 213, 209, 206, 203, 201, 199, 198, 197, 195, 194, 194, 193, 192, 191, 191, 190, 190},
    {347, 346, 345, 345, 344, 343, 342, 341, 340, 338, 337, 335, 333, 330, 327, 324, 320, 315, 308, 300, 291, 281, 270, 258, 248, 239, 231, 225, 219, 215, 212, 209, 206, 204, 202, 201, 199, 198, 197, 196, 195, 194, 194, 193, 192},
    {344, 344, 343, 342, 341, 340, 339, 338, 336, 335, 333, 331, 329, 326, 323, 319, 315, 309, 303, 296, 288, 279, 270, 260, 251, 243, 236, 230, 225, 220, 216, 213, 210, 208, 206, 204, 203, 201, 200, 199, 198, 197, 196, 195, 195},
    {342, 341, 340, 339, 338, 337, 336, 334, 333, 331, 329, 327, 325, 322, 318, 315, 310, 305, 299, 293, 285, 278, 270, 261, 254, 246, 240, 234, 229, 225, 221, 217, 214, 212, 210, 208, 206, 205, 203, 202, 201, 200, 199, 198, 197},
};

static inline uint16_t pk_get_pinwheel_angle(int dr, int dc) {
    int idx_r = dr + 7;
    int idx_c = dc + 22;
    if (idx_r < 0) idx_r = 0; else if (idx_r > 14) idx_r = 14;
    if (idx_c < 0) idx_c = 0; else if (idx_c > 44) idx_c = 44;
    return pinwheel_angle_lut[idx_r][idx_c];
}

static const struct device *led_strip;

static struct led_rgb pixels[STRIP_NUM_PIXELS];

static struct pk_underglow_state state;
static bool is_powered = false;

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_pk_underglow_layer)
static const struct gpio_dt_spec power_gpio = GPIO_DT_SPEC_GET_OR(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), power_gpios, {0});
#else
static const struct gpio_dt_spec power_gpio = {0};
#endif

static struct zmk_led_hsb hsb_scale_min_max(struct zmk_led_hsb hsb) {
    hsb.b = CONFIG_ZMK_PK_UNDERGLOW_BRT_MIN +
            (CONFIG_ZMK_PK_UNDERGLOW_BRT_MAX - CONFIG_ZMK_PK_UNDERGLOW_BRT_MIN) * hsb.b / BRT_MAX;
    return hsb;
}

static struct zmk_led_hsb hsb_scale_zero_max(struct zmk_led_hsb hsb) {
    hsb.b = hsb.b * CONFIG_ZMK_PK_UNDERGLOW_BRT_MAX / BRT_MAX;
    return hsb;
}

static struct led_rgb hsb_to_rgb(struct zmk_led_hsb hsb) {
    float r = 0, g = 0, b = 0;

    uint8_t i = hsb.h / 60;
    float v = hsb.b / ((float)BRT_MAX);
    float s = hsb.s / ((float)SAT_MAX);
    float f = hsb.h / ((float)HUE_MAX) * 6 - i;
    float p = v * (1 - s);
    float q = v * (1 - f * s);
    float t = v * (1 - (1 - f) * s);

    switch (i % 6) {
    case 0:
        r = v;
        g = t;
        b = p;
        break;
    case 1:
        r = q;
        g = v;
        b = p;
        break;
    case 2:
        r = p;
        g = v;
        b = t;
        break;
    case 3:
        r = p;
        g = q;
        b = v;
        break;
    case 4:
        r = t;
        g = p;
        b = v;
        break;
    case 5:
        r = v;
        g = p;
        b = q;
        break;
    }

    struct led_rgb rgb = {r : r * 255, g : g * 255, b : b * 255};

    return rgb;
}

static void zmk_pk_underglow_effect_solid(void) {
    struct led_rgb rgb = hsb_to_rgb(hsb_scale_min_max(state.color));
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }
}

static void zmk_pk_underglow_effect_white(void) {
    struct zmk_led_hsb hsb = state.color;
    hsb.s = WHITE_SATURATION;
    struct led_rgb rgb = hsb_to_rgb(hsb_scale_min_max(hsb));
    
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }
}

static void zmk_pk_underglow_effect_breathe(void) {
    struct zmk_led_hsb hsb = state.color;
    hsb.b = abs(state.animation_step - 1200) / 12;
    struct led_rgb rgb = hsb_to_rgb(hsb_scale_zero_max(hsb));

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }

    state.animation_step += state.effect_speeds[state.current_effect] * 13;

    if (state.animation_step > 2400) {
        state.animation_step = 0;
    }
}

static void zmk_pk_underglow_effect_spectrum(void) {
    struct zmk_led_hsb hsb = state.color;
    hsb.h = state.animation_step;
    struct led_rgb rgb = hsb_to_rgb(hsb_scale_min_max(hsb));

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = rgb;
    }

    state.animation_step += state.effect_speeds[state.current_effect];
    state.animation_step = state.animation_step % HUE_MAX;
}

static void zmk_pk_underglow_effect_swirl(void) {
    struct zmk_led_hsb base_hsb = state.color;
    const uint16_t hue_step = HUE_MAX / STRIP_NUM_PIXELS;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = base_hsb;
        hsb.h = (hue_step * i + state.animation_step) % HUE_MAX;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.effect_speeds[state.current_effect] * 3;
    state.animation_step = state.animation_step % HUE_MAX;
}

static void zmk_pk_underglow_effect_ripple(void) {
    uint32_t now = k_uptime_get_32();
    struct zmk_led_hsb base_hsb = state.color;
    
    // Dim base color
    base_hsb.b = (base_hsb.b * CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS) / 100;
    struct led_rgb base_rgb = hsb_to_rgb(hsb_scale_min_max(base_hsb));
    
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t midx = rgb_pixel_lookup(i);
        int p_row = midx / 12;
        int p_col = midx % 12;
        
        uint8_t peak_b = base_hsb.b;
        
        for (int r = 0; r < MAX_RIPPLES; r++) {
            if (!ripples[r].active) continue;
            uint32_t elapsed = now - ripples[r].start_time;
            
            // Animation speed governs how fast it expands
            float speed_factor = state.effect_speeds[state.current_effect] * 0.6f; 
            float current_radius = (float)elapsed * speed_factor / 100.0f;
            float max_radius = 12.0f; // Max distance across one half of keyboard
            
            if (current_radius > max_radius) {
                ripples[r].active = false;
                continue;
            }
            
            float dist = pk_get_ripple_distance(p_row - ripples[r].row, p_col - ripples[r].col);
            float thickness = 2.0f;
            float dist_from_ring = fabsf(current_radius - dist);
            
            if (dist_from_ring < thickness) {
                float intensity = 1.0f - (dist_from_ring / thickness);
                float fade = 1.0f - (current_radius / max_radius);
                
                uint8_t ripple_b = (uint8_t)(state.color.b * intensity * fade);
                if (ripple_b > peak_b) peak_b = ripple_b;
            }
        }
        
        struct zmk_led_hsb pixel_hsb = state.color;
        pixel_hsb.b = peak_b;
        pixels[i] = hsb_to_rgb(hsb_scale_min_max(pixel_hsb));
    }
}

static void zmk_pk_underglow_effect_rainbow_ripple(void) {
    uint32_t now = k_uptime_get_32();
    struct zmk_led_hsb base_hsb = state.color;
    
    // Global rainbow hue updates on every tick like the spectrum effect
    global_rainbow_hue = (global_rainbow_hue + state.effect_speeds[state.current_effect]) % HUE_MAX;
    
    // Dim base color
    base_hsb.b = (base_hsb.b * CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS) / 100;
    
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t midx = rgb_pixel_lookup(i);
        int p_row = midx / 12;
        int p_col = midx % 12;
        
        uint8_t peak_b = base_hsb.b;
        uint16_t peak_hue = pixel_base_hues[i]; // Default to the LED's stored base hue
        
        for (int r = 0; r < MAX_RIPPLES; r++) {
            if (!ripples[r].active) continue;
            uint32_t elapsed = now - ripples[r].start_time;
            
            // Animation speed governs how fast it expands
            float speed_factor = state.effect_speeds[state.current_effect] * 0.6f; 
            float current_radius = (float)elapsed * speed_factor / 100.0f;
            float max_radius = 12.0f; // Max distance across one half of keyboard
            
            if (current_radius > max_radius) {
                ripples[r].active = false;
                continue;
            }
            
            float dist = pk_get_ripple_distance(p_row - ripples[r].row, p_col - ripples[r].col);
            float thickness = 2.0f;
            float dist_from_ring = fabsf(current_radius - dist);
            
            if (dist_from_ring < thickness) {
                float intensity = 1.0f - (dist_from_ring / thickness);
                float fade = 1.0f - (current_radius / max_radius);
                
                uint8_t ripple_b = (uint8_t)(state.color.b * intensity * fade);
                if (ripple_b > peak_b) {
                    peak_b = ripple_b;
                    peak_hue = ripples[r].hue; // Absorb the wave's hue
                    pixel_base_hues[i] = ripples[r].hue; // Store the hue persistently
                }
            }
        }
        
        struct zmk_led_hsb pixel_hsb = state.color;
        pixel_hsb.h = peak_hue;
        pixel_hsb.b = peak_b;
        pixels[i] = hsb_to_rgb(hsb_scale_min_max(pixel_hsb));
    }
}

static void zmk_pk_underglow_effect_twinkle(void) {
    uint32_t now = k_uptime_get_32();
    struct zmk_led_hsb base_hsb = state.color;
    
    // Dim base color
    base_hsb.b = (base_hsb.b * CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS) / 100;
    struct led_rgb base_rgb = hsb_to_rgb(hsb_scale_min_max(base_hsb));
    
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = base_rgb;
    }
    
    for (int i = 0; i < CONFIG_ZMK_PK_UNDERGLOW_TWINKLE_MAX; i++) {
        if (!twinkles[i].active) {
            // Randomly start a new twinkle
            // But don't do it all at once; only start if random chance allows, to stagger them
            if ((sys_rand32_get() % 100) < 5) {
                twinkles[i].led_index = sys_rand32_get() % STRIP_NUM_PIXELS;
                twinkles[i].start_time = now;
                // 2000ms / speed + random offset (doubled from original)
                twinkles[i].duration = (4000 / state.effect_speeds[state.current_effect]) + (sys_rand32_get() % 2000);
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
        if (lut_index > 31) lut_index = 31;
        
        uint8_t added_b = pk_get_twinkle_sin(lut_index);
        uint8_t twinkle_b = CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS + (added_b * (100 - CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS)) / 100;
        
        // Scale to global brightness
        twinkle_b = (uint8_t)((state.color.b * twinkle_b) / 100);
        
        struct zmk_led_hsb pixel_hsb = state.color;
        pixel_hsb.b = twinkle_b;
        
        pixels[twinkles[i].led_index] = hsb_to_rgb(hsb_scale_min_max(pixel_hsb));
    }
}

static void zmk_pk_underglow_effect_rainbow_twinkle(void) {
    uint32_t now = k_uptime_get_32();
    struct zmk_led_hsb base_hsb = state.color;
    
    // Global rainbow hue updates on every tick like the spectrum effect
    global_rainbow_hue = (global_rainbow_hue + state.effect_speeds[state.current_effect]) % HUE_MAX;
    
    // Dim base color and apply the dynamic rainbow hue
    base_hsb.h = global_rainbow_hue;
    base_hsb.b = (base_hsb.b * CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS) / 100;
    struct led_rgb base_rgb = hsb_to_rgb(hsb_scale_min_max(base_hsb));
    
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = base_rgb;
    }
    
    for (int i = 0; i < CONFIG_ZMK_PK_UNDERGLOW_TWINKLE_MAX; i++) {
        if (!twinkles[i].active) {
            // Randomly start a new twinkle
            if ((sys_rand32_get() % 100) < 5) {
                twinkles[i].led_index = sys_rand32_get() % STRIP_NUM_PIXELS;
                twinkles[i].start_time = now;
                twinkles[i].duration = (4000 / state.effect_speeds[state.current_effect]) + (sys_rand32_get() % 2000);
                twinkles[i].hue = global_rainbow_hue; // Capture the current ambient hue
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
        if (lut_index > 31) lut_index = 31;
        
        uint8_t added_b = pk_get_twinkle_sin(lut_index);
        uint8_t twinkle_b = CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS + (added_b * (100 - CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS)) / 100;
        
        // If the added brightness is essentially 0, it means the star is at the ambient level.
        // It should assume the background color (which we painted in the loop above).
        if (added_b == 0) {
            continue;
        }
        
        struct zmk_led_hsb pixel_hsb = state.color;
        pixel_hsb.h = twinkles[i].hue; // Use the star's captured hue
        
        // Scale to global brightness
        twinkle_b = (uint8_t)((state.color.b * twinkle_b) / 100);
        pixel_hsb.b = twinkle_b;
        
        pixels[twinkles[i].led_index] = hsb_to_rgb(hsb_scale_min_max(pixel_hsb));
    }
}

static void zmk_pk_underglow_effect_pinwheel(void) {
    // Increment the animation step to make it spin!
    // 0.5 revs/sec at max speed (5) = 180 degrees/sec.
    // Timer runs at ~15 ticks/sec (67ms). 180 / 15 = 12 degrees/tick.
    state.animation_step += (state.effect_speeds[state.current_effect] * 12) / 5;
    state.animation_step = state.animation_step % HUE_MAX;
    
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t midx = rgb_pixel_lookup(i);
        int p_row = midx / 12;
        int p_col = midx % 12;
        
        int dr = p_row - (int)center_row;
        int dc = p_col - (int)center_col;
        uint16_t angle_hue = pk_get_pinwheel_angle(dr, dc);
        
        struct zmk_led_hsb pixel_hsb = state.color;
        // The spinning effect comes from animation_step, which increments continuously
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
        pixel_hsb.h = (angle_hue + state.animation_step) % HUE_MAX;
#else
        // Spin in opposite direction on the peripheral side
        pixel_hsb.h = (angle_hue + HUE_MAX - (state.animation_step % HUE_MAX)) % HUE_MAX;
#endif
        
        pixels[i] = hsb_to_rgb(hsb_scale_min_max(pixel_hsb));
    }
}

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
static int zmk_pk_underglow_apply_rgbmap(const struct zmk_behavior_binding *bindings,
                                         size_t bindings_len, uint8_t layer);

static void zmk_pk_underglow_effect_layer(void) {
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
        pixels[i].r -= state.effect_speeds[state.current_effect] < pixels[i].r ? state.effect_speeds[state.current_effect] : pixels[i].r;
        pixels[i].g -= state.effect_speeds[state.current_effect] < pixels[i].g ? state.effect_speeds[state.current_effect] : pixels[i].g;
        pixels[i].b -= state.effect_speeds[state.current_effect] < pixels[i].b ? state.effect_speeds[state.current_effect] : pixels[i].b;
        if (pixels[i].r || pixels[i].g || pixels[i].b) {
            active = true;
        }
    }
    state.animation_step += state.effect_speeds[state.current_effect];

    if (state.animation_step > 255 || !active) {
        zmk_pk_underglow_transient_off();
    }
}

static void zmk_pk_underglow_effect_layer_spectrum(void) {
    state.animation_step += state.effect_speeds[state.current_effect] * 3;
    state.animation_step %= HUE_MAX;
    
    uint8_t top_layer = pk_underglow_top_layer();
    const struct zmk_behavior_binding *rgbmap = pk_underglow_get_bindings(top_layer);
    if (rgbmap != NULL) {
        zmk_pk_underglow_apply_rgbmap(rgbmap, ZMK_KEYMAP_LEN, top_layer);
    }
}
#endif // IS_ENABLED(UNDERGLOW_LAYER_ENABLED)

static void zmk_pk_underglow_tick(struct k_work *work) {
    switch (state.current_effect) {
    case UNDERGLOW_EFFECT_SOLID:
        zmk_pk_underglow_effect_solid();
        break;
    case UNDERGLOW_EFFECT_BREATHE:
        zmk_pk_underglow_effect_breathe();
        break;
    case UNDERGLOW_EFFECT_SPECTRUM:
        zmk_pk_underglow_effect_spectrum();
        break;
    case UNDERGLOW_EFFECT_SWIRL:
        zmk_pk_underglow_effect_swirl();
        break;
    case UNDERGLOW_EFFECT_WHITE:
        zmk_pk_underglow_effect_white();
        break;
    case UNDERGLOW_EFFECT_RIPPLE:
        zmk_pk_underglow_effect_ripple();
        break;
    case UNDERGLOW_EFFECT_RAINBOW_RIPPLE:
        zmk_pk_underglow_effect_rainbow_ripple();
        break;
    case UNDERGLOW_EFFECT_TWINKLE:
        zmk_pk_underglow_effect_twinkle();
        break;
    case UNDERGLOW_EFFECT_RAINBOW_TWINKLE:
        zmk_pk_underglow_effect_rainbow_twinkle();
        break;
    case UNDERGLOW_EFFECT_PINWHEEL:
        zmk_pk_underglow_effect_pinwheel();
        break;
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    case UNDERGLOW_EFFECT_LAYER_INDICATORS:
        zmk_pk_underglow_effect_layer();
        break;
    case UNDERGLOW_EFFECT_LAYER_SPECTRUM:
        zmk_pk_underglow_effect_layer_spectrum();
        break;
#endif
    }

    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}

K_WORK_DEFINE(underglow_tick_work, zmk_pk_underglow_tick);

static void zmk_pk_underglow_tick_handler(struct k_timer *timer) {
    if (!state.on) {
        return;
    }

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_tick_work);
}

K_TIMER_DEFINE(underglow_tick, zmk_pk_underglow_tick_handler, NULL);

#if IS_ENABLED(CONFIG_SETTINGS)
static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            return -EINVAL;
        }

        rc = read_cb(cb_arg, &state, sizeof(state));
        if (rc >= 0) {
            if (state.on) {
                zmk_pk_underglow_transient_on();
            } else {
                zmk_pk_underglow_transient_off();
            }
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
            if (state.layer_enabled) {
                zmk_pk_underglow_set_layer(pk_underglow_top_layer(), true);
            }
#endif
            return 0;
        }

        return rc;
    }

    return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(pk_underglow, "rgb/underglow", NULL, rgb_settings_set, NULL, NULL);

static void zmk_pk_underglow_save_state_work(struct k_work *_work) {
    settings_save_one("rgb/underglow/state", &state, sizeof(state));
}

static struct k_work_delayable underglow_save_work;
#endif

static int zmk_pk_underglow_init(void) {
    int min_row = 999, max_row = -1;
    int min_col = 999, max_col = -1;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t midx = rgb_pixel_lookup(i);
        int r = midx / 12;
        int c = midx % 12;
        if (r < min_row) min_row = r;
        if (r > max_row) max_row = r;
        if (c < min_col) min_col = c;
        if (c > max_col) max_col = c;
    }

    if (STRIP_NUM_PIXELS > 0) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
        // Central side (or unibody): Top Left
        center_row = (float)min_row;
        center_col = (float)min_col;
#else
        // Peripheral side: Top Right
        center_row = (float)min_row;
        center_col = (float)max_col;
#endif
    }

    led_strip = DEVICE_DT_GET(STRIP_CHOSEN);

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_EXT_POWER)
    if (!device_is_ready(ext_power)) {
        LOG_ERR("External power device \"%s\" is not ready", ext_power->name);
        return -ENODEV;
    }
#endif

    if (power_gpio.port != NULL) {
        if (!gpio_is_ready_dt(&power_gpio)) {
            LOG_ERR("Power GPIO is not ready");
            return -ENODEV;
        }
        int rc = gpio_pin_configure_dt(&power_gpio, GPIO_OUTPUT_INACTIVE);
        if (rc < 0) {
            LOG_ERR("Failed to configure power GPIO: %d", rc);
            return rc;
        }
        LOG_INF("Power GPIO configured successfully");
    }

    state = (struct pk_underglow_state){
        color : {
            h : CONFIG_ZMK_PK_UNDERGLOW_HUE_START,
            s : CONFIG_ZMK_PK_UNDERGLOW_SAT_START,
            b : CONFIG_ZMK_PK_UNDERGLOW_BRT_START,
        },
        current_effect : CONFIG_ZMK_PK_UNDERGLOW_EFF_START,
        animation_step : 0,
        on : IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_ON_START)
    };

    for (int i = 0; i < UNDERGLOW_EFFECT_NUMBER; i++) {
        state.effect_speeds[i] = CONFIG_ZMK_PK_UNDERGLOW_SPD_START;
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&underglow_save_work, zmk_pk_underglow_save_state_work);
#endif

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_USB)
    state.on = zmk_usb_is_powered();
#endif

    if (state.on) {
        zmk_pk_underglow_transient_on();
    }
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.layer_enabled) {
        zmk_pk_underglow_set_layer(pk_underglow_top_layer(), true);
    }
#endif
    return 0;
}

int zmk_pk_underglow_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    int ret = k_work_reschedule(&underglow_save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE));
    return MIN(ret, 0);
#else
    return 0;
#endif
}

int zmk_pk_underglow_get_state(bool *on_off) {
    if (!led_strip)
        return -ENODEV;

    *on_off = state.on || state.layer_enabled;
    return 0;
}

int zmk_pk_underglow_on(void) {
    zmk_pk_underglow_transient_on();
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.current_effect == UNDERGLOW_EFFECT_LAYER_INDICATORS) {
        state.layer_enabled = true;
    }
#endif
    return zmk_pk_underglow_save_state();
}

int zmk_pk_underglow_transient_on(void) {
    if (!led_strip)
        return -ENODEV;

    state.on = true;
    state.animation_step = 0;

    if (is_powered) {
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(67));
        return 0;
    }

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_enable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to enable EXT_POWER: %d", rc);
        }
    }
#endif

    if (power_gpio.port != NULL) {
        int rc = gpio_pin_set_dt(&power_gpio, 1);
        if (rc != 0) {
            LOG_ERR("Failed to set power GPIO ON: %d", rc);
        } else {
            LOG_DBG("Power GPIO set ON");
        }
    }

    is_powered = true;
    k_timer_start(&underglow_tick, K_MSEC(10), K_MSEC(67));

    return 0;
}

static void zmk_pk_underglow_off_handler(struct k_work *work) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){r : 0, g : 0, b : 0};
    }

    led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);

    // Sleep briefly to ensure the transmission finishes before cutting power
    k_sleep(K_MSEC(5));

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_EXT_POWER)
    if (ext_power != NULL) {
        int rc = ext_power_disable(ext_power);
        if (rc != 0) {
            LOG_ERR("Unable to disable EXT_POWER: %d", rc);
        }
    }
#endif

    if (power_gpio.port != NULL) {
        int rc = gpio_pin_set_dt(&power_gpio, 0);
        if (rc != 0) {
            LOG_ERR("Failed to set power GPIO OFF: %d", rc);
        } else {
            LOG_DBG("Power GPIO set OFF");
        }
    }
    is_powered = false;
}

K_WORK_DEFINE(underglow_off_work, zmk_pk_underglow_off_handler);

int zmk_pk_underglow_off(void) {
    zmk_pk_underglow_transient_off();
    state.on = false;
    state.layer_enabled = false;
    return zmk_pk_underglow_save_state();
}

int zmk_pk_underglow_transient_off(void) {
    if (!led_strip)
        return -ENODEV;
        
    if (!is_powered)
        return 0;

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &underglow_off_work);

    k_timer_stop(&underglow_tick);

    return 0;
}

int zmk_pk_underglow_calc_effect(int direction) {
    return (state.current_effect + UNDERGLOW_EFFECT_NUMBER + direction) % UNDERGLOW_EFFECT_NUMBER;
}

int zmk_pk_underglow_select_effect(int effect) {
    if (!led_strip)
        return -ENODEV;

    if (effect < 0 || effect >= UNDERGLOW_EFFECT_NUMBER) {
        return -EINVAL;
    }

    state.current_effect = effect;
    state.animation_step = 0;
    
    if (effect == UNDERGLOW_EFFECT_RAINBOW_RIPPLE || effect == UNDERGLOW_EFFECT_RAINBOW_TWINKLE) {
        global_rainbow_hue = state.color.h;
        for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
            pixel_base_hues[i] = state.color.h;
        }
    }
    
    if (effect == UNDERGLOW_EFFECT_TWINKLE || effect == UNDERGLOW_EFFECT_RAINBOW_TWINKLE) {
        for (int i = 0; i < CONFIG_ZMK_PK_UNDERGLOW_TWINKLE_MAX; i++) {
            twinkles[i].active = false;
        }
    }

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    state.layer_enabled = (effect == UNDERGLOW_EFFECT_LAYER_INDICATORS);

    LOG_INF("Selected effect: %d, layer_enabled: %d, state.on: %d", effect, state.layer_enabled, state.on);

    if (state.layer_enabled) {
        zmk_pk_underglow_set_layer(pk_underglow_top_layer(), false);
    } else if (state.on) {
        LOG_INF("Restarting animation timer for effect %d", effect);
        zmk_pk_underglow_transient_on();
    }
#endif
    return zmk_pk_underglow_save_state();
}

int zmk_pk_underglow_cycle_effect(int direction) {
    return zmk_pk_underglow_select_effect(zmk_pk_underglow_calc_effect(direction));
}

int zmk_pk_underglow_toggle(void) {
    return state.on ? zmk_pk_underglow_off() : zmk_pk_underglow_on();
}

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)

static struct led_rgb hex_to_rgb(uint8_t r, uint8_t g, uint8_t b) {
    struct zmk_led_hsb hsb = state.color;
    return (struct led_rgb){
        .r = (hsb.b * r) / 0xff,
        .g = (hsb.b * g) / 0xff,
        .b = (hsb.b * b) / 0xff
    };
}

static int zmk_pk_underglow_apply_rgbmap(const struct zmk_behavior_binding *bindings,
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

static void zmk_pk_underglow_set_layer(uint8_t layer, bool wakeup) {
    LOG_INF("Setting pk underglow layer: %d. layer_enabled: %d, state.on: %d", layer, state.layer_enabled, state.on);
    if (!state.layer_enabled)
        return;

    const struct zmk_behavior_binding *rgbmap = pk_underglow_get_bindings(layer);
    if (rgbmap != NULL && zmk_pk_underglow_apply_rgbmap(rgbmap, ZMK_KEYMAP_LEN, layer)) {
        if (!is_powered) {
            if (!wakeup && !state.on) {
                LOG_DBG("rgb off and no wakeup, abort refresh");
                return;
            }
            zmk_pk_underglow_transient_on();
            
            // Allow power to stabilize before writing the first frame of pixels
            k_sleep(K_MSEC(10));
        }
        k_timer_stop(&underglow_tick);
        state.animation_step = 0;
        int fade_delay = zmk_rgbmap_fade_delay(layer);
        bool animated = zmk_rgbmap_is_animated(layer);
        if (fade_delay > 0) {
            k_timer_start(&underglow_tick, K_SECONDS(fade_delay), K_MSEC(67));
        } else if (animated || fade_delay == 0) {
            k_timer_start(&underglow_tick, K_MSEC(67), K_MSEC(67));
        }
        LOG_DBG("write pixels");
        int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
        if (err < 0) {
            LOG_ERR("Failed to update the RGB strip (%d)", err);
        }
    } else {
        if (state.on)
            zmk_pk_underglow_transient_off();
    }
}
#endif /* IS_ENABLED(UNDERGLOW_LAYER_ENABLED) */

int zmk_pk_underglow_set_hsb(struct zmk_led_hsb color) {
    if (color.h > HUE_MAX || color.s > SAT_MAX || color.b > BRT_MAX) {
        return -ENOTSUP;
    }

    state.color = color;

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.layer_enabled) {
        zmk_pk_underglow_set_layer(pk_underglow_top_layer(), false);
    }
#endif

    return 0;
}

struct zmk_led_hsb zmk_pk_underglow_calc_hue(int direction) {
    struct zmk_led_hsb color = state.color;

    color.h += HUE_MAX + (direction * CONFIG_ZMK_PK_UNDERGLOW_HUE_STEP);
    color.h %= HUE_MAX;

    return color;
}

struct zmk_led_hsb zmk_pk_underglow_calc_sat(int direction) {
    struct zmk_led_hsb color = state.color;

    int s = color.s + (direction * CONFIG_ZMK_PK_UNDERGLOW_SAT_STEP);
    if (s < 0) {
        s = 0;
    } else if (s > SAT_MAX) {
        s = SAT_MAX;
    }
    color.s = s;

    return color;
}

struct zmk_led_hsb zmk_pk_underglow_calc_brt(int direction) {
    struct zmk_led_hsb color = state.color;

    int b = color.b + (direction * CONFIG_ZMK_PK_UNDERGLOW_BRT_STEP);
    color.b = CLAMP(b, 0, BRT_MAX);

    return color;
}

int zmk_pk_underglow_change_hue(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_pk_underglow_calc_hue(direction);

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.layer_enabled) {
        zmk_pk_underglow_set_layer(pk_underglow_top_layer(), false);
    }
#endif

    return zmk_pk_underglow_save_state();
}

int zmk_pk_underglow_change_sat(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_pk_underglow_calc_sat(direction);

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.layer_enabled) {
        zmk_pk_underglow_set_layer(pk_underglow_top_layer(), false);
    }
#endif

    return zmk_pk_underglow_save_state();
}

int zmk_pk_underglow_change_brt(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_pk_underglow_calc_brt(direction);

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.layer_enabled) {
        zmk_pk_underglow_set_layer(pk_underglow_top_layer(), false);
    }
#endif

    return zmk_pk_underglow_save_state();
}

int zmk_pk_underglow_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.effect_speeds[state.current_effect] == 1 && direction < 0) {
        return 0;
    }

    state.effect_speeds[state.current_effect] += direction;

    if (state.effect_speeds[state.current_effect] > 5) {
        state.effect_speeds[state.current_effect] = 5;
    }

    return zmk_pk_underglow_save_state();
}

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_IDLE) ||                                          \
    IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_USB) || IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
struct pk_underglow_sleep_state {
    bool is_awake;
    bool rgb_state_before_sleeping;
};

static struct pk_underglow_sleep_state sleep_state = {
    .is_awake = true,
    .rgb_state_before_sleeping = false
};

static void sync_peripheral_state(uint8_t layer, int state_directive) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    uint32_t param1 = (state.color.h & 0xFFFF) | ((state.color.s & 0xFF) << 16) | ((state.color.b & 0xFF) << 24);
    uint32_t param2 = (state.current_effect & 0xFF) | 
                      ((state.effect_speeds[state.current_effect] & 0xFF) << 8) | 
                      ((layer & 0xFF) << 16) | 
                      ((state.on ? 1 : 0) << 24) | 
                      ((state_directive & 0x03) << 25) |
                      ((state.layer_enabled ? 1 : 0) << 27);

    LOG_DBG("Central: Broadcasting ug_sync with layer %d, state_directive %d", layer, state_directive);
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_pk_underglow_sync)
    struct zmk_behavior_binding binding = {
        .behavior_dev = DEVICE_DT_NAME(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_behavior_pk_underglow_sync)),
        .param1 = param1,
        .param2 = param2,
    };
    struct zmk_behavior_binding_event event = {
        .position = 0,
        .timestamp = k_uptime_get(),
    };
    for (int i = 0; i < 8; i++) {
        zmk_split_central_invoke_behavior(i, &binding, event, true);
        zmk_split_central_invoke_behavior(i, &binding, event, false);
    }
#endif
#endif
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IS_ENABLED(CONFIG_BT)
#include <zephyr/bluetooth/conn.h>

static void sync_peripheral_delayed_work_handler(struct k_work *work) {
    uint8_t layer = pk_underglow_top_layer();
    sync_peripheral_state(layer, 0); // Layer sync
#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_IDLE)
    if (!sleep_state.is_awake) {
        sync_peripheral_state(layer, 2); // Sleep sync
    }
#endif
}

static K_WORK_DELAYABLE_DEFINE(sync_peripheral_delayed_work, sync_peripheral_delayed_work_handler);

static void pk_underglow_bt_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        return;
    }
    // Re-sync peripheral state 500ms after connection to ensure consistency
    k_work_schedule(&sync_peripheral_delayed_work, K_MSEC(500));
}

BT_CONN_CB_DEFINE(pk_underglow_bt_conn_cb) = {
    .connected = pk_underglow_bt_connected,
};
#endif

static int pk_underglow_auto_state(bool target_wake_state) {

    // wake up event while awake, or sleep event while sleeping -> no-op
    if (target_wake_state == sleep_state.is_awake) {
        return 0;
    }
    sleep_state.is_awake = target_wake_state;
    LOG_DBG("Auto state changed. Target wake: %d. Syncing to peripheral...", target_wake_state);
    sync_peripheral_state(pk_underglow_top_layer(), target_wake_state ? 1 : 2);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IS_ENABLED(CONFIG_BT)
    // Force sending a standard payload shortly after waking, simulating a layer change.
    // This allows the peripheral's power circuit ample time to stabilize before
    // receiving the final color state, ensuring the LEDs latch the data correctly.
    if (target_wake_state) {
        k_work_schedule(&sync_peripheral_delayed_work, K_MSEC(100));
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_DBG("Peripheral: Checking if central is connected (is_central_connected=%d)", is_central_connected);
    if (is_central_connected && zmk_activity_get_state() != ZMK_ACTIVITY_SLEEP) {
        LOG_DBG("Peripheral: Central is connected, deferring auto_state to central sync.");
        return 0;
    }
    LOG_DBG("Peripheral: Central NOT connected or entering deep sleep, self-managing idle state.");
#endif

    if (sleep_state.is_awake) {
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
        if (state.layer_enabled) {
            zmk_pk_underglow_set_layer(pk_underglow_top_layer(), true);
            return 0;
        }
#endif
        if (sleep_state.rgb_state_before_sleeping) {
            return zmk_pk_underglow_transient_on();
        } else {
            return zmk_pk_underglow_transient_off();
        }
    } else {
        sleep_state.rgb_state_before_sleeping = state.on;
        return zmk_pk_underglow_transient_off();
    }
}

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
extern uint8_t pk_underglow_peripheral_synced_layer;
void zmk_pk_underglow_set_peripheral_layer(uint8_t layer) {
    pk_underglow_peripheral_synced_layer = layer;
    zmk_pk_underglow_set_layer(layer, true);
}
#endif

static int pk_underglow_event_listener(const zmk_event_t *eh) {

    if (as_zmk_activity_state_changed(eh)) {
        enum zmk_activity_state state = zmk_activity_get_state();
        
        bool should_handle = false;
#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_IDLE)
        should_handle = true;
#else
        if (state == ZMK_ACTIVITY_SLEEP || state == ZMK_ACTIVITY_ACTIVE) {
            should_handle = true;
        }
#endif
        if (should_handle) {
            return pk_underglow_auto_state(state == ZMK_ACTIVITY_ACTIVE);
        }
    }

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    if (as_zmk_layer_state_changed(eh)) {
        if (!sleep_state.is_awake) {
            return ZMK_EV_EVENT_BUBBLE;
        }
        const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
        LOG_DBG("zmk_layer_state_changed: %08x", ev->state);
        uint8_t layer = pk_underglow_top_layer();
        zmk_pk_underglow_set_layer(layer, true);
        sync_peripheral_state(layer, 0);

        return ZMK_EV_EVENT_BUBBLE;
    }
#endif
    if (as_zmk_underglow_color_changed(eh)) {
        const struct zmk_underglow_color_changed *ev = as_zmk_underglow_color_changed(eh);
        uint8_t layer = pk_underglow_top_layer();
        LOG_DBG("refresh layers %d, current: %d, wakeup: %d", ev->layers, layer, ev->wakeup);
        if ((ev->layers & (BIT(layer))) == BIT(layer)) {
            zmk_pk_underglow_set_layer(pk_underglow_top_layer(), ev->wakeup);
        }
        return 0;
    }
#endif /* UNDERGLOW_LAYER_ENABLED */

    if (as_zmk_position_state_changed(eh)) {
        if (state.on && (state.current_effect == UNDERGLOW_EFFECT_RIPPLE || state.current_effect == UNDERGLOW_EFFECT_RAINBOW_RIPPLE)) {
            const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
            if (ev->state && ev->source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
                uint8_t row = ev->position / 12;
                uint8_t col = ev->position % 12;
                ripples[ripple_idx].row = row;
                ripples[ripple_idx].col = col;
                ripples[ripple_idx].start_time = k_uptime_get_32();
                ripples[ripple_idx].hue = (state.current_effect == UNDERGLOW_EFFECT_RAINBOW_RIPPLE) ? global_rainbow_hue : state.color.h;
                ripples[ripple_idx].active = true;
                ripple_idx = (ripple_idx + 1) % MAX_RIPPLES;
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        return pk_underglow_auto_state(zmk_usb_is_powered());
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    static bool was_awake_before_disconnect = false;
    if (as_zmk_split_peripheral_status_changed(eh)) {
        const struct zmk_split_peripheral_status_changed *ev = as_zmk_split_peripheral_status_changed(eh);
        is_central_connected = ev->connected;
        LOG_DBG("Peripheral: Split status changed. Connected: %d", is_central_connected);
        if (!is_central_connected) {
            was_awake_before_disconnect = sleep_state.is_awake;
#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_IDLE)
            LOG_DBG("Peripheral: Disconnected from central. Re-evaluating auto-off idle state.");
            pk_underglow_auto_state(false);
#elif IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_USB)
            LOG_DBG("Peripheral: Disconnected from central. Re-evaluating auto-off usb state.");
            pk_underglow_auto_state(false);
#endif
        } else {
            if (was_awake_before_disconnect) {
                LOG_DBG("Peripheral: Reconnected and was previously awake. Waking up.");
                pk_underglow_auto_state(true);
            }
        }
        return ZMK_EV_EVENT_BUBBLE;
    }
#endif

    return -ENOTSUP;
}

ZMK_LISTENER(pk_underglow, pk_underglow_event_listener);
#endif // IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_IDLE) ||
       // IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_USB) ||
       // IS_ENABLED(UNDERGLOW_LAYER_ENABLED)

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(pk_underglow, zmk_activity_state_changed);
#endif

ZMK_SUBSCRIPTION(pk_underglow, zmk_position_state_changed);

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_USB)
ZMK_SUBSCRIPTION(pk_underglow, zmk_usb_conn_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
ZMK_SUBSCRIPTION(pk_underglow, zmk_split_peripheral_status_changed);
#endif

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
ZMK_SUBSCRIPTION(pk_underglow, zmk_layer_state_changed);
#endif
ZMK_SUBSCRIPTION(pk_underglow, zmk_underglow_color_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
void zmk_pk_underglow_sync_state(uint32_t param1, uint32_t param2) {
    // Unpack param1 (Color)
    state.color.h = param1 & 0xFFFF;
    state.color.s = (param1 >> 16) & 0xFF;
    state.color.b = (param1 >> 24) & 0xFF;

    // Unpack param2 (State & Effect)
    uint8_t old_effect = state.current_effect;
    state.current_effect = param2 & 0xFF;
    bool effect_changed = (old_effect != state.current_effect);
    
    state.effect_speeds[state.current_effect] = (param2 >> 8) & 0xFF;
    uint8_t layer = (param2 >> 16) & 0xFF;
    state.on = (param2 >> 24) & 1;
    int state_directive = (param2 >> 25) & 3;
    state.layer_enabled = (param2 >> 27) & 1;

    LOG_DBG("Peripheral: Extracted ug_sync state. Effect=%d, Hue=%d, Layer=%d, StateDirective=%d", 
            state.current_effect, state.color.h, layer, state_directive);

    // Apply the layer if the effect relies on it
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state_directive != 2) {
        zmk_pk_underglow_set_peripheral_layer(layer);
    }
#endif

    // Apply the state directive
    if (state_directive == 1) {
        zmk_pk_underglow_transient_on();
    } else if (state_directive == 2) {
        zmk_pk_underglow_transient_off();
    } else {
        // Normal sync: Ensure timer runs if the central is on
        if (state.on) {
            if (!is_powered || effect_changed) {
                zmk_pk_underglow_transient_on();
            }
        } else {
            zmk_pk_underglow_transient_off();
        }
    }
}
#endif

SYS_INIT(zmk_pk_underglow_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

struct zmk_led_hsb zmk_pk_underglow_get_color(void) {
    return state.color;
}

struct zmk_led_hsb zmk_pk_underglow_get_eff_color(void) {
    struct zmk_led_hsb hsb = state.color;
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    if (state.current_effect == UNDERGLOW_EFFECT_LAYER_SPECTRUM) {
        hsb.h = (hsb.h + state.animation_step) % HUE_MAX;
    }
#endif
    return hsb;
}

int zmk_pk_underglow_hsb_to_hex(struct zmk_led_hsb hsb) {
    struct led_rgb rgb = hsb_to_rgb(hsb);
    return (rgb.r << 16) | (rgb.g << 8) | rgb.b;
}

#if IS_ENABLED(CONFIG_PM_DEVICE)
static int pk_underglow_pm_action(const struct device *dev, enum pm_device_action action) {
    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        if (is_powered) {
            zmk_pk_underglow_off_handler(NULL);
        }
        break;
    default:
        return -ENOTSUP;
    }
    return 0;
}

static int pk_underglow_pm_init(const struct device *dev) {
    return 0;
}

PM_DEVICE_DEFINE(pk_underglow_pm, pk_underglow_pm_action);
DEVICE_DEFINE(pk_underglow_pm, "pk_underglow_pm", pk_underglow_pm_init, PM_DEVICE_GET(pk_underglow_pm), NULL, NULL, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);
#endif
