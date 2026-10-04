// KEF LS50 Wireless II remote for the Elecrow CrowPanel 2.1" rotary display.
//
// Core 1 (Arduino loop): LVGL, touch, knob button, UI.
// Core 0: Wi-Fi, knob rotation sampling, KEF event polling and commands.

#include <Arduino.h>
#include <lvgl.h>

#include "board.h"
#include "kef_client.h"
#include "ui.h"

namespace {

// Internal RAM is the scarce resource (Wi-Fi packet buffers live there), so
// report it, including the low-water mark, once a minute.
void logMemory() {
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 60000) return;
  lastMs = millis();
  Serial.printf("[mem] internal free %u (lowest %u), PSRAM free %u\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

}  // namespace

void setup() {
  Serial.begin(115200);
  board::begin();
  ui::begin();
  kef::begin();
}

void loop() {
  static uint32_t appliedRevision = UINT32_MAX;

  board::poll();

  board::InputEvent input;
  while (board::nextInput(input)) ui::handleInput(input);

  const uint32_t rev = kef::revision();
  if (rev != appliedRevision) {
    const kef::State state = kef::snapshot();
    appliedRevision = state.revision;
    ui::applyState(state);
  }

  ui::tick();
  lv_timer_handler();
  logMemory();
  delay(2);
}
