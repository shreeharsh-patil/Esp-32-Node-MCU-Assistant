# ESP32 NodeMCU Shreeharsh Assistant

Unique identity: `custom-esp32-nodemcu-pocket-ai`. Target: classic `esp32`,
ESP32-WROOM-32, at least 4 MB flash, PSRAM disabled. This is an official XiaoZhi
source port; it retains the upstream application and Opus pipeline. The
conversation variant enables `CONFIG_POCKET_CUSTOM_BACKEND=y`: authenticated
discovery and WSS go to your Vercel backend. Gemini keys remain on Vercel.
See [backend deployment](../../../backend/README.md).

Build with ESP-IDF 6.1 (upstream minimum 6.0.1; 5.x unsupported):

```powershell
$env:IDF_TOOLS_PATH='D:\Espressif\tools'
$env:PYTHONUTF8='1'
. 'D:\Espressif\esp-idf-v6.1\export.ps1'
python scripts/build.py custom-esp32-nodemcu-pocket-ai --name custom-esp32-nodemcu-pocket-ai --language en-US --wake-word disabled --zip
```

For independent audio/display tests, select
`--name custom-esp32-nodemcu-pocket-ai-diagnostics`. That build initializes the
same hardware and upstream Opus pipeline, with Wi-Fi/cloud disabled.

| Peripheral | Connections |
|---|---|
| ST7789 | 3V3 VCC/BLK; SCK18, MOSI23, CS13, DC27, reset14 |
| INMP441 | 3V3; BCLK26, WS25, SD34, L/R to GND |
| MAX98357A | verified USB 5V/VIN; BCLK26, WS25, DIN22 |
| PTT | BOOT GPIO0; optional external GPIO32 by changing `POCKET_PTT_GPIO` |
| Speaker | 8 ohms / 0.5 W, between SPK+ and SPK- |

All peripherals share ground. Check VIN voltage and the amplifier breakout's
SD/MODE bias before applying power. See [electrical notes](../../../docs/pocket/WIRING.md).

The 240x280 glass now renders in **280x240 landscape**. Hardware axis exchange
moves the centered ST7789 RAM inset to X=20, Y=0. RGB order, inversion and
26 MHz SPI mode 0 are retained. Set `POCKET_LANDSCAPE_FLIPPED` to `1` in
`config.h` for the opposite landscape direction. GPIO wiring is unchanged.
Panel orientation and borders still require observation on the physical display.
BLK has no software control because it is wired to 3V3.

I2S0 owns paired RX/TX channels at 16 kHz, Philips 32-bit stereo, 64 BCLK edges
per WS period. Both channels share identical clocks; GPIO34's left slot is
converted to mono PCM16. Playback duplicates the mono output into both slots.
The upstream resampler handles server Opus rates such as 24 kHz. Hardware clocks
remain running with zero-filled TX DMA while idle; conversation is half duplex.

Conversation firmware defaults to hands-free automatic listening after Wi-Fi
and the custom backend are ready. The board enters upstream AutoStop mode; Gemini
detects the end of a spoken question, and upstream resumes capture after the
reply playback drains. Idle channel closures reopen automatically, with failed
attempts backed off from 2 seconds to 60 seconds. Setup, playback,
offline periods and manual recordings do not trigger automatic opens. The lite
engine streams microphone audio through your backend to Gemini while Listening. This is server
speech detection, not an on-device wake word. There is no microphone capture
for interruption during replies. Serial `handsfree off` (or `stop`) pauses
automatic listening; `handsfree on` resumes. Reboot defaults to on. Offline
diagnostic firmware keeps manual controls and never auto-connects.

Optional PTT uses a 35 ms debounce, a 2.5-second startup delay, and requires a released
button before arming. GPIO0 is never driven. Release BOOT before power-on/reset
and while flashing. Maximum button/serial listening duration is 30 seconds.

The output starts at 20% of a quadratic software volume control. An absolute
8% full-scale peak cap applies at every requested volume, including MCP requests
for 100%. This is a digital limit, not a measured speaker wattage guarantee.
There is no automatic test tone at boot.

The single factory application occupies `0x10000..0x400000`. OTA firmware writes
are disabled in both discovery and manual update paths. Custom discovery returns
audio configuration and server time; there is no official activation or fallback
to stale vendor MQTT settings. There is no stock-board OTA identity or assets
partition. USB flashing is required for updates.

The upstream SPI/LVGL display driver supplies a single 280x20 RGB565 strip
(11,200 bytes). The companion uses a fixed set of rounded LVGL objects, a pure
black background with peach features, natural blinks/glances and bounded 512-byte captions.
The separate `face_animation.h` engine uses monotonic time, smooth attack/release
audio envelopes and no heap allocation. Listening eyes use actual mic RMS;
speaking mouths use PCM accepted by I2S with a 100 ms stale-sample timeout.
This is amplitude animation with bounded DMA latency, not phoneme lip sync or
proof of acoustic playback. See [UI design and validation](../../../docs/pocket/COMPANION_UI.md).
For Wi-Fi use the conversation build. The `Shreeharsh Assistant` setup hotspot is WPA2
protected; its key and `http://192.168.4.1` instructions alternate on the screen.
The form also requires your production HTTPS origin and URL-safe 32-128 character
device token. Settings are saved after DHCP and NVS readback. Missing backend
settings open setup on migration while retaining saved Wi-Fi. Provider keys
are never sent to the ESP32. `setup` reopens
provisioning; `wifi-reset CONFIRM` clears Wi-Fi only. See
[fixes and phone steps](../../../docs/pocket/DISPLAY_WIFI_FIX.md).
Speech models, heavy
emoji/GIF assets, dynamic glyph push, AEC, camera, Bluetooth, touch, and battery
features are disabled. Non-Latin glyphs outside the compiled basic font may be
unavailable. Voice/model/instructions are your Vercel environment settings.

See [getting started](../../../POCKET_README.md),
[hardware tests](../../../docs/pocket/HARDWARE_TESTS.md), and
[source analysis](../../../docs/pocket/SOURCE_ANALYSIS.md).
Compilation is not hardware or real-provider acceptance testing.
