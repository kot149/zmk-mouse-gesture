# ZMK Mouse Gesture Module

A ZMK module that converts combinations of 8-way mouse strokes into key presses or any other behaviors.

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

### 3. Configure Input Listener

Define the gesture patterns in `&zip_mouse_gesture`and add it to the input processor of your pointing device.

```dts
#include <mouse-gesture.dtsi>

&zip_mouse_gesture {
    stroke-size = <300>; // Size of one stroke in a gesture. Note that larger stroke than this value is fine, as duplicate directions will be ignored.
    enable-8way; // Comment out to disable 8-way gesture detection and limit to 4-way only

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
    };

    volume_up {
        pattern = <GESTURE_DOWN_RIGHT GESTURE_LEFT GESTURE_UP_RIGHT>;
        bindings = <&kp C_VOLUME_UP>;
    };

    volume_down {
        pattern = <GESTURE_DOWN_LEFT GESTURE_RIGHT GESTURE_UP_LEFT>;
        bindings = <&kp C_VOLUME_DOWN>;
    };
};

&trackball_listener {
    compatible = "zmk,input-listener";
    device = <&trackball>;

    input-processors = <&zip_mouse_gesture>;
};
```

#### Available Gesture Directions

- 4-way directions
  - `GESTURE_UP` - Upward movement
  - `GESTURE_DOWN` - Downward movement
  - `GESTURE_LEFT` - Leftward movement
  - `GESTURE_RIGHT` - Rightward movement
- 8-way directions (optional)
  - `GESTURE_UP_LEFT` - Diagonal up-left movement
  - `GESTURE_UP_RIGHT` - Diagonal up-right movement
  - `GESTURE_DOWN_LEFT` - Diagonal down-left movement
  - `GESTURE_DOWN_RIGHT` - Diagonal down-right movement

### 5. Perform the gesture

Activate gesture by pressing the activation key and perform the gesture.

## Advanced Usage

- **Automatic Activation**: use [zmk-listeners](https://github.com/ssbb/zmk-listeners) to activate the gesture automatically on specific layers

- **Activate with existing keys**: use [zmk-listeners](https://github.com/ssbb/zmk-listeners) to activate the gesture with existing keys

- **Layer-specific gestures**: define [layer-spesific input processors](https://zmk.dev/docs/keymaps/input-processors/usage#layer-specific-overrides) to trigger different gestures on different layers

## How it works

While `&mouse_gesture` is pressed, `&zip_mouse_gesture` listens to mouse input events and accumulates the movement value.
When accumulated value become bigger than `stroke-size`, the processor judges its direction, then pushes it to mouse gesture sequence.
When matching gesture found for the sequence, its bindings will be invoked.
The accumulated value is reset when the direction is detected or the activation key is released.
The sequence is cleared when a gesture is detected or the activation key is released.

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

Provides trackpad-specific gestures like tap-to-click, inertial scrolling, and circular scrolling. This module focuses on trackpad interactions rather than converting general mouse movement to behaviors.
