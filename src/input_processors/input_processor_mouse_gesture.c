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
#include <zmk/events/mouse_gesture_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define IDLE_THREAD_STACK_SIZE 512
#define IDLE_THREAD_PRIORITY K_PRIO_COOP(7)

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
    size_t pattern_len;
    uint32_t wait_ms;
    uint32_t tap_ms;
    uint8_t pattern[];  // Variable length array at end
};

struct input_processor_mouse_gesture_config {
    uint32_t stroke_size;
    uint32_t movement_threshold;
    uint32_t gesture_cooldown_ms;  // Cooldown period between gestures
    bool enable_eager_mode;  // Execute bindings immediately when gesture pattern is matched
    uint32_t idle_timeout_ms;  // Time to wait for idle before invoking gesture
    struct gesture_pattern **patterns;  // Array of pointers to patterns
    size_t pattern_count;
};

// Deferred behavior execution data
struct deferred_behavior_execution {
    struct k_work work;
    struct zmk_behavior_binding bindings[MAX_DEFERRED_BINDINGS];
    size_t binding_count;
    struct zmk_behavior_binding_event event;
    uint32_t wait_ms;
    uint32_t tap_ms;
};

struct input_processor_mouse_gesture_data {
    struct k_mutex lock;
    bool is_active;
    int32_t acc_x;
    int32_t acc_y;
    uint8_t sequence[MAX_GESTURE_SEQUENCE_LENGTH];
    uint8_t sequence_len;
    int64_t last_gesture_time;  // Timestamp of last gesture execution; used for cooldown period
    uint32_t event_count;       // Counter to detect potential loops
    int64_t last_reset_time;    // Time of last counter reset; used for event loop detection
    struct deferred_behavior_execution deferred_behavior_exec;  // Work queue item
    struct k_work_delayable idle_timeout_work;  // Work queue item for idle timeout
    int64_t last_movement_time;  // Timestamp of last mouse movement; used for idle timeout
    const struct device *dev;  // Back-reference to device for safe work handler access
    struct k_sem idle_sem;
    struct k_thread idle_thread;
    K_THREAD_STACK_MEMBER(idle_stack, IDLE_THREAD_STACK_SIZE);
};

// Forward declarations
static void schedule_gesture_execution(const struct device *dev, const struct gesture_pattern *pattern);
static void clear_gesture_data_locked(struct input_processor_mouse_gesture_data *data);

static uint8_t detect_direction(int32_t x, int32_t y) {

    if (ABS(x) > ABS(y)) {
        return GESTURE_X(x);
    } else {
        return GESTURE_Y(y);
    }

    return GESTURE_NONE;
}

// Check if pattern matches and clears gesture data (should be called while mutex is held)
static struct gesture_pattern* match_gesture_pattern_locked(const struct device *dev, bool clear_even_if_not_matched) {
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

        if (pattern->pattern_len != data->sequence_len) {
            continue;
        }

        bool match = true;
        for (size_t j = 0; j < pattern->pattern_len; j++) {
            if (pattern->pattern[j] != data->sequence[j]) {
                match = false;
                break;
            }
        }

        if (match) {
            LOG_INF("Gesture pattern matched: %zu", i);

            data->last_gesture_time = current_time;
            clear_gesture_data_locked(data);

            schedule_gesture_execution(dev, pattern);

            return (struct gesture_pattern*)pattern;
        }
    }

    if (clear_even_if_not_matched) {
        clear_gesture_data_locked(data);
    }

    return NULL;
}

// Work queue handler for deferred behavior execution
static void deferred_behavior_work_handler(struct k_work *work) {
    struct deferred_behavior_execution *exec = CONTAINER_OF(work, struct deferred_behavior_execution, work);

    LOG_DBG("Executing deferred behavior with %zu bindings", exec->binding_count);

    // Execute behaviors in work queue context (safe from deadlock)
    for (size_t k = 0; k < exec->binding_count; k++) {
        LOG_DBG("Executing deferred binding [%zu/%zu] with wait-ms=%d, tap-ms=%d",
                k + 1, exec->binding_count, exec->wait_ms, exec->tap_ms);

        int ret = zmk_behavior_queue_add(&exec->event, exec->bindings[k], true, k * exec->wait_ms);
        if (ret < 0) {
            LOG_ERR("Failed to queue deferred press event [%zu]: %d", k, ret);
            continue;
        }

        ret = zmk_behavior_queue_add(&exec->event, exec->bindings[k], false, (k * exec->wait_ms) + exec->tap_ms);
        if (ret < 0) {
            LOG_ERR("Failed to queue deferred release event [%zu]: %d", k, ret);
        }
    }

    LOG_DBG("Deferred behavior execution completed");
}

// Work queue handler for idle timeout gesture execution
static void idle_timeout_work_handler(struct k_work *work) {
    struct k_work_delayable *delayed_work = k_work_delayable_from_work(work);
    struct input_processor_mouse_gesture_data *data =
        CONTAINER_OF(delayed_work, struct input_processor_mouse_gesture_data, idle_timeout_work);
    k_sem_give(&data->idle_sem);
}

// Add dedicated thread function for idle-timeout matching
static void idle_thread_fn(void *arg1, void *arg2, void *arg3) {
    const struct device *dev = arg1;
    struct input_processor_mouse_gesture_data *data = dev->data;
    while (1) {
        k_sem_take(&data->idle_sem, K_FOREVER);
        if (k_mutex_lock(&data->lock, K_MSEC(100)) == 0) {
            if (data->is_active && data->sequence_len > 0) {
                match_gesture_pattern_locked(dev, true);
            }
            k_mutex_unlock(&data->lock);
        }
    }
}

// Schedule gesture execution via work queue
static void schedule_gesture_execution(const struct device *dev, const struct gesture_pattern *pattern) {
    if (!pattern || pattern->bindings_len == 0) {
        return;
    }

    struct input_processor_mouse_gesture_data *data = dev->data;
    struct deferred_behavior_execution *exec = &data->deferred_behavior_exec;

    // Prevent scheduling multiple gestures while the previous one is still executing
    if (k_work_is_pending(&exec->work) || k_work_busy_get(&exec->work) != 0) {
        LOG_WRN("Deferred gesture work already pending/running, skipping new schedule");
        return;
    }

    // Prevent work queue overflow
    if (pattern->bindings_len > MAX_DEFERRED_BINDINGS) {
        LOG_WRN("Too many bindings to defer (%zu > %d), truncating",
                pattern->bindings_len, MAX_DEFERRED_BINDINGS);
    }

    // Setup execution data
    exec->binding_count = MIN(pattern->bindings_len, MAX_DEFERRED_BINDINGS);
    memcpy(exec->bindings, pattern->bindings, exec->binding_count * sizeof(struct zmk_behavior_binding));
    exec->wait_ms = pattern->wait_ms;
    exec->tap_ms = pattern->tap_ms;

    exec->event.position = INT32_MAX;
    exec->event.timestamp = k_uptime_get();
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
    exec->event.source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL;
#endif

    // Submit to system work queue
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

    // Check if mouse gesture is active
    if (!data->is_active) {
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

    // Update last movement time and restart idle timer if needed
    data->last_movement_time = current_time;

    // Start/restart idle timeout if configured and not in eager mode
    if (config->idle_timeout_ms > 0 && !config->enable_eager_mode && data->sequence_len > 0) {
        // Reschedule idle timeout efficiently (avoids cancel+schedule loop)
        int ret = k_work_reschedule(&data->idle_timeout_work, K_MSEC(config->idle_timeout_ms));
        if (ret < 0) {
            LOG_WRN("Failed to reschedule idle timeout work: %d", ret);
        } else {
            LOG_DBG("Idle timeout scheduled for %d ms", config->idle_timeout_ms);
        }
    }

    // Check for direction detection
    uint32_t total_distance = ABS(data->acc_x) + ABS(data->acc_y);

    if (total_distance < config->stroke_size) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint8_t direction = detect_direction(data->acc_x, data->acc_y);

    if (direction != GESTURE_NONE) {
        // Check for duplicate direction
        if (data->sequence_len > 0 && data->sequence[data->sequence_len - 1] == direction) {
            LOG_DBG("Ignoring duplicate direction %d", direction);
        } else {
            // Add direction to sequence
            if (data->sequence_len < MAX_GESTURE_SEQUENCE_LENGTH) {
                data->sequence[data->sequence_len++] = direction;
                LOG_DBG("Added direction %d to sequence (length: %d)", direction, data->sequence_len);

                // Start idle timeout if configured and not in eager mode and this is the first direction
                if (config->idle_timeout_ms > 0 && !config->enable_eager_mode && data->sequence_len == 1) {
                    // Reschedule idle timeout efficiently
                    int ret = k_work_reschedule(&data->idle_timeout_work, K_MSEC(config->idle_timeout_ms));
                    if (ret < 0) {
                        LOG_WRN("Failed to reschedule idle timeout work: %d", ret);
                    } else {
                        LOG_DBG("Idle timeout scheduled for %d ms after first direction", config->idle_timeout_ms);
                    }
                }
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
    const struct input_processor_mouse_gesture_config *config = dev->config;
    int ret = 0;

    if (k_mutex_lock(&data->lock, K_NO_WAIT) != 0) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    ret = input_processor_mouse_gesture_handle_event_locked(dev, event, param1, param2, state);

    // Real-time pattern match in eager mode
    if (config->enable_eager_mode) {
        match_gesture_pattern_locked(dev, false);
    }

    k_mutex_unlock(&data->lock);

    return ret;
}

static int input_processor_mouse_gesture_init(const struct device *dev) {
    struct input_processor_mouse_gesture_data *data = dev->data;

    k_mutex_init(&data->lock);

    data->is_active = false;
    data->acc_x = 0;
    data->acc_y = 0;
    data->sequence_len = 0;
    data->last_gesture_time = 0;
    data->event_count = 0;
    data->last_reset_time = k_uptime_get();

    // Initialize work queue for deferred execution
    k_work_init(&data->deferred_behavior_exec.work, deferred_behavior_work_handler);
    data->deferred_behavior_exec.binding_count = 0;

    // Initialize idle timeout work
    k_work_init_delayable(&data->idle_timeout_work, idle_timeout_work_handler);
    data->last_movement_time = 0;
    k_sem_init(&data->idle_sem, 0, 1);
    k_thread_create(&data->idle_thread, data->idle_stack, IDLE_THREAD_STACK_SIZE,
                    idle_thread_fn, (void *)dev, NULL, NULL,
                    IDLE_THREAD_PRIORITY, 0, K_NO_WAIT);
    // Set device back-reference for access in work handlers
    data->dev = dev;

    LOG_INF("Mouse gesture input processor initialized with deferred execution");
    return 0;
}

// Clear gesture data when gesture state changes (called while mutex is held)
static void clear_gesture_data_locked(struct input_processor_mouse_gesture_data *data) {
    data->acc_x = 0;
    data->acc_y = 0;
    data->sequence_len = 0;

    // Cancel any pending idle timeout
    k_work_cancel_delayable(&data->idle_timeout_work);

    LOG_DBG("Gesture data cleared");
}

// Event listener for mouse gesture state changes
static int mouse_gesture_state_listener(const zmk_event_t *eh) {
    struct zmk_mouse_gesture_state_changed *ev = as_zmk_mouse_gesture_state_changed(eh);
    if (ev == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    // Find the first input processor device instance
    const struct device *dev = NULL;

    // This is a simplified approach - in a production system you might want to
    // iterate through all instances or use a device registry
    #if DT_NODE_EXISTS(DT_DRV_INST(0))
    dev = DEVICE_DT_INST_GET(0);
    #endif

    if (dev == NULL) {
        LOG_WRN("No mouse gesture input processor device found");
        return ZMK_EV_EVENT_BUBBLE;
    }

    struct input_processor_mouse_gesture_data *data = dev->data;
    const struct input_processor_mouse_gesture_config *config = dev->config;

    // Update state with mutex protection
    // Longer timeout for state changes
    int ret = k_mutex_lock(&data->lock, K_MSEC(250));
    if (ret != 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    bool old_state = data->is_active;
    data->is_active = ev->is_active;

    // When gesture becomes inactive, check for pattern match in non-eager mode
    if (old_state && !ev->is_active) {
        LOG_INF("Mouse gesture state changed: ACTIVE -> INACTIVE");

        if (!config->enable_eager_mode) {
            // Check for pattern match on deactivation in non-eager mode
            match_gesture_pattern_locked(dev, true);
        } else {
            // clear gesture data if in eager mode
            clear_gesture_data_locked(data);
        }
    } else if (!old_state && ev->is_active) {
        LOG_INF("Mouse gesture state changed: INACTIVE -> ACTIVE");
        // Clear gesture data when activating
        clear_gesture_data_locked(data);
    }

    k_mutex_unlock(&data->lock);

    return ZMK_EV_EVENT_BUBBLE;
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
        gesture_pattern_config_##n##_bindings[DT_PROP_LEN(n, bindings)] =                          \
            TRANSFORMED_BINDINGS(n);                                                               \
                                                                                                   \
    static struct gesture_pattern gesture_pattern_cfg_##n = {                                      \
        .bindings_len = DT_PROP_LEN(n, bindings),                                                  \
        .bindings = gesture_pattern_config_##n##_bindings,                                         \
        .pattern_len = DT_PROP_LEN(n, pattern),                                                    \
        .wait_ms = DT_PROP_OR(n, wait_ms, CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS),                       \
        .tap_ms = DT_PROP_OR(n, tap_ms, CONFIG_ZMK_MACRO_DEFAULT_TAP_MS),                         \
        .pattern = DT_PROP(n, pattern),                                                            \
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
        .enable_eager_mode = DT_INST_PROP_OR(n, enable_eager_mode, false),          \
        .idle_timeout_ms = DT_INST_PROP_OR(n, idle_timeout_ms, 0),                  \
        .patterns = gesture_patterns,                                               \
        .pattern_count = PATTERN_COUNT,                                             \
    };                                                                              \
    DEVICE_DT_INST_DEFINE(n, input_processor_mouse_gesture_init, NULL,              \
                          &input_processor_mouse_gesture_data_##n,                  \
                          &input_processor_mouse_gesture_config_##n, POST_KERNEL,   \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                      \
                          &input_processor_mouse_gesture_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MOUSE_GESTURE_INPUT_PROCESSOR_INST)

// Register event listener
ZMK_LISTENER(mouse_gesture_input_processor, mouse_gesture_state_listener);
ZMK_SUBSCRIPTION(mouse_gesture_input_processor, zmk_mouse_gesture_state_changed);

#endif