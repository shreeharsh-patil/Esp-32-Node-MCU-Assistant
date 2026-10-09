import { createHmac, randomBytes, timingSafeEqual } from 'node:crypto';
import { authorized } from './config.js';
import { keyPoolStatus } from './gemini.js';
import { publicError } from './store.js';

const COOKIE = 'assistant_admin';
const AGE = 8 * 60 * 60;
const sign = (value, token) => createHmac('sha256', token).update(value).digest('base64url');
const local = req => /^(127\.0\.0\.1|localhost)(:\d+)?$/.test(req.headers.host || '');
const cookie = (req, value, age = AGE) => `${COOKIE}=${value}; Path=/; HttpOnly; SameSite=Strict; Max-Age=${age}${local(req) ? '' : '; Secure'}`;

function sessionValid(req, token) {
  if (!token) return false;
  const value = (req.headers.cookie || '').split(';').map(part => part.trim()).find(part => part.startsWith(COOKIE + '='))?.slice(COOKIE.length + 1);
  if (!value || value.length > 1024) return false;
  const [body, signature] = value.split('.');
  if (!body || !signature) return false;
  const expected = sign(body, token);
  if (Buffer.byteLength(expected) !== Buffer.byteLength(signature) || !timingSafeEqual(Buffer.from(expected), Buffer.from(signature))) return false;
  try { return JSON.parse(Buffer.from(body, 'base64url')).expires > Date.now(); } catch { return false; }
}

export function installAdmin(app, config, store, options = {}) {
  const attempts = new Map();
  app.get('/api/admin/session', (req, res) => res.json({ authenticated: sessionValid(req, config.adminToken),
    adminConfigured: Boolean(config.adminToken), databaseConfigured: Boolean(store && config.encryptionKey),
    baseConfigured: Boolean(config.base), localPreview: options.localPreview === true }));
  const originCheck = (req, _res, next) => {
    const origin = req.headers.origin;
    if (!origin || (origin !== config.base && !(local(req) && origin === 'http://' + req.headers.host))) {
      return next(publicError('Open the dashboard on its own domain and try again.', 403));
    }
    next();
  };
  app.post('/api/admin/login', originCheck, (req, res) => {
    if (!config.adminToken) throw publicError('Set a private ADMIN_TOKEN in Vercel, then redeploy.', 503);
    const ip = req.ip || 'unknown';
    const time = Date.now();
    if (attempts.size >= 1024) for (const [id, value] of attempts) if (time - value.since > 600000) attempts.delete(id);
    if (attempts.size >= 1024 && !attempts.has(ip)) throw publicError('Try signing in later.', 429);
    const score = attempts.get(ip) || { since: time, count: 0 };
    if (time - score.since > 600000) { score.since = time; score.count = 0; }
    if (score.count >= 10) throw publicError('Too many sign-in attempts. Try again in 10 minutes.', 429);
    score.count++;
    attempts.set(ip, score);
    if (!authorized('Bearer ' + String(req.body?.token || ''), config.adminToken)) throw publicError('Incorrect admin access code.', 401);
    attempts.delete(ip);
    const body = Buffer.from(JSON.stringify({ expires: time + AGE * 1000, nonce: randomBytes(16).toString('hex') })).toString('base64url');
    res.setHeader('Set-Cookie', cookie(req, body + '.' + sign(body, config.adminToken)));
    res.json({ ok: true });
  });
  app.use('/api/admin', (req, res, next) => {
    res.set('Cache-Control', 'no-store');
    if (!sessionValid(req, config.adminToken)) throw publicError('Sign in to your dashboard.', 401);
    if (req.method !== 'GET') return originCheck(req, res, next);
    next();
  });
  app.post('/api/admin/logout', (req, res) => {
    res.setHeader('Set-Cookie', cookie(req, '', 0));
    res.json({ ok: true });
  });
  app.use('/api/admin', (_req, _res, next) => {
    if (!store) throw publicError('Connect a Neon database and set SETTINGS_ENCRYPTION_KEY to enable the dashboard.', 503);
    next();
  });
  app.get('/api/admin/state', async (_req, res) => {
    const runtime = await store.runtime();
    res.json({ profile: store.safeProfile(await store.profile()), keys: keyPoolStatus(runtime),
      devices: await store.devices(), summary: await store.summary(),
      backend: { base: config.base, model: runtime.model, configured: Boolean(config.base && runtime.apiKeys.length),
        firmwareVersion: '2.5.1' } });
  });
  app.post('/api/admin/profile', async (req, res) => res.json(await store.saveProfile(req.body || {})));
  app.post('/api/admin/devices', async (req, res) => res.status(201).json(await store.addDevice(req.body?.id, req.body?.name)));
  app.post('/api/admin/devices/:id', async (req, res) => res.json(await store.updateDevice(req.params.id, req.body || {})));
  app.post('/api/admin/devices/:id/token', async (req, res) => res.json(await store.rotateToken(req.params.id)));
  app.get('/api/admin/conversations', async (req, res) => res.json(await store.conversations(String(req.query.device || ''))));
  app.delete('/api/admin/conversations', async (req, res) => res.json(await store.deleteHistory(String(req.query.device || ''))));
}
