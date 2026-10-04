// Client for the KEF LS50 Wireless II local HTTP API (port 80).
// See docs/KEF_API.md for the endpoints used.
//
// Two FreeRTOS tasks run on core 0: one long-polls the speaker's event queue
// and keeps State current, the other sends commands. The setters below are
// non-blocking and safe to call from the UI; they update State optimistically
// so the screen responds instantly, and rapid volume changes are coalesced.
#pragma once

#include <Arduino.h>

namespace kef {

enum class Source : uint8_t { Unknown, Wifi, Bluetooth, Tv, Optical, Coaxial, Analog, Standby };
enum class PlayState : uint8_t { Stopped, Playing, Paused };

struct State {
  uint32_t revision = 0;          // bumps on any change
  uint32_t externalRevision = 0;  // bumps when volume/mute/source/power changed elsewhere

  bool wifiConnected = false;
  bool reachable = false;  // speaker answered and the event queue is live
  bool poweredOn = false;

  int volume = 0;
  int maxVolume = 100;  // honours the KEF Connect "volume limit" setting
  bool muted = false;
  Source source = Source::Unknown;

  PlayState playState = PlayState::Stopped;
  char title[128] = {};
  char artist[96] = {};
  char album[96] = {};
  bool canPlayPause = true;  // what the current source supports
  bool canSkip = true;
  uint32_t durationMs = 0;
  uint32_t positionMs = 0;       // at positionStampMs
  uint32_t positionStampMs = 0;  // millis() when positionMs was captured

  char speakerName[48] = {};
};

void begin();

uint32_t revision();
State snapshot();

void setVolume(int volume);
void setMuted(bool muted);
void setSource(Source source);  // also wakes the speakers from standby
void powerOn();
void standby();
void playPause();
void nextTrack();
void previousTrack();

const char *sourceLabel(Source source);

}  // namespace kef
