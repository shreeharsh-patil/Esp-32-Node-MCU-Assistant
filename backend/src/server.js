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

export function createBackend(config, providerFactory = openGemini, options = {}) {
  const store = options.store || (config.databaseUrl ? new DatabaseStore(config) : null);
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
  const runtimeConfig = () => store ? store.runtime() : Promise.resolve(config);
  const ready = runtime => Boolean(runtime.base && runtime.apiKeys?.length);
  const identityFor = async req => {
    const id = req.headers['device-id'] || req.headers['client-id'] || 'pocket-esp32';
    if (!validDeviceId(id)) return null;
    if (store) return store.authenticate(id, req.headers.authorization);
    return authorized(req.headers.authorization, config.token) ? { id, token: config.token } : null;
  };
  app.get('/health', async (_req, res) => {
    try {
      const runtime = await runtimeConfig();
      const paired = Boolean(config.token) || Boolean(store && (await store.devices()).some(device => device.enabled));
      res.json({ status: 'ok', configured: ready(runtime) && paired });
    } catch { res.status(503).json({ status: 'storage_unavailable', configured: false }); }
  });
  app.post('/ota/', async (req, res) => {
    res.set('Cache-Control', 'no-store');
    const identity = await identityFor(req);
    if (!identity) return res.status(401).json({ error: 'Unauthorized' });
    const runtime = await runtimeConfig();
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
      const runtime = await runtimeConfig();
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
    const providerAbort = new AbortController();
    const deadline = setTimeout(() => ws.close(1000, 'Session renewal'), runtime.seconds * 1000);
    const helloDeadline = setTimeout(() => ws.close(1008, 'Client hello required'), 5000);
    let checkingDevice = false;
    const heartbeat = setInterval(async () => {
      if (ws.readyState !== 1) return;
      ws.ping();
      // ESP32's application timeout tracks data messages, not WS control pings.
      // Keep a quiet always-listening session alive until deliberate renewal.
      if (bridge?.provider) bridge.sendJson({ type: 'ping' });
      if (store && !checkingDevice && bridge?.provider) {
        checkingDevice = true;
        try {
          if (!await store.authenticate(identity.id, 'Bearer ' + identity.token)) ws.close(1008, 'Device access changed');
          else await store.touchDevice(identity.id, bridge.speaking ? 'speaking' : bridge.listening ? 'listening' : 'connected');
        } catch { ws.close(1011, 'Device storage unavailable'); }
        finally { checkingDevice = false; }
      }
    }, 20000);
    ws.on('error', () => ws.close(1011, 'Connection error'));
    ws.on('close', () => {
      closed = true;
      providerAbort.abort();
      clearTimeout(deadline); clearTimeout(helloDeadline); clearInterval(heartbeat);
      bridge?.destroy();
      if (journal) (async () => {
        await journal.finish(); await store.endSession(sessionId);
        await store.touchDevice(identity.id, 'offline');
      })().catch(() => console.error('Voice session history cleanup failed'));
    });
    ws.on('message', async (data, binary) => {
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
            if (store) await store.startSession(sessionId, identity.id, runtime.model);
            const provider = await providerFactory(runtime,
              content => { try { bridge.enqueue(content); journal?.observe(content); } catch { ws.close(1009, 'Reply exceeds bounds'); } },
              () => ws.close(1011, 'Voice provider unavailable'), { signal: providerAbort.signal });
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
      } catch {
        console.error('Voice connection rejected or provider failed');
        ws.close(1011, 'Voice connection failed');
      }
    });
  });
  return { server, wss, store };
}

const backend = createBackend(readConfig());
export default backend.server;
