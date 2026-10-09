# Backend validation — 2026-10-09

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
| Google provider | PENDING | No real key configured or Google Live session opened in this work. Tests inject a provider double. |
| Vercel deployment | PENDING | User will host on Vercel; no project deployed or production URL supplied. Function/WebSocket packaging has not been validated on the platform. |
| Physical speech/playback | PENDING | New custom-backend firmware has not yet been flashed; amplifier SD is now enabled, but acoustic retest is outstanding. |

30 Node integration/unit tests passed with zero failures and zero skips.
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
and current official documentation. This does not establish account access,
provider quota, network latency, audible output or long-duration operation.

Use `npm run check-deployment` after configuring private Production variables.
Then run the device acceptance steps in `../docs/pocket/HARDWARE_TESTS.md`.

Provider credentials now come only from private server environment variables.
The dashboard has no key inputs or editing controls, and API attempts to add,
remove or replace keys return HTTP 400. Tests verify changed environment keys
take effect across new instances, legacy dashboard keys are removed, and an
empty environment does not fall back to stored credentials. Browser checks
confirm zero key inputs/edit buttons at desktop and mobile widths; assistant
settings still save successfully. No browser errors/warnings were observed.
Local startup and the deployment checker also passed disposable `.env` loading
and `.env.local` override checks. These checks use test values only.
