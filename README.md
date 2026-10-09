# Shreeharsh Assistant

A hands-free voice assistant for a classic ESP32 NodeMCU with an ST7789 display,
INMP441 microphone and MAX98357A amplifier. The firmware is based on the
[XiaoZhi ESP32 project](https://github.com/78/xiaozhi-esp32) and uses ESP-IDF 6.1.

> **Personal project:** outside contributions and pull requests are not accepted.

The ESP32 connects to your own Vercel-hosted backend, which uses Gemini Live for
speech and replies. Gemini API keys stay in Vercel's private environment; the
device stores only its backend URL and device token. No XiaoZhi account is needed.

## What it does

- Listens automatically when Wi-Fi and the backend are ready; resumes after a reply.
- Shows a 280 × 240 landscape animated face with black background and pink cheeks.
- Uses microphone level for attentive eyes and playback audio for mouth animation.
- Provides a phone setup hotspot at `http://192.168.4.1`.
- Includes a local diagnostics build for display and audio checks.

## Hardware and wiring

This pin map is for the board configuration in this repository. ESP32 pin numbers
below mean GPIO numbers.

| Device pin | Connect to |
|---|---|
| INMP441 VDD | ESP32 3V3 |
| INMP441 GND and L/R | ESP32 GND |
| INMP441 SCK / WS / SD | GPIO26 / GPIO25 / GPIO34 |
| MAX98357A VIN / GND | USB-derived 5V / ESP32 GND |
| MAX98357A BCLK / LRC / DIN | GPIO26 / GPIO25 / GPIO22 |
| MAX98357A SD / GAIN | ESP32 3V3 / leave unconnected |
| Speaker positive / negative | MAX98357A SPK+ / SPK− |

The two modules share GPIO26 and GPIO25. The microphone's **SD** is its data output
to GPIO34; the amplifier's **SD** is its enable/mode pin and connects to 3V3.
Never connect either speaker terminal to ESP32 GND; the amplifier output is
differential. Verify that your board's VIN pin supplies USB 5V before using it
for amplifier VIN. See [full wiring and electrical notes](docs/pocket/WIRING.md).

## Build the firmware

Use ESP-IDF 6.1 and the included canonical builder in PowerShell:

```powershell
$env:IDF_TOOLS_PATH = 'D:\Espressif\tools'
$env:PYTHONUTF8 = '1'
. 'D:\Espressif\esp-idf-v6.1\export.ps1'
python scripts/build.py custom-esp32-nodemcu-pocket-ai --name custom-esp32-nodemcu-pocket-ai
```

For offline display/audio diagnostics, build the diagnostics variant:

```powershell
python scripts/build.py custom-esp32-nodemcu-pocket-ai --name custom-esp32-nodemcu-pocket-ai-diagnostics
```

The conversation image supports Wi-Fi and the custom backend. The diagnostics
image keeps Wi-Fi and cloud access disabled. Release files and checksum reports
can be generated with `python scripts/pocket_snapshot.py conversation` or
`python scripts/pocket_snapshot.py diagnostics`, then `python scripts/pocket_package.py`.

For routine USB updates, use the matching board image and the guarded flasher:

```powershell
python scripts/pocket_flash.py --port COM4
```

Replace `COM4` with the port shown for your board. The normal update preserves
NVS settings; `--erase-all` erases them and is for an intentional fresh migration.
The helper checks the image, chip and flash size before writing. See the
[board build guide](main/boards/custom-esp32-nodemcu-pocket-ai/README.md).

## Set up Wi-Fi and Vercel

Before voice chat, deploy the backend using [backend/README.md](backend/README.md).
Add your Gemini key, device token and other private values to Vercel's Production
Environment Variables, then redeploy. **Never put a real Gemini key in this
repository, on the website, or in ESP32 setup.** The dashboard does not offer
API key entry; it shows read-only provider status.

On the ESP32:

1. Join the **Shreeharsh Assistant** hotspot from your phone. If asked, stay
   connected without internet access.
2. Visit `http://192.168.4.1` while joined to the hotspot.
3. Enter your 2.4 GHz home Wi-Fi details, the backend's production HTTPS URL,
   and the device token configured in Vercel. Do not enter a Gemini API key.
4. Wait for **Connected** and an IP address, then reconnect your phone to home Wi-Fi.
5. Speak after the assistant starts listening; pause for the reply. It listens
   again after playback. The current firmware pauses capture during replies.

See [dashboard setup](backend/DASHBOARD.md) to configure the optional owner
dashboard and Neon storage. Gemini Live is implemented; other AI providers need
an adapter.

## Current test status

The conversation firmware builds and has been flashed to the ESP32. The user
confirmed the new face is visible. The sound path is still under investigation:
I2S tone and replay frames transferred, but no tone or voice replay was audible
at output volumes 20 or 50. Some microphone DMA overruns were also observed.
Treat digital transfer success as separate from working acoustic input/output.
See [sound test results](docs/pocket/AUDIO_RETEST_RESULTS.json) and the
[hardware test guide](docs/pocket/HARDWARE_TESTS.md).

The Vercel backend is not yet deployed/configured, so a real cloud conversation
has not been verified. Build and display checks do not prove audible playback or
end-to-end speech operation.

## Project boundaries

This is a personal project. The maintainer is not accepting outside contributions,
pull requests or feature requests. Keeping this policy in the README communicates
the request; repository access and issue settings are managed separately on GitHub.

This project retains the upstream XiaoZhi source and its MIT license. See
[`LICENSE`](LICENSE) for the license and attribution.
