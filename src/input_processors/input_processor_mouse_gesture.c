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

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)



#define ABS(x) ((x) < 0 ? -(x) : (x))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define MAX_GESTURE_SEQUENCE_LENGTH 8
#define MAX_GESTURE_PATTERNS 16
#define MAX_DEFERRED_BINDINGS 8

/* Message queue definitions for gesture execution */
#define GESTURE_EXEC_MAX_EVENTS 8

struct gesture_exec_msg {
    size_t binding_count;
    struct zmk_behavior_binding bindings[MAX_DEFERRED_BINDINGS];
    uint32_t wait_ms;
    uint32_t tap_ms;
};

K_MSGQ_DEFINE(gesture_exec_msgq, sizeof(struct gesture_exec_msg), GESTURE_EXEC_MAX_EVENTS, 4);

/* Forward declaration for locked event handler */
static int input_processor_mouse_gesture_handle_event_locked(const struct device *dev,
                                                             struct input_event *event);

static void gesture_exec_work_cb(struct k_work *work);
static K_WORK_DEFINE(gesture_exec_work, gesture_exec_work_cb);

/* State change message queue */
struct state_action_msg {
    bool activate;
};
K_MSGQ_DEFINE(state_action_msgq, sizeof(struct state_action_msg), 8, 4);

/* Mouse relative movement message queue */
struct mouse_rel_msg {
    uint16_t code;
    int32_t value;
};
#define MOUSE_REL_MSG_QUEUE_LEN 32
K_MSGQ_DEFINE(mouse_rel_msgq, sizeof(struct mouse_rel_msg), MOUSE_REL_MSG_QUEUE_LEN, 4);

struct gesture_pattern {
    size_t bindings_len;
    const struct zmk_behavior_binding *bindings;
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
    const struct gesture_pattern *const *patterns;  // Array of pointers to patterns
    size_t pattern_count;
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
    struct k_work_delayable idle_timeout_work;  // Work queue item for idle timeout
    int64_t last_movement_time;  // Timestamp of last mouse movement; used for idle timeout
    const struct device *dev;  // Back-reference to device for safe work handler access

};

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
static const struct gesture_pattern *match_gesture_pattern_locked(const struct device *dev, bool clear_even_if_not_matched) {
    const struct input_processor_mouse_gesture_config *config = dev->config;
    struct input_processor_mouse_gesture_data *data = dev->data;
    int64_t current_time = k_uptime_get();

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

            return pattern;
        }
    }

    if (clear_even_if_not_matched) {
        clear_gesture_data_locked(data);
    }

    return NULL;
}

// Work queue handler for idle timeout gesture execution
static void idle_timeout_work_handler(struct k_work *work) {
    struct k_work_delayable *delayed_work = k_work_delayable_from_work(work);
    struct input_processor_mouse_gesture_data *data =
        CONTAINER_OF(delayed_work, struct input_processor_mouse_gesture_data, idle_timeout_work);

    const struct device *dev = data->dev;
    if (!dev) {
        return;
    }

    /* Directly attempt to match gesture pattern instead of waking a dedicated thread */
    if (k_mutex_lock(&data->lock, K_MSEC(50)) == 0) {
        if (data->is_active && data->sequence_len > 0) {
            match_gesture_pattern_locked(dev, true);
        }
        k_mutex_unlock(&data->lock);
    }
}

/* Primary work handler processing message queues */
static void gesture_exec_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    const struct device *dev = DEVICE_DT_INST_GET(0);
    if (!dev) {
        return;
    }
    struct input_processor_mouse_gesture_data *data = dev->data;

    /* -------- 1. State changes & mouse movements (with lock) -------- */
    if (k_mutex_lock(&data->lock, K_FOREVER) == 0) {
        struct state_action_msg s_msg;
        while (k_msgq_get(&state_action_msgq, &s_msg, K_NO_WAIT) == 0) {
            bool old_state = data->is_active;
            data->is_active = s_msg.activate;
            if (old_state && !s_msg.activate) {
                match_gesture_pattern_locked(dev, true);
            } else if (!old_state && s_msg.activate) {
                clear_gesture_data_locked(data);
            }
        }

        struct mouse_rel_msg m_msg;
        while (k_msgq_get(&mouse_rel_msgq, &m_msg, K_NO_WAIT) == 0) {
            struct input_event ev = {
                .type = INPUT_EV_REL,
                .code = m_msg.code,
                .value = m_msg.value,
            };
            input_processor_mouse_gesture_handle_event_locked(dev, &ev);
            if (((struct input_processor_mouse_gesture_config *)dev->config)->enable_eager_mode) {
                match_gesture_pattern_locked(dev, false);
            }
        }
        k_mutex_unlock(&data->lock);
    }

    /* -------- 2. Gesture execution -------- */
    struct gesture_exec_msg g_msg;
    while (k_msgq_get(&gesture_exec_msgq, &g_msg, K_NO_WAIT) == 0) {
        struct zmk_behavior_binding_event event = {
            .position = INT32_MAX,
            .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
        };
        for (size_t k = 0; k < g_msg.binding_count; k++) {
            int ret = zmk_behavior_queue_add(&event, g_msg.bindings[k], true, k * g_msg.wait_ms);
            if (ret < 0) {
                LOG_ERR("Failed to queue press event %zu: %d", k, ret);
                continue;
            }
            ret = zmk_behavior_queue_add(&event, g_msg.bindings[k], false, (k * g_msg.wait_ms) + g_msg.tap_ms);
            if (ret < 0) {
                LOG_ERR("Failed to queue release event %zu: %d", k, ret);
            }
        }
    }

    if (k_msgq_num_used_get(&state_action_msgq) > 0 ||
        k_msgq_num_used_get(&mouse_rel_msgq) > 0 ||
        k_msgq_num_used_get(&gesture_exec_msgq) > 0) {
        k_work_submit(&gesture_exec_work);
    }
}

// Schedule gesture execution via work queue
static void schedule_gesture_execution(const struct device *dev, const struct gesture_pattern *pattern) {
    if (!pattern || pattern->bindings_len == 0) {
        return;
    }

    /* Build execution message */
    struct gesture_exec_msg msg = {0};
    msg.binding_count = MIN(pattern->bindings_len, MAX_DEFERRED_BINDINGS);
    memcpy(msg.bindings, pattern->bindings,
           msg.binding_count * sizeof(struct zmk_behavior_binding));
    msg.wait_ms = pattern->wait_ms;
    msg.tap_ms = pattern->tap_ms;

    int ret = k_msgq_put(&gesture_exec_msgq, &msg, K_MSEC(10));
    if (ret < 0) {
        LOG_WRN("Gesture execution queue full – gesture dropped (len=%zu)", msg.binding_count);
        return;
    }

    /* Ensure work item runs */
    k_work_submit(&gesture_exec_work);
}

// Safe accumulation with overflow protection
static int accumulate_movement_safe(int32_t *accumulator, int32_t delta, const char* axis) {
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
                                                      struct input_event *event) {
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
        int ret = k_work_reschedule(&data->idle_timeout_work, K_MSEC(config->idle_timeout_ms));
        if (ret < 0) {
            LOG_WRN("Failed to reschedule idle timeout work: %d", ret);
        } else {
            LOG_DBG("Idle timeout scheduled for %d ms", config->idle_timeout_ms);
        }
    }

    // Accumulate until stroke size is reached
    uint32_t total_distance = ABS(data->acc_x) + ABS(data->acc_y);

    if (total_distance < config->stroke_size) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint8_t direction = detect_direction(data->acc_x, data->acc_y);

    if (direction != GESTURE_NONE) {
        // Ignore duplicate direction
        if (data->sequence_len > 0 && data->sequence[data->sequence_len - 1] == direction) {
            LOG_DBG("Ignoring duplicate direction %d", direction);
        } else {
            // Add direction to sequence
            if (data->sequence_len < MAX_GESTURE_SEQUENCE_LENGTH) {
                data->sequence[data->sequence_len++] = direction;
                LOG_DBG("Added direction %d to sequence (length: %d)", direction, data->sequence_len);

                // Start idle timeout if configured and not in eager mode and this is the first direction
                if (config->idle_timeout_ms > 0 && !config->enable_eager_mode && data->sequence_len == 1) {
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
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);
    ARG_UNUSED(state);

    /* Only care about REL_X / REL_Y events */
    if (!(event->type == INPUT_EV_REL &&
          (event->code == INPUT_REL_X || event->code == INPUT_REL_Y))) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    /* Ignore small movements here – same check will run later under lock but reduces queue spam */
    const struct input_processor_mouse_gesture_config *config = dev->config;
    if (ABS(event->value) < config->movement_threshold) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct mouse_rel_msg msg = {
        .code = event->code,
        .value = event->value,
    };

    if (k_msgq_put(&mouse_rel_msgq, &msg, K_MSEC(10)) != 0) {
        /* Queue full – drop smallest importance events */
        LOG_WRN("Mouse rel queue full – movement dropped");
    }

    k_work_submit(&gesture_exec_work);

    return ZMK_INPUT_PROC_CONTINUE;
}

static int input_processor_mouse_gesture_init(const struct device *dev) {
    LOG_INF("Mouse gesture input processor init start");


    struct input_processor_mouse_gesture_data *data = dev->data;

    k_mutex_init(&data->lock);

    data->is_active = false;
    data->acc_x = 0;
    data->acc_y = 0;
    data->sequence_len = 0;
    data->last_gesture_time = 0;
    data->event_count = 0;
    data->last_reset_time = k_uptime_get();

    // Initialize idle timeout work
    k_work_init_delayable(&data->idle_timeout_work, idle_timeout_work_handler);
    data->last_movement_time = 0;

    // Set device back-reference for access in work handlers
    data->dev = dev;

    LOG_INF("Mouse gesture input processor init done");
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

    struct state_action_msg msg = {
        .activate = ev->is_active,
    };

    /* Enqueue state change; drop if queue full */
    if (k_msgq_put(&state_action_msgq, &msg, K_MSEC(10)) != 0) {
        LOG_WRN("State action queue full – state change dropped");
    }

    /* Ensure work runs */
    k_work_submit(&gesture_exec_work);

    return ZMK_EV_EVENT_BUBBLE;
}

static const struct zmk_input_processor_driver_api input_processor_mouse_gesture_driver_api = {
    .handle_event = input_processor_mouse_gesture_handle_event,
};

#define TRANSFORMED_BINDINGS(n)                                                                    \
    { LISTIFY(DT_PROP_LEN(n, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), n) }

#define GESTURE_PATTERN_INST(n)                                                                    \
    static const struct zmk_behavior_binding                                                             \
        gesture_pattern_config_##n##_bindings[DT_PROP_LEN(n, bindings)] =                          \
            TRANSFORMED_BINDINGS(n);                                                               \
                                                                                                   \
    static const struct gesture_pattern gesture_pattern_cfg_##n = {                                      \
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

static const struct gesture_pattern *gesture_patterns[] = {DT_INST_FOREACH_CHILD(0, GESTURE_PATTERN_ITEM)};

#define PATTERN_COUNT (ARRAY_SIZE(gesture_patterns))

#define MOUSE_GESTURE_INPUT_PROCESSOR_INST(n)                                       \
    static struct input_processor_mouse_gesture_data                                \
        input_processor_mouse_gesture_data_##n = {};                                \
    static const struct input_processor_mouse_gesture_config                              \
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
