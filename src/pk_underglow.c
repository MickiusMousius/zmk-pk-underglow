/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <math.h>
#include <stdlib.h>

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

#define HUE_MAX 360
#define SAT_MAX 100
#define BRT_MAX 100

BUILD_ASSERT(CONFIG_ZMK_PK_UNDERGLOW_BRT_MIN <= CONFIG_ZMK_PK_UNDERGLOW_BRT_MAX,
             "ERROR: RGB underglow maximum brightness is less than minimum brightness");

enum pk_underglow_effect {
    UNDERGLOW_EFFECT_SOLID,
    UNDERGLOW_EFFECT_BREATHE,
    UNDERGLOW_EFFECT_SPECTRUM,
    UNDERGLOW_EFFECT_SWIRL,
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    UNDERGLOW_EFFECT_LAYER_INDICATORS,
#endif
    UNDERGLOW_EFFECT_NUMBER // Used to track number of underglow effects
};

struct pk_underglow_state {
    struct zmk_led_hsb color;
    uint8_t animation_speed;
    uint8_t current_effect;
    uint16_t animation_step;
    bool on;
    bool layer_enabled;
};

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
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i] = hsb_to_rgb(hsb_scale_min_max(state.color));
    }
}

static void zmk_pk_underglow_effect_breathe(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.b = abs(state.animation_step - 1200) / 12;

        pixels[i] = hsb_to_rgb(hsb_scale_zero_max(hsb));
    }

    state.animation_step += state.animation_speed * 10;

    if (state.animation_step > 2400) {
        state.animation_step = 0;
    }
}

static void zmk_pk_underglow_effect_spectrum(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = state.animation_step;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed;
    state.animation_step = state.animation_step % HUE_MAX;
}

static void zmk_pk_underglow_effect_swirl(void) {
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        struct zmk_led_hsb hsb = state.color;
        hsb.h = (HUE_MAX / STRIP_NUM_PIXELS * i + state.animation_step) % HUE_MAX;

        pixels[i] = hsb_to_rgb(hsb_scale_min_max(hsb));
    }

    state.animation_step += state.animation_speed * 2;
    state.animation_step = state.animation_step % HUE_MAX;
}

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
static void zmk_pk_underglow_effect_layer(void) {
    bool active = false;
    for (int i = 0; i < STRIP_NUM_PIXELS; i++) {
        pixels[i].r -= state.animation_speed < pixels[i].r ? state.animation_speed : pixels[i].r;
        pixels[i].g -= state.animation_speed < pixels[i].g ? state.animation_speed : pixels[i].g;
        pixels[i].b -= state.animation_speed < pixels[i].b ? state.animation_speed : pixels[i].b;
        if (pixels[i].r || pixels[i].g || pixels[i].b) {
            active = true;
        }
    }
    state.animation_step += state.animation_speed;

    if (state.animation_step > 255 || !active) {
        zmk_pk_underglow_transient_off();
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
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    case UNDERGLOW_EFFECT_LAYER_INDICATORS:
        zmk_pk_underglow_effect_layer();
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
                k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
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
        animation_speed : CONFIG_ZMK_PK_UNDERGLOW_SPD_START,
        current_effect : CONFIG_ZMK_PK_UNDERGLOW_EFF_START,
        animation_step : 0,
        on : IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_ON_START)
    };

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&underglow_save_work, zmk_pk_underglow_save_state_work);
#endif

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_USB)
    state.on = zmk_usb_is_powered();
#endif

    if (state.on) {
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
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
        k_timer_start(&underglow_tick, K_NO_WAIT, K_MSEC(50));
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
    k_timer_start(&underglow_tick, K_MSEC(10), K_MSEC(50));

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
        r : (hsb.b * (r)) / 0xff,
        g : (hsb.b * (g)) / 0xff,
        b : (hsb.b * (b)) / 0xff
    };
}

static int zmk_pk_underglow_apply_rgbmap(const struct zmk_behavior_binding *bindings,
                                          size_t rgbmap_len, uint8_t layer) {
    int rc = 0;
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
                .position = midx, .layer = layer, .timestamp = k_uptime_get()};

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
        if (fade_delay >= 0) {
            k_timer_start(&underglow_tick, K_SECONDS(fade_delay), K_MSEC(50));
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

    return zmk_pk_underglow_save_state();
}

int zmk_pk_underglow_change_sat(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_pk_underglow_calc_sat(direction);

    return zmk_pk_underglow_save_state();
}

int zmk_pk_underglow_change_brt(int direction) {
    if (!led_strip)
        return -ENODEV;

    state.color = zmk_pk_underglow_calc_brt(direction);

    return zmk_pk_underglow_save_state();
}

int zmk_pk_underglow_change_spd(int direction) {
    if (!led_strip)
        return -ENODEV;

    if (state.animation_speed == 1 && direction < 0) {
        return 0;
    }

    state.animation_speed += direction;

    if (state.animation_speed > 5) {
        state.animation_speed = 5;
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
                      ((state.animation_speed & 0xFF) << 8) | 
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

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    LOG_DBG("Peripheral: Checking if central is connected (is_central_connected=%d)", is_central_connected);
    if (is_central_connected) {
        LOG_DBG("Peripheral: Central is connected, deferring auto_state to central sync.");
        return 0;
    }
    LOG_DBG("Peripheral: Central NOT connected, self-managing idle state.");
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

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh)) {
        return pk_underglow_auto_state(zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    }
#endif

#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) || !IS_ENABLED(CONFIG_ZMK_SPLIT)
    if (as_zmk_layer_state_changed(eh)) {
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

#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_USB)
    if (as_zmk_usb_conn_state_changed(eh)) {
        return pk_underglow_auto_state(zmk_usb_is_powered());
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_SPLIT) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (as_zmk_split_peripheral_status_changed(eh)) {
        const struct zmk_split_peripheral_status_changed *ev = as_zmk_split_peripheral_status_changed(eh);
        is_central_connected = ev->connected;
        LOG_DBG("Peripheral: Split status changed. Connected: %d", is_central_connected);
        if (!is_central_connected) {
#if IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_IDLE)
            LOG_DBG("Peripheral: Disconnected from central. Re-evaluating auto-off idle state.");
            pk_underglow_auto_state(zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
#elif IS_ENABLED(CONFIG_ZMK_PK_UNDERGLOW_AUTO_OFF_USB)
            LOG_DBG("Peripheral: Disconnected from central. Re-evaluating auto-off usb state.");
            pk_underglow_auto_state(zmk_usb_is_powered());
#endif
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
    state.current_effect = param2 & 0xFF;
    state.animation_speed = (param2 >> 8) & 0xFF;
    uint8_t layer = (param2 >> 16) & 0xFF;
    state.on = (param2 >> 24) & 1;
    int state_directive = (param2 >> 25) & 3;
    state.layer_enabled = (param2 >> 27) & 1;

    LOG_DBG("Peripheral: Extracted ug_sync state. Effect=%d, Hue=%d, Layer=%d, StateDirective=%d", 
            state.current_effect, state.color.h, layer, state_directive);

    // Apply the layer if the effect relies on it
#if IS_ENABLED(UNDERGLOW_LAYER_ENABLED)
    zmk_pk_underglow_set_layer(layer, false);
#endif

    // Apply the state directive
    if (state_directive == 1) {
        zmk_pk_underglow_transient_on();
    } else if (state_directive == 2) {
        zmk_pk_underglow_transient_off();
    } else {
        // Normal sync: Ensure timer runs if the central is on
        if (state.on) {
            zmk_pk_underglow_transient_on();
        } else {
            zmk_pk_underglow_transient_off();
        }
    }
}
#endif

SYS_INIT(zmk_pk_underglow_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
