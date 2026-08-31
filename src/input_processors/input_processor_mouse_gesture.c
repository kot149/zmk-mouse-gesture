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
#include <stdlib.h>
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

struct gesture_node {
    struct gesture_node *child[4];
    const struct gesture_pattern *pattern;
};

struct input_processor_mouse_gesture_data {
    struct k_mutex lock;
    bool is_active;
    int32_t acc_x;
    int32_t acc_y;
    uint8_t last_direction;
    int64_t last_gesture_time;
    uint32_t event_count;
    int64_t last_reset_time;
    struct k_work_delayable idle_timeout_work;
    int64_t last_movement_time;
    struct gesture_node *current_node;
    const struct device *dev;

    size_t gesture_nodes_count;
    struct gesture_node *gesture_trie_root;
};

static struct gesture_node *allocate_gesture_node(const struct device *dev);

static int direction_to_index(uint8_t direction) {
    switch (direction) {
    case GESTURE_UP:
        return 0;
    case GESTURE_DOWN:
        return 1;
    case GESTURE_LEFT:
        return 2;
    case GESTURE_RIGHT:
        return 3;
    default:
        return -1;
    }
}

struct gesture_pattern;

static void build_gesture_trie(const struct device *dev, const struct gesture_pattern *patterns, size_t pattern_count);

/* Message queue definitions for gesture execution */
struct gesture_exec_msg {
    const struct gesture_pattern *pattern;
};

K_MSGQ_DEFINE(gesture_exec_msgq, sizeof(struct gesture_exec_msg), CONFIG_ZMK_MOUSE_GESTURE_EXEC_MAX_EVENTS, 4);

/* Forward declaration for locked event handler */
static int input_processor_mouse_gesture_handle_event_locked(const struct device *dev,
                                                             struct input_event *event);

static void gesture_exec_work_cb(struct k_work *work);
static K_WORK_DEFINE(gesture_exec_work, gesture_exec_work_cb);

struct gesture_pattern {
    size_t bindings_len;
    const struct zmk_behavior_binding *bindings;
    size_t pattern_len;
    uint32_t wait_ms;
    uint32_t tap_ms;
    const uint8_t *pattern;
};

static void build_gesture_trie(const struct device *dev, const struct gesture_pattern *patterns, size_t pattern_count) {
    struct input_processor_mouse_gesture_data *data = dev->data;
    if (!data->gesture_trie_root) {
        data->gesture_trie_root = allocate_gesture_node(dev);
        if (!data->gesture_trie_root) {
            return;
        }
    }

    for (size_t i = 0; i < pattern_count; i++) {
        const struct gesture_pattern *pat = &patterns[i];
        struct gesture_node *node = data->gesture_trie_root;
        for (size_t j = 0; j < pat->pattern_len; j++) {
            int idx = direction_to_index(pat->pattern[j]);
            if (idx < 0) {
                node = NULL;
                break;
            }
            if (!node->child[idx]) {
                node->child[idx] = allocate_gesture_node(dev);
                if (!node->child[idx]) {
                    node = NULL;
                    break;
                }
            }
            node = node->child[idx];
        }
        if (node) {
            node->pattern = pat;
        }
    }
}


struct input_processor_mouse_gesture_config {
    uint32_t stroke_size;
    uint32_t movement_threshold;
    uint32_t gesture_cooldown_ms;  // Cooldown period between gestures
    bool enable_eager_mode;  // Execute bindings immediately when gesture pattern is matched
    bool always_active;
    bool suppress_movement;  // Consume X/Y events while gesture is active
    uint32_t idle_timeout_ms;  // Time to wait for idle before invoking gesture
    uint32_t partial_gesture_timeout_ms; // Discard a stale in-progress gesture after this much idle time
    uint16_t event_code_x;
    uint16_t event_code_y;
    const struct gesture_pattern *patterns;  // Array of pointers to patterns
    size_t pattern_count;
    struct gesture_node *gesture_nodes_pool; // Backing storage for this instance's trie
    size_t gesture_nodes_pool_len;
};

static struct gesture_node *allocate_gesture_node(const struct device *dev) {
    const struct input_processor_mouse_gesture_config *config = dev->config;
    struct input_processor_mouse_gesture_data *data = dev->data;
    if (data->gesture_nodes_count >= config->gesture_nodes_pool_len) {
        return NULL;
    }
    struct gesture_node *node = &config->gesture_nodes_pool[data->gesture_nodes_count++];
    memset(node, 0, sizeof(struct gesture_node));
    return node;
}

static void schedule_gesture_execution(const struct gesture_pattern *pattern);
static void clear_gesture_data_locked(struct input_processor_mouse_gesture_data *data);

static uint8_t detect_direction(int32_t x, int32_t y) {

    if (abs(x) > abs(y)) {
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

    if (!data->current_node) {
        if (clear_even_if_not_matched) {
            clear_gesture_data_locked(data);
        }
        return NULL;
    }

    const struct gesture_node *node = data->current_node;
    bool has_binding = node->pattern != NULL;
    bool has_child = false;
    for (int i = 0; i < 4; i++) {
        if (node->child[i]) {
            has_child = true;
            break;
        }
    }

    // Exit if no binding found (means no gesture pattern matched)
    if (!has_binding) {
        if (clear_even_if_not_matched) {
            clear_gesture_data_locked(data);
        }
        return NULL;
    }

    if (current_time - data->last_gesture_time < config->gesture_cooldown_ms) {
        return NULL;
    }

    // Invoke by idle timeout if duplicate gesture found in eager mode
    if (config->enable_eager_mode && has_child && !clear_even_if_not_matched && config->idle_timeout_ms > 0) {
        int ret = k_work_reschedule(&data->idle_timeout_work, K_MSEC(config->idle_timeout_ms));
        if (ret < 0) {
            LOG_WRN("Failed to reschedule idle timeout work: %d", ret);
        } else {
            LOG_DBG("Idle timeout scheduled for %d ms", config->idle_timeout_ms);
        }
        return NULL;
    }

    const struct gesture_pattern *pattern = node->pattern;
    data->last_gesture_time = current_time;
    schedule_gesture_execution(pattern);
    clear_gesture_data_locked(data);

    return pattern;
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

    if (k_mutex_lock(&data->lock, K_MSEC(50)) == 0) {
        if (data->is_active && data->current_node && data->current_node != data->gesture_trie_root) {
            match_gesture_pattern_locked(dev, true);
        }
        k_mutex_unlock(&data->lock);
    }
}

/* Primary work handler processing message queues */
static void gesture_exec_work_cb(struct k_work *work) {
    ARG_UNUSED(work);

    struct gesture_exec_msg g_msg;
    while (k_msgq_get(&gesture_exec_msgq, &g_msg, K_NO_WAIT) == 0) {
        const struct gesture_pattern *pattern = g_msg.pattern;
        struct zmk_behavior_binding_event event = {
            .position = INT32_MAX,
            .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
            .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
        };
        for (size_t k = 0; k < pattern->bindings_len; k++) {
            int ret = zmk_behavior_queue_add(&event, pattern->bindings[k], true,
                                             k * pattern->wait_ms);
            if (ret < 0) {
                LOG_ERR("Failed to queue press event %zu: %d", k, ret);
                continue;
            }
            ret = zmk_behavior_queue_add(&event, pattern->bindings[k], false,
                                         (k * pattern->wait_ms) + pattern->tap_ms);
            if (ret < 0) {
                LOG_ERR("Failed to queue release event %zu: %d", k, ret);
            }
        }
    }

    if (k_msgq_num_used_get(&gesture_exec_msgq) > 0) {
        k_work_submit(&gesture_exec_work);
    }
}

// Schedule gesture execution via work queue
static void schedule_gesture_execution(const struct gesture_pattern *pattern) {
    if (!pattern || pattern->bindings_len == 0) {
        return;
    }

    struct gesture_exec_msg msg = {.pattern = pattern};

    int ret = k_msgq_put(&gesture_exec_msgq, &msg, K_NO_WAIT);
    if (ret < 0) {
        LOG_WRN("Gesture execution queue full – gesture dropped (len=%zu)",
                pattern->bindings_len);
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
        data->current_node = data->gesture_trie_root;
        data->event_count = 0;
        return ZMK_INPUT_PROC_CONTINUE;
    }

    if (current_time - data->last_gesture_time < config->gesture_cooldown_ms) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    // Discard a stale, partially accumulated gesture.
    if (config->partial_gesture_timeout_ms > 0 && data->last_movement_time > 0 &&
        (current_time - data->last_movement_time) > (int64_t)config->partial_gesture_timeout_ms) {
        LOG_DBG("Idle for %lldms, discarding stale gesture data",
                current_time - data->last_movement_time);
        clear_gesture_data_locked(data);
    }

    // Accumulate with overflow protection
    if (event->code == config->event_code_x) {
        accumulate_movement_safe(&data->acc_x, event->value, "X");
    } else if (event->code == config->event_code_y) {
        accumulate_movement_safe(&data->acc_y, event->value, "Y");
    } else {
        // this should never happen
    }

    // Update last movement time and restart idle timer if needed
    data->last_movement_time = current_time;

    // Reschedule idle timeout if configured and not in eager mode
    if (config->idle_timeout_ms > 0 && !config->enable_eager_mode && data->current_node && data->current_node != data->gesture_trie_root) {
        int ret = k_work_reschedule(&data->idle_timeout_work, K_MSEC(config->idle_timeout_ms));
        if (ret < 0) {
            LOG_WRN("Failed to reschedule idle timeout work: %d", ret);
        } else {
            LOG_DBG("Idle timeout scheduled for %d ms", config->idle_timeout_ms);
        }
    }

    // Accumulate until stroke size is reached
    uint32_t total_distance = abs(data->acc_x) + abs(data->acc_y);

    if (total_distance < config->stroke_size) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint8_t direction = detect_direction(data->acc_x, data->acc_y);

    if (direction != GESTURE_NONE) {
        // Ignore duplicate direction
        if (data->last_direction == direction) {
            LOG_DBG("Ignoring duplicate direction %d", direction);
        } else {
            int dir_idx = direction_to_index(direction);
            if (data->current_node && dir_idx >= 0) {
                struct gesture_node *next_node = data->current_node->child[dir_idx];
                if (next_node) {
                    // Start idle timeout if configured and not in eager mode and this is the first direction
                    if (config->idle_timeout_ms > 0 && !config->enable_eager_mode && data->current_node == data->gesture_trie_root) {
                        int ret = k_work_reschedule(&data->idle_timeout_work, K_MSEC(config->idle_timeout_ms));
                        if (ret < 0) {
                            LOG_WRN("Failed to reschedule idle timeout work: %d", ret);
                        } else {
                            LOG_DBG("Idle timeout scheduled for %d ms after first direction", config->idle_timeout_ms);
                        }
                    }

                    data->current_node = next_node;
                    data->last_direction = direction;
                    LOG_DBG("Moved to next node for direction %d", direction);
                } else {
                    LOG_DBG("No valid transition for direction %d, clearing gesture", direction);
                    clear_gesture_data_locked(data);
                    return ZMK_INPUT_PROC_CONTINUE;
                }
            } else {
                LOG_DBG("Invalid current node or direction %d, clearing gesture", direction);
                clear_gesture_data_locked(data);
                return ZMK_INPUT_PROC_CONTINUE;
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

    /* Only care about the configured relative axis events */
    const struct input_processor_mouse_gesture_config *config = dev->config;
    if (!(event->type == INPUT_EV_REL &&
          (event->code == config->event_code_x || event->code == config->event_code_y))) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    struct input_processor_mouse_gesture_data *data = dev->data;

    k_mutex_lock(&data->lock, K_FOREVER);

    if (!data->is_active) {
        k_mutex_unlock(&data->lock);
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int32_t value = event->value;
    if (abs(value) >= config->movement_threshold) {
        input_processor_mouse_gesture_handle_event_locked(dev, event);
        if (config->enable_eager_mode) {
            match_gesture_pattern_locked(dev, false);
        }
    }

    if (config->suppress_movement) {
        /* Zero the movement instead of returning ZMK_INPUT_PROC_STOP.
         * On zmk's layer-override path, input_listener discards a STOP
         * returned by the override chain (it returns 0 when process-next
         * is unset), so the event reached HID untouched and the pointer
         * kept moving. Writing 0 suppresses movement from anywhere in
         * the chain. */
        event->value = 0;
    }

    k_mutex_unlock(&data->lock);
    return ZMK_INPUT_PROC_CONTINUE;
}

static int input_processor_mouse_gesture_init(const struct device *dev) {
    LOG_INF("Mouse gesture input processor init start");


    struct input_processor_mouse_gesture_data *data = dev->data;
    const struct input_processor_mouse_gesture_config *config = dev->config;
    data->gesture_nodes_count = 0;
    data->gesture_trie_root = NULL;
    build_gesture_trie(dev, config->patterns, config->pattern_count);

    k_mutex_init(&data->lock);

    data->is_active = config->always_active;
    data->acc_x = 0;
    data->acc_y = 0;
    data->last_direction = GESTURE_NONE;
    data->last_gesture_time = 0;
    data->event_count = 0;
    data->last_reset_time = k_uptime_get();

    // Initialize idle timeout work
    k_work_init_delayable(&data->idle_timeout_work, idle_timeout_work_handler);
    data->last_movement_time = 0;

    data->current_node = data->gesture_trie_root;
    // Set device back-reference for access in work handlers
    data->dev = dev;

    LOG_INF("Mouse gesture input processor init done");
    return 0;
}

// Clear gesture data when gesture state changes (called while mutex is held)
static void clear_gesture_data_locked(struct input_processor_mouse_gesture_data *data) {
    data->acc_x = 0;
    data->acc_y = 0;
    data->last_direction = GESTURE_NONE;
    data->current_node = data->gesture_trie_root;

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

#define MOUSE_GESTURE_DEV_ITEM(n) DEVICE_DT_INST_GET(n),
    static const struct device *const mouse_gesture_devs[] = {
        DT_INST_FOREACH_STATUS_OKAY(MOUSE_GESTURE_DEV_ITEM)
    };

    for (size_t i = 0; i < ARRAY_SIZE(mouse_gesture_devs); i++) {
        const struct device *dev = mouse_gesture_devs[i];
        const struct input_processor_mouse_gesture_config *config = dev->config;
        struct input_processor_mouse_gesture_data *data = dev->data;

        k_mutex_lock(&data->lock, K_FOREVER);
        bool old_state = data->is_active;
        data->is_active = config->always_active || ev->is_active;
        if (old_state && !data->is_active) {
            match_gesture_pattern_locked(dev, true);
        } else if (!old_state && data->is_active) {
            clear_gesture_data_locked(data);
        }
        k_mutex_unlock(&data->lock);
    }

    return ZMK_EV_EVENT_BUBBLE;
}

static const struct zmk_input_processor_driver_api input_processor_mouse_gesture_driver_api = {
    .handle_event = input_processor_mouse_gesture_handle_event,
};

#define BINDINGS_ARRAY(node_id) LISTIFY(DT_PROP_LEN(node_id, bindings), ZMK_KEYMAP_EXTRACT_BINDING, (, ), node_id)

#define DECLARE_GESTURE_CHILD(node_id) \
    static const struct zmk_behavior_binding gesture_pattern_bindings_##node_id[] = { BINDINGS_ARRAY(node_id) }; \
    static const uint8_t gesture_pattern_seq_##node_id[] = DT_PROP(node_id, pattern);

#define GESTURE_PATTERN_ENTRY(node_id)                                                    \
    {                                                                                    \
        .bindings_len = DT_PROP_LEN(node_id, bindings),                                   \
        .bindings = gesture_pattern_bindings_##node_id,                                   \
        .pattern_len = DT_PROP_LEN(node_id, pattern),                                     \
        .wait_ms = DT_PROP_OR(node_id, wait_ms, CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS),        \
        .tap_ms = DT_PROP_OR(node_id, tap_ms, CONFIG_ZMK_MACRO_DEFAULT_TAP_MS),           \
        .pattern = gesture_pattern_seq_##node_id,                                         \
    },

/* Worst case (no shared prefixes) one trie node per stroke, plus the root. */
#define GESTURE_PATTERN_LEN_PLUS(node_id) +DT_PROP_LEN(node_id, pattern)
#define GESTURE_TRIE_NODE_COUNT(n) (1 DT_FOREACH_CHILD(DT_DRV_INST(n), GESTURE_PATTERN_LEN_PLUS))

#define MOUSE_GESTURE_INPUT_PROCESSOR_INST(n)                                                         \
    DT_FOREACH_CHILD(DT_DRV_INST(n), DECLARE_GESTURE_CHILD)                                           \
    static const struct gesture_pattern gesture_patterns_##n[] = {                                    \
        DT_FOREACH_CHILD(DT_DRV_INST(n), GESTURE_PATTERN_ENTRY)                                       \
    };                                                                                                \
    static struct gesture_node gesture_trie_nodes_##n[GESTURE_TRIE_NODE_COUNT(n)];                    \
    static struct input_processor_mouse_gesture_data                                                  \
        input_processor_mouse_gesture_data_##n = {};                                                  \
    static const struct input_processor_mouse_gesture_config                                          \
        input_processor_mouse_gesture_config_##n = {                                                  \
        .stroke_size = DT_INST_PROP_OR(n, stroke_size, 200),                                          \
        .movement_threshold = DT_INST_PROP_OR(n, movement_threshold, 0),                             \
        .gesture_cooldown_ms = DT_INST_PROP_OR(n, gesture_cooldown_ms, 500),                          \
        .enable_eager_mode = DT_INST_PROP_OR(n, enable_eager_mode, false),                            \
        .always_active = DT_INST_PROP_OR(n, always_active, false),                                    \
        .suppress_movement = DT_INST_PROP_OR(n, suppress_movement, false),                            \
        .idle_timeout_ms = DT_INST_PROP_OR(n, idle_timeout_ms, 150),                                  \
        .partial_gesture_timeout_ms = DT_INST_PROP_OR(n, partial_gesture_timeout_ms, 400),            \
        .event_code_x = (uint16_t)DT_INST_PROP_OR(n, event_code_x, INPUT_REL_X),                          \
        .event_code_y = (uint16_t)DT_INST_PROP_OR(n, event_code_y, INPUT_REL_Y),                          \
        .patterns = gesture_patterns_##n,                                                             \
        .gesture_nodes_pool = gesture_trie_nodes_##n,                                                 \
        .gesture_nodes_pool_len = ARRAY_SIZE(gesture_trie_nodes_##n),                                 \
        .pattern_count = ARRAY_SIZE(gesture_patterns_##n),                                            \
    };                                                                                                \
    DEVICE_DT_INST_DEFINE(n, input_processor_mouse_gesture_init, NULL,                                \
                          &input_processor_mouse_gesture_data_##n,                                    \
                          &input_processor_mouse_gesture_config_##n, POST_KERNEL,                     \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                        \
                          &input_processor_mouse_gesture_driver_api);

DT_INST_FOREACH_STATUS_OKAY(MOUSE_GESTURE_INPUT_PROCESSOR_INST)

// Register event listener
ZMK_LISTENER(mouse_gesture_input_processor, mouse_gesture_state_listener);
ZMK_SUBSCRIPTION(mouse_gesture_input_processor, zmk_mouse_gesture_state_changed);

#endif
