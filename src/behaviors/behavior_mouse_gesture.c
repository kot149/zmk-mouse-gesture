/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_mouse_gesture

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <drivers/behavior.h>

#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct behavior_mouse_gesture_config {
    // No configuration needed
};

struct behavior_mouse_gesture_data {
    bool is_active;
};

static struct behavior_mouse_gesture_data global_gesture_state = {.is_active = false};

// Public function to get gesture state
bool zmk_mouse_gesture_is_active(void) {
    return global_gesture_state.is_active;
}

static int behavior_mouse_gesture_init(const struct device *dev) {
    struct behavior_mouse_gesture_data *data = dev->data;
    data->is_active = false;

    LOG_INF("Mouse gesture behavior initialized");
    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_mouse_gesture_data *data = dev->data;

    LOG_DBG("Mouse gesture activated");

    data->is_active = true;
    global_gesture_state.is_active = true;

    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    struct behavior_mouse_gesture_data *data = dev->data;

    LOG_DBG("Mouse gesture deactivated");

    data->is_active = false;
    global_gesture_state.is_active = false;

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_mouse_gesture_driver_api = {
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
};

#define MOUSE_GESTURE_INST(n)                                                  \
    static struct behavior_mouse_gesture_data behavior_mouse_gesture_data_##n = { \
        .is_active = false,                                                    \
    };                                                                         \
    static const struct behavior_mouse_gesture_config behavior_mouse_gesture_config_##n = {}; \
    BEHAVIOR_DT_INST_DEFINE(n, behavior_mouse_gesture_init, NULL,             \
                            &behavior_mouse_gesture_data_##n,                  \
                            &behavior_mouse_gesture_config_##n, POST_KERNEL,   \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,               \
                            &behavior_mouse_gesture_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MOUSE_GESTURE_INST)

#endif
