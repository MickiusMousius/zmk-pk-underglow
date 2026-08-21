#include "pk_underglow_internal.h"

/*
 * ==============================================================================
 * ADDING A NEW EFFECT
 * ==============================================================================
 * To add a new effect, simply add a new entry to the `pk_underglow_effects`
 * array below.
 *
 * Each effect requires:
 *  - .name: A human-readable name (15 characters or less).
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
    {.name = "Solid Hue", .render = zmk_pk_underglow_effect_solid},
    {.name = "Breathe Hue", .render = zmk_pk_underglow_effect_breathe, .is_animated = true},
    {.name = "Spectrum", .render = zmk_pk_underglow_effect_spectrum, .is_animated = true},
    {.name = "Swirl", .render = zmk_pk_underglow_effect_swirl, .is_animated = true},
    {.name = "Pinwheel", .render = zmk_pk_underglow_effect_pinwheel, .is_animated = true},
    {.name = "Ripple",
     .render = zmk_pk_underglow_effect_ripple,
     .pos_changed = zmk_pk_underglow_effect_ripple_trigger,
     .is_animated = true},
    {.name = "Ripple Disco",
     .render = zmk_pk_underglow_effect_rainbow_ripple,
     .pos_changed = zmk_pk_underglow_effect_rainbow_ripple_trigger,
     .select = zmk_pk_underglow_effect_rainbow_ripple_select,
     .is_animated = true},
    {.name = "Twinkle",
     .render = zmk_pk_underglow_effect_twinkle,
     .select = zmk_pk_underglow_effect_twinkle_select,
     .is_animated = true},
    {.name = "Twinkle Disco",
     .render = zmk_pk_underglow_effect_rainbow_twinkle,
     .select = zmk_pk_underglow_effect_rainbow_twinkle_select,
     .is_animated = true},
    {.name = "Layer Indicator", .render = zmk_pk_underglow_effect_layer, .is_layer_indicator = true},
};


const int pk_underglow_effects_count = sizeof(pk_underglow_effects) / sizeof(pk_underglow_effects[0]);
