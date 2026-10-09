# Your assistant dashboard

This is a single-owner XiaoZhi-style console for **Shreeharsh Assistant**.
It includes assistant settings, read-only Gemini key-pool status, device
access controls and conversation history. Gemini keys are managed only in
private Vercel environment variables. It uses your own editable backend
and Gemini account; there is no XiaoZhi cloud account or activation step.

## First deployment

1. Deploy this backend to your own Vercel project using the [README](README.md).
   Use Express, Node 24.x and Fluid compute. When importing the whole firmware
   repository, set Root Directory to `backend`; the standalone ZIP is already
   the backend root.
2. Connect **Neon PostgreSQL** to this same Vercel project through
   Storage/Marketplace. Choose Production for its connection settings. Ensure
   the provided PostgreSQL connection string is configured privately as
   `DATABASE_URL`. See [Neon on Vercel](https://vercel.com/marketplace/neon).
3. Run this command locally **three separate times** and save the results in
   private Production variables: `ADMIN_TOKEN`, `SETTINGS_ENCRYPTION_KEY`, and
   `DEVICE_TOKEN`. Each must have its own value.

   ```powershell
   node -e "process.stdout.write(require('node:crypto').randomBytes(32).toString('hex'))"
   ```

4. Set `PUBLIC_BASE_URL` to your stable production HTTPS origin. Configure
   `GEMINI_API_KEYS=key_one,key_two,key_three` privately in Vercel Production.
   A single key can use `GEMINI_API_KEY` instead. No `NEXT_PUBLIC_` prefix.
   Variables must exist in Vercel Production; uploading a local `.env` file is
   not sufficient. Redeploy after adding these variables.
5. Open your production domain. Sign in with `ADMIN_TOKEN`. The server creates
   its database tables on first use, then keeps saved settings across restarts.
   Keep `SETTINGS_ENCRYPTION_KEY` unchanged with this database. If it is lost,
   encrypted assistant settings and transcripts cannot be recovered. Back up it and the
   database privately. Changing `ADMIN_TOKEN` signs out existing admin sessions.
6. Open **API status** to check the masked environment key pool. There is no
   key entry, addition or removal control on the website. To change keys, update
   private Vercel Production variables and redeploy. Choose a Live model
   available to your Google project and your preferred voice in **My assistant**.
7. Configure the ESP32 with the production URL and `DEVICE_TOKEN` through its
   **Shreeharsh Assistant** Wi-Fi hotspot at **http://192.168.4.1**. Stay connected
   to that hotspot when the phone reports no internet. Enter home 2.4 GHz Wi-Fi
   details and the device token, then wait for Connected. Never put the Gemini
   or admin key in the device setup page.
8. The device appears automatically after its first authenticated discovery
   request. The custom conversation firmware listens without repeated button
   presses. Run the real deployment and physical audio checks in the README.

## What each screen does

| Screen | Controls |
| --- | --- |
| Overview | Connected devices seen within 60 seconds, saved session count, masked key pool and recent connections. Refresh to update these values. |
| My assistant | Name, voice, Live model, instructions, key cooldown and transcript saving. Saves apply to new sessions. |
| API status | Read-only masked environment key pool and this instance's cooldowns. Manage keys only through private Vercel variables and redeploy. |
| Devices | Rename, pause/enable access, create a separate device token or rotate one. New tokens are shown once. Active sessions recheck authorization every 20 seconds. |
| Conversations | Latest 100 encrypted transcript messages, optionally filtered by device. Clear history deletes the selected transcripts; session counts remain. |
| Settings | Backend connection details and provisioning guidance. Infrastructure variables remain private in Vercel. |

Device IDs are the **Device-Id MAC address** sent by this firmware, for example
`aa:bb:cc:dd:ee:ff`. The easiest first setup uses `DEVICE_TOKEN`: the device then
registers itself and shows its ID. Rotate its token for individual credentials,
copy the new token, and run serial `setup` to enter it on the provisioning page.
After rotation, the old/global token no longer authenticates that device.
Alternatively, create a device using its exact Device-Id before provisioning.
Per-device tokens are stored as hashes and never returned by device listings.

The same owner profile applies to all devices. Multiple users, separate agent
profiles, another provider adapter, voice recording storage and persistent
model memory are not implemented. History is a transcript viewer: it is not
automatically sent back as memory when a voice session renews.

Gemini pools rotate on new sessions and skip rate-limited keys. Setup quota
errors try the next eligible key immediately; an active error closes the session
so firmware can reconnect. Cooldown is per instance, resets on configuration
changes or cold starts, and does not increase Google's project-level quota.
Keys from one Google project share limits. If every key is cooling down, the
backend waits for eligibility rather than continuing provider requests.

## Private access and storage

Admin sign-in uses an eight-hour signed HttpOnly SameSite cookie, Secure on the
production domain. Mutating dashboard requests require the same origin. There
is no browser localStorage token. Logout clears the browser cookie; rotating
`ADMIN_TOKEN` invalidates signed cookies on the next deployment. The local
sign-in throttle is per instance rather than an account-wide distributed limit.

Assistant settings and transcript text use AES-256-GCM with a fresh nonce per
record. Database connection details, owner credentials and encryption keys are
never returned to the dashboard. Gemini keys are read only from server
environment variables; older dashboard-stored keys are removed automatically
without changing voice, instructions or history. Empty environment keys leave
the provider unconfigured, with no stored-key fallback. Device metadata, session times and model names
are stored as ordinary database metadata. Raw microphone/reply audio is not
persisted. Disabling history affects new sessions and retains existing messages
until you clear them. Transcript writes are bounded and asynchronous; a database
write failure can leave gaps in history without generating a fake conversation.

Production uses `@neondatabase/serverless` over HTTPS. Local automated checks
use an in-memory PostgreSQL emulator and provider doubles. They verify SQL
behavior, persistence across store instances, encryption, owner controls and
the audio contract; they do not prove a deployed Neon connection or Google
account access. The browser preview fixture is test-only, uses disposable
in-memory data and has no live voice connection.
