/**
 * Ripple Effect
 * 
 * Visual Description:
 * Expanding shockwaves of light that radiate outwards from a specific point when triggered.
 * Similar to a drop of water hitting a pond, the wave travels across the keys, fading out as it reaches the edges.
 * Can be configured to display a solid color wave or a shifting rainbow spectrum.
 */
#include "../pk_underglow_internal.h"
#include <zephyr/kernel.h>
#include <stdlib.h>

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

static inline uint16_t pk_get_ripple_distance(int dr, int dc) {
    int abs_r = abs(dr);
    int abs_c = abs(dc);
    if (abs_r > 7 || abs_c > 22) return 9900;
    return ripple_distance_lut[abs_r][abs_c];
}

void zmk_pk_underglow_effect_ripple_trigger(uint8_t row, uint8_t col) {
    ripples[ripple_idx].row = row;
    ripples[ripple_idx].col = col;
    ripples[ripple_idx].start_time = k_uptime_get_32();
    ripples[ripple_idx].hue = (state.current_effects[active_profile_index] == UNDERGLOW_EFFECT_RAINBOW_RIPPLE) ? global_rainbow_hue : state.colors[active_profile_index].h;
    ripples[ripple_idx].active = true;
    ripple_idx = (ripple_idx + 1) % MAX_RIPPLES;
}

static void process_ripples(bool is_rainbow) {
    uint32_t now = k_uptime_get_32();
    struct zmk_led_hsb base_hsb = state.colors[active_profile_index];
    
    if (is_rainbow) {
        global_rainbow_hue = (global_rainbow_hue + state.effect_speeds[state.current_effects[active_profile_index]]) % HUE_MAX;
    }
    
    base_hsb.b = (base_hsb.b * CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS) / 100;
    
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t midx = rgb_pixel_lookup(i);
        int p_row = midx / 12;
        int p_col = midx % 12;
        
        uint8_t peak_b = base_hsb.b;
        uint16_t peak_hue = is_rainbow ? pixel_base_hues[i] : 0; 
        
        for (int r = 0; r < MAX_RIPPLES; r++) {
            if (!ripples[r].active) continue;
            uint32_t elapsed = now - ripples[r].start_time;
            
            // Animation speed governs how fast it expands
            // elapsed is in ms. speed_factor * 60 (to replace 0.6f * 100).
            uint32_t current_radius = (elapsed * state.effect_speeds[state.current_effects[active_profile_index]] * 60) / 100;
            uint32_t max_radius = 1200; // Max distance across one half of keyboard (12.0f * 100)
            
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

void zmk_pk_underglow_effect_ripple(void) {
    process_ripples(false);
}

void zmk_pk_underglow_effect_rainbow_ripple(void) {
    process_ripples(true);
}
