/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/settings/settings.h>

#include <math.h>
#include <stdlib.h>
#include <zephyr/random/random.h>

#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <drivers/ext_power.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/led_strip.h>

#include <zmk/pk_underglow.h>
#include <zmk/pk_underglow_layer.h>

#include <zmk/activity.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/underglow_color_changed.h>
#include <zmk/events/usb_conn_state_changed.h>
#include <zmk/hid_indicators.h>
#include <zmk/matrix.h>
#include <zmk/usb.h>

#include <zmk/events/layer_state_changed.h>
#include <zmk/pk_underglow_queue.h>
#include <zmk/workqueue.h>

#include <zmk/endpoints.h>
#include <zmk/events/endpoint_changed.h>

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

#include "pk_underglow_internal.h"

/* ==========================================================================
 * 1. INTERNAL STATE & VARIABLES
 * ========================================================================== */

uint16_t global_rainbow_hue = 0;
uint16_t pixel_base_hues[STRIP_NUM_PIXELS];

static const struct device *led_strip;

struct led_rgb pixels[STRIP_NUM_PIXELS];

struct pk_underglow_state state;

uint8_t active_profile_index = 0;

static uint8_t get_active_profile(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    struct zmk_endpoint_instance endpoint = zmk_endpoints_selected();
    int index = zmk_endpoint_instance_to_index(endpoint);
    if (index < 0 || index >= ZMK_ENDPOINT_COUNT) {
        return 0;
    }
    return (uint8_t)index;
#else
    return 0;
#endif
}


static bool is_powered = false;
static uint64_t power_on_uptime = 0;

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_EXT_POWER)
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_pk_underglow_layer)
static const struct gpio_dt_spec power_gpio =
    GPIO_DT_SPEC_GET_OR(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), power_gpios, {0});
#else
static const struct gpio_dt_spec power_gpio = {0};
#endif

/* ==========================================================================
 * 2. BACKGROUND WORK QUEUE HANDLERS
 * ========================================================================== */

void pk_ug_task_render_frame_execute(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    uint8_t new_profile = get_active_profile();
    if (active_profile_index != new_profile) {
        active_profile_index = new_profile;

        // Re-initialize the effect for the new profile
        zmk_pk_underglow_select_effect(state.current_effects[active_profile_index]);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        pk_ug_queue_push_sync(pk_underglow_top_layer(), 0);
#endif
    }
#endif

    if (pk_underglow_effects[state.current_effects[active_profile_index]].render) {
        pk_underglow_effects[state.current_effects[active_profile_index]].render();
    }

    int err = led_strip_update_rgb(led_strip, pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the RGB strip (%d)", err);
    }
}


static void zmk_pk_underglow_tick_handler(struct k_timer *timer) {
    if (!state.on) {
        return;
    }
    pk_ug_queue_push(PK_UG_TASK_RENDER_FRAME);
}


K_TIMER_DEFINE(underglow_tick, zmk_pk_underglow_tick_handler, NULL);

#if IS_ENABLED(CONFIG_SETTINGS) && (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
static int rgb_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;
    int rc;

    if (settings_name_steq(name, "state", &next) && !next) {
        if (len != sizeof(state)) {
            LOG_WRN("Underglow state size mismatch (got %d, expected %d). Ignoring.", len, sizeof(state));
            return 0;
        }

        rc = read_cb(cb_arg, &state, sizeof(state));
        if (rc >= 0) {
            active_profile_index = get_active_profile();

            if (pk_underglow_effects[state.current_effects[active_profile_index]].select) {
                pk_underglow_effects[state.current_effects[active_profile_index]].select();
            }

            if (state.on) {
                zmk_pk_underglow_transient_on();
            } else {
                zmk_pk_underglow_transient_off();
            }
            if (state.layer_enabled) {
                zmk_pk_underglow_set_layer(pk_underglow_top_layer(), true);
            }
            return 0;
        }

        return rc;
    }

    return -ENOENT;
}


SETTINGS_STATIC_HANDLER_DEFINE(pk_underglow, "rgb/underglow", NULL, rgb_settings_set, NULL, NULL);

static void zmk_pk_underglow_save_state_work(struct k_work *_work) { pk_ug_queue_push(PK_UG_TASK_SAVE_SETTINGS); }

void pk_ug_task_save_settings_execute(void) { settings_save_one("rgb/underglow/state", &state, sizeof(state)); }

static struct k_work_delayable underglow_save_work;
#else
void pk_ug_task_save_settings_execute(void) {}
#endif

/* ==========================================================================
 * 3. SYSTEM INITIALIZATION
 * ========================================================================== */

static int zmk_pk_underglow_init(void) {
    pk_ug_queue_init();

    int min_row = 999, max_row = -1;
    int min_col = 999, max_col = -1;

    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        uint8_t midx = rgb_pixel_lookup(i);
        int r = midx / PK_UG_MATRIX_COLS;
        int c = midx % PK_UG_MATRIX_COLS;
        if (r < min_row)
            min_row = r;
        if (r > max_row)
            max_row = r;
        if (c < min_col)
            min_col = c;
        if (c > max_col)
            max_col = c;
    }

    if (STRIP_NUM_PIXELS > 0) {
#if DT_HAS_COMPAT_STATUS_OKAY(zmk_pk_underglow_layer) &&                                                               \
    DT_NODE_HAS_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), center_column)
        center_col = DT_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), center_column);
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
        // Central side (or unibody): Top Left fallback
        center_col = min_col;
#else
        // Peripheral side: Top Right fallback
        center_col = max_col + 1;
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_pk_underglow_layer) &&                                                               \
    DT_NODE_HAS_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), center_row)
        center_row = DT_PROP(DT_COMPAT_GET_ANY_STATUS_OKAY(zmk_pk_underglow_layer), center_row);
#else
        center_row = min_row;
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

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    state = (struct pk_underglow_state){animation_step : 0, on : false};
#else
    state = (struct pk_underglow_state){animation_step : 0, on : IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_ON_START)};
#endif

    active_profile_index = get_active_profile();

    for (int i = 0; i < ZMK_ENDPOINT_COUNT; i++) {
        state.colors[i] = (struct zmk_led_hsb){
            .h = CONFIG_ZMK_PK_UNDERGLOW_HUE_START,
            .s = CONFIG_ZMK_PK_UNDERGLOW_SAT_START,
            .b = CONFIG_ZMK_PK_UNDERGLOW_BRT_START,
        };
        state.current_effects[i] = CONFIG_ZMK_PK_UNDERGLOW_EFF_START;
    }

    for (int i = 0; i < pk_underglow_effects_count; i++) {
        state.effect_speeds[i] = CONFIG_ZMK_PK_UNDERGLOW_SPD_START;
    }

    // Initialize the starting effect so that its internal state is properly
    // primed (necessary for the peripheral half, which does not load settings
    // from flash)
    if (pk_underglow_effects[state.current_effects[active_profile_index]].select) {
        pk_underglow_effects[state.current_effects[active_profile_index]].select();
    }

#if IS_ENABLED(CONFIG_SETTINGS) && (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
    k_work_init_delayable(&underglow_save_work, zmk_pk_underglow_save_state_work);
#endif

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_USB)
    state.on = zmk_usb_is_powered();
#endif

    if (state.on) {
        zmk_pk_underglow_transient_on();
    }
    if (state.layer_enabled) {
        zmk_pk_underglow_set_layer(pk_underglow_top_layer(), true);
    }
    return 0;
}


/* ==========================================================================
 * 4. PUBLIC API & STATE MUTATORS
 * ========================================================================== */

int zmk_pk_underglow_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS) && (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL))
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
    state.on = true;
    if (pk_underglow_effects[state.current_effects[active_profile_index]].is_layer_indicator) {
        state.layer_enabled = true;
    }
    return zmk_pk_underglow_save_state();
}


int zmk_pk_underglow_transient_on(void) {
    if (!led_strip)
        return -ENODEV;

    state.animation_step = 0;

    pk_ug_queue_push(PK_UG_TASK_POWER_ON);
    k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(PK_UG_FRAME_DURATION));

    return 0;
}


void pk_ug_task_power_on_execute(void) {
    if (is_powered) {
        return;
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

    k_sleep(K_MSEC(PK_UG_POWER_STABILIZATION_MS));

    is_powered = true;
    power_on_uptime = k_uptime_get();
}


void pk_ug_task_power_off_execute(void) {
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

    pk_ug_queue_push(PK_UG_TASK_POWER_OFF);

    k_timer_stop(&underglow_tick);

    return 0;
}


int zmk_pk_underglow_calc_effect(int direction) {
    return (state.current_effects[active_profile_index] + pk_underglow_effects_count + direction) %
           pk_underglow_effects_count;
}


int zmk_pk_underglow_select_effect(int effect) {
    if (!led_strip)
        return -ENODEV;

    if (effect < 0 || effect >= pk_underglow_effects_count) {
        return -EINVAL;
    }

    state.current_effects[active_profile_index] = effect;
    state.animation_step = 0;

    if (pk_underglow_effects[effect].select) {
        pk_underglow_effects[effect].select();
    }

    state.layer_enabled = pk_underglow_effects[effect].is_layer_indicator;

    LOG_INF("Selected effect: %d, layer_enabled: %d, state.on: %d", effect, state.layer_enabled, state.on);

    if (state.layer_enabled) {
        zmk_pk_underglow_set_layer(pk_underglow_top_layer(), false);
    } else if (state.on) {
        LOG_INF("Restarting animation timer for effect %d", effect);
        zmk_pk_underglow_transient_on();
    }
    return zmk_pk_underglow_save_state();
}


int zmk_pk_underglow_cycle_effect(int direction) {
    return zmk_pk_underglow_select_effect(zmk_pk_underglow_calc_effect(direction));
}


int zmk_pk_underglow_toggle(void) { return state.on ? zmk_pk_underglow_off() : zmk_pk_underglow_on(); }

void zmk_pk_underglow_set_layer(uint8_t layer, bool wakeup) {
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
        }

        k_timer_stop(&underglow_tick);
        state.animation_step = 0;
        int fade_delay = zmk_rgbmap_fade_delay(layer);
        bool animated = zmk_rgbmap_is_animated(layer);
        if (fade_delay > 0) {
            k_timer_start(&underglow_tick, K_SECONDS(fade_delay), K_MSEC(PK_UG_FRAME_DURATION));
        } else if (animated || fade_delay == 0) {
            k_timer_start(&underglow_tick, K_MSEC(PK_UG_FRAME_DURATION), K_MSEC(PK_UG_FRAME_DURATION));
        }

        // Enqueue a render frame to the background thread to write the pixels.
        // If power is turning on, this will safely execute AFTER the power stabilizes.
        pk_ug_queue_push(PK_UG_TASK_RENDER_FRAME);
    } else {
        zmk_pk_underglow_transient_off();
    }
}


int zmk_pk_underglow_set_hsb(struct zmk_led_hsb color) {
    if (color.h > HUE_MAX || color.s > SAT_MAX || color.b > BRT_MAX) {
        return -ENOTSUP;
    }

    state.colors[active_profile_index] = color;

    if (state.layer_enabled) {
        zmk_pk_underglow_set_layer(pk_underglow_top_layer(), false);
    }

    return zmk_pk_underglow_save_state();
}


struct zmk_led_hsb zmk_pk_underglow_calc_hue(int direction) {
    struct zmk_led_hsb color = state.colors[active_profile_index];

    color.h += HUE_MAX + (direction * CONFIG_ZMK_PK_UNDERGLOW_HUE_STEP);
    color.h %= HUE_MAX;

    return color;
}


struct zmk_led_hsb zmk_pk_underglow_calc_sat(int direction) {
    struct zmk_led_hsb color = state.colors[active_profile_index];

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
    struct zmk_led_hsb color = state.colors[active_profile_index];

    int b = color.b + (direction * CONFIG_ZMK_PK_UNDERGLOW_BRT_STEP);
    color.b = CLAMP(b, 0, BRT_MAX);

    return color;
}


int zmk_pk_underglow_change_hue(int direction) {
    if (!led_strip)
        return -ENODEV;
    return zmk_pk_underglow_set_hsb(zmk_pk_underglow_calc_hue(direction));
}


int zmk_pk_underglow_change_sat(int direction) {
    if (!led_strip)
        return -ENODEV;
    return zmk_pk_underglow_set_hsb(zmk_pk_underglow_calc_sat(direction));
}


int zmk_pk_underglow_change_brt(int direction) {
    if (!led_strip)
        return -ENODEV;
    return zmk_pk_underglow_set_hsb(zmk_pk_underglow_calc_brt(direction));
}


int zmk_pk_underglow_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.effect_speeds[state.current_effects[active_profile_index]] == 1 && direction < 0) {
        return 0;
    }

    state.effect_speeds[state.current_effects[active_profile_index]] += direction;

    if (state.effect_speeds[state.current_effects[active_profile_index]] > 5) {
        state.effect_speeds[state.current_effects[active_profile_index]] = 5;
    }

    return zmk_pk_underglow_save_state();
}


/* ==========================================================================
 * 5. EVENT LISTENERS & AUTO STATE
 * ========================================================================== */

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_IDLE) || IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_USB)
struct pk_underglow_sleep_state {
    bool is_awake;
    bool rgb_state_before_sleeping;
};


static struct pk_underglow_sleep_state sleep_state = {.is_awake = true, .rgb_state_before_sleeping = false};

void pk_ug_task_sync_state_execute(uint8_t layer, uint8_t state_directive) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    uint32_t param1 = (state.colors[active_profile_index].h & 0xFFFF) |
                      ((state.colors[active_profile_index].s & 0xFF) << 16) |
                      ((state.colors[active_profile_index].b & 0xFF) << 24);
    uint32_t param2 = (state.current_effects[active_profile_index] & 0xFF) |
                      ((state.effect_speeds[state.current_effects[active_profile_index]] & 0xFF) << 8) |
                      ((layer & 0xFF) << 16) | ((state.on ? 1 : 0) << 24) | ((state_directive & 0x03) << 25) |
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

static int sync_retries = 0;

static void sync_peripheral_delayed_work_handler(struct k_work *work);

static K_WORK_DELAYABLE_DEFINE(sync_peripheral_delayed_work, sync_peripheral_delayed_work_handler);

static void sync_peripheral_delayed_work_handler(struct k_work *work) {
    uint8_t layer = pk_underglow_top_layer();
    pk_ug_queue_push_sync(layer, 0); // Layer sync
#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_IDLE)
    if (!sleep_state.is_awake) {
        pk_ug_queue_push_sync(layer, 2); // Sleep sync
    }
#endif

    // Retry the sync a few times to ensure it succeeds even if ZMK drops
    // the initial asynchronous messages due to slow GATT discovery.
    if (sync_retries < 3) {
        sync_retries++;
        k_work_schedule(&sync_peripheral_delayed_work, K_MSEC(PK_UG_SYNC_RETRY_MS));
    }
}


static void pk_underglow_bt_connected(struct bt_conn *conn, uint8_t err) {
    if (err) {
        return;
    }

    sync_retries = 0;
    // Send the first sync, and retry 3 more times if needed
    k_work_schedule(&sync_peripheral_delayed_work, K_MSEC(PK_UG_SYNC_DELAY_MS));
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
    pk_ug_queue_push_sync(pk_underglow_top_layer(), target_wake_state ? 1 : 2);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) && IS_ENABLED(CONFIG_BT)
    // Force sending a standard payload shortly after waking, simulating a layer
    // change. This allows the peripheral's power circuit ample time to stabilize
    // before receiving the final color state, ensuring the LEDs latch the data
    // correctly.
    if (target_wake_state) {
        k_work_schedule(&sync_peripheral_delayed_work, K_MSEC(PK_UG_WAKE_SYNC_DELAY_MS));
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_DBG("Peripheral: Checking if central is connected (is_central_connected=%d)", is_central_connected);
    if (is_central_connected && zmk_activity_get_state() != ZMK_ACTIVITY_SLEEP) {
        LOG_DBG("Peripheral: Central is connected, deferring auto_state to central "
                "sync.");
        return 0;
    }
    LOG_DBG("Peripheral: Central NOT connected or entering deep sleep, "
            "self-managing idle state.");
#endif

    if (sleep_state.is_awake) {
        if (state.layer_enabled) {
            zmk_pk_underglow_set_layer(pk_underglow_top_layer(), true);
            return 0;
        }
#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        return 0;
#else
        if (sleep_state.rgb_state_before_sleeping) {
            return zmk_pk_underglow_transient_on();
        } else {
            return zmk_pk_underglow_transient_off();
        }
#endif
    } else {
        sleep_state.rgb_state_before_sleeping = state.on;
        return zmk_pk_underglow_transient_off();
    }
}


#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
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

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    if (as_zmk_layer_state_changed(eh)) {
        if (!sleep_state.is_awake) {
            return ZMK_EV_EVENT_BUBBLE;
        }
        const struct zmk_layer_state_changed *ev = as_zmk_layer_state_changed(eh);
        LOG_DBG("zmk_layer_state_changed: %08x", ev->state);
        uint8_t layer = pk_underglow_top_layer();
        zmk_pk_underglow_set_layer(layer, true);
        pk_ug_queue_push_sync(layer, 0);

        return ZMK_EV_EVENT_BUBBLE;
    }
#endif
    if (as_zmk_underglow_color_changed(eh)) {
        const struct zmk_underglow_color_changed *ev = as_zmk_underglow_color_changed(eh);
        uint8_t layer = pk_underglow_top_layer();
        LOG_DBG("refresh layers %d, current: %d, wakeup: %d", ev->layers, layer, ev->wakeup);
        if (ev->layers & BIT(layer)) {
            zmk_pk_underglow_set_layer(pk_underglow_top_layer(), ev->wakeup);
        }
        return 0;
    }

    if (as_zmk_position_state_changed(eh)) {
        if (state.on && pk_underglow_effects[state.current_effects[active_profile_index]].pos_changed) {
            const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
            if (ev->state && ev->source == ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
                uint8_t row = ev->position / PK_UG_MATRIX_COLS;
                uint8_t col = ev->position % PK_UG_MATRIX_COLS;
                pk_underglow_effects[state.current_effects[active_profile_index]].pos_changed(row, col);
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
            LOG_DBG("Peripheral: Disconnected from central. Re-evaluating auto-off "
                    "idle state.");
            pk_underglow_auto_state(false);
#elif IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_USB)
            LOG_DBG("Peripheral: Disconnected from central. Re-evaluating auto-off "
                    "usb state.");
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

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
ZMK_SUBSCRIPTION(pk_underglow, zmk_layer_state_changed);
#endif
ZMK_SUBSCRIPTION(pk_underglow, zmk_underglow_color_changed);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
static int zmk_pk_underglow_endpoint_changed(const zmk_event_t *eh) {
    uint8_t new_profile = get_active_profile();
    if (active_profile_index != new_profile) {
        active_profile_index = new_profile;

        // Re-initialize the effect for the new profile (calls .select, updates layer_enabled, resets animation)
        zmk_pk_underglow_select_effect(state.current_effects[active_profile_index]);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
        pk_ug_queue_push_sync(pk_underglow_top_layer(), 0);
#endif
    }
    return ZMK_EV_EVENT_BUBBLE;
}


ZMK_LISTENER(pk_underglow_endpoint, zmk_pk_underglow_endpoint_changed);
ZMK_SUBSCRIPTION(pk_underglow_endpoint, zmk_endpoint_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
void zmk_pk_underglow_sync_state(uint32_t param1, uint32_t param2) {
    // Unpack param1 (Color)
    state.colors[active_profile_index].h = param1 & 0xFFFF;
    state.colors[active_profile_index].s = (param1 >> 16) & 0xFF;
    state.colors[active_profile_index].b = (param1 >> 24) & 0xFF;

    // Unpack param2 (State & Effect)
    uint8_t old_effect = state.current_effects[active_profile_index];
    state.current_effects[active_profile_index] = param2 & 0xFF;
    bool effect_changed = (old_effect != state.current_effects[active_profile_index]);

    state.effect_speeds[state.current_effects[active_profile_index]] = (param2 >> 8) & 0xFF;
    uint8_t layer = (param2 >> 16) & 0xFF;
    state.on = (param2 >> 24) & 1;
    int state_directive = (param2 >> 25) & 3;
    state.layer_enabled = (param2 >> 27) & 1;

    LOG_DBG("Peripheral: Extracted ug_sync state. Effect=%d, Hue=%d, Layer=%d, "
            "StateDirective=%d",
            state.current_effects[active_profile_index], state.colors[active_profile_index].h, layer, state_directive);

    // Apply the layer if the effect relies on it
    if (state_directive != 2) {
        zmk_pk_underglow_set_peripheral_layer(layer);
    }

    // Apply the state directive
    if (state_directive == 1) {
        zmk_pk_underglow_transient_on();
    } else if (state_directive == 2) {
        zmk_pk_underglow_transient_off();
    } else {
        // Normal sync
        if (state.layer_enabled) {
            // Do nothing regarding power. The call to zmk_pk_underglow_set_peripheral_layer()
            // above has already definitively turned the LEDs ON or OFF based on the layer map.
        } else if (state.on) {
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

struct zmk_led_hsb zmk_pk_underglow_get_color(void) { return state.colors[active_profile_index]; }

const char *zmk_pk_underglow_get_effect_name(void) {
    return pk_underglow_effects[state.current_effects[active_profile_index]].name;
}


uint8_t zmk_pk_underglow_get_speed(void) { return state.effect_speeds[state.current_effects[active_profile_index]]; }


bool zmk_pk_underglow_is_on(void) { return state.on; }


int zmk_pk_underglow_hsb_to_hex(struct zmk_led_hsb hsb) {
    struct led_rgb rgb = hsb_to_rgb(hsb);
    return (rgb.r << 16) | (rgb.g << 8) | rgb.b;
}


#if IS_ENABLED(CONFIG_PM_DEVICE)
static int pk_underglow_pm_action(const struct device *dev, enum pm_device_action action) {
    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        if (is_powered) {
            pk_ug_task_power_off_execute();
        }
        break;
    default:
        return -ENOTSUP;
    }
    return 0;
}


static int pk_underglow_pm_init(const struct device *dev) { return 0; }

PM_DEVICE_DEFINE(pk_underglow_pm, pk_underglow_pm_action);
DEVICE_DEFINE(pk_underglow_pm, "pk_underglow_pm", pk_underglow_pm_init, PM_DEVICE_GET(pk_underglow_pm), NULL, NULL,
              POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);
#endif
