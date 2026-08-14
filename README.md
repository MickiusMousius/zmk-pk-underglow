# pk_underglow Module

## What it does
The `pk_underglow` module is a powerful extension for ZMK that allows you to define per-key RGB underglow colors and effects. It enables you to precisely control the lighting of each individual LED on your keyboard and dynamically change the lighting based on the active layer, connection state, or custom effects.

## Why use it?
Standard keyboard backlighting applies a single global color or effect across the entire board. `pk_underglow` gives you fine-grained, per-key control. You can use it to highlight specific key clusters (e.g., WASD for gaming, navigation keys for mouse layers), provide visual feedback for connection status on specific keys, or create complex layer-dependent visual maps.

## Key Features
- **Per-Key RGB Mapping:** Assign specific colors or dynamic effects to individual keys, independent of the global underglow effect.
- **Layer-Based Lighting:** Define unique lighting layouts for different keymap layers. Lighting automatically updates when layers become active.
- **Dynamic Connection Indicators:** Use specific behaviors to show Bluetooth and USB connection status directly on your keys (e.g., pulsing when active, solid when connected, off when disconnected).
- **Per-Profile State Tracking:** The module automatically stores and restores your current underglow color, active effect, and animation speed for each individual output profile (USB and Bluetooth 0-4). You can configure a unique aesthetic for each connected device, and the lighting will seamlessly update when you switch profiles.
- **Per-Effect Animation Speeds:** Each global effect tracks its own independent animation speed. You can configure a slow, relaxing Breathe effect alongside a fast, responsive Ripple effect without constantly adjusting a global speed setting.

- **Split Keyboard Synchronization:** Colors and effects are seamlessly synchronized between central and peripheral halves of split keyboards.

## Architecture & Performance Optimization
This module was engineered from the ground up to minimize computational overhead and maximize energy efficiency, particularly for wireless split keyboards running Zephyr:

- **Modular Effect Subsystem:** The core `pk_underglow.c` acts strictly as a hardware manager—handling Zephyr power states, peripheral synchronization, layer map updates, and dispatching tasks. Each visual effect (e.g., `twinkle.c`, `layer.c`) is fully encapsulated, decoupling heavy mathematical rendering from hardware state management.
- **Deduplicated Work Queue:** To ensure thread safety and prevent processor bottlenecks, all backlight update requests (e.g., rendering frames, powering on/off, or syncing layers) are dispatched to a centralized work queue. The queue aggressively deduplicates redundant sync and render requests so the processor doesn't waste cycles drawing identical frames or spamming the Bluetooth link.
- **Fast Integer Math:** Intensive floating-point operations (like HSB-to-RGB color space conversions) have been completely replaced with highly optimized integer math and fast bit-shifting. This significantly reduces CPU load, yielding smoother animations and better battery life.
- **Precomputed Lookup Tables (LUTs):** Complex trigonometric animations—like Twinkle's pulsing sine waves, Pinwheel's angular sweeps, and Ripple's radial distance calculations—are driven by static, precomputed C-arrays (LUTs) rather than live mathematical calculations on the MCU.
- **Efficient Peripheral Synchronization:** Keeping split halves perfectly in sync without saturating the Bluetooth link is challenging. This module implements a targeted sync protocol that only transmits minimal, discrete state changes (like a new active layer index or a sleep state transition) to the peripheral side, ensuring both halves reflect the exact same visuals without wasting energy transmitting high-frequency color payloads.

---

## Supported Effects
The `pk_underglow` module supports both standard ZMK underglow animations and heavily optimized custom effects.

### Standard ZMK Effects
- **`UNDERGLOW_EFFECT_SOLID`**: A static, uniform color applied across the entire keyboard. Provides a clean and consistent backlight without any animation.
- **`UNDERGLOW_EFFECT_BREATHE`**: A smooth, pulsing animation where the entire keyboard gracefully fades in and out. The brightness oscillates between a minimum and maximum threshold to simulate a slow, rhythmic "breathing" motion.
- **`UNDERGLOW_EFFECT_SPECTRUM`**: A smooth transition through the entire color spectrum. The entire keyboard changes color simultaneously, slowly shifting from red to green to blue and back again.
- **`UNDERGLOW_EFFECT_SWIRL`**: A rolling wave of colors that flows continuously across the keyboard. Creates a dynamic gradient where the hue shifts based on the physical position of the LEDs, simulating a flowing river of rainbow light.

### Custom Reactive & Dynamic Effects
- **`UNDERGLOW_EFFECT_WHITE`**: A specialized static effect that sets all LEDs to pure white. It completely bypasses the standard hue and saturation processing for maximum brightness and pure illumination. Saturation can be adjusted via Kconfig.
- **`UNDERGLOW_EFFECT_RIPPLE`**: Expanding shockwaves of light that radiate outwards from the specific key you pressed. Similar to a drop of water hitting a pond, the wave travels across the keys, fading out as it reaches the edges. Fully isolated per physical keyboard half.
- **`UNDERGLOW_EFFECT_RAINBOW_RIPPLE`**: A reactive effect where each keypress shoots out a ripple matching a dynamically cycling global rainbow hue. LEDs permanently absorb the color of the last passing ripple, creating a vibrant, evolving canvas as you type.
- **`UNDERGLOW_EFFECT_TWINKLE`**: Simulates a starry night sky. Individual LEDs randomly light up (twinkle) and then slowly fade back into a dimmer background state. Creates a subtle, sparkling appearance across the keyboard. Driven entirely by static LUTs for near-zero CPU overhead.
- **`UNDERGLOW_EFFECT_RAINBOW_TWINKLE`**: Twinkling stars that lock in the dynamically cycling background hue the moment they are born, melting back into the background when they fade out.
- **`UNDERGLOW_EFFECT_PINWHEEL`**: A spinning gradient of hues radiating outwards from a central point. The colors sweep around the center in a continuous circular motion, creating a dynamic, rotating "pinwheel" of light perfectly centered on each keyboard half.
- **`UNDERGLOW_EFFECT_LAYER_INDICATORS`**: Bypasses global animations to highlight specific keys based on the currently active ZMK layer. Allows you to color-code functional groupings (e.g. Navigation, Numpad, Symbols) while leaving the rest of the board dim. A standout feature is the ability to freely mix and match behaviors within the same layer map: you can combine static custom colors, dynamic shifted hue cells (that track the global hue), and real-time state indicators (like Bluetooth profiles or USB status) all on a single active layer. Fully integrates with the Devicetree configuration mappings.

---

## Creating Custom Effects
This module was designed to make it incredibly simple to add your own custom underglow effects in C. All effects are decoupled from the hardware state management.

To add a new effect:
1. Create a new `.c` file in the `src/effects/` directory (e.g., `my_effect.c`).
2. Write your render function which modifies the `pixels` array directly.
3. Open `src/effects_register.c` and add a new entry to the `pk_underglow_effects` array:

```c
const struct pk_underglow_effect_ops pk_underglow_effects[] = {
    // ... existing effects ...
    {
        .name = "MyEffect",
        .render = zmk_pk_underglow_effect_my_effect,
        // Optional functions:
        // .select = zmk_pk_underglow_effect_my_effect_init,
        // .pos_changed = zmk_pk_underglow_effect_my_effect_trigger
    },
};
```

Each effect requires:
- `.name`: A human-readable name (10 characters or less).
- `.render`: The function that runs every frame (67ms) to update the LEDs.

Optional functions:
- `.select`: Called once when the user cycles to this effect. Great for initializing state or resetting variables.
- `.pos_changed`: Called whenever a key is pressed. Perfect for reactive effects like ripple or twinkle.
- `.is_layer_indicator`: Set to `true` if this effect renders a layer map instead of a global animation.

---

# Installation

To use this module, you need to add it to your ZMK user config repository.

Open your `config/west.yml` file and add this repository to your `remotes` and `projects` sections:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    # Add your GitHub username & organization here where the module is hosted
    - name: your_github_username
      url-base: https://github.com/your_github_username

  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    # Add the pk_underglow module here
    - name: zmk-pk-underglow
      remote: your_github_username
      revision: main

  self:
    path: config
```

## Configuration

To use the module, you will need to update your board or shield's `.dts` or `.overlay` file to declare the `zmk_pk_underglow_layer` node and ensure your hardware LED strip is correctly configured as the standard `zmk,underglow` chosen node.

### 1. Hardware Requirements & `chosen` Node
The `pk_underglow` module controls LEDs using ZMK's standard underglow subsystem. Therefore, you must first have an LED strip (like WS2812) configured in your board's device tree. 

Your root `/` block must contain a `chosen` node pointing `zmk,underglow` to your LED strip:
```dts
/ {
    chosen {
        zmk,underglow = &led_strip;
    };
};
```

### 2. The `pk_underglow` Node
Next, you must add the `pk_underglow` driver node to your device tree (e.g., in your shield's `.overlay` or `.dtsi` file). This node tells the module how to map matrix key positions to actual LED indices on your physical hardware strip.

```dts
/ {
    pk_underglow: pk_underglow {
        compatible = "zmk,pk-underglow-layer";
        
        // Optional: A GPIO pin to enable/disable power to the LEDs
        power-gpios = <&gpio0 28 GPIO_ACTIVE_HIGH>; 
        
        // Maps the index of your physical keys (top-left to bottom-right) 
        // to the physical index of the LED on the LED strip chain.
        // Example: The 0th key is lit by the 5th LED in the strip chain.
        pixel-lookup = <
             5  4  3  2  1  0
            12 13 14 15 16 17
            29 28 27 26 25 24
            36 37 38 39 40
        >;
    };
};
```

### 3. Enable in `Kconfig`
Enable the feature in your `board.conf` or `shield.conf` (or globally in your `zmk.conf`):
```ini
CONFIG_ZMK_PK_UNDERGLOW=y

# (Optional) Set the saturation for the custom White global effect (0-100)
CONFIG_ZMK_PK_UNDERGLOW_WHITE_SATURATION=5

# (Optional) Set the maximum number of concurrent twinkling stars for the Twinkle effect
CONFIG_ZMK_PK_UNDERGLOW_TWINKLE_MAX=5

# (Optional) Set the ambient background brightness percentage (0-100) for reactive effects (Ripple, Twinkle)
CONFIG_ZMK_PK_UNDERGLOW_AMBIENT_BRIGHTNESS=5
```

---

# How to Use in Your Keymap

Add a reference to `&pk_underglow` in your `.keymap` file. Inside this node, define your lighting layers. Each lighting layer matches one or more of your standard keymap layers via the `layer-id` property.

### Example Keymap Definition:

```dts
&pk_underglow {
    layer_default {
        // Matches multiple standard layers (e.g., MAC, WINDOWS, SYMBOLS)
        layer-id = <0 1 2 3>;
        bindings = <
            &eff_hue 45  &eff_hue 0   &eff_hue 0   &eff_hue 0
            &eff_hue 45  &eff_hue 0   &eff_hue 0   &eff_hue 0
            &eff_hue 45  &eff_hue 45  &eff_hue 45  &eff_hue 45
        >;
    };

    layer_mouse {
        // Matches a specific custom layer
        layer-id = <4>;
        bindings = <
            &eff_hue 45  &ug GREEN    &ug PURPLE   &ug PINK
            &eff_hue 45  &ug TEAL     &eff_hue 0   &eff_hue 0
            &eff_hue 45  &ug GREEN    &ug TEAL     &eff_hue 45
        >;
    };

    layer_settings {
        layer-id = <5>;
        animated; // Ensures dynamic behaviors like &eff_bt pulse correctly
        bindings = <
            &eff_usb     &eff_bt 0    &eff_bt 1    &eff_bt 2
            &ug BLACK    &ug PURPLE   &ug BLACK    &ug PURPLE
            &ug BLACK    &ug GREEN    &ug GREEN    &ug RED
        >;
    };
};
```

## Supported Behaviors

The module provides several behaviors that can be mapped to individual LEDs in the `bindings` array:

### `&ug COLOR_NAME` or `&ug 0xRRGGBB` (Solid Color)
Sets the key to a static, solid color regardless of global effects. 
- You can use predefined names: `GREEN`, `PURPLE`, `RED`, `TEAL`, `PINK`, `GOLD`, `ORANGE`, `BLACK` (turns the LED off).
- You can use hex codes: `&ug 0x0078d7`.

### `&eff_hue OFFSET` (Moving Hue Offset)
Retrieves the *current global hue* of your keyboard and adds the `OFFSET` parameter (in degrees, 0-359).
- **How it works with global effects:** If your keyboard is running an animated global effect (like Spectrum or Swirl), the global hue is constantly changing. Using `&eff_hue` on your keys with different offsets (e.g. `&eff_hue 0`, `&eff_hue 15`, `&eff_hue 30`) will create a cascading wave or rolling rainbow effect across those specific keys.
- **How it works statically:** If your keyboard is running a solid global effect, `&eff_hue` will simply display a static color shifted by your offset from the base global color.

### `&eff_bt PROFILE_INDEX` (Bluetooth Indicator)
Dynamically indicates the status of a specific Bluetooth profile (0-4). This is extremely useful for a Settings or Bluetooth layer.
- **Pulsing Green:** The profile is currently connected *and* is the active output.
- **Solid Green:** The profile is connected in the background, but is *not* the active output.
- **Black/Off:** The profile has no active connection.

### `&eff_usb` (USB Indicator)
Dynamically indicates the status of the physical USB connection.
- **Pulsing Teal (#14B8A6):** USB is connected *and* is the active output.
- **Solid Teal:** USB is connected in the background, but is *not* the active output (e.g., you are typing over Bluetooth while charging).
- **Black/Off:** USB is disconnected.

> **Note:** For layers utilizing dynamic pulsing effects like `&eff_bt` or `&eff_usb`, make sure to include the `animated;` property in the layer definition (as seen in `layer_settings` above).

# Attribution

Significant portions of the core per-key underglow rendering logic in this module are based on ZMK Pull Request #2752 by darknao.

The original pull request can be found here: https://github.com/zmkfirmware/zmk/pulls/2752

This repository preserves the original MIT license.
