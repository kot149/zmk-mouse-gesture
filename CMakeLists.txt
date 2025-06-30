# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_MOUSE_GESTURE)
  zephyr_library()
  zephyr_library_include_directories(include)
  zephyr_library_sources(src/behaviors/behavior_mouse_gesture.c)
  zephyr_library_sources(src/input_processors/input_processor_mouse_gesture.c)
endif()