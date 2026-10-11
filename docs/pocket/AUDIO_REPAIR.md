# Audio repair and device acceptance — 2026-10-11

Scope: current GitHub `main` at `c2f9b541d33f52f8d6b542f8424a7bd6ca972959`,
classic ESP32-WROOM-32 / 30-pin NodeMCU, custom Pocket board only. The animated
black face, display pins, backend implementation, credentials and other boards
are retained. No device flash or credential erase has been performed for this
repair. Device installation requires the owner's approval.

## Findings and changes

1. **Confirmed capture scheduling defect:** `AudioService::PrintDiagnostics()`
   held `audio_queue_mutex_` throughout UART logging of queues and task stacks.
   Capture acquires the same mutex to enqueue PCM. At 115200 baud, those lines
   can exceed the original RX queue's 40 ms margin (5 queued 128-frame blocks
   at 16 kHz). The October 9 speech log shows drops during diagnostic polling.
   The repaired method snapshots queue counts and releases the mutex before
   printing. A native regression compiles the actual method and checks that
   another thread can acquire the mutex at every logger call. This is a real
   starvation mechanism, but the logs do not establish the cause of every
   dropped block, including the two in the clean retest.
2. **Stale capture across the idle grace:** RX clocks keep running, and the
   service can leave input enabled for 15 seconds without reading. A subsequent
   recording may not call `EnableInput(true)` again. The old flush happened only
   on enable, before its logging and upstream warmup. The repair marks unread
   idle DMA data stale in the overflow callback, flushes immediately before the
   first read, and resets the DC filter. Intentional idle/warmup discards before
   that read are excluded from active-capture overruns. Real drops after capture
   starts remain counted. The callback registration result is now checked;
   failure prevents a misleading ready/zero-overrun result.
3. **Bounded DMA margin:** 8 descriptors instead of 6, with 128 frames each.
   The queued RX margin is 56 ms and total paired DMA payload is 16,384 B, an
   increase of 4,096 B. GPIOs, stereo slot width and clock domain are unchanged.
   Actual heap minima, scheduling and zero-drop acceptance require a device run.
4. **Speaker measurements previously described pre-gain PCM:** the UI's
   existing amplitude measurement remains unchanged. Diagnostics now also
   report actual post-conversion PCM16 RMS/peak accepted by I2S. They are
   digital data measurements, never acoustic or electrical measurements.
5. **Controlled tone:** explicit 440 Hz, 300 ms, tapered waveform, temporary
   diagnostic gain equivalent to volume 50, about 120 PCM16 peak counts after
   attenuation. Stored volume (including mute) is restored without NVS writes.
   The absolute 8% digital cap remains. A DMA ring of silence follows the tone;
   the idle service power timer cannot mute it partway through. No boot tone.
6. **Independent microphone recording:** `mic` captures five seconds of 16 kHz
   mono PCM16 after left-slot conversion, DC removal and configured gain. RMS,
   peak, clipping percentage, captured frame count and error/overrun deltas
   print about every 250 ms, with full-recording RMS/peak at the end. A 512 B
   scratch buffer avoids allocating the 160 KB recording. The host capture tool
   follows UART markers, switches temporarily to 921600 baud, and ACKs before
   capture begins. Indexed hexadecimal PCM streams at about 68 KB/s, below that
   UART's 92 KB/s capacity; 115200 baud cannot carry real-time PCM16 at 16 kHz.
   A CRC32 detects corrupted as well as missing/reordered serial samples. Both
   device and host return to 115200 afterward. Without an ACK, the device
   restores 115200 and performs a five-second levels-only test, without export.
   UART contention can still cause drops: require zero overrun deltas and valid
   CRC32 in a physical run, not just sufficient theoretical bandwidth. It never
   needs the amplifier. A narrow input mutex serializes the direct
   diagnostic reader with idle service power-stop requests. `testaudio` retains
   the upstream Opus replay path and now rejects a capture PASS if DMA drops
   increased.

INMP441 conversion is already correct for this configuration: signed 24-bit
MSB-aligned data in a 32-bit left slot, reduced to PCM16 using an arithmetic
16-bit shift, then DC removal and bounded gain. Right-slot data is discarded.
The gain defaults to 4 and can saturate; clipping is now reported. There is no
on-device speech recognizer or noise-suppression model on this classic board.
No unverified filter or gain increase was added.

GPIO verification (source assignment PASS; soldered wire continuity not yet
verified):

| Peripheral signal | GPIO / supply |
|---|---|
| INMP441 VDD; GND; L/R | 3V3; common GND; GND |
| INMP441 SCK / WS / SD | 26 / 25 / 34 |
| MAX98357A VIN; GND | verified 5 V source; common GND |
| MAX98357A BCLK / LRCLK / DIN | 26 / 25 / 22 |
| MAX98357A GAIN / SD_MODE | GAIN initially open; SD_MODE requires verified module bias |
| Speaker | SPK+ / SPK−; neither terminal grounded |
| ST7789 SCK / MOSI / CS / DC / RST | 18 / 23 / 13 / 27 / 14 |
| ST7789 VCC / BLK / GND | 3V3 / 3V3 / common GND |
| BOOT/PTT | 0, input only |

TX configuration is already Philips I2S, 32-bit stereo, both slots carrying
identical signed mono PCM. One paired I2S0 master drives GPIO26/25 at
1.024 MHz/16 kHz. GPIO34 receives mic data; GPIO22 transmits amplifier data.
Display GPIO18/23/13/27/14 and BOOT GPIO0 do not conflict. The normal output
gain is quadratic: volume 20 multiplies PCM by 0.0032, volume 50 by 0.02.
The old 6000-count tone therefore had only about 19/120 counts peak after gain;
digital frame counts alone never established audible output. The new tone is
still deliberately quiet. Neither amplifier enable nor speaker power is
controlled/measured by a firmware GPIO.

## Device evidence and remaining electrical issue

The original [October 9 results](AUDIO_RETEST_RESULTS.json) remain unchanged:
successful I2S transfers and real Opus loopback; no sound; nonzero varying mic
samples; 12 active DMA overruns, including 2 without diagnostic polling.
Speech intelligibility and electrical signals were not verified.

On October 11 the owner connected COM4 with amplifier SD **open**, and reported
no multimeter available. A read-only 115200-baud `diag` showed installed IDF
v6.1 firmware, setup mode, volume 20, 56,524 B free internal heap and DHCP pending.
A single installed-firmware `tone` accepted 1,920 frames with zero TX errors.
The owner reported **no sound**. This was the older 120 ms tone, not the repaired
image. Logs are saved locally as `AUDIO_REPAIR_INSTALLED_DIAG.log` and
`AUDIO_REPAIR_INSTALLED_TONE.log`. Zero idle RX overruns is not a capture test.

SD open does not establish enable. The SmartElex guide's SD-ground example
conflicts with the MAX98357A chip's low=shutdown behavior, and the guide does
not establish whether its labeled header has an inverter. Check the module
circuit and actual bias, rather than assuming either source describes this
physical breakout. See [electrical measurement procedure](WIRING.md#smartelex-sdmode-ambiguity-and-silence-measurements).
The owner identified the exact [Robocraze SmartElex listing](https://robocraze.com/products/smartelex-max98357a-i2s-audio-breakout-amplifier-for-raspberry-pi-and-microcontrollers),
SKU TIFCC0264. It specifies a 5 V supply, but supplies no SD/MODE schematic or
enable-bias instructions; product identification does not resolve the polarity
conflict in the branded guide.
Without a meter/scope these tests stay pending. Do not ground either speaker
terminal or connect the amplifier's 5 V supply to the microphone.

## Build and computer validation

Activate ESP-IDF 6.1 as described in the board README, then use the canonical
entry point for each variant:

```powershell
python scripts/build.py custom-esp32-nodemcu-pocket-ai --name custom-esp32-nodemcu-pocket-ai --language en-US --wake-word disabled --zip
python scripts/build.py custom-esp32-nodemcu-pocket-ai --name custom-esp32-nodemcu-pocket-ai-diagnostics --language en-US --wake-word disabled --zip
$env:PYTHONUTF8='1'
python scripts/pocket_host_tests.py
```

The host suite includes signed PCM/slot alignment, 440 Hz output surviving the
cap, GPIO non-conflict, real diagnostic locking, loss/truncation/reordering
checks for PCM export and WAV format. Existing Wi-Fi bounds, hands-free retry/
resume and WebSocket frame tests remain included. Backend `npm test` checks
Opus conversion, local transport, authentication and provider lifecycle with
mocked provider access; it is not a deployed Gemini voice conversation.
Machine-readable results and firmware hashes are in `AUDIO_REPAIR_RESULTS.json`.

Final validation: **PASS**, 112 host tests; **PASS**, 30 backend tests;
**PASS**, both Pocket variants and the representative `bread-compact-esp32-lcd`
build with ESP-IDF v6.1. Image inspection verified classic ESP32 chip headers,
boot/app checksums, partition bounds and the 4 MB/no-PSRAM configuration. The
diagnostics merged image is 2,229,424 bytes; conversation is 2,244,704 bytes.
Their app images fit the 0x3f0000 factory partition. Local checksum verification
is separate from installation and uses no serial access. Physical heap minima,
PCM export reliability and repaired-image audio remain **NOT YET VERIFIED**.

## Local retest after approved installation

Use the **diagnostics** image first: it initializes the same face and audio
with Wi-Fi/Gemini disabled. The guarded flasher writes boot/table/app separately
and preserves the NVS sectors. Do not flash a merged binary across NVS. Verify
image checksums with `pocket_flash.py --diagnostics --verify-only` before the
owner-approved installation; do not pass `--erase-all`.

After activating the SDK in the repository root, verification uses no serial
access or writes:

```powershell
python scripts/pocket_flash.py --diagnostics --verify-only
python scripts/pocket_flash.py --verify-only
```

Only after explicit approval to install the diagnostics image:

```powershell
python scripts/pocket_flash.py --port COM4 --diagnostics
python scripts/pocket_serial_capture.py --port COM4 --seconds 20 --command diag --command tone --output docs/pocket/audio-offline.log
```

After local hardware acceptance and approval to install conversation firmware:

```powershell
python scripts/pocket_flash.py --port COM4
```

1. Send `diag`; check the repaired 16,384 B DMA line and signal expectations.
2. Send `tone`; listen and separately measure VIN, SD/MODE, BCLK, LRCLK, DIN,
   and differential speaker audio as described in WIRING. `TX PASS` still does
   not establish enabled amplifier or acoustic output.
3. Record a short phrase such as "hello pocket". This capture command waits
   five seconds before `mic`, then exports WAV. Start speaking about six seconds
   after launching it and continue for five seconds. The saved log contains
   `MIC speak now`; the capture tool does not display live log lines. It
   automatically follows the temporary baud change;
   repeat for quiet and spoken baselines:

   ```powershell
   python scripts/pocket_serial_capture.py --port COM4 --seconds 20 --command mic --command diag --output docs/pocket/mic-speech.log --wav pocket-release/mic-speech.wav
   ```

   Play the WAV on your computer, independently of the ESP32 speaker. Record
   whether the words are recognizable. Check RMS/peak changes and clipped
   samples, not just nonzero data. If export is incomplete the tool refuses a
   WAV. Corrupted recordings fail CRC32 validation. Completed recordings with
   DMA drops can still be inspected; the tool
   reports their overrun/error deltas. `mic` firmware PASS requires successful
   capture with no transfer errors or added active overruns; the host additionally
   verifies the export's CRC32. Neither establishes speech intelligibility.
   A terminal-only `mic` falls back to levels without export after the host ACK
   timeout. To extract a saved complete log:

   ```powershell
   python scripts/pocket_serial_capture.py --extract-only --output docs/pocket/mic-speech.log --wav pocket-release/mic-speech.wav
   ```

4. Send `testaudio`; speak for 1.7 seconds and listen to the upstream Opus
   replay. Compare against the independent WAV. Use the repaired `diag` during
   recording to check the mutex fix. Repeat quiet and diagnostic-polling runs;
   require zero new active DMA drops and RX/TX errors, stable heap and stacks.
   Any remaining drops require timing/heap evidence; do not label them fixed
   merely because these builds and host regressions passed.
5. After local capture and speaker acceptance, install the conversation image
   with approval. Provision a 2.4 GHz network and the production HTTPS backend
   plus device token. Keep Gemini keys only on the backend. Verify DHCP, backend
   hello, real input STT and incoming Opus/PCM. Speak, wait for audible reply,
   confirm return to Listening, then speak again. Test reconnection separately.

The source path remains mic → lite engine → 16 kHz/60 ms Opus → authenticated
WSS → backend/Gemini → 24 kHz Opus → decoder/resampler → fixed 16 kHz I2S.
The application defers automatic capture until playback queues drain, and the
board reopens idle sessions with bounded retry. No new protocol defect was
confirmed by this audit or local backend tests; the current device remains in
setup without DHCP. A working router/backend cannot cure an offline tone's
silent amplifier.

## Acceptance report

Statuses refer to physical evidence; repaired-image physical acceptance remains
pending until installed and tested. Historical failures are retained.

| Test | Result | Evidence |
|---|---|---|
| GPIO configuration | PASS | Source config + host regression: audio 26/25/34/22; display 18/23/13/27/14; BOOT 0 |
| I2S initialization | PASS | October 9 boot/transfer logs; October 11 installed-image tone accepted; repair image not yet installed |
| Microphone recording | NOT YET VERIFIED | Five-second PCM export needs repaired image; October 9 historical capture had 111,360 varying samples |
| Speech intelligibility | NOT YET VERIFIED | Independent spoken WAV and human listening required |
| RX DMA stability | FAIL | 12 October 9 drops including 2 clean-run drops; repaired-image stability not yet verified |
| Speaker digital output | PASS | October 11 installed-image 1,920 frames, zero TX errors; repaired image not yet installed |
| Amplifier electrical output | NOT YET VERIFIED | Actual VIN, SD/MODE, physical I2S and differential 440 Hz measurements missing |
| Audible speaker output | FAIL | Owner heard no October 11 tone with SD open; repaired image not yet installed |
| Wi-Fi connectivity | NOT YET VERIFIED | October 11 installed device in setup, DHCP pending; no new credentials written |
| Gemini voice conversation | NOT YET VERIFIED | Local backend tests pass; deployed hello, STT, audible reply and next turn required |
