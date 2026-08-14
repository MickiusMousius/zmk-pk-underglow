/**
 * Ripple Effect
 *
 * Visual Description:
 * Expanding shockwaves of light that radiate outwards from a specific point
 * when triggered. Similar to a drop of water hitting a pond, the wave travels
 * across the keys, fading out as it reaches the edges. Can be configured to
 * display a solid color wave or a shifting rainbow spectrum.
 */
#include "../pk_underglow_internal.h"
#include <math.h>
#include <stdlib.h>
#include <zephyr/kernel.h>

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

/**
 * Calculates the Euclidean distance between two points on the key matrix.
 *
 * This lookup table maps the coordinate distance (dr, dc) to a radial distance
 * scaled by 100. It is dynamically generated on the first run using the exact
 * physical dimensions of the board (PK_UG_MATRIX_ROWS and PK_UG_MATRIX_COLS).
 * This eliminates floating-point math during the hot rendering loop while
 * guaranteeing the table perfectly fits any keyboard layout.
 */
static uint16_t ripple_distance_lut[PK_UG_MATRIX_ROWS * 2][PK_UG_MATRIX_COLS * 2];
static bool ripple_lut_initialized = false;

static void generate_ripple_lut(void) {
    for (int r = 0; r < PK_UG_MATRIX_ROWS * 2; r++) {
        for (int c = 0; c < PK_UG_MATRIX_COLS * 2; c++) {
            ripple_distance_lut[r][c] = (uint16_t)(sqrtf((r * r) + (c * c)) * 100);
        }
    }
    ripple_lut_initialized = true;
}


static inline uint16_t pk_get_ripple_distance(int dr, int dc) {
    if (!ripple_lut_initialized) {
        generate_ripple_lut();
    }

    int abs_r = abs(dr);
    int abs_c = abs(dc);
    if (abs_r >= PK_UG_MATRIX_ROWS * 2 || abs_c >= PK_UG_MATRIX_COLS * 2)
        return 9900;
    return ripple_distance_lut[abs_r][abs_c];
}


static void trigger_ripple(uint8_t row, uint8_t col, bool is_rainbow) {
    ripples[ripple_idx].row = row;
    ripples[ripple_idx].col = col;
    ripples[ripple_idx].start_time = k_uptime_get_32();
    ripples[ripple_idx].hue = is_rainbow ? global_rainbow_hue : state.colors[active_profile_index].h;
    ripples[ripple_idx].active = true;
    ripple_idx = (ripple_idx + 1) % MAX_RIPPLES;
}


void zmk_pk_underglow_effect_ripple_trigger(uint8_t row, uint8_t col) { trigger_ripple(row, col, false); }

void zmk_pk_underglow_effect_rainbow_ripple_trigger(uint8_t row, uint8_t col) { trigger_ripple(row, col, true); }

static void process_ripples(bool is_rainbow) {
    uint32_t now = k_uptime_get_32();
    struct zmk_led_hsb base_hsb = state.colors[active_profile_index];

    if (is_rainbow) {
        global_rainbow_hue =
            (global_rainbow_hue + state.effect_speeds[state.current_effects[active_profile_index]]) % HUE_MAX;
    }

    base_hsb.b = (base_hsb.b * CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS) / 100;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t midx = rgb_pixel_lookup(i);
        int p_row = midx / PK_UG_MATRIX_COLS;
        int p_col = midx % PK_UG_MATRIX_COLS;

        uint8_t peak_b = base_hsb.b;
        uint16_t peak_hue = is_rainbow ? pixel_base_hues[i] : 0;

        for (int r = 0; r < MAX_RIPPLES; r++) {
            if (!ripples[r].active)
                continue;
            uint32_t elapsed = now - ripples[r].start_time;

            // Animation speed governs how fast it expands
            // elapsed is in ms. speed_factor * 60 (to replace 0.6f * 100).
            uint32_t current_radius =
                (elapsed * state.effect_speeds[state.current_effects[active_profile_index]] * 60) / 100;
            uint32_t max_radius = (PK_UG_MATRIX_COLS * 100); // Max distance across the keyboard

            if (current_radius > max_radius) {
                ripples[r].active = false;
                continue;
            }

            uint16_t dist = pk_get_ripple_distance(p_row - ripples[r].row, p_col - ripples[r].col);
            uint32_t thickness = 200; // 2.0f * 100

            uint32_t dist_from_ring = (current_radius > dist) ? (current_radius - dist) : (dist - current_radius);

            if (dist_from_ring < thickness) {
                // intensity: 0 to 100
                uint32_t intensity = 100 - ((dist_from_ring * 100) / thickness);
                // fade: 0 to 100
                uint32_t fade = 100 - ((current_radius * 100) / max_radius);

                uint8_t ripple_b = (uint8_t)((state.colors[active_profile_index].b * intensity * fade) / 10000);

                if (ripple_b > peak_b) {
                    peak_b = ripple_b;
                    if (is_rainbow) {
                        peak_hue = ripples[r].hue;
                        pixel_base_hues[i] = ripples[r].hue;
                    }
                }
            }
        }

        struct zmk_led_hsb pixel_hsb = state.colors[active_profile_index];
        if (is_rainbow) {
            pixel_hsb.h = peak_hue;
        }
        pixel_hsb.b = peak_b;
        pixels[i] = hsb_to_rgb(hsb_scale_min_max(pixel_hsb));
    }
}


void zmk_pk_underglow_effect_ripple(void) { process_ripples(false); }

void zmk_pk_underglow_effect_rainbow_ripple(void) { process_ripples(true); }

void zmk_pk_underglow_effect_rainbow_ripple_select(void) {
    global_rainbow_hue = state.colors[active_profile_index].h;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixel_base_hues[i] = state.colors[active_profile_index].h;
    }
}
