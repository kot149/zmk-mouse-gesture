/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_mouse_gesture

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>
#include <zephyr/input/input.h>
#include <zephyr/sys/util.h>
#include <drivers/input_processor.h>
#include <errno.h>
#include <limits.h>

#include <zmk/keymap.h>
#include <dt-bindings/zmk/mouse-gesture.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <drivers/behavior.h>
#include <zmk/mouse_gesture.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ABS(x) ((x) < 0 ? -(x) : (x))

#define MAX_GESTURE_SEQUENCE_LENGTH 8
#define MAX_GESTURE_PATTERNS 16

// Gesture pattern definition (from behavior)
struct gesture_pattern {
    size_t bindings_len;
    struct zmk_behavior_binding *bindings;
    size_t gesture_len;
    uint8_t gesture[];  // Variable length array at end
};

struct input_processor_mouse_gesture_config {
    uint32_t stroke_size;
    uint32_t movement_threshold;
    struct gesture_pattern **patterns;  // Array of pointers to patterns
    size_t pattern_count;
};

struct input_processor_mouse_gesture_data {
    struct k_mutex lock;
    int32_t acc_x;
    int32_t acc_y;
    uint8_t sequence[MAX_GESTURE_SEQUENCE_LENGTH];
    uint8_t sequence_len;
};

static uint8_t detect_direction(int32_t x, int32_t y) {
    uint16_t abs_x = ABS(x);
    uint16_t abs_y = ABS(y);

    if (abs_x * 5 > abs_y * 12) {
        return GESTURE_X(x);
    } else if (abs_y * 5 > abs_x * 12) {
        return GESTURE_Y(y);
    } else {
        return GESTURE_XY(x, y);
    }

    return GESTURE_NONE;
}

// Check if current sequence matches any configured patterns
static bool match_gesture_pattern(const struct device *dev) {
    const struct input_processor_mouse_gesture_config *config = dev->config;
    struct input_processor_mouse_gesture_data *data = dev->data;

    if (config->pattern_count == 0) {
        LOG_DBG("Skipping gesture pattern matching: No gesture patterns configured");
        return false;
    }

    if (data->sequence_len == 0) {
        LOG_DBG("Skipping gesture pattern matching: No gesture sequence");
        return false;
    }

    for (size_t i = 0; i < config->pattern_count; i++) {
        const struct gesture_pattern *pattern = config->patterns[i];

        if (pattern->gesture_len != data->sequence_len) {
            continue;
        }

        bool match = true;
        for (size_t j = 0; j < pattern->gesture_len; j++) {
            if (pattern->gesture[j] != data->sequence[j]) {
                match = false;
                break;
            }
        }

        if (match) {
            LOG_INF("Gesture pattern matched: %zu", i);

            // Clear state immediately to prevent re-entry
            data->sequence_len = 0;

            // Execute bindings using behavior queue for non-blocking execution
            struct zmk_behavior_binding_event binding_event = {
                .position = INT32_MAX,
                .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
                .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
            };

            // Queue binding execution to prevent blocking
            for (size_t k = 0; k < pattern->bindings_len; k++) {
                LOG_DBG("Queueing binding [%d]", k);
                // Press event (immediate)
                int ret = zmk_behavior_queue_add(&binding_event, pattern->bindings[k], true, 0);
                if (ret < 0) {
                    LOG_ERR("Failed to queue press event: %d", ret);
                } else {
                    // Release event with delay (non-blocking)
                    ret = zmk_behavior_queue_add(&binding_event, pattern->bindings[k], false, 10);
                    if (ret < 0) {
                        LOG_ERR("Failed to queue release event: %d", ret);
                    }
                }
            }

            return true;
        }
    }

    LOG_DBG("No gesture pattern matched");

    return false;
}

// Safe accumulation with overflow protection
static int accumulate_movement_safe(int32_t *accumulator, int32_t delta, const char* axis) {
    // Check for overflow
    if ((*accumulator > 0 && delta > INT32_MAX - *accumulator) ||
        (*accumulator < 0 && delta < INT32_MIN - *accumulator)) {
        LOG_WRN("Movement accumulator overflow on %s axis, resetting (acc=%d, delta=%d)",
                axis, *accumulator, delta);
        *accumulator = delta;
        return -EOVERFLOW;
    }

    *accumulator += delta;
    return 0;
}

static int input_processor_mouse_gesture_handle_event_locked(const struct device *dev,
                                                      struct input_event *event,
                                                      uint32_t param1, uint32_t param2,
                                                      struct zmk_input_processor_state *state) {
    struct input_processor_mouse_gesture_data *data = dev->data;
    const struct input_processor_mouse_gesture_config *config = dev->config;
    int ret = 0;

    // Check if mouse gesture is active
    if (!zmk_mouse_gesture_is_active()) {
        data->acc_x = 0;
        data->acc_y = 0;
        data->sequence_len = 0;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // Only process relative x/y events
    if (!(event->type == INPUT_EV_REL && (event->code == INPUT_REL_X || event->code == INPUT_REL_Y))) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // Cut off small movements
    if (ABS(event->value) < config->movement_threshold) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // Accumulate with overflow protection
    if (event->code == INPUT_REL_X) {
        accumulate_movement_safe(&data->acc_x, event->value, "X");
    } else if (event->code == INPUT_REL_Y) {
        accumulate_movement_safe(&data->acc_y, event->value, "Y");
    }

    // Check for direction detection
    uint32_t total_distance = ABS(data->acc_x) + ABS(data->acc_y);

    LOG_DBG("Accumulation: %s=%d => acc=(%d,%d), total_dist=%d, thresh=%d",
            (event->code == INPUT_REL_X) ? "X" : "Y", event->value, data->acc_x, data->acc_y, total_distance, config->stroke_size);

    if (total_distance < config->stroke_size) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint8_t direction = detect_direction(data->acc_x, data->acc_y);

    if (direction != GESTURE_NONE) {
        LOG_DBG("Direction detected: %d", direction);

        // Add direction to sequence
        if (data->sequence_len < MAX_GESTURE_SEQUENCE_LENGTH) {
            data->sequence[data->sequence_len++] = direction;

            LOG_DBG("Added direction %d to sequence (length: %d)", direction, data->sequence_len);

            ret = match_gesture_pattern(dev);
            if (ret) {
                LOG_DBG("Gesture pattern matched, clearing sequence");
                data->sequence_len = 0;
            }
        } else {
            LOG_WRN("Gesture sequence too long, clearing");
            data->sequence_len = 0;
        }

        // Reset accumulation for next direction
        data->acc_x = 0;
        data->acc_y = 0;

        LOG_DBG("Accumulation reset for next direction");
    } else {
        LOG_DBG("No significant direction detected");
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static int input_processor_mouse_gesture_handle_event(const struct device *dev,
                                                      struct input_event *event,
                                                      uint32_t param1, uint32_t param2,
                                                      struct zmk_input_processor_state *state) {
    struct input_processor_mouse_gesture_data *data = dev->data;
    int ret = 0;

    ret = k_mutex_lock(&data->lock, K_MSEC(100));
    if (ret < 0) {
        LOG_ERR("Error locking for gesture processing %d", ret);
        return ZMK_INPUT_PROC_CONTINUE;
    }

    ret = input_processor_mouse_gesture_handle_event_locked(dev, event, param1, param2, state);

    k_mutex_unlock(&data->lock);

    return ret;
}

static int input_processor_mouse_gesture_init(const struct device *dev) {
    struct input_processor_mouse_gesture_data *data = dev->data;

    k_mutex_init(&data->lock);

    data->acc_x = 0;
    data->acc_y = 0;
    data->sequence_len = 0;

    LOG_INF("Mouse gesture input processor initialized");
    return 0;
}


static struct zmk_input_processor_driver_api input_processor_mouse_gesture_driver_api = {
    .handle_event = input_processor_mouse_gesture_handle_event,
};

// Device tree binding transformation (from behavior)
#define TRANSFORMED_BINDINGS(n)                                                                    \
    { LISTIFY(DT_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), n) }

// Gesture pattern instance creation
#define GESTURE_PATTERN_INST(n)                                                                    \
    static struct zmk_behavior_binding                                                             \
        gesture_pattern_config_##n##_bindings[DT_PROP_LEN(n, bindings)] =                         \
            TRANSFORMED_BINDINGS(n);                                                               \
                                                                                                   \
    static struct gesture_pattern gesture_pattern_cfg_##n = {                                      \
        .bindings_len = DT_PROP_LEN(n, bindings),                                                  \
        .bindings = gesture_pattern_config_##n##_bindings,                                         \
        .gesture_len = DT_PROP_LEN(n, gesture),                                                    \
        .gesture = DT_PROP(n, gesture),                                                            \
    };

// Apply to all child nodes
DT_INST_FOREACH_CHILD(0, GESTURE_PATTERN_INST)

// Create array of pattern pointers
#define GESTURE_PATTERN_ITEM(n) &gesture_pattern_cfg_##n,
#define GESTURE_PATTERN_UTIL_ONE(n) 1 +

static struct gesture_pattern *gesture_patterns[] = {DT_INST_FOREACH_CHILD(0, GESTURE_PATTERN_ITEM)};

#define PATTERN_COUNT (DT_INST_FOREACH_CHILD(0, GESTURE_PATTERN_UTIL_ONE) 0)

#define MOUSE_GESTURE_INPUT_PROCESSOR_INST(n)                                       \
    static struct input_processor_mouse_gesture_data                                \
        input_processor_mouse_gesture_data_##n = {};                                \
    static struct input_processor_mouse_gesture_config                              \
        input_processor_mouse_gesture_config_##n = {                                \
        .stroke_size = DT_INST_PROP_OR(n, stroke_size, 1000),                       \
        .movement_threshold = DT_INST_PROP_OR(n, movement_threshold, 10),           \
        .patterns = gesture_patterns,                                               \
        .pattern_count = PATTERN_COUNT,                                             \
    };                                                                              \
    DEVICE_DT_INST_DEFINE(n, input_processor_mouse_gesture_init, NULL,              \
                          &input_processor_mouse_gesture_data_##n,                  \
                          &input_processor_mouse_gesture_config_##n, POST_KERNEL,   \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                          &input_processor_mouse_gesture_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MOUSE_GESTURE_INPUT_PROCESSOR_INST)

#endif