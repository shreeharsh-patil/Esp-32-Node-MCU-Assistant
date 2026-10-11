# Audio repair and device acceptance — 2026-10-11

Scope: current GitHub `main` at `c2f9b541d33f52f8d6b542f8424a7bd6ca972959`,
classic ESP32-WROOM-32 / 30-pin NodeMCU, custom Pocket board only. The animated
black face, display pins, backend implementation, credentials and other boards
are retained. After explicit owner approval ("flash"), the diagnostics image was
installed on COM4. NVS was privately backed up and verified byte-for-byte
unchanged afterward. Conversation firmware has not been installed for this
repair; no credential erase was performed.

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
   The owner-requested `tone long` extends playback to five seconds and uses
   temporary volume 100 with a 24,000-count source waveform, approximately
   1,920 PCM16 peak counts after gain (16 times the short tone). It retains the
   absolute cap, taper, trailing silence and restoration of volume/output state.
   The owner approved installation and heard a very light buzz from the extended
   test. Clear tone reproduction remains **NOT YET VERIFIED**.
   After the faint buzz, the owner requested a louder test and specified 80%.
   `tone loud` uses an 80% digital gain/cap for five seconds, approximately
   19,240 PCM16 peak counts with the same 24,000-count source waveform. This is
   about ten times the sample amplitude of `tone long`. Initially only the
   explicit idle test used the higher gain. The owner heard a clear tone on its
   repeat and approved applying the higher gain to normal voice playback too.
   Normal playback now has the 80% digital cap, with an 80% default volume and
   saved preferences retained. Quiet tests still explicitly use 8%. The 80%
   limit is not an acoustic loudness percentage.
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
| MAX98357A power input; GND | Initially ESP32 VIN, later owner-reported 3V3; actual voltage unmeasured; common GND |
| MAX98357A BCLK / LRCLK / DIN | 26 / 25 / 22 |
| MAX98357A GAIN / SD_MODE | GAIN initially open; SD_MODE requires verified module bias |
| Speaker | SPK+ / SPK−; neither terminal grounded |
| ST7789 SCK / MOSI / CS / DC / RST | 18 / 23 / 13 / 27 / 14 |
| ST7789 VCC / BLK / GND | 3V3 / 3V3 / common GND |
| BOOT/PTT | 0, input only |

TX configuration is already Philips I2S, 32-bit stereo, both slots carrying
identical signed mono PCM. One paired I2S0 master drives GPIO26/25 at
1.024 MHz/16 kHz. GPIO34 receives mic data; GPIO22 transmits amplifier data.
Display GPIO18/23/13/27/14 and BOOT GPIO0 do not conflict. With the original
8% output gain, quadratic volume scaling multiplied PCM by 0.0032 at volume
20 and 0.02 at volume 50. The old 6000-count tone therefore had only about
19/120 counts peak after gain; digital frame counts alone never established
audible output. The owner-approved normal 80% output gain now gives factors
of 0.032, 0.2 and 0.512 at volumes 20, 50 and 80 respectively. The short
`tone` remains deliberately quiet; the owner heard a clear repeated `tone loud`.
Neither amplifier enable nor speaker power is controlled/measured by a firmware
GPIO.

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

### Approved diagnostics installation and physical retest

On October 11, the owner approved installing the diagnostics image. The guarded
flasher identified an ESP32-D0WD-V3, revision 3.1, with 4 MB flash and verified
all written component hashes. The entire 0x9000–0xefff NVS region matched its
private pre-flash backup byte-for-byte after installation. Private backups are
ignored and are not part of the PR.

The new image started with its expected offline board identity and IDF v6.1.
I2S initialization/start passed. The driver's full-duplex RX-master-to-slave
message is expected: the paired TX channel owns their common clock. Startup
internal heap was 112,464 B, minimum 110,592 B. Network/cloud are disabled in
this firmware, so this higher heap margin is not a conversation measurement.

The repaired tone accepted 6,080 frames (4,800 tone plus 1,280 trailing silence),
zero TX errors and nonzero matching left/right words. Post-gain PCM16 peak was
120, and a logged example was L=R=0xffca0000 (signed PCM16 −54). The owner again
reported **no sound**, with SD left open. Acoustic output remains **FAIL**;
physical VIN, enable and I2S data at the amplifier remain unmeasured.
The owner confirmed the amplifier power wire is connected to ESP32 VIN and
the speaker wires connect only to SPK+ and SPK−. Those intended connections
are confirmed by description, not by voltage or continuity measurements.

The owner subsequently requested a controlled SD-to-GND trial following the
SmartElex guide and confirmed making that connection with USB disconnected.
The same repaired tone again accepted 6,080 frames, PCM16 peak 120, matching
L/R words and zero TX errors. The owner reported **no sound**. This trial does
not establish a defective amplifier: grounding a direct chip SD_MODE input is
the documented shutdown state. Return SD to open with power disconnected;
restoration has been requested but not yet confirmed. The owner also clarified
that GAIN is open. Later uncovered photos show one amplifier IC and passives,
but do not establish the SD traces. Amplifier supply/enable/data remain unmeasured.
The additional local log is `AUDIO_REPAIR_SD_GND_TONE.log`.

For a subsequent power test the owner was instructed to move only the amplifier
power wire from VIN to ESP32 3V3, with SD and GAIN open. The 3V3 configuration
again accepted 6,080 frames with zero TX errors and PCM16 peak 120. The listening
result has not been supplied, so acoustic output for this configuration is
**NOT YET VERIFIED**. Log: `AUDIO_REPAIR_3V3_POWER_TONE.log`. Neither the supply
voltage nor the 30-pin clone's USB-to-VIN circuit has been electrically measured.
The owner requested increased volume and duration after this test.

The owner then requested starting the five-second test. Updated diagnostics
were flashed with all component hashes verified, and NVS again matched the
before/after backups byte-for-byte. `tone long` ran for 5,000 ms, accepted
81,280 frames including trailing silence, reached post-gain PCM16 peak 1,924,
and logged matching L/R words `0xfc920000` (PCM16 -878), with zero TX errors.
The owner reported **"little noise kind of sound from speaker"**, clarified as
**"a buzz sound but it was very light"**. Some sound is now reported; clear
440 Hz reproduction and useful speech volume remain **NOT YET VERIFIED**.
Logs: `AUDIO_REPAIR_LONG_TONE_FLASH.log` and `AUDIO_REPAIR_LONG_TONE_PLAYBACK.log`.

The owner requested a further increase to an 80% digital cap. That diagnostics
image flashed with all component hashes verified and NVS matched byte-for-byte.
The first five-second `tone loud` accepted 81,280 frames, PCM16 peak 19,244 and
zero TX errors; the owner initially reported no sound. On the requested repeat,
the same waveform added 81,280 frames (162,560 cumulative), with matching L/R
words `0xddb20000` (PCM16 -8782) and zero TX errors. The owner reported **"Clear
tone"**. Audible tone output is now **PASS** in the current reported 3V3-powered
wiring, with SD/GAIN left open. Electrical supply/bias/continuity remain unmeasured.
The old attenuation was a confirmed contributor to insufficient test level;
this does not establish it as the sole cause of every earlier silent trial.
The owner subsequently approved applying the higher gain to normal voice
playback and testing microphone/Opus replay. No SD resistor trial was performed.

The higher normal gain image subsequently flashed with all component hashes
verified. The 24,576-byte NVS backup matched after the flash; then the requested
`volume 80` intentionally changed only the audio volume preference. Spoken
Opus replay captured 27,840 frames and played 26,880, with zero RX/TX errors and
active DMA drops. Its last output block had RMS 513 and peak 1081; these are
not recording-wide measurements. The owner reported **"No speech heard"**.
One late packet remained in the test queue and blocked the subsequent idle-only
tone command. After restart, `tone loud` again accepted 81,280 frames with
peak 19,244 and zero TX errors, but the owner reported **"Faint buzz or click"**.
The earlier clear tone is retained as a historical observation; consistent
speaker output and intelligible speech are not established. Supply and
connection checks remain necessary before attributing this variation to
firmware gain. The owner checked the unplugged speaker/power/GND joints and
reported them firm before another retest.
That retest added another 81,280 frames with peak 19,244 and zero TX errors.
The owner described a horn-like sound, then confirmed it was **clearly audible
for five seconds**. The latest tone therefore passes acoustic acceptance;
the earlier faint/silent observations remain in the record. A closer-microphone
speech replay follows to distinguish quiet input from speaker-path failure.
The first closer-microphone trial is invalid for speech acceptance because the
owner did not speak. A requested repeat, with continuous speech instructed
before capture, recorded 27,840 frames and replayed 26,880 with zero RX/TX errors
and active drops. Last-block output RMS/peak increased to 1218/2807. Its audible
result is unconfirmed; the owner requested moving to a Vercel-hosted backend
test before answering the listening question. Backend syntax and all 30 tests
passed again, and the prepared conversation image passed checksum verification.

Both five-second recordings exported 80,000 frames with valid CRC32 and no RX
errors or active DMA overruns. The initial environment recording had RMS 2459.46,
peak 32768 and 0.14% clipping (ambient conditions were not controlled). The
requested spoken phrase recording had RMS 913.66, peak 4546 and **0% clipping**.
After listening to that WAV on the computer, the owner reported **"Clear
recognizable words"**. This independently establishes intelligible microphone
speech without needing the ESP32 amplifier.

An upstream `testaudio` run with two `diag` commands during recording captured
27,840 frames and sent 26,880 replay frames through the real Opus path, with
zero added RX/TX errors and zero active DMA drops. Internal heap returned to
112,192 B, with a minimum of 97,636 B. Observed minimum remaining task stacks:
audio_input 3012 B, audio_output 972 B, opus_codec 4708 B, pocket_control 1768 B.
These short offline runs establish bounded-path stability, not prolonged
Wi-Fi/Gemini conversation stability or acoustic replay.

Local logs: `AUDIO_REPAIR_DIAGNOSTICS_BOOT.log`,
`AUDIO_REPAIR_DIAGNOSTICS_TONE.log`, `AUDIO_REPAIR_MIC_BASELINE.log`,
`AUDIO_REPAIR_MIC_SPEECH.log` and `AUDIO_REPAIR_OPUS_STRESS.log`. Listening
artifacts are `pocket-release/mic-baseline.wav` and `mic-speech.wav`. Runtime
facts and CRC32 values are recorded in `AUDIO_REPAIR_RESULTS.json`.

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

Final validation: **PASS**, 113 host tests (including the extended tone);
**PASS**, 30 backend tests from the prior audio repair;
**PASS**, both Pocket variants and the representative `bread-compact-esp32-lcd`
build with ESP-IDF v6.1. Image inspection verified classic ESP32 chip headers,
boot/app checksums, partition bounds and the 4 MB/no-PSRAM configuration. The
diagnostics merged image is 2,229,840 bytes; conversation is 2,245,120 bytes.
Their app images fit the 0x3f0000 factory partition. Local checksum verification
is separate from installation and uses no serial access. Short offline PCM
export, clear microphone speech and heap/stack checks passed after installation;
amplifier electrical output and complete conversation remain unverified.

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

For the owner-requested louder five-second test, after installing the updated
diagnostics firmware with explicit approval:

```powershell
python scripts/pocket_serial_capture.py --port COM4 --seconds 18 --command tone-long --command diag --output docs/pocket/audio-long-tone.log
```

For the subsequently requested 80% digital-limit test, use the updated image
and `tone loud` (the prior short and long commands retain their original levels):

```powershell
python scripts/pocket_serial_capture.py --port COM4 --seconds 18 --command tone-loud --command diag --output docs/pocket/audio-80-tone.log
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

Statuses refer to physical evidence after approved diagnostics installation.
Historical failures are retained in the preceding evidence and JSON. PASS for
short offline tests does not establish prolonged conversation stability.

| Test | Result | Evidence |
|---|---|---|
| GPIO configuration | PASS | Source config + host regression: audio 26/25/34/22; display 18/23/13/27/14; BOOT 0 |
| I2S initialization | PASS | Repaired diagnostics image booted; shared 16 kHz/32-bit clock start ESP_OK |
| Microphone recording | PASS | Two five-second 80,000-frame exports, valid CRC32, zero RX errors |
| Speech intelligibility | PASS | Owner heard clear recognizable words in independently played spoken WAV |
| RX DMA stability | PASS | Zero drops in both five-second captures and Opus capture with diagnostic polling; prolonged conversation not yet verified |
| Speaker digital output | PASS | Repeated 80% test: 81,280 added frames, PCM16 peak 19,244, matching L/R words, zero TX errors; prior Opus replay 26,880 frames |
| Amplifier electrical output | NOT YET VERIFIED | Actual VIN, SD/MODE, physical I2S and differential 440 Hz measurements missing |
| Audible speaker tone | PASS, latest retest | Owner confirmed horn-like tone clearly audible for five seconds after checking joints; earlier faint/silent observations retained |
| Audible speech replay | NOT YET VERIFIED on latest repeat | Initial volume-80 replay not heard; stronger-input repeat sent 26,880 frames without errors, listening result unconfirmed |
| Wi-Fi connectivity | NOT YET VERIFIED | Current diagnostics image intentionally disables network; NVS preserved |
| Gemini voice conversation | NOT YET VERIFIED | Local backend tests pass; deployed hello, STT, audible reply and next turn required |
