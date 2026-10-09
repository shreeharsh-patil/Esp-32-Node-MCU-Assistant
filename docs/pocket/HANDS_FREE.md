# Hands-free conversation on the classic ESP32

The new conversation image starts listening automatically after saved Wi-Fi
and your custom backend are ready. No BOOT press is needed. Speak when the
face is Listening, pause after the question, then wait for the reply. Capture
resumes after playback drains.

The firmware sends upstream `listen/start` with `mode=auto` to your backend.
Gemini Live detects speech and the end of the question. Microphone audio is
Opus-encoded and streamed while Listening, including silence. There is no
local wake word/VAD model on this classic no-PSRAM ESP32. Capture pauses during
replies to avoid speaker feedback. Speaking over a reply is not supported.
One Gemini key handles recognition, reasoning and speech on the backend.
See [Vercel deployment](../../backend/README.md).

The board's existing control task opens automatic sessions only while Idle,
connected and playback-idle. It rechecks those conditions on the application
task and never toggles an existing Listening session off. Failures use bounded
2/4/8/16/32/60-second retries. Successful listening restores the short restart
delay. The upstream Wi-Fi manager owns router recovery. Custom discovery has
a short retry budget. Settings are retained by the guarded USB flasher.

Serial `handsfree off` or `stop` pauses automatic capture until `handsfree on`
or reboot. BOOT remains optional. Diagnostics stays offline and never auto-records.

Vercel sessions renew after 270 seconds, inside the configured function duration.
Context lasts only within the current Live session; renewals/disconnects reset it.
Renewal can interrupt a reply. Persistent memory, session resumption and voice
interruption during replies are not implemented.

See `HANDS_FREE_VALIDATION.json` for executed and pending checks. Historically,
the device reached the official voice server and captured 125,280 microphone
frames without RX/TX errors when manually started over USB. That is microphone
and network evidence, not proof of this new pipeline or audible replies. The
user confirmed amplifier SD is now connected to 3V3; acoustic retesting is pending.
The new custom image has not been installed because the production backend and
private provider credentials have not been configured.
