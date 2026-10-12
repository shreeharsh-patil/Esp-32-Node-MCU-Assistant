# Pocket reliability audit — 2026-10-12

Scope: the installed 4 MB/no-PSRAM Pocket board, its live audio lifecycle,
Vercel voice backend, optional persistence/tools, and generated release package.
Source repairs: `2866cd6`, `7dcd68a`, `30039db`, and `c90ed35`.

| Reproduced fault | Repair and evidence |
|---|---|
| A provider audio burst longer than about 5.5 seconds overflowed the old PCM queue and closed the connection mid-reply | A bounded 1 MiB queue accommodates about 22 seconds of pending PCM. A six-second burst now sends all 100 Opus frames and its final TTS stop without closing the socket |
| Initial audio could arrive before the main task entered Speaking, then be ignored or cleared by decoder reset | The custom backend reply prepares the decoder before accepting packets. A compiled production-code fixture retains the first packet through deferred and repeated state handling |
| Start and stop arriving before the main task handled a short reply could skip the microphone pause and prevent the next listening command | Speaking capture pause is applied on the application task before the queued stop. The same fixture verifies capture is paused even when start/audio/stop arrive together |
| A partial I2S timeout discarded the rest of a PCM chunk, and accepted frame counters were lost on early return | Retry only the unsent bytes, with three bounded attempts. An injected DMA timeout test verifies exact byte order, no duplicated samples, bounded stalls, and correct accepted-frame counts |
| A rejected or transiently unavailable Gemini key could fail a session even when another configured key worked | Setup failover now covers rejected keys and transient provider failures; cooldown prevents repeatedly selecting the failed slot. Invalid-model errors still stop immediately. Tests cover callback-only close, active rate limits, cancellation and late SDK cleanup |
| Tool calls after the fourth call were silently omitted, leaving their IDs unanswered | Every call receives a response. At most four execute; excess calls receive an explicit bounded error |
| Missing encryption configuration, session metrics writes or heartbeat storage failures could block or terminate voice | Construct optional storage only with both database/encryption variables. Settings use a short budget and last known/environment fallback; metrics write asynchronously. Already authenticated sessions survive temporary storage outages. New database-backed authentication still fails closed, and healthy revocation checks remain enforced |
| Closing sockets continued processing microphone packets and emitted repeated generic errors | Ignore packets once closing starts; log only safe failure categories and numeric status codes |
| Windows tests tried deleting their current directory or launched the WSL shim | Restore cwd before cleanup, read workflow UTF-8 explicitly, and select Git Bash on Windows |
| Release traversal could include local NVS/device backups and unrelated files | Both checksum and ZIP generation now use an explicit release inventory. A fixture proves private backups, environment files and local audio recordings are excluded |
| Owner reports gaps between words | Increase bounded compressed playback lead from 180 to 360 ms; prime a slow provider start for at most 120 ms. Ready bursts and completed short replies skip that wait. Deterministic timing tests cover ready/slow/complete starts and bounded catch-up after provider pauses |
| Owner reports distortion on loud words | Replace the sample waveshaper with one linear gain per 128-sample block. Quiet voice retains its boost; loud peaks stay below 65% PCM full scale, with approximately 200 ms gain release. Native and actual production-writer fixtures check waveform proportionality, matching stereo slots, peak bounds and accepted samples. Saved volume and diagnostic tone levels are preserved |
| Every 24 kHz reply frame allocated an extra PCM buffer and ran a separate rate converter on the 16 kHz Pocket output | The custom backend decodes Opus directly at the supported output rate. Real 24 kHz Opus packets decode to 960 samples at 16 kHz; compiled production-code fixtures verify decoder reuse and preserve legacy/unsupported-rate conversion paths |
| Reply finalization waited 700 ms of silence and hosting was in the US | Use 500 ms with the existing 300 ms speech prefix retained; host the voice function in Mumbai (`bom1`). Production synthetic speech measurements are recorded separately from physical acceptance |

Validation: 41 backend tests, syntax check, production dependency audit with zero
reported vulnerabilities, and 117 host tests pass. Pocket conversation,
diagnostics and representative `bread-compact-esp32-lcd` variants build with
ESP-IDF 6.1. The latest conversation image is installed on COM4, component flash
hashes verified, and all 24,576 NVS bytes match before/after flashing. It returned
to Listening with zero RX/TX errors and active capture overruns. Latest audible
speech acceptance is still pending; earlier two short audible replies do not
prove the new cracking repair. See `AUDIO_REPAIR_RESULTS.json` and
`backend/VALIDATION.json` for deployment and measurement details.

The 500 ms silence value follows the lower end of Google's recommended range
for keeping natural pauses intact: [Live VAD guidance](https://ai.google.dev/gemini-api/docs/live-api/capabilities#understanding-vad-parameters-and-their-impact-on-quality).
Direct output-rate decoding uses the [Opus decoder API](https://opus-codec.org/docs/opus_api-1.5/group__opus__decoder.html).
Region selection follows [Vercel function region configuration](https://vercel.com/docs/functions/configuring-functions/region).
Provider generation and web tools still take variable time; these repairs do
not promise instantaneous replies or establish amplifier electrical stability.

Production still needs `DATABASE_URL`, `SETTINGS_ENCRYPTION_KEY`, and
`FIRECRAWL_API_KEYS` before hosted memory and current-web search can be tested.
The direct time tool is available without Firecrawl. Extended conversation
stability and electrical amplifier measurements remain separate hardware checks.
