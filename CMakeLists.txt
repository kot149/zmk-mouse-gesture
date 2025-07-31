# Copyright (c) 2024 The ZMK Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_MOUSE_GESTURE)
  target_include_directories(app PRIVATE include)

  if ((NOT CONFIG_ZMK_SPLIT) OR CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    target_sources_ifdef(CONFIG_ZMK_MOUSE_GESTURE app PRIVATE
      src/behaviors/behavior_mouse_gesture.c
      src/events/mouse_gesture_state_changed.c
      src/input_processors/input_processor_mouse_gesture.c
    )
  endif()
endif()
