#include "../pk_underglow_internal.h"
#include <math.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/random/random.h>

#define FIREWORKS_MAX 5

struct pk_firework {
    uint8_t row;
    uint8_t col;
    uint32_t start_time;
    uint16_t duration;
    uint16_t hue;
    bool active;
};


static struct pk_firework fireworks[FIREWORKS_MAX];

static uint16_t fw_distance_lut[64][64];
static bool fw_lut_initialized = false;

static void generate_fw_lut(void) {
    for (int r = 0; r < 64; r++) {
        for (int c = 0; c < 64; c++) {
            fw_distance_lut[r][c] = (uint16_t)(sqrtf((r * r) + (c * c)) * 100);
        }
    }
    fw_lut_initialized = true;
}


static inline uint16_t pk_get_fw_distance(int dr, int dc) {
    if (!fw_lut_initialized) {
        generate_fw_lut();
    }
    int abs_r = abs(dr);
    int abs_c = abs(dc);
    if (abs_r >= 64 || abs_c >= 64)
        return 9900;
    return fw_distance_lut[abs_r][abs_c];
}


void zmk_pk_underglow_effect_fireworks(void) {
    uint32_t now = k_uptime_get_32();

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = (struct led_rgb){0, 0, 0};
    }

    for (int i = 0; i < FIREWORKS_MAX; i++) {
        if (!fireworks[i].active) {
            if ((sys_rand32_get() % 100) < 5) {
                fireworks[i].active = true;
                fireworks[i].row = sys_rand32_get() % PK_UG_MATRIX_ROWS;
                fireworks[i].col = sys_rand32_get() % PK_UG_MATRIX_COLS;
                fireworks[i].start_time = now;
                fireworks[i].duration = 600 + (sys_rand32_get() % 800); // 600ms - 1400ms duration
                fireworks[i].hue = sys_rand32_get() % HUE_MAX;
            }
            continue;
        }

        uint32_t elapsed = now - fireworks[i].start_time;
        if (elapsed >= fireworks[i].duration) {
            fireworks[i].active = false;
            continue;
        }

        uint32_t current_radius = (elapsed * 600) / fireworks[i].duration; // Max radius 6.00 * 100

        for (int p = 0; p < STRIP_NUM_PIXELS; p++) {
            uint8_t midx = rgb_pixel_lookup(p);
            if (midx == 255)
                continue;

            int p_row = midx / PK_UG_MATRIX_COLS;
            int p_col = midx % PK_UG_MATRIX_COLS;

            uint16_t dist = pk_get_fw_distance(p_row - fireworks[i].row, p_col - fireworks[i].col);
            uint32_t thickness = 150; // 1.50

            uint32_t dist_from_ring = (current_radius > dist) ? (current_radius - dist) : (dist - current_radius);

            if (dist_from_ring < thickness) {
                uint32_t intensity = 100 - ((dist_from_ring * 100) / thickness);
                uint32_t fade = 100 - ((elapsed * 100) / fireworks[i].duration);

                uint8_t b = (uint8_t)((100 * intensity * fade) / 10000);

                struct zmk_led_hsb hsb = {.h = fireworks[i].hue, .s = 100, .b = b};

                struct led_rgb rgb = hsb_to_rgb(hsb_scale_min_max(hsb));

                pixels[p].r = MIN(255, pixels[p].r + rgb.r);
                pixels[p].g = MIN(255, pixels[p].g + rgb.g);
                pixels[p].b = MIN(255, pixels[p].b + rgb.b);
            }
        }
    }
}
