import express from 'express';
import { createServer } from 'node:http';
import { randomUUID } from 'node:crypto';
import { WebSocketServer } from 'ws';
import { readConfig, authorized, discovery } from './config.js';
import { openGemini } from './gemini.js';
import { VoiceBridge, validHello } from './bridge.js';
import { DatabaseStore, validDeviceId } from './store.js';
import { installAdmin } from './admin.js';
import { ConversationJournal } from './history.js';
import { fileURLToPath } from 'node:url';
import { setTimeout as sleep } from 'node:timers/promises';

async function withinBudget(work, milliseconds) {
  let timer;
  try {
    return await Promise.race([Promise.resolve().then(work), new Promise((_resolve, reject) => {
      timer = setTimeout(() => reject(new Error('Storage timeout')), milliseconds);
    })]);
  } finally { clearTimeout(timer); }
}

export function createBackend(config, providerFactory = openGemini, options = {}) {
  const store = options.store || (config.databaseUrl && config.encryptionKey ? new DatabaseStore(config) : null);
  const storageBudgetMs = options.storageBudgetMs || 1500;
  const app = express();
  app.disable('x-powered-by');
  app.use((req, res, next) => {
    res.set('X-Content-Type-Options', 'nosniff');
    res.set('Referrer-Policy', 'same-origin');
    res.set('Content-Security-Policy', "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; connect-src 'self'; frame-ancestors 'none'; base-uri 'self'; form-action 'self'");
    if (req.path.startsWith('/api/')) res.set('Cache-Control', 'no-store');
    next();
  });
  app.use('/api/admin', express.json({ limit: '20kb' }));
  app.use(express.json({ limit: '8kb' }));
  installAdmin(app, config, store, options);
  app.use(express.static(fileURLToPath(new URL('../public/', import.meta.url)), { maxAge: 0 }));
  const runtimeConfig = () => store ? withinBudget(() => store.runtime(), storageBudgetMs) : Promise.resolve(config);
  const voiceRuntime = async () => {
    try { return await runtimeConfig(); }
    catch {
      console.warn('Voice settings storage unavailable; using last known or environment settings');
      return store.runtimeCache || config;
    }
  };
  const ready = runtime => Boolean(runtime.base && runtime.apiKeys?.length);
  const identityFor = async req => {
    const id = req.headers['device-id'] || req.headers['client-id'] || 'pocket-esp32';
    if (!validDeviceId(id)) return null;
    // Device revocation must still fail closed if authentication storage is
    // unavailable. Only optional settings/history may fall back to the env.
    if (store) return withinBudget(() => store.authenticate(id, req.headers.authorization), storageBudgetMs);
    return authorized(req.headers.authorization, config.token) ? { id, token: config.token } : null;
  };
  app.get('/health', async (_req, res) => {
    try {
      const runtime = await runtimeConfig();
      const paired = Boolean(config.token) || Boolean(store &&
        (await withinBudget(() => store.devices(), storageBudgetMs)).some(device => device.enabled));
      res.json({ status: 'ok', configured: ready(runtime) && paired });
    } catch { res.status(503).json({ status: 'storage_unavailable', configured: false }); }
  });
  app.post('/ota/', async (req, res) => {
    res.set('Cache-Control', 'no-store');
    const identity = await identityFor(req);
    if (!identity) return res.status(401).json({ error: 'Unauthorized' });
    const runtime = await voiceRuntime();
    if (!ready(runtime)) return res.status(503).json({ error: 'Configure your backend settings first' });
    return res.json(discovery({ ...runtime, token: identity.token }));
  });
  app.use((error, _req, res, _next) => {
    // Do not send SDK errors, request bodies or secret values back to clients.
    if (error.publicMessage) return res.status(error.status).json({ error: error.publicMessage });
    if (error.type === 'entity.too.large' || error.type === 'entity.parse.failed') {
      return res.status(error.status === 413 ? 413 : 400).json({ error: 'Invalid request' });
    }
    console.error('Backend request failed');
    res.status(503).json({ error: 'Backend storage or configuration is unavailable. Check private server settings.' });
  });
  const server = createServer(app);
  const wss = new WebSocketServer({ noServer: true, maxPayload: 16384 });
  let upgrades = 0;
  server.on('upgrade', async (req, socket, head) => {
    socket.on('error', () => {});
    let path;
    try { path = new URL(req.url, 'http://localhost').pathname; }
    catch { socket.end('HTTP/1.1 400 Rejected\r\nConnection: close\r\n\r\n'); return; }
    const status = path !== '/ws' ? 404 : wss.clients.size + upgrades >= 4 ? 429 : 0;
    if (status) { socket.end(`HTTP/1.1 ${status} Rejected\r\nConnection: close\r\n\r\n`); return; }
    upgrades++;
    try {
      const identity = await identityFor(req);
      if (!identity) { socket.end('HTTP/1.1 401 Rejected\r\nConnection: close\r\n\r\n'); return; }
      const runtime = await voiceRuntime();
      if (!ready(runtime)) { socket.end('HTTP/1.1 503 Rejected\r\nConnection: close\r\n\r\n'); return; }
      if (!socket.destroyed) wss.handleUpgrade(req, socket, head, ws => wss.emit('connection', ws, { identity, runtime }));
    } catch { socket.end('HTTP/1.1 503 Rejected\r\nConnection: close\r\n\r\n'); }
    finally { upgrades--; }
  });
  wss.on('connection', (ws, { identity, runtime }) => {
    const sessionId = randomUUID();
    const journal = store ? new ConversationJournal(store, sessionId, identity.id, runtime.historyEnabled) : null;
    let bridge = null;
    let opening = false;
    let closed = false;
    let sessionWrite = Promise.resolve();
    const providerAbort = new AbortController();
    // A timed renewal during a reply discards its remaining audio on the
    // ESP32. Give the active reply a short grace period, then reconnect.
    const renew = () => {
      if (closed || ws.readyState !== 1) return;
      if (bridge && (bridge.speaking || bridge.pendingBytes || bridge.pcm.length ||
        Date.now() < bridge.playbackDrainUntil)) {
        renewalTimer = setTimeout(renew, 250);
        return;
      }
      ws.close(1000, 'Session renewal');
    };
    let renewalTimer = setTimeout(renew, runtime.seconds * 1000);
    const renewalLimit = setTimeout(() => ws.close(1000, 'Session renewal'),
      (runtime.seconds + 25) * 1000);
    const helloDeadline = setTimeout(() => ws.close(1008, 'Client hello required'), 5000);
    let checkingDevice = false;
    let storageFailureLogged = false;
    const heartbeat = setInterval(async () => {
      if (ws.readyState !== 1) return;
      ws.ping();
      // ESP32's application timeout tracks data messages, not WS control pings.
      // Keep a quiet always-listening session alive until deliberate renewal.
      if (bridge?.provider) bridge.sendJson({ type: 'ping' });
      if (store && !checkingDevice && bridge?.provider) {
        checkingDevice = true;
        try {
          if (!await withinBudget(() => store.authenticate(identity.id, 'Bearer ' + identity.token), storageBudgetMs)) {
            ws.close(1008, 'Device access changed');
          } else {
            await withinBudget(() => store.touchDevice(identity.id,
              bridge.speaking ? 'speaking' : bridge.listening ? 'listening' : 'connected'), storageBudgetMs);
          }
          storageFailureLogged = false;
        } catch {
          // The session was already authenticated and has a hard renewal
          // deadline. A temporary history outage must not cut off its reply.
          if (!storageFailureLogged) console.warn('Active voice session storage unavailable');
          storageFailureLogged = true;
        }
        finally { checkingDevice = false; }
      }
    }, options.heartbeatMs || 20000);
    ws.on('error', () => ws.close(1011, 'Connection error'));
    ws.on('close', code => {
      console.info('Voice connection closed', code, bridge?.speaking ? 'during_reply' : 'between_replies');
      closed = true;
      providerAbort.abort();
      clearTimeout(renewalTimer); clearTimeout(renewalLimit);
      clearTimeout(helloDeadline); clearInterval(heartbeat);
      bridge?.destroy();
      if (journal) (async () => {
        await sessionWrite;
        await journal.finish(); await store.endSession(sessionId);
        await store.touchDevice(identity.id, 'offline');
      })().catch(() => console.error('Voice session history cleanup failed'));
    });
    ws.on('message', async (data, binary) => {
      if (closed || ws.readyState !== 1) return;
      try {
        if (!bridge) {
          if (opening || binary) throw new Error('Client hello required');
          const hello = JSON.parse(data.toString());
          if (!validHello(hello)) throw new Error('Unsupported audio parameters');
          opening = true;
          clearTimeout(helloDeadline);
          bridge = new VoiceBridge(ws);
          let expired = false;
          const timer = setTimeout(() => { expired = true; providerAbort.abort(); ws.close(1011, 'Voice provider timeout'); }, 8000);
          try {
            if (store) sessionWrite = Promise.resolve().then(() =>
              store.startSession(sessionId, identity.id, runtime.model))
              .catch(() => console.warn('Voice session metrics write failed'));
            const memory = store && runtime.historyEnabled && typeof store.recentTurns === 'function' ?
              await Promise.race([store.recentTurns(identity.id).catch(() => []), sleep(1200).then(() => [])]) : [];
            const provider = await providerFactory(runtime,
              content => { try { bridge.enqueue(content); journal?.observe(content); } catch { ws.close(1009, 'Reply exceeds bounds'); } },
              () => ws.close(1011, 'Voice provider unavailable'), { signal: providerAbort.signal, memory, timezone: hello.timezone });
            if (closed || expired) { provider.close(); return; }
            bridge.provider = provider;
            bridge.sendJson({ type: 'hello', transport: 'websocket', session_id: sessionId,
              audio_params: { format: 'opus', sample_rate: 24000, channels: 1, frame_duration: 60 } });
          } finally { clearTimeout(timer); }
          return;
        }
        if (!bridge.provider) throw new Error('Voice provider not ready');
        if (binary) bridge.handlePacket(Buffer.from(data));
        else bridge.handleControl(JSON.parse(data.toString()));
      } catch (error) {
        const status = Number(error?.status);
        // Log only known protocol failures, never SDK errors or client text.
        const reason = ({
          'Invalid audio size': 'invalid_audio_size',
          'Audio rate limit': 'input_rate_limit',
          'Expected a 60ms mono frame': 'invalid_frame_duration',
          'Voice provider not ready': 'provider_not_ready',
          'Client hello required': 'client_hello_required',
          'Unsupported audio parameters': 'unsupported_audio_parameters',
        })[error?.message] || 'unexpected_error';
        console.error('Voice connection failed', bridge?.provider ? 'audio_or_control' :
          opening ? 'provider_setup' : 'client_hello',
          Number.isInteger(status) && status >= 400 && status <= 599 ? status : 'unknown', reason);
        ws.close(1011, 'Voice connection failed');
      }
    });
  });
  return { server, wss, store };
}

const backend = createBackend(readConfig());
export default backend.server;
