#include "board.h"

#include <Adafruit_CST8XX.h>
#include <Arduino_GFX_Library.h>
#include <PCF8574.h>
#include <Wire.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>
#include <lvgl.h>

#include <atomic>

#include "config.h"

// Pin map and init sequence follow Elecrow's reference sketch:
// https://github.com/Elecrow-RD/CrowPanel-2.1inch-HMI-ESP32-Rotary-Display-480-480-IPS-Round-Touch-Knob-Screen

namespace board {
namespace {

constexpr int kWidth = 480;
constexpr int kHeight = 480;
constexpr size_t kFrameBytes = kWidth * kHeight * 2;
constexpr int kBounceBufferLines = 40;  // panel DMA bounce buffers (x2), internal SRAM

constexpr int kPinSda = 38;
constexpr int kPinScl = 39;
constexpr int kPinEncA = 42;
constexpr int kPinEncB = 4;
constexpr int kPinBacklight = 6;

constexpr uint8_t kAddrExpander = 0x21;
constexpr uint8_t kAddrTouch = 0x15;

// PCF8574 I/O expander lines.
constexpr uint8_t kExpTouchReset = P0;
constexpr uint8_t kExpTouchInt = P2;
constexpr uint8_t kExpLcdPower = P3;
constexpr uint8_t kExpLcdReset = P4;
constexpr uint8_t kExpEncoderButton = P5;

PCF8574 expander(kAddrExpander, kPinSda, kPinScl);
Adafruit_CST8XX touch;

// ---------------------------------------------------------------------------
// Display
//
// The RGB panel is driven through esp_lcd directly; Arduino_GFX is only used
// to send the ST7701 init commands. The driver owns three PSRAM framebuffers:
//  - LVGL renders in direct mode into buffers 0 and 1 and swaps at vsync, so a
//    half-drawn frame is never on screen (no tearing).
//  - Page slides (slideFrame) cycle through all three, so the next frame can
//    be composed while the previous one waits for its vsync.

Arduino_DataBus *panelBus = new Arduino_SWSPI(
    GFX_NOT_DEFINED /* DC: ST7701 uses 9-bit SPI */, 16 /* CS */, 2 /* SCK */, 1 /* SDA */,
    GFX_NOT_DEFINED /* MISO */);

esp_lcd_panel_handle_t panel = nullptr;
void *framebuffers[3] = {};
int shownFb = 0;     // on screen
int pendingFb = -1;  // handed to the driver, goes on screen at the next vsync
SemaphoreHandle_t vsyncSem;

bool IRAM_ATTR onVsync(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(vsyncSem, &woken);
  return woken == pdTRUE;
}

void initPanel() {
  // ST7701 register setup over 9-bit SPI, as Arduino_RGB_Display::begin() does.
  panelBus->begin();
  panelBus->sendCommand(0x01);  // software reset
  delay(120);
  panelBus->batchOperation(st7701_type5_init_operations, sizeof(st7701_type5_init_operations));
  // Keep MADCTL's BGR bit set (as Elecrow's sketch does).
  panelBus->beginWrite();
  panelBus->writeCommand(0x36);
  panelBus->write(0x08);
  panelBus->endWrite();

  esp_lcd_rgb_panel_config_t cfg = {};
  cfg.clk_src = LCD_CLK_SRC_DEFAULT;
  cfg.timings.pclk_hz = 12000000;  // ~45 Hz refresh
  cfg.timings.h_res = kWidth;
  cfg.timings.v_res = kHeight;
  cfg.timings.hsync_pulse_width = 4;
  cfg.timings.hsync_back_porch = 20;
  cfg.timings.hsync_front_porch = 10;
  cfg.timings.vsync_pulse_width = 4;
  cfg.timings.vsync_back_porch = 20;
  cfg.timings.vsync_front_porch = 10;
  cfg.data_width = 16;
  cfg.bits_per_pixel = 16;
  cfg.num_fbs = 3;
  cfg.bounce_buffer_size_px = kWidth * kBounceBufferLines;  // slack while Wi-Fi/CPU use PSRAM
  cfg.dma_burst_size = 64;
  cfg.hsync_gpio_num = 15;
  cfg.vsync_gpio_num = 7;
  cfg.de_gpio_num = 40;
  cfg.pclk_gpio_num = 41;
  cfg.disp_gpio_num = -1;
  // Data lines, LSB first: blue, green, red. Red and blue are deliberately
  // crossed relative to Elecrow's pin map: this panel shows R/B swapped against
  // LVGL's RGB565, and crossing the pins fixes that at no cost.
  const int dataPins[16] = {
      46, 3,  8,  18, 17,     // D0-D4   (Elecrow labels these R0-R4)
      14, 13, 12, 11, 10, 9,  // D5-D10  green
      5,  45, 48, 47, 21,     // D11-D15 (Elecrow labels these B0-B4)
  };
  memcpy(cfg.data_gpio_nums, dataPins, sizeof(dataPins));
  cfg.flags.fb_in_psram = 1;

  ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&cfg, &panel));
  esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
  callbacks.on_vsync = onVsync;
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, nullptr));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 3, &framebuffers[0], &framebuffers[1],
                                                     &framebuffers[2]));
  for (void *fb : framebuffers) memset(fb, 0, kFrameBytes);
}

// Queues framebuffer `index` for the next vsync without waiting for it.
void queueFb(int index) {
  xSemaphoreTake(vsyncSem, 0);  // a vsync from before this point isn't our swap
  esp_lcd_panel_draw_bitmap(panel, 0, 0, kWidth, kHeight, framebuffers[index]);
  pendingFb = index;
}

// Waits until the queued framebuffer (if any) is on screen.
void waitForQueuedFb() {
  if (pendingFb < 0) return;
  xSemaphoreTake(vsyncSem, pdMS_TO_TICKS(50));
  shownFb = pendingFb;
  pendingFb = -1;
}

// LVGL draws straight into the hidden framebuffer, so there's nothing to copy:
// once the frame is complete, show that buffer and wait for the swap, after
// which the other one is free to draw into.
void flushDisplay(lv_display_t *display, const lv_area_t *, uint8_t *pxMap) {
  if (lv_display_flush_is_last(display)) {
    queueFb(pxMap == framebuffers[0] ? 0 : 1);
    waitForQueuedFb();
  }
  lv_display_flush_ready(display);
}

// ---------------------------------------------------------------------------
// Touch

// While set, LVGL is told "no touch" until the finger lifts: used when a touch
// wakes a dimmed screen (so it can't press whatever was under it) and when a
// page swipe takes over the finger.
bool swallowTouch = false;

// Last raw sample, for swipe detection in the UI.
bool lastTouchDown = false;
int16_t lastTouchX = 0, lastTouchY = 0;

void readTouch(lv_indev_t *, lv_indev_data_t *data) {
  static int16_t stableX = -1;
  static int16_t stableY = -1;

  int16_t x, y;
  lastTouchDown = readTouchPoint(x, y);
  if (!lastTouchDown) {
    swallowTouch = false;
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  lastTouchX = x;
  lastTouchY = y;
  if (isDimmed()) {
    swallowTouch = true;
    lv_display_trigger_activity(nullptr);
  }
  if (swallowTouch) {
    data->state = LV_INDEV_STATE_RELEASED;
    return;
  }
  // Ignore jitter of a few pixels so held buttons don't redraw constantly.
  if (stableX < 0 || abs(x - stableX) >= 4 || abs(y - stableY) >= 4) {
    stableX = x;
    stableY = y;
  }
  data->point.x = stableX;
  data->point.y = stableY;
  data->state = LV_INDEV_STATE_PRESSED;
}

// ---------------------------------------------------------------------------
// Rotary encoder and button

QueueHandle_t inputQueue;
std::atomic<uint32_t> rotationCount{0};

void pushInput(Input type) {
  const InputEvent ev{type, millis()};
  xQueueSend(inputQueue, &ev, 0);
}

// Sampled from a task rather than an ISR: the EC28A contacts bounce enough that
// edge interrupts double-count, and 2 ms polling is plenty for a hand-turned knob.
// Each detent toggles A once (high on one click, low on the next), so both
// edges count. B != A after an edge means counter-clockwise either way.
void encoderTask(void *) {
  int stableA = digitalRead(kPinEncA);
  int lastSample = stableA;
  for (;;) {
    const int a = digitalRead(kPinEncA);
    // Debounce: accept a new level only once it reads the same twice in a row.
    if (a == lastSample && a != stableA) {
      stableA = a;
      rotationCount++;
      const bool ccw = digitalRead(kPinEncB) != a;
      pushInput((ccw != ENCODER_REVERSE) ? Input::RotateCcw : Input::RotateCw);
    }
    lastSample = a;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void pollButton() {
  static uint8_t raw = HIGH;
  static uint8_t stable = HIGH;
  static uint32_t rawChangedMs = 0;
  static uint32_t pressedMs = 0;
  static uint32_t releasedMs = 0;
  static uint32_t rotationAtPress = 0;
  static uint8_t clicks = 0;
  static bool longFired = false;

  const uint8_t sample = expander.digitalRead(kExpEncoderButton, true);
  const uint32_t now = millis();

  if (sample != raw) {
    raw = sample;
    rawChangedMs = now;
  }
  if (raw != stable && now - rawChangedMs >= 30) {
    stable = raw;
    if (stable == LOW) {
      pressedMs = now;
      longFired = false;
      rotationAtPress = rotationCount;
    } else if (!longFired && rotationCount == rotationAtPress) {
      // Only count presses where the knob wasn't turned at the same time.
      clicks++;
      releasedMs = now;
    }
  }

  if (stable == LOW && !longFired && now - pressedMs >= BUTTON_LONG_PRESS_MS) {
    longFired = true;
    clicks = 0;
    pushInput(Input::LongPress);
  }
  if (clicks >= 2) {
    clicks = 0;
    pushInput(Input::DoubleClick);
  } else if (clicks == 1 && stable == HIGH && now - releasedMs >= BUTTON_DOUBLE_CLICK_MS) {
    clicks = 0;
    pushInput(Input::Click);
  }
}

// ---------------------------------------------------------------------------
// Backlight

uint8_t backlightLevel = 0;
uint8_t backlightTarget = BACKLIGHT_ACTIVE;

}  // namespace

void begin() {
  inputQueue = xQueueCreate(32, sizeof(InputEvent));

  Wire.begin(kPinSda, kPinScl);
  expander.pinMode(kExpTouchReset, OUTPUT);
  expander.pinMode(kExpTouchInt, OUTPUT);
  expander.pinMode(kExpLcdPower, OUTPUT);
  expander.pinMode(kExpLcdReset, OUTPUT);
  expander.pinMode(kExpEncoderButton, INPUT_PULLUP);
  if (!expander.begin()) Serial.println("[board] PCF8574 not found");

  // Panel power + reset.
  expander.digitalWrite(kExpLcdPower, HIGH);
  delay(100);
  expander.digitalWrite(kExpLcdReset, HIGH);
  delay(100);
  expander.digitalWrite(kExpLcdReset, LOW);
  delay(120);
  expander.digitalWrite(kExpLcdReset, HIGH);
  delay(120);

  // Touch reset.
  expander.digitalWrite(kExpTouchReset, HIGH);
  delay(100);
  expander.digitalWrite(kExpTouchReset, LOW);
  delay(120);
  expander.digitalWrite(kExpTouchReset, HIGH);
  delay(120);
  expander.digitalWrite(kExpTouchInt, HIGH);
  delay(120);

  vsyncSem = xSemaphoreCreateBinary();
  initPanel();

  if (!touch.begin(&Wire, kAddrTouch)) Serial.println("[board] touch controller not found");

  pinMode(kPinEncA, INPUT);
  pinMode(kPinEncB, INPUT);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });

  lv_display_t *display = lv_display_create(kWidth, kHeight);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  lv_display_set_flush_cb(display, flushDisplay);
  // Direct mode: LVGL draws only what changed, straight into two of the
  // panel's framebuffers, and keeps them in sync itself.
  lv_display_set_buffers(display, framebuffers[0], framebuffers[1], kFrameBytes,
                         LV_DISPLAY_RENDER_MODE_DIRECT);

  lv_indev_t *touchIndev = lv_indev_create();
  lv_indev_set_type(touchIndev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(touchIndev, readTouch);
  lv_indev_set_display(touchIndev, display);

  ledcAttach(kPinBacklight, 5000, 8);
  ledcWrite(kPinBacklight, 0);
  // Elecrow's reference sketch releases this line once the panel is running.
  expander.digitalWrite(kExpLcdPower, LOW);

  xTaskCreatePinnedToCore(encoderTask, "encoder", 2048, nullptr, 2, nullptr, 0);

  Serial.printf("[board] ready. internal RAM free %u, PSRAM free %u\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void poll() {
  pollButton();

  // Fade the backlight a few steps per loop.
  if (backlightLevel != backlightTarget) {
    const int diff = int(backlightTarget) - int(backlightLevel);
    const int step = diff > 0 ? max(1, diff / 6) : min(-1, diff / 10);
    backlightLevel = uint8_t(int(backlightLevel) + step);
    ledcWrite(kPinBacklight, backlightLevel);
  }
}

bool nextInput(InputEvent &out) {
  return xQueueReceive(inputQueue, &out, 0) == pdTRUE;
}

void slideFrame(const uint16_t *left, const uint16_t *right, uint32_t strideBytes, int offset) {
  offset = constrain(offset, 0, kWidth);

  // Compose into a buffer that's neither on screen nor queued, so this
  // overlaps the wait for the previous frame's vsync.
  int target = 0;
  while (target == shownFb || target == pendingFb) ++target;
  auto *dst = static_cast<uint16_t *>(framebuffers[target]);
  const size_t leftBytes = (kWidth - offset) * 2;
  const size_t rightBytes = offset * 2;
  const auto *l = reinterpret_cast<const uint8_t *>(left) + offset * 2;
  const auto *r = reinterpret_cast<const uint8_t *>(right);
  for (int y = 0; y < kHeight; ++y) {
    if (leftBytes) memcpy(dst, l, leftBytes);
    if (rightBytes) memcpy(dst + (kWidth - offset), r, rightBytes);
    dst += kWidth;
    l += strideBytes;
    r += strideBytes;
  }

  waitForQueuedFb();  // the previous frame gets its full vsync on screen
  queueFb(target);
}

void endSlide() {
  waitForQueuedFb();

  // LVGL's next frame goes into its "active" buffer, which mustn't be the one
  // on screen. Buffer 2 is never LVGL's, so park the current frame there.
  lv_draw_buf_t *active = lv_display_get_buf_active(lv_display_get_default());
  if (active && active->data == framebuffers[shownFb]) {
    memcpy(framebuffers[2], framebuffers[shownFb], kFrameBytes);
    queueFb(2);
    waitForQueuedFb();
  }
  lv_obj_invalidate(lv_screen_active());
}

bool readTouchPoint(int16_t &x, int16_t &y) {
  if (!touch.touched()) return false;
  const CST_TS_Point p = touch.getPoint(0);
  x = constrain(p.x, 0, kWidth - 1);
  y = constrain(p.y - 20, 0, kHeight - 1);  // panel offset per Elecrow
  return true;
}

void ignoreTouchUntilRelease() {
  swallowTouch = true;
}

bool lastTouch(int16_t &x, int16_t &y) {
  x = lastTouchX;
  y = lastTouchY;
  return lastTouchDown && !swallowTouch;
}

void setBacklightTarget(uint8_t level) {
  backlightTarget = level;
}

bool isDimmed() {
  return backlightTarget < BACKLIGHT_ACTIVE;
}

}  // namespace board
