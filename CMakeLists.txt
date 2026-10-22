# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_MOUSE_GESTURE)
  target_include_directories(app PRIVATE include)

  if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if(CONFIG_INPUT_THREAD_STACK_SIZE LESS 2048)
      message(FATAL_ERROR
        "CONFIG_INPUT_THREAD_STACK_SIZE=${CONFIG_INPUT_THREAD_STACK_SIZE} is too small for mouse gestures. "
        "Remove the setting to use the module default of 2048, or set it to 2048 or higher."
      )
    endif()

    target_sources_ifdef(CONFIG_ZMK_MOUSE_GESTURE app PRIVATE
      src/behaviors/behavior_mouse_gesture.c
      src/events/mouse_gesture_state_changed.c
      src/input_processors/input_processor_mouse_gesture.c
    )
  endif()
endif()
