# Shreeharsh Assistant on Vercel

This is your editable backend for the classic ESP32 Pocket Assistant. Your
**Gemini API key or key pool** supplies recognition, reasoning and spoken replies
through Gemini Live. No XiaoZhi account or cloud activation is needed by the
new custom-backend firmware. The existing firmware/audio drivers are still
based on the MIT-licensed XiaoZhi source.

The flow is:

`ESP32 microphone → Opus/WSS → your Vercel backend → Gemini Live → PCM/Opus → ESP32 speaker`

The included XiaoZhi-style dashboard manages your assistant, devices
and saved conversations, with read-only provider status. Open your production domain to sign in. Settings and
transcripts persist encrypted in Neon PostgreSQL. Gemini keys come only from
private Vercel environment variables and are never stored in dashboard records. See [dashboard setup and controls](DASHBOARD.md).

The backend owns the provider key, voice, model and assistant instructions.
The ESP32 stores only your backend's HTTPS address and a separate device token.
The provider adapter is in `src/gemini.js`; `src/bridge.js` owns the device's
audio protocol. Gemini is the implemented provider; other providers require
an adapter, rather than just a different environment variable.

## Deploy

1. Put this project in your own Git repository and import it into Vercel.
   Set **Root Directory to `backend`**, framework **Express**, Node.js **24.x**.
   Use the included `vercel.json` and default output settings. Vercel installs
   the pinned dependencies from `package-lock.json`.
   If deploying the separate backend ZIP, the extracted folder itself is the
   project root; do not set a second `backend` subdirectory.
   You can also deploy directly from PowerShell without GitHub: open this
   `backend` folder and run `npx --yes vercel@latest --prod`. Follow Vercel's
   login/new-project prompts. The first deployment will report unconfigured
   until you add the variables in step 2 and redeploy.
2. In Vercel **Project → Settings → Environment Variables**, add the variables
   below to **Production**. Enter secret values privately in Vercel.
   A local `.env` file alone does not configure the hosted deployment.
   Generate the device token locally with:

   ```powershell
   node -e "process.stdout.write(require('node:crypto').randomBytes(32).toString('hex'))"
   ```

   For the dashboard, connect a **Neon PostgreSQL database** from this project's
   Storage/Marketplace and ensure its Production connection string is available
   as `DATABASE_URL`. Generate two more independent random values with the
   command above: one for `ADMIN_TOKEN`, one for `SETTINGS_ENCRYPTION_KEY`.
   Keep the encryption key across redeployments; stored data needs that same key.
   The application creates its own tables on first use. See [DASHBOARD.md](DASHBOARD.md)
   for the complete sequence.

3. Deploy, then copy the stable **production domain** Vercel assigns, such as
   `https://shreeharsh-assistant.vercel.app`. Set `PUBLIC_BASE_URL` to that exact
   HTTPS origin and **redeploy**. Use the production domain, not an expiring
   preview link. It may differ from the example if the name is taken.
4. Ensure **Fluid compute is enabled**. Vercel currently supports Node.js
   WebSockets in public beta on Fluid compute. This Express app exports its
   HTTP server, as required for WebSocket upgrades. The included configuration
   requests a 300-second function limit and closes sessions after 270 seconds.
   See [Vercel WebSockets](https://vercel.com/docs/functions/websockets) and
   [Express deployment](https://vercel.com/docs/frameworks/backend/express).
5. The production endpoint must be accessible to the ESP32 without a Vercel
   login page. Check Deployment Protection for this project if an authentication
   page appears. Application authentication remains enforced by `DEVICE_TOKEN`.
6. Open `https://YOUR-PRODUCTION-DOMAIN/health`. It must return
   `{"status":"ok","configured":true}`. This checks required settings, not
   whether Google accepts your key. Run the deployment check below for that.

| Variable | Value |
| --- | --- |
| `ADMIN_TOKEN` | Private 32–128 character URL-safe dashboard access code. Use an independently generated 64-character random value. |
| `SETTINGS_ENCRYPTION_KEY` | Private 64-character hexadecimal encryption key. Generate independently and keep it with the database. |
| `DATABASE_URL` | Private Neon PostgreSQL connection string; required for the dashboard and persistent settings/history. |
| `GEMINI_API_KEY` | Private Gemini API key from [Google AI Studio](https://aistudio.google.com/apikey). Use the plural variable for multiple keys. The account needs access/quota for the selected Live model. |
| `GEMINI_API_KEYS` | Optional comma-separated key pool. Takes precedence over the singular variable; use this instead when rotating multiple keys. |
| `GEMINI_KEY_COOLDOWN_SECONDS` | Optional cooldown after a key reports a rate limit; default `60`, allowed `15..3600`. |
| `DEVICE_TOKEN` | The random 64-character token you generated. It authenticates the ESP32 and is separate from the Gemini key. |
| `PUBLIC_BASE_URL` | Your exact production HTTPS origin, without `/ws`, `/ota/`, query strings or credentials. |
| `GEMINI_LIVE_MODEL` | Optional; default `gemini-3.8-live`. Use a currently available Live model compatible with PCM audio. |
| `GEMINI_VOICE` | Optional; default `Kore`. |
| `ASSISTANT_INSTRUCTIONS` | Optional; change the assistant's name, language, personality and reply length. Defaults to friendly, short replies in the user's language. |
| `SESSION_SECONDS` | Optional; default `270`, allowed `30..270`. |

### Multiple Gemini keys

The website shows read-only masked key status. It has no key entry or removal
controls, and its API rejects credential edits. Configure keys only in private
Vercel Production environment variables. Those values remain authoritative
across redeployments, including when a saved dashboard profile already exists.
**My assistant** still controls voice, model, instructions, history and cooldown.

Set this private Production variable to a comma-separated list, then redeploy:

```dotenv
GEMINI_API_KEYS=key_one,key_two,key_three
```

`GEMINI_API_KEYS` takes precedence over `GEMINI_API_KEY`. The original singular
variable also accepts a comma-separated list for compatibility. Spaces around
commas are trimmed, empty entries ignored and duplicate keys removed. The pool
allows up to 32 keys. Every new Live connection chooses the next key; an active
session retains its key so audio/context continue without interruption. On a
429/RESOURCE_EXHAUSTED/quota-limit response, that key enters a 60-second cooldown
and is skipped. During setup the backend immediately tries the next available
key. A rate limit during an active session closes it; the ESP32 automatically
reconnects using another available key. An interrupted question may need to be
repeated and the new session resets context. When every key is cooling down,
the backend stops making provider requests until one becomes eligible; firmware
retry backoff remains active. Cooldown is adjustable with the variable above.
Keys are never returned to the device or printed in logs;
logs show only a slot number.

Rotation is per Vercel instance, with a random first slot on a cold start;
there is no shared global counter. Google applies limits per project, so keys
from the same project share its quota. See [Google rate limits](https://ai.google.dev/gemini-api/docs/rate-limits).

Keep `GEMINI_API_KEY`, `GEMINI_API_KEYS` and `DEVICE_TOKEN` secret. Do not add `NEXT_PUBLIC_` or
`VITE_` prefixes. Do not commit a real `.env` file. Provider usage and Vercel
compute are billed or limited according to your accounts; always-listening
mode transmits silence as well as speech.

## Connect the ESP32

Use the **new custom-backend conversation firmware**, not diagnostics or the
previous official-service image. The guarded USB flasher preserves existing
Wi-Fi/settings. On the first custom-backend boot, missing backend settings
open the setup hotspot even when a home Wi-Fi network was already saved.

1. Join **Shreeharsh Assistant** on your phone using the random setup key shown
   on the ESP32 screen. Choose **Stay connected** when the phone reports no
   internet. Temporarily disable mobile data/automatic network switching if
   your browser keeps abandoning this hotspot.
2. Open **http://192.168.4.1** while still connected to that hotspot.
3. Enter your **2.4 GHz Wi-Fi name/password**, **production backend HTTPS address**
   and the same **DEVICE_TOKEN** you saved in Vercel. Do not enter the Gemini key.
4. Wait for **Connected** and an IP address. The device saves the settings after
   DHCP succeeds and starts the assistant. Rejoin your home Wi-Fi on the phone.
5. When the face shows Listening, speak normally and pause at the end of the
   question. It replies and resumes listening after playback drains. No BOOT
   press is needed. Wait for the reply to finish before speaking again.

The face remains the black/peach landscape UI. Serial `handsfree off` (or
`stop`) pauses automatic microphone streaming; `handsfree on` resumes it.
Reboot defaults to hands-free on. Serial `setup` reopens provisioning without
erasing stored settings. Re-enter the device token when submitting setup again.

## Check a real deployment

In a local copy of this folder, run `npm ci`. Create a private `.env` or `.env.local` from
`.env.example` with `PUBLIC_BASE_URL` and `DEVICE_TOKEN`. The local check does
not need the Gemini key; that key must already be configured on Vercel.

```powershell
npm run check-deployment
```

This authenticates discovery and opens a real provider session through your
deployment. It checks the negotiated audio format and closes immediately.
It does not prove microphone recognition or audible playback. After it passes,
test two spoken questions on the physical device and inspect `diag` for audio
errors and heap pressure. Amplifier SD enable was confirmed by the user, but
earlier speaker tests were silent; acoustic playback needs a new check.

## Development and limitations

```powershell
npm ci
npm run build
npm test
npm run dev
```

Local development reads `.env`, then `.env.local` overrides, and listens only
at `127.0.0.1:8080`. Existing process variables take precedence over both files.
Copy `.env.example` to `.env` and enter your keys privately, for example:

```dotenv
GEMINI_API_KEYS=your_first_key,your_second_key,your_third_key
```

Run `npm run dev` after saving. Both private files are ignored by Git and excluded
from release ZIPs. Hosted Production needs the same values entered in Vercel's
Environment Variables; a local file does not configure that deployment. Every new deployment reads its provider keys from the server environment.
Saved dashboard settings cannot override them.
The firmware requires a public HTTPS origin; it will not accept this local HTTP
address. The tests use a provider double and real Opus codecs. Production
always connects to Google; missing keys produce a failure, not fake replies.

Input is 16 kHz mono PCM after decoding 60 ms Opus frames. Output is 24 kHz
mono PCM encoded into paced 60 ms Opus frames, including a padded final frame.
The device resamples output to its existing I2S clocks. Google's automatic
speech detection ends each question; there is no on-device wake word and no
capture during replies. See [Google Live audio and VAD](https://ai.google.dev/gemini-api/docs/live-api/capabilities).

Queues, packet sizes and input rate are bounded. Up to four concurrent sockets
are accepted per instance; this is not an account-wide rate limit or a
multi-user platform. Use a separate token/project for a separate trust group.
The firmware uses TLS but does not enable flash encryption: a physical device
readout can reveal its device token.

Vercel sessions renew before the function deadline. The firmware reconnects
automatically, with failed attempts backed off to 60 seconds. **Conversation
context is kept only within the current Live session** and resets on renewal,
disconnect or backend restart. Persistent memory/session resumption is not
implemented. A renewal can interrupt an in-progress reply; test recovery on
your actual deployment before relying on long conversations.

### Troubleshooting

- `/health` says `configured:false`: set the production origin, configure private Gemini keys
  and configure a device token or create an enabled device. Preview variables
  do not configure Production. Provider keys always come from server environment
  variables; the database stores the other assistant settings.
- `/health` says `storage_unavailable`, or dashboard cannot load: check Production
  `DATABASE_URL`, database availability and the original `SETTINGS_ENCRYPTION_KEY`.
- HTTP 401: ESP32 and Vercel device tokens differ. Reopen setup and enter the
  current token. `/ota/` and `/ws` are intentionally private.
- A Vercel login page: production Deployment Protection is preventing device
  access. Adjust protection on this project's device endpoint.
- Discovery works but voice closes: check Live model access/quota and Vercel
  runtime logs. The backend deliberately returns generic errors without keys
  or raw provider payloads.
- Listening but no sound: inspect speaker enable/VIN and physical playback,
  then inspect I2S counters. A successful network handshake is not speaker proof.

Local validation is recorded in `VALIDATION.md`. Real deployment, speech
recognition, speaker output and endurance remain pending until your key and
production endpoint are configured.
