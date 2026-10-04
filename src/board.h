// Hardware layer for the Elecrow CrowPanel 2.1" rotary display:
// ST7701 RGB panel, CST826 touch, rotary encoder + push button, backlight.
#pragma once

#include <Arduino.h>

namespace board {

enum class Input : uint8_t {
  RotateCw,
  RotateCcw,
  Click,
  DoubleClick,
  LongPress,
};

struct InputEvent {
  Input type;
  uint32_t timeMs;  // when the knob produced it (for volume acceleration)
};

// Powers up the panel and touch controller, initialises LVGL (lv_init) and
// registers the display and touch input with it.
void begin();

// Samples the knob button and fades the backlight. Call every loop().
void poll();

// Pops the next knob event. Returns false when the queue is empty.
bool nextInput(InputEvent &out);

// --- Page slides, done outside LVGL ---------------------------------------
// LVGL redraws the whole screen in PSRAM for every frame of a scroll, which on
// this board manages ~12 fps. A slide instead copies rows of two pre-rendered
// 480x480 RGB565 pages straight into a spare framebuffer and swaps at vsync
// (~40 fps).
//
// Shows `left` shifted left by `offset` px (0..480) with `right` filling in.
void slideFrame(const uint16_t *left, const uint16_t *right, uint32_t strideBytes, int offset);
// Hands the screen back to LVGL (its buffers are left consistent and a full
// redraw is requested).
void endSlide();
// Direct touch read for tracking a swipe while LVGL is paused.
bool readTouchPoint(int16_t &x, int16_t &y);
// Make LVGL see "no touch" until the finger lifts (the swipe owns it).
void ignoreTouchUntilRelease();
// The touch point LVGL last saw; false if not touching (or touch is ignored).
bool lastTouch(int16_t &x, int16_t &y);

// Backlight target (0-255); the change is faded in poll().
void setBacklightTarget(uint8_t level);
bool isDimmed();

}  // namespace board
