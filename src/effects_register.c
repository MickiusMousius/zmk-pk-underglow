#include "pk_underglow_internal.h"

/*
 * ==============================================================================
 * ADDING A NEW EFFECT
 * ==============================================================================
 * To add a new effect, simply add a new entry to the `pk_underglow_effects`
 * array below.
 *
 * Each effect requires:
 *  - .name: A human-readable name (10 characters or less).
 *  - .render: The function that renders the effect.
 *
 * Optional functions:
 *  - .select: Called once when the effect is switched to.
 *  - .pos_changed: Called when a key is pressed (useful for reactive effects
 * like ripple).
 *  - .is_layer_indicator: Set to `true` if this effect renders layer
 * indicators.
 *
 * Example:
 *  {
 *      .name = "MyEffect",
 *      .render = my_custom_effect_render,
 *      .select = my_custom_effect_init
 *  },
 * ==============================================================================
 */

// Effect Prototypes
void zmk_pk_underglow_effect_solid(void);
void zmk_pk_underglow_effect_white(void);
void zmk_pk_underglow_effect_breathe(void);
void zmk_pk_underglow_effect_spectrum(void);
void zmk_pk_underglow_effect_swirl(void);

void zmk_pk_underglow_effect_ripple(void);
void zmk_pk_underglow_effect_rainbow_ripple(void);
void zmk_pk_underglow_effect_twinkle(void);
void zmk_pk_underglow_effect_rainbow_twinkle(void);
void zmk_pk_underglow_effect_pinwheel(void);
void zmk_pk_underglow_effect_layer(void);

void zmk_pk_underglow_effect_rainbow_ripple_select(void);
void zmk_pk_underglow_effect_twinkle_select(void);
void zmk_pk_underglow_effect_rainbow_twinkle_select(void);


const struct pk_underglow_effect_ops pk_underglow_effects[] = {
    {.name = "White", .render = zmk_pk_underglow_effect_white},
    {.name = "Solid", .render = zmk_pk_underglow_effect_solid},
    {.name = "Breathe", .render = zmk_pk_underglow_effect_breathe},
    {.name = "Spectrum", .render = zmk_pk_underglow_effect_spectrum},
    {.name = "Swirl", .render = zmk_pk_underglow_effect_swirl},
    {.name = "Pinwheel", .render = zmk_pk_underglow_effect_pinwheel},
    {.name = "Ripple", .render = zmk_pk_underglow_effect_ripple, .pos_changed = zmk_pk_underglow_effect_ripple_trigger},
    {.name = "RbRipple",
     .render = zmk_pk_underglow_effect_rainbow_ripple,
     .pos_changed = zmk_pk_underglow_effect_rainbow_ripple_trigger,
     .select = zmk_pk_underglow_effect_rainbow_ripple_select},
    {.name = "Twinkle", .render = zmk_pk_underglow_effect_twinkle, .select = zmk_pk_underglow_effect_twinkle_select},
    {.name = "RbTwinkle",
     .render = zmk_pk_underglow_effect_rainbow_twinkle,
     .select = zmk_pk_underglow_effect_rainbow_twinkle_select},
    {.name = "Layer", .render = zmk_pk_underglow_effect_layer, .is_layer_indicator = true},
};


const int pk_underglow_effects_count = sizeof(pk_underglow_effects) / sizeof(pk_underglow_effects[0]);
