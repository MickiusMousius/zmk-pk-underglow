/**
 * Pinwheel Effect
 *
 * Visual Description:
 * A spinning gradient of hues radiating outwards from a central point (such as the top right key).
 * The colors sweep around the center in a continuous circular motion, creating a dynamic, rotating "pinwheel" of light.
 */
#include "../pk_underglow_internal.h"
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define PINWHEEL_ROW_SPREAD (PK_UG_MATRIX_ROWS * 2)
#define PINWHEEL_COL_SPREAD (PK_UG_MATRIX_COLS * 2)

/**
 * Calculates the hue angle (0-360) for a given point relative to a center.
 * 
 * This lookup table maps the coordinate distance (dr, dc) to an angle. It is 
 * dynamically generated on the first run using the exact physical dimensions 
 * of the board (PK_UG_MATRIX_ROWS and PK_UG_MATRIX_COLS). The table spans the 
 * maximum possible positive and negative distances across the matrix.
 * This eliminates floating-point math during the hot rendering loop while
 * guaranteeing the table perfectly fits any keyboard layout.
 */
static uint16_t pinwheel_angle_lut[PINWHEEL_ROW_SPREAD][PINWHEEL_COL_SPREAD];
static bool pinwheel_lut_initialized = false;

static void generate_pinwheel_lut(void) {
    for (int dr = -PK_UG_MATRIX_ROWS; dr < PK_UG_MATRIX_ROWS; dr++) {
        for (int dc = -PK_UG_MATRIX_COLS; dc < PK_UG_MATRIX_COLS; dc++) {
            int r_idx = dr + PK_UG_MATRIX_ROWS;
            int c_idx = dc + PK_UG_MATRIX_COLS;
            
            float angle = atan2f((float)dr, (float)dc) + M_PI;
            pinwheel_angle_lut[r_idx][c_idx] = (uint16_t)(((angle) / (2.0f * M_PI)) * 360.0f) % 360;
        }
    }
    pinwheel_lut_initialized = true;
}

static inline uint16_t pk_get_pinwheel_angle(int dr, int dc) {
    if (!pinwheel_lut_initialized) {
        generate_pinwheel_lut();
    }

    int idx_r = dr + PK_UG_MATRIX_ROWS;
    int idx_c = dc + PK_UG_MATRIX_COLS;
    
    if (idx_r < 0)
        idx_r = 0;
    else if (idx_r >= PINWHEEL_ROW_SPREAD)
        idx_r = PINWHEEL_ROW_SPREAD - 1;
        
    if (idx_c < 0)
        idx_c = 0;
    else if (idx_c >= PINWHEEL_COL_SPREAD)
        idx_c = PINWHEEL_COL_SPREAD - 1;
        
    return pinwheel_angle_lut[idx_r][idx_c];
}

void zmk_pk_underglow_effect_pinwheel(void) {
    // Increment the animation step to make it spin!
    // 0.5 revs/sec at max speed (5) = 180 degrees/sec.
    // Timer runs at ~15 ticks/sec (67ms). 180 / 15 = 12 degrees/tick.
    state.animation_step += (state.effect_speeds[state.current_effects[active_profile_index]] * 12) / 5;
    state.animation_step = state.animation_step % HUE_MAX;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t midx = rgb_pixel_lookup(i);
        int p_row = midx / PK_UG_MATRIX_COLS;
        int p_col = midx % PK_UG_MATRIX_COLS;

        int dr = p_row - center_row;
        int dc = p_col - center_col;
        uint16_t angle_hue = pk_get_pinwheel_angle(dr, dc);

        struct zmk_led_hsb pixel_hsb = state.colors[active_profile_index];
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
        pixel_hsb.h = (angle_hue + state.animation_step) % HUE_MAX;
#else
        pixel_hsb.h = (angle_hue + HUE_MAX - (state.animation_step % HUE_MAX)) % HUE_MAX;
#endif

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(pixel_hsb));
    }
}
