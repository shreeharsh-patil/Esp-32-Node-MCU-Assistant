# Backend validation — 2026-10-12

Story: automatic ESP32 microphone capture reaches your authenticated Vercel
backend, Gemini Live generates audio, and the ESP32 plays it before listening
for the next question.

| Boundary | Result | Evidence |
| --- | --- | --- |
| Syntax and dependencies | PASS | Node 24.18.0; `npm run build`; `npm audit` reports zero vulnerabilities. |
| Device authentication/discovery | PASS locally | HTTP tests reject missing/wrong authentication, supply the WSS/audio contract and omit the Gemini key. |
| Device audio transport | PASS locally | Real 16 kHz Opus decoded to PCM16; real 24 kHz reply encoded/decoded; final partial frame padded; TTS stop follows all packets. |
| Turn lifecycle | PASS locally | Input ignored before start/during playback; next start resumes capture; quiet sessions receive a 20-second data heartbeat; provider failure releases the session. |
| Bounds | PASS locally | Malformed/oversized packets, HTTP body limits, wrong negotiated audio, input rate and provider queue/text/audio limits checked. |
| Google provider | PASS for two prior short live turns | Real Gemini recognized `Hello` and `Open YouTube`; both complete replies were clearly audible per owner. The latest audit revision needs another live acceptance test. |
| Vercel deployment | PASS | Production health, authenticated discovery and real provider audio negotiation pass. Voice function runs in Mumbai (`bom1`); current deployment ID is in `VALIDATION.json`. |
| Physical speech/playback | Latest acceptance pending | Updated COM4 firmware hashes and NVS preservation verified; device returned to Listening with zero RX/TX errors. Owner reports cracking on the earlier version; latest audible repair needs a live listen. |

45 Node integration/unit tests passed with zero failures and zero skips.
New regressions cover a six-second PCM burst, partial optional database setup,
storage outages without voice interruption, fail-closed device authentication,
rejected/transient Gemini key failover, responses for excess tool-call IDs,
bounded startup buffering and pacing, and direct 16 kHz decoding of 24 kHz Opus.
See `../docs/pocket/RELIABILITY_AUDIT.md` for the firmware and packaging repairs.
The dashboard checks cover encrypted storage across fresh store instances,
profile revisions and validation, private owner cookies and same-origin
mutations, per-device hashed tokens and active-session revocation, transcript
filtering/deletion, history opt-out and bounded asynchronous writes. Saved
voice/instructions reach an authenticated WebSocket provider session.
Database tests use pg-mem; production uses Neon. A deployed Neon connection
is still pending, and SQL emulator results do not establish hosted persistence.

Browser verification covered sign-in, assistant voice/instructions save and
reload, read-only masked provider status, device creation, token clearing
on modal close, rename/access pause, transcript rendering/filtering and the
clear-history cancellation flow. Desktop (1280×900) and mobile (390×844)
layouts were inspected; mobile sign-out works, no document overflow occurred,
and the final browser flow had no console errors/warnings. Screenshots show
explicitly labeled local test data, with no live Gemini conversation.

Key-pool parsing, cyclic rotation, independent pools, failed attempts and the
Live adapter's fixed per-session key/stream cleanup were checked with test clients.
Setup 429 failover, active-session quota cooldown/reconnect, an exhausted pool
without retry requests, and late SDK session cleanup after cancellation were
also checked with test clients. Real quota errors still require a deployed test.
The provider boundary was checked against the installed Google SDK's types
and official documentation. The two physical live turns establish short reply
audibility; prolonged stability, hosted memory/search and quota recovery remain
unverified on hardware.

Use `npm run check-deployment` after configuring private Production variables.
Then run the device acceptance steps in `../docs/pocket/HARDWARE_TESTS.md`.

The owner requested both faster replies and repair of gaps/distortion. Provider
VAD now finalizes after 500 ms of silence, with 300 ms prefix padding retained.
Playback sends up to 360 ms ahead and delays a slow start by at most 120 ms,
without waiting for an entire answer. Safe numeric timing logs contain frame
counts, startup delay and provider-chunk gaps. Production tests with generated
host speech exercise recognition, real Gemini replies and 16 kHz Opus decoding;
their timing measurements in `VALIDATION.json` do not establish physical speaker
quality or a guaranteed response time.

Provider credentials now come only from private server environment variables.
The dashboard has no key inputs or editing controls, and API attempts to add,
remove or replace keys return HTTP 400. Tests verify changed environment keys
take effect across new instances, legacy dashboard keys are removed, and an
empty environment does not fall back to stored credentials. Browser checks
confirm zero key inputs/edit buttons at desktop and mobile widths; assistant
settings still save successfully. No browser errors/warnings were observed.
Local startup and the deployment checker also passed disposable `.env` loading
and `.env.local` override checks. These checks use test values only.

Clock update: 45 backend tests pass. Device timezone metadata is optional and validated; the current-time tool returns the same configured local time as the clock. Per-turn STT fragments are assembled before delivery. Midnight and US DST cases are covered. Physical voice/display acceptance remains pending. See `../docs/pocket/CLOCK_UI.md`.

Live clock check: generated host speech asking the current time produced the correct Asia/Kolkata reply and 34 Opus frames. The owner confirms the physical clock is visible. Production still needs DATABASE_URL for saved history; SETTINGS_ENCRYPTION_KEY has been added privately. A new capture token bucket accepts the firmware's bounded 40-frame network burst without removing the sustained rate limit; hardware acceptance remains pending.
