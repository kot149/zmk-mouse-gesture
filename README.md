# ZMK Mouse Gesture Module

A ZMK module that converts combinations of 4-direction mouse strokes into key presses or any other behaviors.

> [!warning]
> 🚧 This module is still under development. 🚧
>
> Its behavior is not stable and may cause the keyboard to crash. Its behavior and API may change without notice.

## Installation

Add the Module to your `west.yml`.

```yml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: kot149
      url-base: https://github.com/kot149
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-mouse-gesture
      remote: kot149
      revision: v1
  self:
    path: config
```

## Usage

### 1. Include `mouse-gesture.dtsi`

```c
#include <mouse-gesture.dtsi>
```

### 2. Add activation key
```dts
#include <mouse-gesture.dtsi>

/ {
    keymap {
        compatible = "zmk,keymap";

        default_layer {
            bindings = <
                &mouse_gesture // Activates mouse gesture while it is pressed
                &mouse_gesture_on // Activates mouse gesture on each press
                &mouse_gesture_off // Deactivates mouse gesture on each press
                &mouse_gesture_toggle // Toggle mouse gesture on/off on each press
            >;
        };
    };
};
```

### 3. Configure Input Processor

Define the gesture patterns in `&zip_mouse_gesture`and add it to the input processor of your pointing device.

```dts
#include <mouse-gesture.dtsi>

&zip_mouse_gesture {
    // stroke-size = <300>;
    // enable-eager-mode;
    // idle-timeout-ms = <1000>;

    history_back {
        pattern = <GESTURE_RIGHT>;
        bindings = <&kp LA(LEFT)>;
    };

    history_forward {
        pattern = <GESTURE_LEFT>;
        bindings = <&kp LA(RIGHT)>;
    };

    close_tab {
        pattern = <GESTURE_DOWN GESTURE_RIGHT>;
        bindings = <&kp LC(W)>;
    };

    new_tab {
        pattern = <GESTURE_DOWN GESTURE_LEFT>;
        bindings = <&kp LC(T)>;
        wait-ms = <20>;  // Optional: wait time between behaviors (default: CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS)
        tap-ms = <40>;   // Optional: press duration for each behavior (default: CONFIG_ZMK_MACRO_DEFAULT_TAP_MS)
    };
};

&trackball_listener {
    compatible = "zmk,input-listener";
    device = <&trackball>;

    input-processors = <&zip_mouse_gesture>;
};
```

#### Options

- `stroke-size`: Size of one stroke in a gesture. Note that larger stroke than this value is fine, as duplicate directions will be ignored.
- `enable-eager-mode`: Execute bindings immediately when gesture pattern is matched. When not set, bindings are executed when activation key is released.
- `idle-timeout-ms`: Time in milliseconds to wait for idle before invoking gesture. When set to 0 (default), idle timeout is disabled. This can invoke gesture earlier than the release of activation key when eager mode is not enabled.

### 5. Perform the gesture

Activate gesture by pressing the activation key and perform the gesture.

## Advanced Usage

- **Automatic Activation**: use [zmk-listeners](https://github.com/ssbb/zmk-listeners) to activate the gesture automatically on specific layers

- **Activate with existing keys**: use [zmk-listeners](https://github.com/ssbb/zmk-listeners) to activate the gesture with existing keys

- **Layer-specific gestures**: define [layer-spesific input processors](https://zmk.dev/docs/keymaps/input-processors/usage#layer-specific-overrides) to trigger different gestures on different layers

## How it works

While `&mouse_gesture` is pressed, `&zip_mouse_gesture` listens to mouse input events and accumulates the movement value.
When accumulated value become bigger than `stroke-size`, the processor judges its direction, then pushes it to mouse gesture sequence.
When matching gesture found for the sequence, its bindings will be invoked according to the configured execution mode:

**Default Mode (enable-eager-mode NOT specified):**
- Pattern matching and binding execution occur only when gesture recognition becomes inactive (i.e., when the activation key is released)
- This allows you to perform complex gesture sequences without interruption
- The complete accumulated gesture sequence is evaluated at the end

**Eager Mode (enable-eager-mode specified):**
- Pattern matching occurs in real-time during gesture input
- Bindings are executed immediately when a gesture pattern is matched
- This provides instant feedback and faster gesture execution, but increases computational cost
- Complex gesture may be interrupted by smaller one

**Idle Timeout Mode (idle-timeout-ms > 0 and enable-eager-mode NOT specified):**
- Pattern matching and binding execution occur when mouse movement becomes idle for the specified duration
- This allows gesture execution without releasing the activation key, useful for continuous gesture operations
- The idle timer is reset on each mouse movement and starts after the first gesture direction is detected
- Provides a middle ground between default mode and eager mode

The accumulated value is reset when the direction is detected or the activation key is released.
The sequence is cleared when a gesture is detected, idle timeout triggers, or the activation key is released.

## Related Works

### zmk-input-processor-keybind (by [te9no](https://github.com/te9no/zmk-input-processor-keybind) and [zettaface](https://github.com/zettaface/zmk-input-processor-keybind))

Converts mouse movement into key presses (e.g., arrow keys).

While both keybind and mouse-gesture modules handle mouse move events and trigger behaviors, they serve different purposes:

- **keybind**: Direct, continuous conversion of mouse movement to key presses
- **mouse-gesture**: Pattern recognition that triggers single-shot behaviors based on gesture sequences

### [input-processor-behaviors](https://zmk.dev/docs/keymaps/input-processors/behaviors)

Official ZMK input processor that converts mouse button presses into behaviors.
Unlike keybind and mouse-gesture modules, this feature is intended to process mouse button clicks rather than mouse movement; It does not quantize mouse movement or consider its direction.

### [zmk-input-gestures](https://github.com/halfdane/zmk-input-gestures)

Provides trackpad-specific gestures like tap-to-click, inertial cursor, and circular scrolling. This module focuses on trackpad interactions rather than converting general mouse movement to behaviors.
