#include "ui.h"

#include <Preferences.h>
#include <lvgl.h>

#include "config.h"

LV_FONT_DECLARE(font_volume_132);
LV_FONT_DECLARE(font_title_30);
LV_FONT_DECLARE(font_body_22);
LV_FONT_DECLARE(font_small_16);

namespace ui {
namespace {

// ---------------------------------------------------------------------------
// Look

constexpr uint32_t kBg = 0x0A0A0C;
constexpr uint32_t kSurface = 0x17171C;
constexpr uint32_t kSurfacePressed = 0x2A2A32;
constexpr uint32_t kTrack = 0x1E1E24;
constexpr uint32_t kText = 0xF4F4F6;
constexpr uint32_t kTextDim = 0xA0A0AA;
constexpr uint32_t kTextFaint = 0x62626C;
constexpr uint32_t kMutedArc = 0x3C3C44;

// Accent colour themes, chosen on the Colour page and saved in flash.
struct Theme {
  const char *name;
  uint32_t accent;
  uint32_t ink;  // text/icons drawn on top of the accent
};
constexpr Theme kThemes[] = {
    {"Copper", 0xFF8A3D, 0x170B03}, {"Crimson", 0xE5484D, 0xFFFFFF},
    {"Rose", 0xF472B6, 0x1A0610},   {"Violet", 0x8B5CF6, 0xFFFFFF},
    {"Blue", 0x3B82F6, 0xFFFFFF},   {"Teal", 0x2DD4BF, 0x04201C},
    {"Lime", 0xA3E635, 0x142004},   {"Gold", 0xF5C542, 0x1F1700},
};
constexpr int kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);

int themeIndex = UI_DEFAULT_THEME;
uint32_t accent = kThemes[UI_DEFAULT_THEME].accent;
uint32_t accentPressed = (accent >> 1) & 0x7F7F7F;
uint32_t onAccent = kThemes[UI_DEFAULT_THEME].ink;

constexpr uint32_t kToastMs = 1400;

lv_color_t col(uint32_t hex) {
  return lv_color_hex(hex);
}

// ---------------------------------------------------------------------------
// Widgets

enum Page : uint8_t { kPageSources, kPageVolume, kPagePlayer, kPageTheme, kPageCount };

struct SourceButton {
  kef::Source source;
  const char *icon;
  lv_obj_t *btn;
  lv_obj_t *iconLabel;
  lv_obj_t *nameLabel;
};

// Clockwise from the top of the ring.
SourceButton sourceButtons[] = {
    {kef::Source::Wifi, LV_SYMBOL_WIFI},       {kef::Source::Bluetooth, LV_SYMBOL_BLUETOOTH},
    {kef::Source::Tv, LV_SYMBOL_VIDEO},        {kef::Source::Optical, LV_SYMBOL_CHARGE},
    {kef::Source::Coaxial, LV_SYMBOL_GPS},     {kef::Source::Analog, LV_SYMBOL_AUDIO},
};
constexpr int kSourceCount = sizeof(sourceButtons) / sizeof(sourceButtons[0]);
constexpr int kFocusPower = kSourceCount;  // focus slot after the six sources
constexpr lv_point_t kRingPos[kSourceCount] = {
    {0, -152}, {132, -76}, {132, 76}, {0, 152}, {-132, 76}, {-132, -76},
};

lv_obj_t *dots[kPageCount];

// Pages sit side by side; the active one is on screen at x = 0 and the others
// just off it (not hidden, so they can still be snapshotted).
lv_obj_t *content[kPageCount];
Page activePage = kPageVolume;
int pendingPage = -1;  // page change requested from an LVGL callback, run in tick()

// Page snapshots for smooth swipes: while a page is idle it's rendered once
// into `snap`, and a swipe slides those images (see board::slideFrame) rather
// than having LVGL redraw every widget on every frame.
lv_draw_buf_t *snap[kPageCount];
bool snapStale[kPageCount];  // all set in begin()
uint32_t staleSinceMs[kPageCount];
constexpr uint32_t kSnapshotDelayMs = 250;  // let a page settle before re-rendering it

void markStale(Page page) {
  if (!snapStale[page]) staleSinceMs[page] = millis();
  snapStale[page] = true;
}

lv_obj_t *volArc, *volNumber, *volCaption, *muteBtn, *muteIcon;
lv_obj_t *chip, *chipIcon, *chipLabel;

lv_obj_t *powerBtn, *powerIcon;
int focus = 0;
bool focusVisible = false;

lv_obj_t *progressArc, *playerStatus, *titleLabel, *artistLabel, *albumLabel;
lv_obj_t *prevBtn, *playBtn, *nextBtn, *playBtnIcon, *timeLabel;

lv_obj_t *standbyOverlay, *standbyName, *wakeIcon;
lv_obj_t *connOverlay, *connTitle, *connDetail, *spinner;

lv_obj_t *toast, *toastIcon, *toastLabel;
uint32_t toastHideAt = 0;

lv_obj_t *themeSwatches[kThemeCount];
lv_obj_t *themeChecks[kThemeCount];
lv_obj_t *themeName;
Preferences prefs;
uint32_t saveThemeAtMs = 0;  // debounced so turning the knob doesn't wear the flash

void applyTheme(int index, bool save);

kef::State shown;
bool haveState = false;
uint32_t lastDetentMs = 0;

// ---------------------------------------------------------------------------
// Helpers

lv_obj_t *makeLabel(lv_obj_t *parent, const lv_font_t *font, uint32_t color, const char *text = "") {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, col(color), 0);
  lv_label_set_text(l, text);
  return l;
}

lv_obj_t *makeCircleButton(lv_obj_t *parent, int size, uint32_t bg, uint32_t bgPressed) {
  lv_obj_t *b = lv_button_create(parent);
  lv_obj_remove_style_all(b);
  lv_obj_set_size(b, size, size);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(b, col(bg), 0);
  lv_obj_set_style_bg_color(b, col(bgPressed), LV_STATE_PRESSED);
  lv_obj_set_style_transform_width(b, -3, LV_STATE_PRESSED);
  lv_obj_set_style_transform_height(b, -3, LV_STATE_PRESSED);
  // Encoder focus ring (applied manually on the sources page).
  lv_obj_set_style_outline_width(b, 3, LV_STATE_FOCUS_KEY);
  lv_obj_set_style_outline_color(b, col(kText), LV_STATE_FOCUS_KEY);
  lv_obj_set_style_outline_pad(b, 5, LV_STATE_FOCUS_KEY);
  lv_obj_set_style_opa(b, LV_OPA_30, LV_STATE_DISABLED);
  return b;
}

lv_obj_t *makeOverlay() {
  lv_obj_t *o = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(o);
  lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
  lv_obj_set_style_bg_color(o, col(kBg), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);  // swallow touches meant for pages underneath
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

void setText(lv_obj_t *label, const char *text) {
  // Re-setting identical text would restart scrolling labels and redraw for nothing.
  if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

void setEnabled(lv_obj_t *btn, bool enabled) {
  if (lv_obj_has_state(btn, LV_STATE_DISABLED) == enabled) {
    if (enabled)
      lv_obj_remove_state(btn, LV_STATE_DISABLED);
    else
      lv_obj_add_state(btn, LV_STATE_DISABLED);
  }
}

void setHidden(lv_obj_t *obj, bool hidden) {
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) != hidden) {
    if (hidden)
      lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
  }
}

const char *sourceIcon(kef::Source source) {
  for (const auto &s : sourceButtons)
    if (s.source == source) return s.icon;
  return LV_SYMBOL_AUDIO;
}

void formatTime(char *out, size_t size, uint32_t ms) {
  const uint32_t s = ms / 1000;
  snprintf(out, size, "%lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// ---------------------------------------------------------------------------
// Rendering

void renderDots(Page page) {
  for (int i = 0; i < kPageCount; ++i) {
    lv_obj_set_width(dots[i], i == page ? 22 : 8);
    lv_obj_set_style_bg_color(dots[i], col(i == page ? accent : kTextFaint), 0);
  }
}

void renderFocus() {
  for (int i = 0; i < kSourceCount; ++i) {
    if (focusVisible && focus == i)
      lv_obj_add_state(sourceButtons[i].btn, LV_STATE_FOCUS_KEY);
    else
      lv_obj_remove_state(sourceButtons[i].btn, LV_STATE_FOCUS_KEY);
  }
  if (focusVisible && focus == kFocusPower)
    lv_obj_add_state(powerBtn, LV_STATE_FOCUS_KEY);
  else
    lv_obj_remove_state(powerBtn, LV_STATE_FOCUS_KEY);
  markStale(kPageSources);
}

void renderVolume() {
  markStale(kPageVolume);
  const bool muted = shown.muted;
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", shown.volume);

  if (!lv_obj_has_state(volArc, LV_STATE_PRESSED)) lv_arc_set_value(volArc, shown.volume);
  setText(volNumber, buf);
  setText(toastLabel, buf);
  setText(toastIcon, muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);

  lv_obj_set_style_arc_color(volArc, col(muted ? kMutedArc : accent), LV_PART_INDICATOR);
  lv_obj_set_style_text_color(volNumber, col(muted ? kTextFaint : kText), 0);
  setText(volCaption, muted ? "MUTED" : "VOLUME");
  lv_obj_set_style_text_color(volCaption, col(muted ? accent : kTextFaint), 0);

  setText(muteIcon, muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX);
  lv_obj_set_style_bg_color(muteBtn, col(muted ? accent : kSurface), 0);
  lv_obj_set_style_text_color(muteIcon, col(muted ? onAccent : kText), 0);
}

void renderSource() {
  markStale(kPageVolume);  // source chip
  markStale(kPageSources);
  const kef::Source src = shown.source;
  setText(chipIcon, sourceIcon(src));
  setText(chipLabel, src == kef::Source::Unknown ? "Source" : kef::sourceLabel(src));

  for (auto &b : sourceButtons) {
    const bool active = shown.poweredOn && b.source == src;
    lv_obj_set_style_bg_color(b.btn, col(active ? accent : kSurface), 0);
    lv_obj_set_style_bg_color(b.btn, col(active ? accentPressed : kSurfacePressed), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(b.iconLabel, col(active ? onAccent : kText), 0);
    lv_obj_set_style_text_color(b.nameLabel, col(active ? onAccent : kTextDim), 0);
  }
  lv_obj_set_style_text_color(powerIcon, col(shown.poweredOn ? accent : kTextFaint), 0);
}

void renderPlayer() {
  markStale(kPagePlayer);
  const kef::Source src = shown.source;
  const bool streaming = src == kef::Source::Wifi || src == kef::Source::Bluetooth;
  const bool playing = shown.playState == kef::PlayState::Playing;

  const char *title = shown.title;
  const char *artist = shown.artist;
  if (!*title) title = streaming ? "Nothing playing" : kef::sourceLabel(src);
  if (!*artist && !streaming) artist = "External input";
  setText(titleLabel, title);
  setText(artistLabel, artist);
  setText(albumLabel, shown.album);

  char status[48];
  const char *stateText = playing ? "PLAYING" : shown.playState == kef::PlayState::Paused ? "PAUSED" : "IDLE";
  char sourceUpper[16] = {};
  const char *label = kef::sourceLabel(src);
  for (size_t i = 0; label[i] && i < sizeof(sourceUpper) - 1; ++i) sourceUpper[i] = toupper(label[i]);
  snprintf(status, sizeof(status), "%s  \xC2\xB7  %s", sourceUpper, stateText);  // middle dot
  setText(playerStatus, status);

  setText(playBtnIcon, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  setEnabled(playBtn, shown.canPlayPause);
  setEnabled(prevBtn, shown.canSkip);
  setEnabled(nextBtn, shown.canSkip);
  // The play triangle looks off-centre without a nudge.
  lv_obj_align(playBtnIcon, LV_ALIGN_CENTER, playing ? 0 : 3, 0);
}

void renderProgress() {
  uint32_t pos = shown.positionMs;
  if (shown.playState == kef::PlayState::Playing) pos += millis() - shown.positionStampMs;
  const uint32_t dur = shown.durationMs;
  if (dur && pos > dur) pos = dur;

  const int value = dur ? int(uint64_t(pos) * 1000 / dur) : 0;
  if (lv_arc_get_value(progressArc) != value) lv_arc_set_value(progressArc, value);

  char a[12], b[12], text[28] = "";
  if (dur) {
    formatTime(a, sizeof(a), pos);
    formatTime(b, sizeof(b), dur);
    snprintf(text, sizeof(text), "%s / %s", a, b);
  } else if (pos) {
    formatTime(text, sizeof(text), pos);
  }
  // The progress clock isn't worth a re-snapshot every second; a swipe
  // refreshes the player snapshot itself if music is playing.
  setText(timeLabel, text);
}

bool takeSnapshot(int page) {
  lv_obj_update_layout(content[page]);
  if (!snap[page]) snap[page] = lv_snapshot_create_draw_buf(content[page], LV_COLOR_FORMAT_RGB565);
  if (!snap[page] ||
      lv_snapshot_take_to_draw_buf(content[page], LV_COLOR_FORMAT_RGB565, snap[page]) != LV_RESULT_OK)
    return false;
  snapStale[page] = false;
  return true;
}

// Called from tick(): re-renders at most one out-of-date page snapshot, once
// it has been unchanged for a moment and nothing is being touched, so swipes
// can usually start instantly.
void refreshSnapshots() {
  lv_indev_t *touch = lv_indev_get_next(nullptr);
  if (touch && lv_indev_get_state(touch) == LV_INDEV_STATE_PRESSED) return;
  const uint32_t now = millis();
  for (int i = 0; i < kPageCount; ++i) {
    if (snapStale[i] && now - staleSinceMs[i] >= kSnapshotDelayMs) {
      takeSnapshot(i);
      return;
    }
  }
}

void renderOverlays() {
  const bool connecting = !shown.wifiConnected || !shown.reachable;
  setHidden(connOverlay, !connecting);
  if (connecting) {
    setText(connTitle, shown.wifiConnected ? "Looking for speakers" : "Connecting to Wi-Fi");
    setText(connDetail, shown.wifiConnected ? KEF_HOST : WIFI_SSID);
  }
  setHidden(standbyOverlay, connecting || shown.poweredOn);
  setText(standbyName, *shown.speakerName ? shown.speakerName : "KEF LS50 Wireless II");
}

void showToast() {
  if (activePage == kPageVolume) return;
  setHidden(toast, false);
  toastHideAt = millis() + kToastMs;
}

// ---------------------------------------------------------------------------
// Actions

// Page changes slide, so they run from tick() rather than inside an LVGL
// event callback (the slide takes over the screen while it runs).
void goToPage(Page page) {
  pendingPage = page;
}

void togglePower() {
  if (shown.poweredOn)
    kef::standby();
  else
    kef::powerOn();
}

void activateFocus() {
  if (focus == kFocusPower)
    togglePower();
  else
    kef::setSource(sourceButtons[focus].source);
}

void focusCurrentSource() {
  focus = 0;
  for (int i = 0; i < kSourceCount; ++i)
    if (sourceButtons[i].source == shown.source) focus = i;
}

void changeVolume(int direction, uint32_t timeMs) {
  // Spin faster, move further.
  const uint32_t gap = timeMs - lastDetentMs;
  lastDetentMs = timeMs;
  const int step = gap < 30 ? 3 : gap < 70 ? 2 : 1;
  kef::setVolume(kef::snapshot().volume + direction * step);
  showToast();
}

// ---------------------------------------------------------------------------
// Pages

void buildVolumePage(lv_obj_t *tile) {
  volArc = lv_arc_create(tile);
  lv_obj_set_size(volArc, 452, 452);
  lv_obj_center(volArc);
  lv_arc_set_bg_angles(volArc, 135, 45);  // 270° sweep, gap at the bottom
  lv_arc_set_range(volArc, 0, 100);
  // Only the ring itself takes touches; without this the arc claims its whole
  // bounding box (nearly the full page) and swallows every page swipe.
  lv_obj_add_flag(volArc, LV_OBJ_FLAG_ADV_HITTEST);
  lv_obj_set_ext_click_area(volArc, 8);  // a little slack for fingers on the ring
  lv_obj_set_style_pad_all(volArc, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_width(volArc, 26, LV_PART_MAIN);
  lv_obj_set_style_arc_color(volArc, col(kTrack), LV_PART_MAIN);
  lv_obj_set_style_arc_width(volArc, 26, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(volArc, col(accent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(volArc, col(kText), LV_PART_KNOB);
  lv_obj_set_style_pad_all(volArc, 2, LV_PART_KNOB);
  lv_obj_set_style_shadow_width(volArc, 0, LV_PART_KNOB);
  lv_obj_add_event_cb(
      volArc, [](lv_event_t *) { kef::setVolume(lv_arc_get_value(volArc)); },
      LV_EVENT_VALUE_CHANGED, nullptr);

  chip = lv_button_create(tile);
  lv_obj_remove_style_all(chip);
  lv_obj_set_size(chip, LV_SIZE_CONTENT, 44);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(chip, col(kSurface), 0);
  lv_obj_set_style_bg_color(chip, col(kSurfacePressed), LV_STATE_PRESSED);
  lv_obj_set_style_pad_hor(chip, 18, 0);
  lv_obj_set_style_pad_column(chip, 10, 0);
  lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(chip, LV_ALIGN_CENTER, 0, -124);
  chipIcon = makeLabel(chip, &lv_font_montserrat_20, accent, LV_SYMBOL_WIFI);
  chipLabel = makeLabel(chip, &lv_font_montserrat_20, kText, "Source");
  lv_obj_add_event_cb(chip, [](lv_event_t *) { goToPage(kPageSources); }, LV_EVENT_CLICKED, nullptr);

  volNumber = makeLabel(tile, &font_volume_132, kText, "--");
  lv_obj_align(volNumber, LV_ALIGN_CENTER, 0, 2);

  volCaption = makeLabel(tile, &lv_font_montserrat_16, kTextFaint, "VOLUME");
  lv_obj_set_style_text_letter_space(volCaption, 4, 0);
  lv_obj_align(volCaption, LV_ALIGN_CENTER, 0, 82);

  muteBtn = makeCircleButton(tile, 64, kSurface, kSurfacePressed);
  lv_obj_align(muteBtn, LV_ALIGN_CENTER, 0, 148);
  muteIcon = makeLabel(muteBtn, &lv_font_montserrat_24, kText, LV_SYMBOL_VOLUME_MAX);
  lv_obj_center(muteIcon);
  lv_obj_add_event_cb(muteBtn, [](lv_event_t *) { kef::setMuted(!shown.muted); }, LV_EVENT_CLICKED, nullptr);
}

void buildSourcesPage(lv_obj_t *tile) {
  for (int i = 0; i < kSourceCount; ++i) {
    SourceButton &s = sourceButtons[i];
    s.btn = makeCircleButton(tile, 104, kSurface, kSurfacePressed);
    lv_obj_align(s.btn, LV_ALIGN_CENTER, kRingPos[i].x, kRingPos[i].y);
    s.iconLabel = makeLabel(s.btn, &lv_font_montserrat_28, kText, s.icon);
    lv_obj_align(s.iconLabel, LV_ALIGN_CENTER, 0, -12);
    s.nameLabel = makeLabel(s.btn, &lv_font_montserrat_14, kTextDim, kef::sourceLabel(s.source));
    lv_obj_align(s.nameLabel, LV_ALIGN_CENTER, 0, 24);
    lv_obj_add_event_cb(
        s.btn,
        [](lv_event_t *e) {
          const int index = int(intptr_t(lv_event_get_user_data(e)));
          focus = index;
          kef::setSource(sourceButtons[index].source);
        },
        LV_EVENT_CLICKED, (void *)intptr_t(i));
  }

  powerBtn = makeCircleButton(tile, 88, kSurface, kSurfacePressed);
  lv_obj_center(powerBtn);
  powerIcon = makeLabel(powerBtn, &lv_font_montserrat_32, kTextFaint, LV_SYMBOL_POWER);
  lv_obj_center(powerIcon);
  lv_obj_add_event_cb(powerBtn, [](lv_event_t *) { togglePower(); }, LV_EVENT_CLICKED, nullptr);
}

void buildPlayerPage(lv_obj_t *tile) {
  progressArc = lv_arc_create(tile);
  lv_obj_set_size(progressArc, 468, 468);
  lv_obj_center(progressArc);
  lv_arc_set_rotation(progressArc, 270);
  lv_arc_set_bg_angles(progressArc, 0, 360);
  lv_arc_set_range(progressArc, 0, 1000);
  lv_arc_set_value(progressArc, 0);
  lv_obj_remove_style(progressArc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(progressArc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_pad_all(progressArc, 0, LV_PART_MAIN);
  lv_obj_set_style_arc_width(progressArc, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_color(progressArc, col(kTrack), LV_PART_MAIN);
  lv_obj_set_style_arc_width(progressArc, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(progressArc, col(accent), LV_PART_INDICATOR);

  playerStatus = makeLabel(tile, &font_small_16, kTextFaint);
  lv_obj_set_style_text_letter_space(playerStatus, 2, 0);
  lv_obj_align(playerStatus, LV_ALIGN_CENTER, 0, -150);

  titleLabel = makeLabel(tile, &font_title_30, kText);
  lv_obj_set_width(titleLabel, 340);
  lv_obj_set_style_text_align(titleLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_align(titleLabel, LV_ALIGN_CENTER, 0, -92);

  artistLabel = makeLabel(tile, &font_body_22, kTextDim);
  lv_obj_set_width(artistLabel, 330);
  lv_obj_set_style_text_align(artistLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(artistLabel, LV_LABEL_LONG_DOT);
  lv_obj_align(artistLabel, LV_ALIGN_CENTER, 0, -46);

  albumLabel = makeLabel(tile, &font_small_16, kTextFaint);
  lv_obj_set_width(albumLabel, 310);
  lv_obj_set_style_text_align(albumLabel, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(albumLabel, LV_LABEL_LONG_DOT);
  lv_obj_align(albumLabel, LV_ALIGN_CENTER, 0, -14);

  prevBtn = makeCircleButton(tile, 72, kSurface, kSurfacePressed);
  lv_obj_align(prevBtn, LV_ALIGN_CENTER, -118, 72);
  lv_obj_center(makeLabel(prevBtn, &lv_font_montserrat_24, kText, LV_SYMBOL_PREV));
  lv_obj_add_event_cb(prevBtn, [](lv_event_t *) { kef::previousTrack(); }, LV_EVENT_CLICKED, nullptr);

  playBtn = makeCircleButton(tile, 104, accent, accentPressed);
  lv_obj_align(playBtn, LV_ALIGN_CENTER, 0, 72);
  playBtnIcon = makeLabel(playBtn, &lv_font_montserrat_32, onAccent, LV_SYMBOL_PLAY);
  lv_obj_align(playBtnIcon, LV_ALIGN_CENTER, 3, 0);
  lv_obj_add_event_cb(playBtn, [](lv_event_t *) { kef::playPause(); }, LV_EVENT_CLICKED, nullptr);

  nextBtn = makeCircleButton(tile, 72, kSurface, kSurfacePressed);
  lv_obj_align(nextBtn, LV_ALIGN_CENTER, 118, 72);
  lv_obj_center(makeLabel(nextBtn, &lv_font_montserrat_24, kText, LV_SYMBOL_NEXT));
  lv_obj_add_event_cb(nextBtn, [](lv_event_t *) { kef::nextTrack(); }, LV_EVENT_CLICKED, nullptr);

  timeLabel = makeLabel(tile, &font_small_16, kTextFaint);
  lv_obj_align(timeLabel, LV_ALIGN_CENTER, 0, 156);
}

// Colour page: a ring of accent swatches with the current one named in the
// middle. Tap one, or turn the knob to preview them live.
void buildThemePage(lv_obj_t *page) {
  // Clockwise from the top, 45 degrees apart.
  constexpr lv_point_t kPos[kThemeCount] = {
      {0, -150}, {106, -106}, {150, 0}, {106, 106}, {0, 150}, {-106, 106}, {-150, 0}, {-106, -106},
  };
  static_assert(kThemeCount == 8, "swatch ring is laid out for eight themes");
  for (int i = 0; i < kThemeCount; ++i) {
    lv_obj_t *swatch = makeCircleButton(page, 72, kThemes[i].accent, (kThemes[i].accent >> 1) & 0x7F7F7F);
    lv_obj_align(swatch, LV_ALIGN_CENTER, kPos[i].x, kPos[i].y);
    lv_obj_set_style_border_color(swatch, col(kText), 0);
    lv_obj_add_event_cb(
        swatch, [](lv_event_t *e) { applyTheme(int(intptr_t(lv_event_get_user_data(e))), true); },
        LV_EVENT_CLICKED, (void *)intptr_t(i));
    themeChecks[i] = makeLabel(swatch, &lv_font_montserrat_24, kThemes[i].ink, LV_SYMBOL_OK);
    lv_obj_center(themeChecks[i]);
    themeSwatches[i] = swatch;
  }
  lv_obj_t *caption = makeLabel(page, &lv_font_montserrat_16, kTextFaint, "COLOUR");
  lv_obj_set_style_text_letter_space(caption, 4, 0);
  lv_obj_align(caption, LV_ALIGN_CENTER, 0, -26);
  themeName = makeLabel(page, &font_title_30, kText, "");
  lv_obj_align(themeName, LV_ALIGN_CENTER, 0, 10);
}

void buildPageDots() {
  lv_obj_t *row = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(row);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(row, LV_SIZE_CONTENT, 8);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 8, 0);
  lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -14);
  for (auto &dot : dots) {
    dot = lv_obj_create(row);
    lv_obj_remove_style_all(dot);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
  }
}

void buildOverlays() {
  // Volume toast, shown when the knob is turned away from the volume page.
  toast = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(toast);
  lv_obj_remove_flag(toast, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(toast, LV_SIZE_CONTENT, 48);
  lv_obj_set_style_radius(toast, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(toast, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(toast, col(kSurfacePressed), 0);
  lv_obj_set_style_pad_hor(toast, 20, 0);
  lv_obj_set_style_pad_column(toast, 10, 0);
  lv_obj_set_flex_flow(toast, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(toast, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 34);
  toastIcon = makeLabel(toast, &lv_font_montserrat_20, accent, LV_SYMBOL_VOLUME_MAX);
  toastLabel = makeLabel(toast, &lv_font_montserrat_24, kText, "--");
  lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);

  // Standby: one big wake button.
  standbyOverlay = makeOverlay();
  lv_obj_t *wake = makeCircleButton(standbyOverlay, 150, kSurface, kSurfacePressed);
  lv_obj_set_style_border_width(wake, 2, 0);
  lv_obj_set_style_border_color(wake, col(kTrack), 0);
  lv_obj_align(wake, LV_ALIGN_CENTER, 0, -30);
  wakeIcon = makeLabel(wake, &lv_font_montserrat_48, accent, LV_SYMBOL_POWER);
  lv_obj_center(wakeIcon);
  lv_obj_add_event_cb(wake, [](lv_event_t *) { kef::powerOn(); }, LV_EVENT_CLICKED, nullptr);
  lv_obj_align(makeLabel(standbyOverlay, &font_title_30, kText, "Standby"), LV_ALIGN_CENTER, 0, 82);
  standbyName = makeLabel(standbyOverlay, &font_small_16, kTextFaint);
  lv_obj_align(standbyName, LV_ALIGN_CENTER, 0, 120);
  lv_obj_t *hint = makeLabel(standbyOverlay, &font_small_16, kTextFaint, "Tap or press the knob to wake");
  lv_obj_align(hint, LV_ALIGN_CENTER, 0, 150);
  lv_obj_add_flag(standbyOverlay, LV_OBJ_FLAG_HIDDEN);

  // Connecting: shown at boot and whenever Wi-Fi or the speaker drops out.
  connOverlay = makeOverlay();
  spinner = lv_spinner_create(connOverlay);
  lv_spinner_set_anim_params(spinner, 1100, 240);
  lv_obj_set_size(spinner, 72, 72);
  lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -40);
  lv_obj_set_style_arc_width(spinner, 6, LV_PART_MAIN);
  lv_obj_set_style_arc_color(spinner, col(kTrack), LV_PART_MAIN);
  lv_obj_set_style_arc_width(spinner, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(spinner, col(accent), LV_PART_INDICATOR);
  connTitle = makeLabel(connOverlay, &font_body_22, kText, "Starting");
  lv_obj_align(connTitle, LV_ALIGN_CENTER, 0, 40);
  connDetail = makeLabel(connOverlay, &font_small_16, kTextFaint);
  lv_obj_align(connDetail, LV_ALIGN_CENTER, 0, 72);
}

// ---------------------------------------------------------------------------
// Themes

void renderThemePage() {
  for (int i = 0; i < kThemeCount; ++i) {
    const bool selected = i == themeIndex;
    lv_obj_set_style_border_width(themeSwatches[i], selected ? 4 : 0, 0);
    setHidden(themeChecks[i], !selected);
  }
  setText(themeName, kThemes[themeIndex].name);
  lv_obj_set_style_text_color(themeName, col(accent), 0);
  markStale(kPageTheme);
}

// Recolours everything that uses the accent. `save` stores the choice in
// flash a second later (so spinning through colours writes it once).
void applyTheme(int index, bool save) {
  themeIndex = (index % kThemeCount + kThemeCount) % kThemeCount;
  accent = kThemes[themeIndex].accent;
  accentPressed = (accent >> 1) & 0x7F7F7F;
  onAccent = kThemes[themeIndex].ink;

  // Widgets styled once at build time.
  lv_obj_set_style_text_color(chipIcon, col(accent), 0);
  lv_obj_set_style_arc_color(progressArc, col(accent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(playBtn, col(accent), 0);
  lv_obj_set_style_bg_color(playBtn, col(accentPressed), LV_STATE_PRESSED);
  lv_obj_set_style_text_color(playBtnIcon, col(onAccent), 0);
  lv_obj_set_style_text_color(toastIcon, col(accent), 0);
  lv_obj_set_style_text_color(wakeIcon, col(accent), 0);
  lv_obj_set_style_arc_color(spinner, col(accent), LV_PART_INDICATOR);

  // Widgets whose colour depends on state.
  renderVolume();
  renderSource();
  renderPlayer();
  renderDots(activePage);
  renderThemePage();
  for (int i = 0; i < kPageCount; ++i) markStale(Page(i));

  if (save) saveThemeAtMs = millis() + 1000;
}

void loadTheme() {
  prefs.begin("kefknob", false);
  themeIndex = prefs.getUChar("theme", UI_DEFAULT_THEME);
  if (themeIndex >= kThemeCount) themeIndex = UI_DEFAULT_THEME;
}

void saveThemeIfDue() {
  if (saveThemeAtMs && int32_t(millis() - saveThemeAtMs) >= 0) {
    saveThemeAtMs = 0;
    prefs.putUChar("theme", uint8_t(themeIndex));
  }
}

// ---------------------------------------------------------------------------
// Pages and slides

constexpr int kScreen = 480;

void setPage(Page page) {
  activePage = page;
  for (int i = 0; i < kPageCount; ++i) lv_obj_set_x(content[i], (i - page) * kScreen);
  renderDots(page);
  if (page == kPageSources) {
    focusCurrentSource();
  } else {
    focusVisible = false;
  }
  renderFocus();
  setHidden(toast, true);
}

// A swipe needs both pages as up-to-date images. The player's clock isn't
// tracked for staleness, so refresh it whenever music is playing.
bool readyToSlide(Page a, Page b) {
  for (Page p : {a, b}) {
    const bool clockMoving = p == kPagePlayer && shown.playState == kef::PlayState::Playing;
    if ((snapStale[p] || clockMoving || !snap[p]) && !takeSnapshot(p)) return false;
  }
  return true;
}

// progress: 0 = `from` fully shown, kScreen = its neighbour in `dir` fully shown.
void drawSlide(Page from, int dir, int progress) {
  const uint32_t stride = snap[from]->header.stride;
  if (dir > 0) {
    board::slideFrame(reinterpret_cast<const uint16_t *>(snap[from]->data),
                      reinterpret_cast<const uint16_t *>(snap[from + 1]->data), stride, progress);
  } else {
    board::slideFrame(reinterpret_cast<const uint16_t *>(snap[from - 1]->data),
                      reinterpret_cast<const uint16_t *>(snap[from]->data), stride, kScreen - progress);
  }
}

// Eases from one progress value to another, one frame per vsync.
void animateSlide(Page from, int dir, int fromProgress, int toProgress, uint32_t durationMs) {
  const uint32_t start = millis();
  for (;;) {
    const float t = min(1.0f, float(millis() - start) / durationMs);
    const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);  // ease-out cubic
    drawSlide(from, dir, fromProgress + int((toProgress - fromProgress) * eased));
    if (t >= 1.0f) return;
  }
}

void finishSlide(Page page) {
  setPage(page);
  board::endSlide();
  lv_display_trigger_activity(nullptr);
}

// Animated page change (knob double-press, source chip). Wraps from the last
// page to the first without animation.
void slideToPage(Page target) {
  const int dir = target - activePage;
  if (dir == 0) return;
  if (abs(dir) != 1 || !readyToSlide(activePage, target)) {
    setPage(target);
    return;
  }
  animateSlide(activePage, dir, 0, kScreen, 260);
  finishSlide(target);
}

// Finger-driven swipe: once a horizontal drag is recognised, the swipe owns the
// touch and tracks the finger directly, then eases to whichever page it was
// thrown towards.
struct SwipeTracker {
  bool down = false;
  bool eligible = false;
  int16_t startX = 0, startY = 0;
} swipe;

bool overlayVisible() {
  return !lv_obj_has_flag(standbyOverlay, LV_OBJ_FLAG_HIDDEN) ||
         !lv_obj_has_flag(connOverlay, LV_OBJ_FLAG_HIDDEN);
}

bool swipeCanStartAt(int16_t x, int16_t y) {
  if (overlayVisible()) return false;
  if (activePage == kPageVolume) {
    // Drags that start on the volume ring belong to the ring.
    const int dx = x - kScreen / 2, dy = y - kScreen / 2;
    if (dx * dx + dy * dy > 185 * 185) return false;
  }
  return true;
}

void trackSwipe(int dir) {
  const Page from = activePage;
  const Page to = Page(from + dir);
  if (!readyToSlide(from, to)) return;

  // Take the finger away from LVGL so whatever it pressed doesn't click.
  board::ignoreTouchUntilRelease();
  lv_indev_reset(nullptr, nullptr);

  int progress = 0;
  float velocity = 0;  // px per ms towards `to`
  uint32_t lastMs = millis();
  int16_t x, y;
  while (board::readTouchPoint(x, y)) {
    const int next = constrain((swipe.startX - x) * dir, 0, kScreen);
    const uint32_t now = millis();
    if (now > lastMs) velocity = 0.6f * velocity + 0.4f * float(next - progress) / (now - lastMs);
    lastMs = now;
    progress = next;
    drawSlide(from, dir, progress);
  }

  const bool commit = progress > kScreen / 3 || velocity > 0.5f;
  const int target = commit ? kScreen : 0;
  const uint32_t duration = 60 + uint32_t(abs(target - progress)) * 220 / kScreen;
  animateSlide(from, dir, progress, target, duration);
  finishSlide(commit ? to : from);
}

void detectSwipe() {
  int16_t x, y;
  if (!board::lastTouch(x, y)) {
    swipe.down = false;
    return;
  }
  if (!swipe.down) {
    swipe = {true, swipeCanStartAt(x, y), x, y};
    return;
  }
  if (!swipe.eligible) return;
  const int dx = x - swipe.startX, dy = y - swipe.startY;
  if (abs(dy) > 30 && abs(dy) > abs(dx)) {
    swipe.eligible = false;  // vertical movement: not a page swipe
    return;
  }
  if (abs(dx) < 20) return;
  const int dir = dx < 0 ? 1 : -1;  // finger moving left reveals the page to the right
  const int target = activePage + dir;
  swipe.eligible = false;
  if (target >= 0 && target < kPageCount) trackSwipe(dir);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public

void begin() {
  for (bool &stale : snapStale) stale = true;
  loadTheme();
  lv_display_t *display = lv_display_get_default();
  lv_display_set_theme(display, lv_theme_default_init(display, col(accent), col(kTextDim), true,
                                                      &lv_font_montserrat_16));

  lv_obj_t *screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, col(kBg), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  for (int i = 0; i < kPageCount; ++i) {
    // Opaque so its snapshot includes the background.
    content[i] = lv_obj_create(screen);
    lv_obj_remove_style_all(content[i]);
    lv_obj_set_size(content[i], LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(content[i], col(kBg), 0);
    lv_obj_set_style_bg_opa(content[i], LV_OPA_COVER, 0);
    lv_obj_remove_flag(content[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(content[i], LV_OBJ_FLAG_SCROLLABLE);
  }

  buildSourcesPage(content[kPageSources]);
  buildVolumePage(content[kPageVolume]);
  buildPlayerPage(content[kPagePlayer]);
  buildThemePage(content[kPageTheme]);
  buildPageDots();
  buildOverlays();

  applyTheme(themeIndex, false);
  setPage(kPageVolume);
}

void applyState(const kef::State &s) {
  const bool first = !haveState;
  const kef::State prev = shown;
  shown = s;
  haveState = true;

  // Someone used the KEF app/remote: light the screen so the change is visible.
  if (!first && s.externalRevision != prev.externalRevision) {
    lv_display_trigger_activity(nullptr);
    if (s.volume != prev.volume || s.muted != prev.muted) showToast();
  }

  if (first || s.volume != prev.volume || s.muted != prev.muted) renderVolume();
  if (first || s.source != prev.source || s.poweredOn != prev.poweredOn) renderSource();
  if (first || s.source != prev.source || s.playState != prev.playState ||
      s.canPlayPause != prev.canPlayPause || s.canSkip != prev.canSkip ||
      strcmp(s.title, prev.title) || strcmp(s.artist, prev.artist) || strcmp(s.album, prev.album))
    renderPlayer();
  renderProgress();
  renderOverlays();
}

void handleInput(const board::InputEvent &input) {
  lv_display_trigger_activity(nullptr);
  if (!haveState || !shown.wifiConnected || !shown.reachable) return;

  if (!shown.poweredOn) {
    if (input.type == board::Input::Click || input.type == board::Input::LongPress) kef::powerOn();
    return;
  }

  const Page page = activePage;
  switch (input.type) {
    case board::Input::RotateCw:
    case board::Input::RotateCcw: {
      const int direction = input.type == board::Input::RotateCw ? 1 : -1;
      if (page == kPageSources) {
        if (focusVisible) focus = (focus + direction + kSourceCount + 1) % (kSourceCount + 1);
        focusVisible = true;
        renderFocus();
      } else if (page == kPageTheme) {
        applyTheme(themeIndex + direction, true);  // live preview, saved once you stop
      } else {
        changeVolume(direction, input.timeMs);
      }
      break;
    }
    case board::Input::Click:
      if (page == kPageSources) {
        if (focusVisible) activateFocus();
        focusVisible = true;
        renderFocus();
      } else if (page == kPageVolume) {
        kef::setMuted(!shown.muted);
      } else if (page == kPageTheme) {
        goToPage(kPageVolume);
      } else if (shown.canPlayPause) {
        kef::playPause();
      }
      break;
    case board::Input::DoubleClick:
      goToPage(Page((page + 1) % kPageCount));
      break;
    case board::Input::LongPress:
      kef::standby();
      break;
  }
}

void tick() {
  static uint32_t lastProgressMs = 0;
  const uint32_t now = millis();

  if (now - lastProgressMs >= 250) {
    lastProgressMs = now;
    if (haveState) renderProgress();
  }
  if (toastHideAt && int32_t(now - toastHideAt) >= 0) {
    toastHideAt = 0;
    setHidden(toast, true);
  }
  if (pendingPage >= 0) {
    const Page target = Page(pendingPage);
    pendingPage = -1;
    slideToPage(target);
  }
  detectSwipe();
  refreshSnapshots();
  saveThemeIfDue();

  const uint32_t idle = lv_display_get_inactive_time(nullptr);
  board::setBacklightTarget(idle > DIM_AFTER_MS ? BACKLIGHT_DIM : BACKLIGHT_ACTIVE);
}

}  // namespace ui
