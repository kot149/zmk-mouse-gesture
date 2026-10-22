# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_MOUSE_GESTURE)
  if(CONFIG_INPUT_THREAD_STACK_SIZE LESS 2048)
    message(WARNING
      "CONFIG_INPUT_THREAD_STACK_SIZE=${CONFIG_INPUT_THREAD_STACK_SIZE} is below "
      "the recommended 2048 bytes for mouse gestures. "
      "Set CONFIG_INPUT_THREAD_STACK_SIZE=2048 or higher to reduce the risk of stack overflow."
    )
  endif()

  target_include_directories(app PRIVATE include)

  if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources_ifdef(CONFIG_ZMK_MOUSE_GESTURE app PRIVATE
      src/behaviors/behavior_mouse_gesture.c
      src/events/mouse_gesture_state_changed.c
      src/input_processors/input_processor_mouse_gesture.c
    )
  endif()
endif()
