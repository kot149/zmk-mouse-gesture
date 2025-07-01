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
#include <zephyr/sys/util_macro.h>
#include <drivers/input_processor.h>
#include <errno.h>
#include <limits.h>
#include <string.h>

#include <zmk/keymap.h>
#include <dt-bindings/zmk/mouse-gesture.h>
#include <zmk/behavior.h>
#include <zmk/behavior_queue.h>
#include <drivers/behavior.h>
#include <zmk/mouse_gesture.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define ABS(x) ((x) < 0 ? -(x) : (x))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define MAX_GESTURE_SEQUENCE_LENGTH 8
#define MAX_GESTURE_PATTERNS 16
#define MAX_DEFERRED_BINDINGS 8

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
    uint32_t gesture_cooldown_ms;  // Cooldown period between gestures
    bool enable_8way;  // true: 8 directions, false: 4 directions only
    struct gesture_pattern **patterns;  // Array of pointers to patterns
    size_t pattern_count;
};

// Deferred execution data
struct deferred_gesture_execution {
    struct k_work work;
    struct zmk_behavior_binding bindings[MAX_DEFERRED_BINDINGS];
    size_t binding_count;
    struct zmk_behavior_binding_event event;
};

struct input_processor_mouse_gesture_data {
    struct k_mutex lock;
    int32_t acc_x;
    int32_t acc_y;
    uint8_t sequence[MAX_GESTURE_SEQUENCE_LENGTH];
    uint8_t sequence_len;
    int64_t last_gesture_time;  // Timestamp of last gesture execution
    uint32_t event_count;       // Counter to detect potential loops
    int64_t last_reset_time;    // Time of last counter reset
    struct deferred_gesture_execution deferred_exec;  // Work queue item
};

static uint8_t detect_direction(int32_t x, int32_t y, bool enable_8way) {
    uint16_t abs_x = ABS(x);
    uint16_t abs_y = ABS(y);

    if (enable_8way) {
        if (abs_x * 5 > abs_y * 12) {
            return GESTURE_X(x);
        } else if (abs_y * 5 > abs_x * 12) {
            return GESTURE_Y(y);
        } else {
            return GESTURE_XY(x, y);
        }
    } else {
        if (abs_x > abs_y) {
            return GESTURE_X(x);
        } else {
            return GESTURE_Y(y);
        }
    }

    return GESTURE_NONE;
}

// Check if pattern matches and update state atomically (called while mutex is held)
static struct gesture_pattern* check_and_process_pattern_locked(const struct device *dev) {
    const struct input_processor_mouse_gesture_config *config = dev->config;
    struct input_processor_mouse_gesture_data *data = dev->data;
    int64_t current_time = k_uptime_get();

    // Early validation
    if (config->pattern_count == 0 || data->sequence_len == 0) {
        return NULL;
    }

    // Check cooldown period
    if (current_time - data->last_gesture_time < config->gesture_cooldown_ms) {
        LOG_DBG("Still in cooldown period");
        return NULL;
    }

    // Find matching pattern
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

            // Update all state atomically - no separate flag reset needed
            data->last_gesture_time = current_time;
            data->sequence_len = 0;  // Clear sequence
            // Note: NOT setting gesture_in_progress to true - keep it simple

            return (struct gesture_pattern*)pattern;
        }
    }

    return NULL;
}

// Work queue handler for deferred gesture execution
static void deferred_gesture_work_handler(struct k_work *work) {
    struct deferred_gesture_execution *exec = CONTAINER_OF(work, struct deferred_gesture_execution, work);

    LOG_DBG("Executing deferred gesture with %zu bindings", exec->binding_count);

    // Execute behaviors in work queue context (safe from deadlock)
    for (size_t k = 0; k < exec->binding_count; k++) {
        LOG_DBG("Executing deferred binding [%zu/%zu]", k + 1, exec->binding_count);

        int ret = zmk_behavior_queue_add(&exec->event, exec->bindings[k], true, k * 30);
        if (ret < 0) {
            LOG_ERR("Failed to queue deferred press event [%zu]: %d", k, ret);
            continue;
        }

        ret = zmk_behavior_queue_add(&exec->event, exec->bindings[k], false, (k * 30) + 80);
        if (ret < 0) {
            LOG_ERR("Failed to queue deferred release event [%zu]: %d", k, ret);
        }
    }

    LOG_DBG("Deferred gesture execution completed");
}

// Schedule gesture execution via work queue (completely asynchronous)
static void schedule_gesture_execution(const struct device *dev, struct gesture_pattern *pattern) {
    if (!pattern || pattern->bindings_len == 0) {
        return;
    }

    struct input_processor_mouse_gesture_data *data = dev->data;
    struct deferred_gesture_execution *exec = &data->deferred_exec;

    // Prevent work queue overflow
    if (pattern->bindings_len > MAX_DEFERRED_BINDINGS) {
        LOG_WRN("Too many bindings to defer (%zu > %d), truncating",
                pattern->bindings_len, MAX_DEFERRED_BINDINGS);
    }

    // Setup execution data
    exec->binding_count = MIN(pattern->bindings_len, MAX_DEFERRED_BINDINGS);
    memcpy(exec->bindings, pattern->bindings, exec->binding_count * sizeof(struct zmk_behavior_binding));

    exec->event.position = INT32_MAX;
    exec->event.timestamp = k_uptime_get();
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    exec->event.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
#endif

    // Submit to system work queue (completely asynchronous)
    int ret = k_work_submit(&exec->work);
    if (ret < 0) {
        LOG_ERR("Failed to submit gesture work: %d", ret);
    } else {
        LOG_DBG("Gesture execution scheduled successfully");
    }
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
    int64_t current_time = k_uptime_get();

    // Event loop protection
    if (current_time - data->last_reset_time > 1000) {  // Reset every second
        data->event_count = 0;
        data->last_reset_time = current_time;
    }

    data->event_count++;
    if (data->event_count > 1000) {  // Prevent event loops
        LOG_ERR("Too many events in short time, possible loop detected");
        data->sequence_len = 0;
        data->event_count = 0;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // Check if mouse gesture is active (early exit)
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

    if (total_distance < config->stroke_size) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint8_t direction = detect_direction(data->acc_x, data->acc_y, config->enable_8way);

    if (direction != GESTURE_NONE) {
        // Check for duplicate direction
        if (data->sequence_len > 0 && data->sequence[data->sequence_len - 1] == direction) {
            LOG_DBG("Ignoring duplicate direction %d", direction);
        } else {
            // Add direction to sequence
            if (data->sequence_len < MAX_GESTURE_SEQUENCE_LENGTH) {
                data->sequence[data->sequence_len++] = direction;
                LOG_DBG("Added direction %d to sequence (length: %d)", direction, data->sequence_len);
            } else {
                LOG_WRN("Gesture sequence too long, clearing");
                data->sequence_len = 0;
            }
        }

        // Reset accumulation for next direction
        data->acc_x = 0;
        data->acc_y = 0;
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static int input_processor_mouse_gesture_handle_event(const struct device *dev,
                                                      struct input_event *event,
                                                      uint32_t param1, uint32_t param2,
                                                      struct zmk_input_processor_state *state) {
    struct input_processor_mouse_gesture_data *data = dev->data;
    int ret = 0;

    // Single mutex operation - acquire, process, check pattern, release
    ret = k_mutex_lock(&data->lock, K_MSEC(5));
    if (ret < 0) {
        LOG_WRN("Failed to acquire mutex for gesture processing: %d", ret);
        return ZMK_INPUT_PROC_CONTINUE;
    }

    ret = input_processor_mouse_gesture_handle_event_locked(dev, event, param1, param2, state);

    // Check for pattern match and update state atomically
    struct gesture_pattern *matched_pattern = check_and_process_pattern_locked(dev);

    k_mutex_unlock(&data->lock);

    // Schedule pattern execution via work queue (deadlock-safe)
    if (matched_pattern) {
        LOG_DBG("Pattern matched, scheduling deferred execution");
        schedule_gesture_execution(dev, matched_pattern);
    }

    return ret;
}

static int input_processor_mouse_gesture_init(const struct device *dev) {
    struct input_processor_mouse_gesture_data *data = dev->data;

    k_mutex_init(&data->lock);

    data->acc_x = 0;
    data->acc_y = 0;
    data->sequence_len = 0;
    data->last_gesture_time = 0;
    data->event_count = 0;
    data->last_reset_time = k_uptime_get();

    // Initialize work queue for deferred execution
    k_work_init(&data->deferred_exec.work, deferred_gesture_work_handler);
    data->deferred_exec.binding_count = 0;

    LOG_INF("Mouse gesture input processor initialized with deferred execution");
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
        .gesture_cooldown_ms = DT_INST_PROP_OR(n, gesture_cooldown_ms, 200),        \
        .enable_8way = DT_INST_PROP_OR(n, enable_8way, false),                      \
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