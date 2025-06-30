/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

// 8-way gesture direction constants
#define GESTURE_NONE       0
#define GESTURE_UP         1
#define GESTURE_DOWN       2
#define GESTURE_LEFT       4
#define GESTURE_RIGHT      8
#define GESTURE_UP_LEFT    (GESTURE_UP | GESTURE_LEFT)
#define GESTURE_UP_RIGHT   (GESTURE_UP | GESTURE_RIGHT)
#define GESTURE_DOWN_LEFT  (GESTURE_DOWN | GESTURE_LEFT)
#define GESTURE_DOWN_RIGHT (GESTURE_DOWN | GESTURE_RIGHT)

#define GESTURE_X(x) (x > 0 ? GESTURE_RIGHT : GESTURE_LEFT)
#define GESTURE_Y(y) (y > 0 ? GESTURE_DOWN : GESTURE_UP)
#define GESTURE_XY(x, y) (GESTURE_X(x) | GESTURE_Y(y))
