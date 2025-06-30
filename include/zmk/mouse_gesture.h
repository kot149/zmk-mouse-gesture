/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>

/**
 * @brief Check if mouse gesture is currently active
 * 
 * @return true if mouse gesture is active, false otherwise
 */
bool zmk_mouse_gesture_is_active(void);