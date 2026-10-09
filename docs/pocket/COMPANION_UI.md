# Landscape companion UI

The implementation remains in the existing XiaoZhi/ESP-IDF firmware with the
board's user-owned Vercel/Gemini backend.
The device is a classic ESP32-D0WD-V3 with 4 MB flash and no PSRAM. Its ST7789
uses ESP-IDF `esp_lcd` and LVGL 9.5; it does not use Arduino/TFT_eSPI. Existing
GPIO assignments, paired I2S0 clocks, resampling, Opus, hands-free/PTT policy and
existing transport/activation interfaces are retained. A board-owned protected
SoftAP portal replaces the old setup page for this board. Wake word,
AEC and firmware OTA stay disabled as in the existing no-PSRAM build.

The UI uses pure black around warm white capsule eyes, soft glowing highlights,
pastel pink oval cheeks and a small expressive mouth. It uses
no logos, GIF assets, downloaded graphics, permanent status bars or debug panels.
Raw initialization strings, status details, version, heap and diagnostics stay
on serial. The actual provisioning SSID and xiaozhi.me pairing destination are
shown when needed because they are required operational identifiers.

## Display configuration

`main/boards/custom-esp32-nodemcu-pocket-ai/config.h` selects logical 280x240,
`DISPLAY_SWAP_XY=true`, X gap=20 and Y gap=0. The centered native 240x280 window
occupies rows 20..299 of ST7789's 240x320 memory; swapping axes moves the gap to
X. `POCKET_LANDSCAPE_FLIPPED=0` and `1` choose opposite hardware mirror directions.
The ESP-IDF driver and LVGL port receive the same flags. RGB/inversion/26 MHz
SPI and all physical wiring remain unchanged. Validate both orientation and
the visible borders on the actual module; simulation cannot establish them.

The centered face layout gently moves upward when a short two-line contextual
caption is required. Its old rounded plate, borders and cream rim are now black
and visually disappear into the opaque root. Captions use the
existing antialiased 16-pixel basic font, static bounded UTF-8 buffers and ellipsis
rather than an endless marquee. Long replies remain audible through the existing
pipeline; the small screen displays a bounded excerpt. The compiled font's
existing language limitations are unchanged. Wi-Fi setup shows the real SSID
and setup address. A narrow optional Display interface hook receives the actual
upstream activation code directly, so pairing does not depend on parsing raw
server text or generating a substitute code.

## Animation and integration

- `face_animation.h`: allocation-free deterministic time-based engine, seeded
  once on device; variable blink intervals, occasional double blinks, short
  eased glances that return to center, breathing, happy/curious/shy expressions,
  sleepy half-eyes, brief surprise with smooth recovery, processing scans,
  concerned errors, thinking dots and boot emergence.
- `face_renderer.*`: fixed LVGL primitives and persistent curve point arrays.
  Integer geometry and opacity are updated only when changed. Hardware RGB565
  quantization made large gradients visibly banded in the native render, so the
  background/root/plate use opaque black; eyes keep a small warm-white gradient
  and rounded highlights. Each cheek reuses five rounded oval layers with
  pink-to-black color falloff. Happy/shy expressions strengthen the blush,
  playback amplitude gently pulses it, and sleepy mode dims it.
- `pocket_display.*`: application state priority, caption filtering, provisioning,
  connection/error feedback, theme identity, power-save expression and cleanup.
  It never mutates the application state machine. Listening/playback cancels
  explicit serial face previews.
- `pocket_audio_codec.*`: small atomic RMS/timestamp observer after successful
  I2S writes; existing PCM conversion, volume cap, DMA and clock configuration
  are unchanged. The observer does not allocate, change samples, wait on UI,
  move audio buffers or invoke graphics from an audio task.

Mic and playback signals have separate noise floors and 45 ms attack / 160 ms
release smoothing. Silence closes the mouth even while the service still reports
Speaking. Playback RMS is measured before volume attenuation but is suppressed
when muted or disabled and expires after 100 ms without a successful write.
I2S DMA can queue up to approximately 48 ms, so this is approximate amplitude
alignment, not measured acoustic synchronization or phoneme lip sync. A serial
`face speaking` preview without actual PCM leaves the mouth closed deliberately.

The attentive listening eyes expand and brighten from actual microphone RMS.
For hands-free turns, a confirmed user transcript arms the visual response wait;
after 900 ms of microphone quiet the eyes look slightly upward. Speech resuming
restores the attentive pose. Noise alone cannot trigger a completed-question pose.
The 450-RMS threshold and quiet interval affect expressions only, never capture
or server speech detection. Replies and new capture sessions reset the visual
wait; a 60-second timeout retains the existing retry/error feedback.

The mouth follows actual successfully written output PCM while the eyes blink
independently. Active listening, replies, connection work, setup and errors take
priority over idle emotion previews. Facial geometry and thinking dots remain
above the bottom caption area, including transitions into Wi-Fi instructions.

Active animation requests 33 ms (~30 fps), calm idle 66 ms (~15 fps) and sleepy
100 ms (~10 fps). If a measured LVGL rendering pass exceeds 28 ms, the UI falls
back to at least 66 ms. The existing graphics task stays priority 1. Rendering
uses partial dirty regions and a single 280x20 RGB565 DMA strip (11,200 bytes),
not a full framebuffer or double-buffered image cache. No decorative delay or
new graphics task is introduced. Serial `diag` reports actual UI ticks, rendered
passes, long tick gaps and latest/maximum render durations; these counters must
be measured on the device before claiming a sustained frame rate.

Backlight BLK is physically tied to 3V3: firmware cannot dim that LED or promise
battery savings from dimming. Sleepy mode reduces eye luminance and drawing rate.
No tear-sync wire exists; partial redraw reduces transfer load, but absence of
visible tearing must be observed on the real panel.

## Verification and reproducible preview

`python -X utf8 scripts/pocket_host_tests.py` runs the upstream suite and native
audio/protocol/face tests. The face test advances three simulated hours through
all expressions, captions, full-scale levels and silence; it checks screen/glow
bounds, real-level response, mouth closure and UTF-8 truncation. Simulation is
not a three-hour device endurance test.

The headless harness in `scripts/pocket_ui_sim` compiles the same LVGL renderer
and actual firmware font, uses the same 11,200-byte RGB565 strip, validates every
flush rectangle and checks heap stability across repeated creation/destruction.
Its synthetic state, setup data and audio levels are labelled simulation;
they are never installed as service responses on the device. The harness also
checks that cheek center pixels stay pink in every expression, including dim
sleepy rendering, and that stale white screen edges are cleared.

```powershell
cmake -S scripts/pocket_ui_sim -B pocket-ui-sim-build -G Ninja
cmake --build pocket-ui-sim-build
.\pocket-ui-sim-build\pocket_ui_sim.exe docs/pocket/ui-preview
python scripts/pocket_ui_preview.py # Pillow required for PNG/GIF conversion
```

Use a native host C/C++ toolchain for that harness, not the Xtensa toolchain.
`ui-preview/companion-preview.png`, `boot-preview.gif` and
`audio-reactive-preview.gif` are generated from actual
LVGL captures. Firmware build results, measured device results and still-pending
checks are recorded separately in `UI_VALIDATION.json`. Build success or a
headless capture does not establish audio quality, Wi-Fi recovery, panel rotation,
flicker, watchdog freedom or long-duration device memory stability.

## USB flashing on the detected board

Release BOOT, keep USB connected, and use COM4 with the guarded prebuilt flasher:

```powershell
python scripts/pocket_flash.py --port COM4 # normal operation and Wi-Fi setup
python scripts/pocket_flash.py --port COM4 --diagnostics # offline tests only
```

These commands preserve existing NVS with this partition layout. Do not add
`--erase-all` for the UI update. Diagnostics deliberately disables networking;
use the conversation build for phone setup. `colors` and
`home` still work; `face NAME` previews an expression for 10 seconds while idle.
`diag` exposes technical information only through serial. The prior acoustic
tests were silent; the amplifier SD/SD_MODE enable change and audible playback
still need verification before claiming a working voice assistant.

## Physical acceptance for this face update

After backend deployment and a USB update, confirm that speech brightens/widens
the eyes, the pause after a recognized question moves the gaze upward, and an
audible reply moves the mouth with independent blinks. Quiet PCM must close the
mouth; a stuck-open mouth during silence is a failure. Tune the visual RMS floor
only after recording actual quiet/speech levels from serial diagnostics.

Check `face happy`, `face shy`, `face surprised`, `face curious`, `face sleepy`
and `face processing` while idle/setup. Cheeks should be oval and pink, even when
dim; no white strips, clipping or persistent previous frames should remain.
Verify both hotspot/password and provisioning-address instructions fit below the
face. Measure `diag` during a full multi-turn conversation and reconnect, then
check minimum heap, task stacks and render timings over an extended run. The
current renderer captures and builds do not establish these physical results.
