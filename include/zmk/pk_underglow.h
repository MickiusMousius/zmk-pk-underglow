/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

struct zmk_led_hsb {
    uint16_t h;
    uint8_t s;
    uint8_t b;
};

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100

int zmk_pk_underglow_toggle(void);
int zmk_pk_underglow_get_state(bool *state);
int zmk_pk_underglow_on(void);
int zmk_pk_underglow_off(void);
int zmk_pk_underglow_transient_on(void);
int zmk_pk_underglow_transient_off(void);
int zmk_pk_underglow_cycle_effect(int direction);
int zmk_pk_underglow_calc_effect(int direction);
int zmk_pk_underglow_select_effect(int effect);
struct zmk_led_hsb zmk_pk_underglow_calc_hue(int direction);
struct zmk_led_hsb zmk_pk_underglow_calc_sat(int direction);
struct zmk_led_hsb zmk_pk_underglow_calc_brt(int direction);
int zmk_pk_underglow_change_hue(int direction);
int zmk_pk_underglow_change_sat(int direction);
int zmk_pk_underglow_change_brt(int direction);
int zmk_pk_underglow_change_spd(int direction);
int zmk_pk_underglow_set_hsb(struct zmk_led_hsb color);

struct zmk_led_hsb zmk_pk_underglow_get_color(void);
struct zmk_led_hsb zmk_pk_underglow_get_eff_color(void);
int zmk_pk_underglow_hsb_to_hex(struct zmk_led_hsb hsb);

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
void zmk_pk_underglow_set_peripheral_layer(uint8_t layer);
void zmk_pk_underglow_sync_state(uint32_t param1, uint32_t param2);
#endif
