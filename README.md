# KEF Knob

A desk remote for **KEF LS50 Wireless II** speakers, built on the
**Elecrow CrowPanel 2.1" ESP32-S3 rotary display**: a 480×480 round touchscreen
inside a rotary knob. It talks to the speakers directly over your LAN through
their local HTTP API, so there's no cloud and no hub.

## What it does

| Page (swipe) | Shows | Touch | Knob |
|---|---|---|---|
| **Sources** (left) | Ring of Wi-Fi, Bluetooth, TV, Optical, Coaxial, Aux, with power in the middle | Tap a source to switch to it; tap ⏻ for standby/wake | Turn to move the highlight, press to select |
| **Volume** (home) | 270° volume arc, big number, current source, mute | Drag the arc; tap the speaker button to mute; tap the source chip to jump to Sources | Turn = volume (speeds up when you spin fast), press = mute |
| **Now Playing** | Title / artist / album, state, progress ring, time | ⏮ ⏯ ⏭ | Turn = volume (a pop-up shows the level), press = play/pause |
| **Colour** (far right) | Ring of 8 accent colours, current one named in the middle | Tap a colour | Turn to preview colours live, press to go back to Volume |

Swipe left or right anywhere except on the volume ring. The page follows your finger
and settles when you let go. Your colour choice is saved and survives a restart.

On every page:

- **Double-press the knob** to go to the next page.
- **Long-press the knob** to put the speakers in standby.
- In standby the screen shows a large wake button. Tap it or press the knob to wake.
- Changes made elsewhere (KEF Connect app, remote, TV) appear live, and the screen lights up to show them.
- The screen dims after 30 s idle. A touch on a dimmed screen only wakes it, so you won't press a button by accident.

## Setup

1. **Give the speaker a fixed IP.** In your router, add a DHCP reservation for the
   primary LS50 W2. You can find its IP in KEF Connect under the speaker's settings, or in
   your router's client list.
2. **Add your details:**
   ```
   copy include\secrets.example.h include\secrets.h
   ```
   Then edit `include/secrets.h` and set `WIFI_SSID`, `WIFI_PASSWORD` and `KEF_HOST`.
   The ESP32 only supports **2.4 GHz** Wi-Fi.
3. **Build and flash** with [PlatformIO](https://platformio.org/). The VS Code extension
   works, or use the CLI:
   ```
   pio run -t upload
   pio device monitor
   ```
   Connect the board with USB-C. If upload fails, hold **BOOT**, tap **RESET**, release
   BOOT and try again.

The first build downloads ESP-IDF and recompiles it with display-friendly settings
(`custom_sdkconfig` in `platformio.ini`), which takes several minutes. Later builds are
quick. Two requirements for that build:

- **The project path must not contain spaces.** The ESP-IDF build fails on them.
- **On Windows, build from PowerShell or VS Code, not Git Bash.** ESP-IDF's tools refuse to run under MSYS.

## Customising

`include/config.h`: default colour theme (before one is picked on the Colour page), backlight
levels, dim timeout, knob direction and click timings. The themes themselves are the `kThemes`
list in `src/ui.cpp`. Add or change entries there, keeping eight for the swatch ring.

## Project layout

```
platformio.ini        board, PSRAM, flash and library versions
include/config.h      user tunables
include/secrets.h     Wi-Fi and speaker IP (git-ignored)
include/lv_conf.h     LVGL 9.1 configuration
src/board.*           panel (ST7701 RGB), touch (CST826), knob, backlight
src/kef_client.*      KEF API: event long-poll and command sender (FreeRTOS tasks, core 0)
src/ui.*              LVGL UI: pages, overlays, input handling
src/lv_mem_psram.c    puts LVGL's memory in PSRAM so internal RAM is left for Wi-Fi
src/fonts/            Montserrat fonts generated with lv_font_conv (Latin-1 for track info)
docs/KEF_API.md       the speaker API reference
```

## How it talks to the speakers

The firmware subscribes to the speaker's event queue (`/api/event/modifyQueue`) and
long-polls it (`/api/event/pollQueue`), so changes show up within milliseconds and no
polling loop runs.

Commands go out on a separate task. Each change updates the screen immediately (an
optimistic update) and is then sent to the speaker. If you spin the knob quickly, the
changes are merged and only the latest volume is sent. Speaker echoes of in-between
values are ignored for a moment so the arc doesn't jitter. See
[docs/KEF_API.md](docs/KEF_API.md) for the endpoint list.

## How the display works

The panel is a 16-bit RGB interface that streams continuously from a framebuffer in
PSRAM, so memory bandwidth is the limiting resource. A few things keep it smooth:

- **Display-friendly ESP-IDF settings** (`custom_sdkconfig`): code and constants can be
  read from PSRAM while flash is busy, the display interrupts are IRAM-safe, the data
  cache is larger, and Wi-Fi buffers are kept out of PSRAM. Without these you get
  flickering lines and an image that occasionally jumps.
- **No tearing.** The `esp_lcd` RGB driver has three framebuffers. LVGL renders in direct
  mode into two of them and swaps at vsync, so a half-drawn frame is never shown.
- **Fast page swipes.** Redrawing the whole UI each frame of a scroll only manages about
  12 fps on this chip. Instead, each page is pre-rendered into an image in PSRAM while
  it's idle. A swipe then copies rows of the two page images straight into a spare
  framebuffer, using all three buffers so composing overlaps the vsync wait. That gives
  about 40 fps (`board::slideFrame`, and `trackSwipe` in `ui.cpp`).
- **LVGL objects live in PSRAM** (`src/lv_mem_psram.c`), which leaves internal RAM for
  Wi-Fi. Low internal RAM shows up as dropped packets and laggy volume changes.

## Hardware notes

The pin map and panel init sequence come from Elecrow's reference code
([repo](https://github.com/Elecrow-RD/CrowPanel-2.1inch-HMI-ESP32-Rotary-Display-480-480-IPS-Round-Touch-Knob-Screen)):

- **I²C (38/39):** PCF8574 expander @0x21 (LCD power/reset, touch reset, knob button on P5) and CST826 touch @0x15
- **Encoder:** A=GPIO42, B=GPIO4
- **Backlight:** GPIO6 (PWM)
- **RGB panel:** 16-bit, 12 MHz pixel clock (~45 Hz); ST7701 init over 9-bit SPI on CS=16, SCK=2, SDA=1.
  The red and blue data lines are deliberately crossed relative to Elecrow's pin map
  (see `initPanel()`), because the panel shows R/B swapped against LVGL's RGB565.

## Troubleshooting

- **Stuck on "Looking for speakers":** check that `KEF_HOST` is correct and the speaker is on the same
  network. From a PC, `http://<KEF_HOST>/api/getData?path=player:volume&roles=value` should return JSON.
- **Knob turns the wrong way:** set `ENCODER_REVERSE 1` in `config.h`.
- **Colours wrong (red and blue swapped, e.g. the orange accent looks blue):** swap the
  `D0-D4` and `D11-D15` rows in `dataPins` in `board.cpp` `initPanel()`.
- **Volume lags or commands fail (`[kef] setData ... slow/failed` on serial):** usually a weak Wi-Fi
  link. On mesh networks the ESP32 tends to join whichever node answers first, so the knob scans and
  joins the strongest node itself. If the signal later drops below −60 dBm, it rescans every 30 s and
  moves when another node is at least 10 dB stronger (logged as `[wifi] roaming`). If nothing better
  is found, it waits 5 minutes before scanning again. If no node reaches it well, add one nearer the knob.
- **Flickering lines or a jumping image:** the `custom_sdkconfig` settings in `platformio.ini`
  prevent this. Check that the build actually applied them (`sdkconfig.crowpanel21` should contain
  `CONFIG_SPIRAM_XIP_FROM_PSRAM=y`).
- **Serial log:** `pio device monitor` shows connection events, slow or failed commands, and a
  once-a-minute `[mem]` line. Internal RAM should stay well above 20 KB, since Wi-Fi needs it.

## Credits and licence

- KEF API details are from the community's reverse engineering, especially
  [pykefcontrol](https://github.com/N0ciple/pykefcontrol). This project isn't affiliated
  with KEF.
- Board pin map and panel init are from
  [Elecrow's CrowPanel examples](https://github.com/Elecrow-RD/CrowPanel-2.1inch-HMI-ESP32-Rotary-Display-480-480-IPS-Round-Touch-Knob-Screen).
- UI built with [LVGL](https://lvgl.io) (MIT). Display init uses
  [Arduino_GFX](https://github.com/moononournation/Arduino_GFX).
- Fonts in `src/fonts/` are generated from [Montserrat](https://github.com/JulietaUla/Montserrat)
  and are covered by the SIL Open Font License 1.1 (see `src/fonts/OFL.txt`).

The project's own code is under the MIT licence (see `LICENSE`).
