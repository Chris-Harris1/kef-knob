#pragma once

#if __has_include("secrets.h")
#include "secrets.h"
#else
#error "Copy include/secrets.example.h to include/secrets.h and set your Wi-Fi and speaker IP."
#endif

// Accent colour used until one is picked on the Colour page (index into
// kThemes in ui.cpp: 0 Copper, 1 Crimson, 2 Rose, 3 Violet, 4 Blue, 5 Teal,
// 6 Lime, 7 Gold). The on-device choice is saved and overrides this.
#define UI_DEFAULT_THEME 0

// Backlight levels (0-255) and how long before the screen dims.
#define BACKLIGHT_ACTIVE 230
#define BACKLIGHT_DIM    18
#define DIM_AFTER_MS     30000

// Set to 1 if turning the knob clockwise lowers the volume.
#define ENCODER_REVERSE 0

// Knob gestures.
#define BUTTON_DOUBLE_CLICK_MS 260
#define BUTTON_LONG_PRESS_MS   800
