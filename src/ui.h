// Round-screen UI: three swipeable pages (Sources | Volume | Now Playing)
// plus overlays for standby, connection status and a volume toast.
#pragma once

#include "board.h"
#include "kef_client.h"

namespace ui {

void begin();
void applyState(const kef::State &state);
void handleInput(const board::InputEvent &input);
void tick();  // progress bar, toast timeout, backlight dimming

}  // namespace ui
