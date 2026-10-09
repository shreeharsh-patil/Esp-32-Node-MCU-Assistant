# Black display and reliable Wi-Fi setup

Target: classic ESP32-D0WD-V3 / NodeMCU, 4 MB flash, no PSRAM, native 240x280
ST7789 with logical 280x240 landscape. All TFT/I2S/PTT pins are unchanged.

## Root causes established from source and logs

1. The installed image was the **offline diagnostic variant**. Its serial log
   explicitly states `network and cloud disabled`. It cannot create a hotspot
   or connect to a router; USB connection does not switch firmware variants.
2. The SPI LCD constructor deliberately filled the panel white before enabling
   it. Its startup clear also omitted the configured display offsets. A new
   optional initial RGB565 color preserves other boards' defaults, while this
   board requests black and clears the full visible offset window.
3. The companion renderer deliberately used a cream screen, translucent root,
   pale rounded rims and a charcoal plate. Those are firmware-rendered pixels,
   not proof of a physical white bezel. The user confirmed the animated face
   with a white area around it. They are now removed visually with opaque black
   screen/root/plate and no pale border. Screen/top/system children are cleaned
   once before creating this board's only UI. No inherited branded chat UI is
   created, and only LVGL writes the panel after its startup clear.
4. The previous upstream setup AP creates only an AP netif on a first boot with
   no saved station credentials. Its credential test marks success on
   `WIFI_EVENT_STA_CONNECTED`, also before DHCP. Association does not establish
   usable IP connectivity. The new board-owned portal creates both AP and STA
   interfaces and accepts success only from its STA `IP_EVENT_STA_GOT_IP` with
   a nonzero address. It then checks the saved credential values in NVS.
5. The earlier custom display suppressed generic Wi-Fi notifications. The board
   now sends explicit minimal Connecting/Connected/Failed/Reconnecting/setup
   feedback, with bounded two-line captions and space reserved below the face.

These findings do not prove that any specific router is incompatible. Network
credentials, router availability, RF conditions and service access still require
device tests. ESP32 supports 2.4 GHz Wi-Fi; a 5 GHz-only network will not work.

## Provisioning and recovery

`PocketWifi` owns a fixed two-entry command queue and a priority-2 worker. HTTP
handlers enqueue bounded requests and return immediately; connection waits,
timeouts, scans and DNS checks do not run on audio/application/UI tasks.
Only application-scheduled callbacks change application state. Wi-Fi events are
registered once, and temporary AP/STA netifs plus HTTP server are stopped and
destroyed before handing control back to the upstream station manager.

The hotspot has the exact requested name `Shreeharsh Assistant` on every start,
without a generated suffix. WPA2-PSK uses a randomly generated 16-character key
shown only on the LCD. It has two client slots, an explicit
HTTP address (no captive-DNS dependency), mobile layout, a capped 16-network
scan list and correctly JSON-escaped SSIDs. HTTP requests are capped at 768
bytes to include the custom backend origin/token. Passwords and device tokens
are not printed, returned by status APIs, or included in UI
captions. Inputs validate SSID byte length and open/WPA passphrase/hex-PSK forms.

Each portal trial has a 15-second DHCP deadline. There are at most three
attempts, with 1- and 2-second backoff. Recognized authentication/handshake
rejections stop early; network-not-found and timeout errors remain recoverable
in the portal. Failed credentials are not persisted. Successful DHCP and NVS
readback produce Connected plus the local IP; normal operation resumes after
the phone acknowledges completion or automatically after 15 seconds.

Normal station mode retains the upstream saved-network scanning and bounded
10..300-second exponential scan backoff after limited direct reconnects. It
automatically recovers from a temporary outage after a successful connection.
On startup, saved Wi-Fi gets 60 seconds before falling back to setup; saved
credentials remain intact. `setup` can reopen the portal. `wifi-reset CONFIRM`
clears only SSID/password/channel keys using the existing SsidManager API.

Credentials use the existing `wifi` NVS namespace and key formats. This release
does **not** enable flash/NVS encryption or secure boot; physical flash readout
protection is not claimed. The raw device backup is private and excluded from
the release ZIP. Firmware OTA remains disabled on this single-app partition.

## Phone steps

1. Keep USB connected and release BOOT. Use the **conversation** firmware.
2. On the ESP32, read the alternating setup captions: hotspot name plus key,
   then `Open 192.168.4.1 / Choose 2.4GHz Wi-Fi`.
3. In phone Wi-Fi settings join `Shreeharsh Assistant`, enter the displayed key, and
   choose **stay connected** if warned about no internet. Temporarily disable
   mobile-data switching/VPN only if the browser cannot reach the local address.
4. Type **http://192.168.4.1** explicitly in Chrome/Safari. Select your home
   2.4 GHz SSID or type a hidden SSID, enter its password and press Connect.
5. Wait for Connected plus an IP. The hotspot then closes automatically. Rejoin
   your home network. If an activation code appears, pair at xiaozhi.me.

## Build and install

Activate ESP-IDF 6.1, then from the project directory:

```powershell
python scripts/build.py custom-esp32-nodemcu-pocket-ai --name custom-esp32-nodemcu-pocket-ai --language en-US --wake-word disabled
python scripts/pocket_snapshot.py conversation
python scripts/build.py custom-esp32-nodemcu-pocket-ai --name custom-esp32-nodemcu-pocket-ai-diagnostics --language en-US --wake-word disabled
python scripts/pocket_snapshot.py diagnostics
python -X utf8 scripts/pocket_host_tests.py
python scripts/pocket_package.py
python scripts/pocket_flash.py --port COM4
```

The guarded flasher verifies chip, 4 MB capacity, component hashes and partition
layout, then writes boot/table/app separately while preserving NVS. Do not use
`--diagnostics` for Wi-Fi or `--erase-all` for this update. A merged binary at
address zero clears its included NVS area; prefer the guarded script.

## Verification scope and device test matrix

`UI_VALIDATION.json` and device logs distinguish builds, simulation and actual
hardware. Native LVGL tests start with white simulated GRAM, verify black edge
pixels during all expression transitions, bounded dirty flushes, four UI
create/destroy cycles and stable heap. The animation model advances three
simulated hours; this is not hardware endurance. Native Wi-Fi tests check
credential boundaries and retry/deadline exhaustion at large timestamps.

Physical checks:

| Test | Required observation |
| --- | --- |
| Cold boot / reset | Black clear; no white flash, strips, old labels or clipped face |
| Idle, blink, thinking, happy, sleepy | Smooth centered horizontal features; serial `face NAME`, then `home` |
| Mic / speaking | Real capture and accepted PCM drive UI; silent PCM closes mouth |
| First boot / setup | Protected `Shreeharsh Assistant` hotspot; both alternating captions readable |
| Correct 2.4 GHz password | DHCP IP before saved success; automatic exit to assistant |
| Wrong password | Failed status, no saved bad credentials; another attempt possible |
| Unavailable / 5 GHz-only SSID | Bounded failure and usable portal; no watchdog reset |
| Router off, then on | Reconnecting state, bounded scan backoff, automatic DHCP recovery |
| Reboot after setup | Saved credentials connect without entering them again |
| Wi-Fi reset | Only Wi-Fi cleared; same UUID/activation identity and firmware |
| Extended idle and conversation | Inspect `diag` heap/minimum/largest blocks, stacks, UI and audio errors |

No tear-sync wire exists and the backlight is tied to 3V3. Frame rate adapts to
measured render duration; sustained 30 fps and artifact-free physical motion
must be observed. Prior audible tests were silent; the user subsequently confirmed
amplifier SD is connected to 3V3. Acoustic retesting is pending. Wi-Fi fixes do
not establish audible playback or a cloud reply. See `../../backend/README.md`
for the new Vercel origin/device token setup.

## Files changed for this fix

- `main/display/lcd_display.h`, `.cc`: optional startup clear color and offset window.
- `main/boards/custom-esp32-nodemcu-pocket-ai/face_animation.h`: center geometry.
- `face_renderer.cc`: opaque black surface and removal of pale plate/borders.
- `pocket_display.h`, `.cc`: single-screen cleanup and explicit Wi-Fi captions.
- `pocket_board.cc`: normal provisioning integration, serial Wi-Fi reset/diagnostics.
- `pocket_wifi.h`, `.cc`, `pocket_wifi_policy.h`: board-owned protected portal and bounded trials.
- `scripts/tests/pocket_wifi_test.cpp`, `test_pocket_board.py`: native Wi-Fi validation.
- `scripts/pocket_ui_sim/main.cpp`: black-edge and stale-white redraw checks.
- `README.md`, board README, `docs/pocket/COMPANION_UI.md`, this report,
  validation JSON and generated preview/log/release artifacts.

The larger initial Pocket port and earlier UI/audio changes remain preserved.
