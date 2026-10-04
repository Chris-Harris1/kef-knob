#include "kef_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>

#include <initializer_list>

#include "config.h"

namespace kef {
namespace {

constexpr const char *kPathVolume = "player:volume";
constexpr const char *kPathMute = "settings:/mediaPlayer/mute";
constexpr const char *kPathSource = "settings:/kef/play/physicalSource";
constexpr const char *kPathStatus = "settings:/kef/host/speakerStatus";
constexpr const char *kPathPlayer = "player:player/data";
constexpr const char *kPathPlayTime = "player:player/data/playTime";
constexpr const char *kPathName = "settings:/deviceName";
constexpr const char *kPathMaxVolume = "settings:/kef/host/maximumVolume";
constexpr const char *kPathVolumeLimit = "settings:/kef/host/volumeLimit";
constexpr const char *kPathControl = "player:player/control";

constexpr const char *kWatchedPaths[] = {
    kPathVolume, kPathMute,    kPathSource,    kPathStatus,      kPathPlayer,
    kPathPlayTime, kPathName, kPathMaxVolume, kPathVolumeLimit,
};

// After a local change, ignore speaker events for that value for a moment:
// they're usually echoes of intermediate values we already moved past.
constexpr uint32_t kEchoGuardMs = 900;
// Waking from standby takes a few seconds, during which the speaker still
// reports "standby"; don't let that bounce the UI back.
constexpr uint32_t kPowerGuardMs = 6000;
constexpr int kPollTimeoutS = 10;

struct SourceInfo {
  Source id;
  const char *api;
  const char *label;
};

constexpr SourceInfo kSources[] = {
    {Source::Wifi, "wifi", "Wi-Fi"},
    {Source::Bluetooth, "bluetooth", "Bluetooth"},
    {Source::Tv, "tv", "TV"},
    {Source::Optical, "optic", "Optical"},
    {Source::Coaxial, "coaxial", "Coaxial"},
    {Source::Analog, "analog", "Aux"},
    {Source::Standby, "standby", "Standby"},
};

// Large JSON documents (track metadata) go to PSRAM instead of internal heap.
struct SpiRamAllocator : ArduinoJson::Allocator {
  void *allocate(size_t size) override {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  void deallocate(void *ptr) override { heap_caps_free(ptr); }
  void *reallocate(void *ptr, size_t size) override {
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
};
SpiRamAllocator spiRam;

enum class Control : uint8_t { PlayPause, Next, Previous };

struct Pending {
  int volume = -1;
  int8_t muted = -1;
  Source source = Source::Unknown;  // Standby here means "go to standby"
  bool powerOn = false;
  Control controls[6];
  uint8_t controlCount = 0;

  bool empty() const {
    return volume < 0 && muted < 0 && source == Source::Unknown && !powerOn && controlCount == 0;
  }
};

SemaphoreHandle_t mutex;
TaskHandle_t commandTaskHandle;

// Everything below is guarded by `mutex`.
State state;
Pending pending;
uint32_t volumeTouchedMs = 0;
uint32_t muteTouchedMs = 0;
uint32_t sourceTouchedMs = 0;
uint32_t powerTouchedMs = 0;
bool limitEnabled = false;
int limitValue = 100;

JsonDocument valueFilter;  // for getData: [ <value> ]
JsonDocument eventFilter;  // for pollQueue: [ {path, itemValue: <value>} ]

class Lock {
 public:
  Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mutex); }
};

bool recent(uint32_t stampMs, uint32_t windowMs) {
  return stampMs != 0 && millis() - stampMs < windowMs;
}

void markChanged(bool external = false) {
  state.revision++;
  if (external) state.externalRevision++;
}

void copyText(char *dst, size_t size, const char *src) {
  strlcpy(dst, src ? src : "", size);
}

Source parseSource(const char *api) {
  for (const auto &s : kSources)
    if (strcmp(api, s.api) == 0) return s.id;
  return Source::Unknown;
}

const char *apiName(Source source) {
  for (const auto &s : kSources)
    if (s.id == source) return s.api;
  return "";
}

void notifyCommandTask() {
  xTaskNotifyGive(commandTaskHandle);
}

void buildFilters() {
  auto describeValue = [](JsonObject v) {
    v["type"] = true;
    v["i32_"] = true;
    v["i64_"] = true;
    v["bool_"] = true;
    v["string_"] = true;
    v["kefPhysicalSource"] = true;
    v["kefSpeakerStatus"] = true;
    v["state"] = true;
    v["status"]["duration"] = true;
    v["controls"] = true;
    v["trackRoles"]["title"] = true;
    v["trackRoles"]["mediaData"]["metaData"]["artist"] = true;
    v["trackRoles"]["mediaData"]["metaData"]["album"] = true;
  };
  describeValue(valueFilter.add<JsonObject>());
  JsonObject event = eventFilter.add<JsonObject>();
  event["path"] = true;
  describeValue(event["itemValue"].to<JsonObject>());
}

// ---------------------------------------------------------------------------
// Applying speaker values to State

void applyValue(const char *path, JsonVariantConst v) {
  const uint32_t now = millis();
  Lock lock;

  if (strcmp(path, kPathVolume) == 0) {
    if (!v["i32_"].is<int>()) return;
    const int volume = v["i32_"];
    if (pending.volume >= 0 || recent(volumeTouchedMs, kEchoGuardMs)) return;
    if (volume != state.volume) {
      state.volume = volume;
      markChanged(true);
    }

  } else if (strcmp(path, kPathMute) == 0) {
    if (!v["bool_"].is<bool>()) return;
    const bool muted = v["bool_"];
    if (pending.muted >= 0 || recent(muteTouchedMs, kEchoGuardMs)) return;
    if (muted != state.muted) {
      state.muted = muted;
      markChanged(true);
    }

  } else if (strcmp(path, kPathSource) == 0) {
    const Source source = parseSource(v["kefPhysicalSource"] | "");
    if (source == Source::Unknown) return;
    if (pending.source != Source::Unknown || pending.powerOn) return;
    if (recent(powerTouchedMs, kPowerGuardMs)) return;
    if (source == Source::Standby) {
      if (state.poweredOn) {
        state.poweredOn = false;
        markChanged(true);
      }
      return;
    }
    if (recent(sourceTouchedMs, kEchoGuardMs)) return;
    if (source != state.source || !state.poweredOn) {
      state.source = source;
      state.poweredOn = true;
      markChanged(true);
    }

  } else if (strcmp(path, kPathStatus) == 0) {
    const char *status = v["kefSpeakerStatus"] | "";
    if (!*status || recent(powerTouchedMs, kPowerGuardMs)) return;
    const bool on = strcmp(status, "standby") != 0;
    if (on != state.poweredOn) {
      state.poweredOn = on;
      markChanged(true);
    }

  } else if (strcmp(path, kPathPlayer) == 0) {
    const char *stateText = v["state"] | "";
    const PlayState play = strcmp(stateText, "playing") == 0  ? PlayState::Playing
                           : strcmp(stateText, "paused") == 0 ? PlayState::Paused
                                                              : PlayState::Stopped;
    const char *title = v["trackRoles"]["title"] | "";
    JsonVariantConst meta = v["trackRoles"]["mediaData"]["metaData"];

    // Keep the interpolated track position sensible across track/state changes.
    if (strcmp(title, state.title) != 0) {
      state.positionMs = 0;
      state.positionStampMs = now;
    } else if (state.playState == PlayState::Playing && play != PlayState::Playing) {
      state.positionMs += now - state.positionStampMs;
      state.positionStampMs = now;
    } else if (state.playState != PlayState::Playing && play == PlayState::Playing) {
      state.positionStampMs = now;
    }

    state.playState = play;
    copyText(state.title, sizeof(state.title), title);
    copyText(state.artist, sizeof(state.artist), meta["artist"] | "");
    copyText(state.album, sizeof(state.album), meta["album"] | "");
    state.durationMs = v["status"]["duration"] | 0u;
    // e.g. TV/optical report {"pause":false,"next_":false,"previous":false}
    JsonVariantConst controls = v["controls"];
    state.canPlayPause = controls["pause"] | true;
    state.canSkip = (controls["next_"] | true) || (controls["previous"] | true);
    markChanged();

  } else if (strcmp(path, kPathPlayTime) == 0) {
    if (v["i64_"].isNull()) return;
    const int64_t ms = v["i64_"].as<int64_t>();  // -1 when the source has no position (TV etc.)
    state.positionMs = ms > 0 ? uint32_t(ms) : 0;
    state.positionStampMs = now;
    markChanged();

  } else if (strcmp(path, kPathName) == 0) {
    copyText(state.speakerName, sizeof(state.speakerName), v["string_"] | "");
    markChanged();

  } else if (strcmp(path, kPathMaxVolume) == 0 || strcmp(path, kPathVolumeLimit) == 0) {
    if (v["i32_"].is<int>()) limitValue = v["i32_"];
    if (v["bool_"].is<bool>()) limitEnabled = v["bool_"];
    state.maxVolume = limitEnabled ? constrain(limitValue, 1, 100) : 100;
    markChanged();
  }
}

// ---------------------------------------------------------------------------
// HTTP

String urlEncode(const char *s) {
  static const char *hex = "0123456789ABCDEF";
  String out;
  for (; *s; ++s) {
    const char c = *s;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

String apiUrl(const String &pathAndQuery) {
  return String("http://") + KEF_HOST + pathAndQuery;
}

// One per task. `reuse` keeps the TCP connection open between requests: on a
// marginal Wi-Fi link every new connection risks a lost SYN, which lwIP only
// retransmits after ~3 s, so the connect timeout must allow for that too.
struct Http {
  WiFiClient client;
  HTTPClient http;
  bool reuse = false;

  int request(const String &url, const String *postBody, String &response, uint16_t timeoutMs) {
    http.setReuse(reuse);
    http.setConnectTimeout(4000);
    http.setTimeout(timeoutMs);
    if (!http.begin(client, url)) return -1;
    int code;
    if (postBody) {
      http.addHeader("Content-Type", "application/json");
      code = http.POST(*postBody);
    } else {
      code = http.GET();
    }
    if (code == HTTP_CODE_OK) response = http.getString();
    http.end();
    return code;
  }
};

bool fetchValue(Http &h, const char *path) {
  String body;
  const String url = apiUrl("/api/getData?path=" + urlEncode(path) + "&roles=value");
  if (h.request(url, nullptr, body, 3000) != HTTP_CODE_OK) return false;
  JsonDocument doc(&spiRam);
  if (deserializeJson(doc, body, DeserializationOption::Filter(valueFilter))) return false;
  applyValue(path, doc[0]);
  return true;
}

bool fetchWithRetry(Http &h, const char *path) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (fetchValue(h, path)) return true;
    vTaskDelay(pdMS_TO_TICKS(150));
  }
  Serial.printf("[kef] couldn't read %s\n", path);
  return false;
}

// The event queue only reports changes, so read everything once up front.
// The core values are required; the rest can legitimately be empty when idle.
bool fetchOptional(Http &h) {
  bool all = true;
  for (const char *path : {kPathPlayer, kPathPlayTime, kPathName, kPathMaxVolume, kPathVolumeLimit})
    all &= fetchWithRetry(h, path);
  return all;
}

// `complete` reports whether the optional values loaded too; if not, the
// caller retries them later (they're only pushed again when they change).
bool fetchAll(Http &h, bool &complete) {
  for (const char *path : {kPathVolume, kPathMute, kPathSource, kPathStatus})
    if (!fetchWithRetry(h, path)) return false;
  complete = fetchOptional(h);
  return true;
}

bool createQueue(Http &h, String &queueId) {
  JsonDocument req;
  JsonArray subscribe = req["subscribe"].to<JsonArray>();
  for (const char *path : kWatchedPaths) {
    JsonObject item = subscribe.add<JsonObject>();
    item["path"] = path;
    item["type"] = "itemWithValue";
  }
  req["unsubscribe"].to<JsonArray>();
  String body;
  serializeJson(req, body);

  String response;
  if (h.request(apiUrl("/api/event/modifyQueue"), &body, response, 3000) != HTTP_CODE_OK)
    return false;
  JsonDocument doc;
  if (deserializeJson(doc, response) || !doc.is<const char *>()) return false;
  queueId = doc.as<const char *>();  // e.g. "{0b3c...}" (braces included)
  return queueId.length() > 2;
}

// Long-polls the event queue. Returns false if the queue is gone or the speaker
// is unreachable; an empty result after the timeout is a success.
bool pollEvents(Http &h, const String &queueId) {
  String body;
  const String url = apiUrl("/api/event/pollQueue?queueId=" + urlEncode(queueId.c_str()) +
                            "&timeout=" + String(kPollTimeoutS));
  if (h.request(url, nullptr, body, (kPollTimeoutS + 3) * 1000) != HTTP_CODE_OK) return false;
  JsonDocument doc(&spiRam);
  if (deserializeJson(doc, body, DeserializationOption::Filter(eventFilter))) return false;
  for (JsonVariantConst event : doc.as<JsonArrayConst>()) {
    const char *path = event["path"];
    if (path && !event["itemValue"].isNull()) applyValue(path, event["itemValue"]);
  }
  return true;
}

bool setData(Http &h, const char *path, const char *roles, JsonVariantConst value) {
  JsonDocument req;
  req["path"] = path;
  req["roles"] = roles;
  req["value"] = value;
  String body;
  serializeJson(req, body);
  String response;
  const uint32_t startMs = millis();
  int code = h.request(apiUrl("/api/setData"), &body, response, 2500);
  if (code < 0) {
    // Usually a kept-alive socket the network dropped: retry on a fresh one.
    h.http.end();
    h.client.stop();
    code = h.request(apiUrl("/api/setData"), &body, response, 2500);
  }
  const unsigned long elapsedMs = millis() - startMs;
  if (code != HTTP_CODE_OK)
    Serial.printf("[kef] setData %s failed: %d (%lu ms)\n", path, code, elapsedMs);
  else if (elapsedMs > 500)
    Serial.printf("[kef] setData %s slow: %lu ms\n", path, elapsedMs);
  return code == HTTP_CODE_OK;
}

void sendSource(Http &h, const char *api) {
  JsonDocument v;
  v["type"] = "kefPhysicalSource";
  v["kefPhysicalSource"] = api;
  setData(h, kPathSource, "value", v);
}

void sendVolume(Http &h, int volume) {
  JsonDocument v;
  v["type"] = "i32_";
  v["i32_"] = volume;
  setData(h, kPathVolume, "value", v);
}

void sendMute(Http &h, bool muted) {
  JsonDocument v;
  v["type"] = "bool_";
  v["bool_"] = muted;
  setData(h, kPathMute, "value", v);
}

void sendControl(Http &h, Control control) {
  JsonDocument v;
  // "pause" toggles play/pause on the KEF player.
  v["control"] = control == Control::Next ? "next" : control == Control::Previous ? "previous" : "pause";
  setData(h, kPathControl, "activate", v);
}

// ---------------------------------------------------------------------------
// Tasks

void setLink(bool wifi, bool reachable) {
  Lock lock;
  if (state.wifiConnected != wifi || state.reachable != reachable) {
    state.wifiConnected = wifi;
    state.reachable = reachable;
    markChanged();
  }
}

// Logs what the radio can see when it can't join, which is usually a
// 5 GHz-only SSID (the ESP32-S3 is 2.4 GHz only) or a typo.
void reportWifiTrouble() {
  Serial.printf("[wifi] can't join \"%s\" (status %d). 2.4 GHz networks in range:\n", WIFI_SSID,
                int(WiFi.status()));
  const int n = WiFi.scanNetworks();
  for (int i = 0; i < n; ++i)
    Serial.printf("  %-32s ch%-3d %d dBm\n", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
  WiFi.scanDelete();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

// Mesh networks don't always hand the ESP32 their nearest node, and on a weak
// node replies stall for seconds. While the link is weak, look for a clearly
// stronger node with the same SSID and move to it.
void roamTask(void *) {
  constexpr int kWeakRssi = -60;
  constexpr int kMinGainDb = 10;  // hysteresis so we don't flap between nodes
  // A scan takes the radio off-channel for ~2 s, delaying commands. If a scan
  // finds nothing better, we're on the best node already: back off.
  constexpr uint32_t kBackoffMs = 5 * 60 * 1000;
  uint32_t nextScanMs = 5000;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (int32_t(millis() - nextScanMs) < 0) continue;
    nextScanMs = millis() + 30000;
    if (WiFi.status() == WL_CONNECTED && WiFi.RSSI() < kWeakRssi) {
      const int current = WiFi.RSSI();
      const int n = WiFi.scanNetworks(false, false, false, 120);
      int best = -1;
      Serial.printf("[wifi] weak link (%d dBm via %s), scanned %d networks:\n", current,
                    WiFi.BSSIDstr().c_str(), n);
      for (int i = 0; i < n; ++i) {
        if (WiFi.SSID(i) != WIFI_SSID) continue;
        Serial.printf("[wifi]   %s ch%d %d dBm\n", WiFi.BSSIDstr(i).c_str(), WiFi.channel(i), WiFi.RSSI(i));
        if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
      }
      if (best >= 0 && WiFi.RSSI(best) >= current + kMinGainDb &&
          memcmp(WiFi.BSSID(best), WiFi.BSSID(), 6) != 0) {
        uint8_t bssid[6];
        memcpy(bssid, WiFi.BSSID(best), sizeof(bssid));
        const int channel = WiFi.channel(best);
        Serial.printf("[wifi] roaming: %d dBm -> %s ch%d (%d dBm)\n", current,
                      WiFi.BSSIDstr(best).c_str(), channel, WiFi.RSSI(best));
        WiFi.scanDelete();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD, channel, bssid);
      } else {
        WiFi.scanDelete();
        nextScanMs = millis() + kBackoffMs;
      }
    }
  }
}

// Scans first and joins the strongest node broadcasting our SSID, instead of
// whichever node answers first (often a distant one on this mesh).
void joinStrongest() {
  const int n = WiFi.scanNetworks(false, false, false, 120);
  int best = -1;
  for (int i = 0; i < n; ++i)
    if (WiFi.SSID(i) == WIFI_SSID && (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best))) best = i;
  if (best < 0) {
    WiFi.scanDelete();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    return;
  }
  uint8_t bssid[6];
  memcpy(bssid, WiFi.BSSID(best), sizeof(bssid));
  const int channel = WiFi.channel(best);
  Serial.printf("[wifi] joining strongest node %s ch%d (%d dBm)\n", WiFi.BSSIDstr(best).c_str(),
                channel, WiFi.RSSI(best));
  WiFi.scanDelete();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD, channel, bssid);
}

void pollTask(void *) {
  joinStrongest();

  Http h;
  String queueId;
  uint8_t failures = 0;
  uint32_t disconnectedSinceMs = millis();
  uint32_t refetchAtMs = 0;
  bool wasConnected = false;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      if (wasConnected) {
        wasConnected = false;
        disconnectedSinceMs = millis();
        Serial.println("[wifi] disconnected");
      }
      if (millis() - disconnectedSinceMs > 20000) {
        reportWifiTrouble();
        disconnectedSinceMs = millis();
      }
      queueId = "";
      setLink(false, false);
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (!wasConnected) {
      wasConnected = true;
      Serial.printf("[wifi] connected to \"%s\" via %s ch%d, IP %s, RSSI %d dBm, internal RAM free %u\n",
                    WIFI_SSID, WiFi.BSSIDstr().c_str(), WiFi.channel(),
                    WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                    heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
    if (queueId.isEmpty()) {
      bool complete = true;
      if (!fetchAll(h, complete) || !createQueue(h, queueId)) {
        queueId = "";
        setLink(true, false);
        vTaskDelay(pdMS_TO_TICKS(3000));
        continue;
      }
      refetchAtMs = complete ? 0 : millis() + 5000;
      failures = 0;
      setLink(true, true);
      Serial.printf("[kef] connected to %s, queue %s\n", KEF_HOST, queueId.c_str());
    }
    if (refetchAtMs && int32_t(millis() - refetchAtMs) >= 0)
      refetchAtMs = fetchOptional(h) ? 0 : millis() + 5000;
    if (pollEvents(h, queueId)) {
      failures = 0;
      continue;
    }
    // Queue expired or a hiccup: rebuild it (and re-read everything) after two misses.
    if (++failures >= 2) queueId = "";
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void commandTask(void *) {
  Http h;
  h.reuse = true;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    // Drain: anything requested while we were sending is picked up next pass,
    // so a fast knob spin results in only a handful of volume requests.
    for (;;) {
      Pending job;
      {
        Lock lock;
        if (pending.empty()) break;
        job = pending;
        pending = Pending{};
      }
      if (WiFi.status() != WL_CONNECTED) continue;

      if (job.powerOn) sendSource(h, "powerOn");
      if (job.source != Source::Unknown) sendSource(h, apiName(job.source));
      if (job.muted >= 0) sendMute(h, job.muted);
      if (job.volume >= 0) sendVolume(h, job.volume);
      for (uint8_t i = 0; i < job.controlCount; ++i) sendControl(h, job.controls[i]);

      Lock lock;
      const uint32_t now = millis();
      if (job.volume >= 0) volumeTouchedMs = now;
      if (job.muted >= 0) muteTouchedMs = now;
      if (job.source != Source::Unknown) sourceTouchedMs = now;
    }
  }
}

void queueControl(Control control) {
  {
    Lock lock;
    if (pending.controlCount < sizeof(pending.controls) / sizeof(pending.controls[0]))
      pending.controls[pending.controlCount++] = control;
  }
  notifyCommandTask();
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API

void begin() {
  mutex = xSemaphoreCreateMutex();
  buildFilters();

  WiFi.mode(WIFI_STA);
  // On a mesh, join the strongest node rather than the first one heard.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setHostname("kef-remote");
  WiFi.setSleep(false);  // modem sleep adds 100ms+ latency to every request
  WiFi.setAutoReconnect(true);
  // The poll task joins the network (after a scan, so it doesn't block the UI).

  xTaskCreatePinnedToCore(commandTask, "kef-cmd", 8192, nullptr, 3, &commandTaskHandle, 0);
  xTaskCreatePinnedToCore(pollTask, "kef-poll", 8192, nullptr, 2, nullptr, 0);
  xTaskCreatePinnedToCore(roamTask, "wifi-roam", 4096, nullptr, 1, nullptr, 0);
}

uint32_t revision() {
  Lock lock;
  return state.revision;
}

State snapshot() {
  Lock lock;
  return state;
}

void setVolume(int volume) {
  {
    Lock lock;
    volume = constrain(volume, 0, state.maxVolume);
    const uint32_t now = millis();
    pending.volume = volume;
    volumeTouchedMs = now;
    if (state.volume != volume) {
      state.volume = volume;
      markChanged();
    }
    if (state.muted) {  // turning the volume unmutes, like the KEF remote
      state.muted = false;
      pending.muted = 0;
      muteTouchedMs = now;
      markChanged();
    }
  }
  notifyCommandTask();
}

void setMuted(bool muted) {
  {
    Lock lock;
    pending.muted = muted ? 1 : 0;
    muteTouchedMs = millis();
    state.muted = muted;
    markChanged();
  }
  notifyCommandTask();
}

void setSource(Source source) {
  if (source == Source::Standby) return standby();
  if (source == Source::Unknown) return;
  {
    Lock lock;
    const uint32_t now = millis();
    pending.source = source;
    pending.powerOn = false;  // selecting a source wakes the speaker by itself
    sourceTouchedMs = now;
    state.source = source;
    if (!state.poweredOn) {
      state.poweredOn = true;
      powerTouchedMs = now;
    }
    markChanged();
  }
  notifyCommandTask();
}

void powerOn() {
  {
    Lock lock;
    pending.powerOn = true;
    if (pending.source == Source::Standby) pending.source = Source::Unknown;
    powerTouchedMs = millis();
    state.poweredOn = true;
    markChanged();
  }
  notifyCommandTask();
}

void standby() {
  {
    Lock lock;
    pending.source = Source::Standby;
    pending.powerOn = false;
    powerTouchedMs = millis();
    state.poweredOn = false;
    markChanged();
  }
  notifyCommandTask();
}

void playPause() {
  {
    Lock lock;
    const uint32_t now = millis();
    if (state.playState == PlayState::Playing) {
      state.positionMs += now - state.positionStampMs;
      state.playState = PlayState::Paused;
    } else {
      state.playState = PlayState::Playing;
    }
    state.positionStampMs = now;
    markChanged();
  }
  queueControl(Control::PlayPause);
}

void nextTrack() {
  queueControl(Control::Next);
}

void previousTrack() {
  queueControl(Control::Previous);
}

const char *sourceLabel(Source source) {
  for (const auto &s : kSources)
    if (s.id == source) return s.label;
  return "";
}

}  // namespace kef
