/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * Runtime control over which layers each gesture processor acts on.
 *
 * A keyboard with several gesture sets normally attaches them with
 * input-listener layer overrides, which fixes the layer-to-set mapping at
 * build time. Put every instance in the listener's base chain instead and this
 * mask decides which one acts, so the mapping becomes something a
 * configuration UI can change.
 */

/** @brief Number of mouse-gesture processor instances in this build. */
uint8_t zmk_mouse_gesture_count(void);

/**
 * @brief The instance's devicetree display-name, for a UI to label it with.
 *
 * @retval NULL if @p index is out of range.
 */
const char *zmk_mouse_gesture_name(uint8_t index);

/**
 * @brief Layers this instance currently acts on, as a bitmask.
 *
 * Zero means every layer, which is the default and what a keyboard with a
 * single gesture set wants.
 */
uint32_t zmk_mouse_gesture_get_active_layers(uint8_t index);

/**
 * @brief Set the layers an instance acts on.
 *
 * @param persist Write to settings as well as applying. Pass false to try a
 *                mapping out without spending a flash write.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p index is out of range.
 * @retval Negative errno from the settings layer if persisting failed; the
 *         in-memory change is applied regardless.
 */
int zmk_mouse_gesture_set_active_layers(uint8_t index, uint32_t mask, bool persist);
