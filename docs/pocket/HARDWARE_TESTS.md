# Hardware acceptance: A-N

Diagnostic firmware was flashed on COM4 with user approval. Startup and serial
commands passed; the user confirmed correct display colors and upright text.
I2S capture, tone transfers and Opus loopback produced real input/output frames.
The user heard no sound at volume 20 or during three brief volume-50 tones,
so audible playback has not passed. Volume was restored to 20 after the retest.
See DEVICE_DIAGNOSTICS_RESULTS.json and the DEVICE_DIAGNOSTICS_*.log files.
Those initial visual observations tested the earlier portrait UI. The redesigned
280x240 landscape UI is tracked separately in UI_VALIDATION.json and UI_DEVICE_*.log;
earlier PASS results must not be read as landscape or new-animation acceptance.

A read-only ROM probe on COM4 did pass: ESP32-D0WD-V3 revision v3.1, dual core,
4 MB detected flash. See `DEVICE_PROBE.log` and `DEVICE_PROBE.json` for actual
results. This identifies the hardware without proving firmware startup or audio.

Start with `pocket-xiaozhi-esp32-diagnostics.bin` at 0x0. This independent build
has no Wi-Fi or cloud traffic and uses the same GPIOs, I2S, display driver, and
real upstream Opus pipeline as the conversation firmware. Serial: 115200 8N1.

| Test | Procedure | Required evidence / PASS criterion | Status |
|---|---|---|---|
| A: startup | Release BOOT, reset, send `diag` | Classic ESP32, detected flash >=4 MB, no reboot loop | PASS: 60 s smoke test |
| B: colors | Send `colors` | Visible left-to-right red/green/blue; no swapped colors | Earlier portrait PASS; redesigned UI tracked separately |
| C: text/rotation | Read borders/text, then `home` | All 280x240 landscape pixels positioned correctly; readable upright text | Earlier portrait PASS; landscape check tracked separately |
| D: I2S init | Read boot logs and `diag` | Driver allocation succeeds; no RX/TX allocation errors | PASS: actual boot log |
| E: recording | Send `mic`, speak during 5 seconds, then `diag`; use capture utility for WAV | Actual captured frames, changing RMS/peak, clipping percentage; no RX errors/drops | Historical digital PASS; repaired capture and speech quality not yet verified |
| F: speaker | Verify VIN and SD bias, then `tone` | Quiet 440 Hz tapered tone; differential output within safe measured limit | Audible FAIL at volumes 20 and 50; wiring check and power measurement pending |
| G: shared clocks | Probe GPIO26 and 25 in listening/playback | Exactly one clock domain; WS16 kHz, BCLK1.024 MHz; no conflicting drivers | PENDING |
| H: Opus | Send `testaudio`, speak, wait 5 seconds | Upstream encode/decode produces output frames and recognizable replay | Digital PASS; initial audible FAIL |
| I: Wi-Fi | Flash custom image, provision Wi-Fi/backend from phone | Settings persist and connect to the 2.4 GHz router | Prior DHCP confirmed; custom setup PENDING |
| J: service | Run backend deployment check, observe device WSS | Custom discovery and real Gemini hello accepted | Local transport PASS; deployment PENDING |
| K: credentials | Set private server key/token; test an incorrect device token | Correct token accepted, incorrect rejected; no provider key on device | Local auth PASS; deployment PENDING |
| L: conversation | Speak without BOOT, pause for reply, speak again after playback | Actual STT/subtitles and intelligible service-generated audio; automatic return to Listening | PENDING hands-free validation |
| M: stability | 100 turns / >=1 hour, collect periodic `diag` | Heap stabilizes; largest block/DMA/stacks adequate; no watchdog/codec errors | PENDING |
| N: recovery | Disable router/network during idle/listen/playback and restore | Clear error, bounded retries, responsive PTT, subsequent real conversation | PENDING |

In the repaired firmware, `mic` records five seconds of mono PCM and the capture
utility exports it for computer playback, without using the amplifier. A host
ACK enables a temporary 921600-baud PCM stream using 512 B scratch space; no ACK
falls back to levels only, at 115200 baud. RMS, peak, clipping percentage, frame
count and error/overrun deltas print about every 250 ms. Both ends restore 115200
baud. CRC32 and indexed rows validate exported data. `testaudio` retains the bounded
upstream Opus recording/replay. See [WAV capture instructions](AUDIO_REPAIR.md).
Earlier firmware used both command names for Opus replay and cannot export PCM.
Driver transfer PASS messages only establish software transfer success on
the running board; they do not prove that the microphone or speaker is physically
working. A microphone can clock zeros from a disconnected data line. Compare
RMS/peak while speaking and listen to the replay. TEST H's output-frame counter
is useful digital evidence, but recognizable acoustic replay is still required.

`tone` is explicit only. The repaired version uses a 300 ms ramped waveform at
a fixed temporary diagnostic gain (equivalent to volume 50), restoring the
user's volume without writing NVS. It prints PCM peak after gain and queues a
DMA ring of silence to drain the tone. The absolute output cap is retained.
Earlier firmware used 120 ms at the user's saved volume, including zero/mute.
There is no automatic test tone at boot. The owner subsequently requested a
manual louder test and specified 80%. `tone loud` runs for five seconds with
an 80% digital gain/cap and restores volume/output state. The owner heard a clear
tone on the repeat and approved applying that gain to normal voice playback.
Quiet test commands retain their 8% cap. Electrical measurements and clear
spoken-audio replay still require acceptance.

`diag` reports actual current/minimum internal heap, largest allocation, DMA heap,
queue occupancy/capacities, microphone RMS/peak, RX/TX frame counts/errors,
active-capture DMA overruns, post-gain TX RMS/peak, cumulative mic clipped samples,
expected GPIO signals, state, flash/chip/reset reason, and available task
stack high-water marks. Idle RX DMA drops are intentionally not counted as capture
errors because the shared clocks remain running. Warmup/idle samples discarded
before the first read of a new session are excluded; drops after capture begins
are counted. The repaired RX queue has 56 ms scheduling margin (8 descriptors)
and paired RX/TX DMA payload storage is 16,384 bytes, up by 4,096 bytes. Zero
overruns still require measurement on the repaired firmware. `testaudio` capture
PASS now also requires no added active DMA overruns.

Capture logs using ESP-IDF monitor or any serial terminal. Send commands with a
newline. `colors`/`home` are independent of backend access; `tone`/`testaudio`
require the idle setup/diagnostic state. In conversation firmware use `setup` to
return to that state before local audio tests.

Useful troubleshooting:

- Blank display: verify BLK/VCC at 3V3, common ground, CS/DC/RES wires, then SPI
  initialization logs. Check `DISPLAY_OFFSET_Y`, inversion, RGB/BGR, mirroring,
  and swap flags against the physical module; rebuild after adjusting `config.h`.
- Mic stuck at zero: check GPIO34 SD, left-channel L/R ground, BCLK/WS timing,
  3V3 supply, and actual 32-bit Philips framing.
- Silent playback: check measured 5V supply and exact SD/MODE bias, differential
  speaker connections, mute/volume setting, TX frame counter, and clock continuity.
- Audio underrun/stutter: correlate TX errors, RX overruns, queue occupancy,
  heap/largest blocks, network conditions, and task stack minima. The current
  driver can report timeout/overflow but cannot electrically diagnose the amplifier.
- Backend errors: check the production HTTPS origin, matching device token,
  private Vercel variables, Live model access/quota and TLS. Run
  `npm run check-deployment` in `backend` before physical voice testing.
- Memory failures: capture `diag` before and after the failed turn. No-PSRAM
  operation needs runtime acceptance; successful linking does not measure peak RAM.

There is no OTA physical test because firmware writes are disabled and the flash
table contains only a factory slot. USB re-flashing is the supported update path.

## Current installed face firmware sound retest — 2026-10-09

The conversation image with the new face is installed on COM4. Six short tones
(three at volume 20, three at 50) completed digital I2S transfers, but the user
heard no sound. Two instructed speech recordings also produced real Opus output
frames, and the user heard neither replay. Volume was restored to 20.

Four local recordings captured 111,360 microphone frames. The signal was
nonzero and varied during the speech test. RX/TX transfer errors stayed zero,
but 12 DMA overruns occurred; two occurred in a recording without diagnostic
polling. These observations are partial digital evidence, not microphone
speech-quality or stable-capture acceptance. Speaker/amplifier electrical
signals, power/enable and acoustic output still require physical diagnosis.

See `AUDIO_RETEST_RESULTS.json` and its listed logs for the current evidence.
The earlier offline acceptance table above remains historical.
