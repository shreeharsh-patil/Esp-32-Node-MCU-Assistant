import { neon } from '@neondatabase/serverless';
import { createCipheriv, createDecipheriv, createHash, randomBytes, randomUUID } from 'node:crypto';
import { authorized } from './config.js';

export const keyId = key => createHash('sha256').update(key).digest('hex').slice(0, 16);
export const tokenHash = token => createHash('sha256').update(token).digest('hex');
const now = () => new Date().toISOString();
export const validDeviceId = id => typeof id === 'string' && /^[A-Za-z0-9:_-]{1,64}$/.test(id);
export function publicError(message, status = 400) { return Object.assign(new Error(message), { publicMessage: message, status }); }

export function encrypt(value, key) {
  const iv = randomBytes(12);
  const cipher = createCipheriv('aes-256-gcm', Buffer.from(key, 'hex'), iv);
  const bytes = Buffer.concat([cipher.update(JSON.stringify(value)), cipher.final()]);
  return [iv, cipher.getAuthTag(), bytes].map(part => part.toString('base64')).join('.');
}
export function decrypt(value, key) {
  const [iv, tag, bytes] = value.split('.').map(part => Buffer.from(part, 'base64'));
  const cipher = createDecipheriv('aes-256-gcm', Buffer.from(key, 'hex'), iv);
  cipher.setAuthTag(tag);
  return JSON.parse(Buffer.concat([cipher.update(bytes), cipher.final()]).toString());
}

const schema = [
  `CREATE TABLE IF NOT EXISTS assistant_config (id INTEGER PRIMARY KEY, revision TEXT NOT NULL, encrypted TEXT NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS assistant_devices (id TEXT PRIMARY KEY, name TEXT NOT NULL, token_hash TEXT,
    enabled BOOLEAN NOT NULL DEFAULT true, last_seen TEXT, state TEXT NOT NULL DEFAULT 'offline')`,
  `CREATE TABLE IF NOT EXISTS assistant_sessions (id TEXT PRIMARY KEY, device_id TEXT NOT NULL,
    started_at TEXT NOT NULL, ended_at TEXT, state TEXT NOT NULL, model TEXT NOT NULL)`,
  `CREATE TABLE IF NOT EXISTS assistant_messages (id TEXT PRIMARY KEY, session_id TEXT NOT NULL,
    device_id TEXT NOT NULL, role TEXT NOT NULL, encrypted TEXT NOT NULL, created_at TEXT NOT NULL)`,
  `CREATE INDEX IF NOT EXISTS assistant_messages_device ON assistant_messages (device_id, created_at)`,
  `CREATE INDEX IF NOT EXISTS assistant_sessions_started ON assistant_sessions (started_at)`,
];

export class DatabaseStore {
  constructor(config, query) {
    this.config = config;
    this.queryOverride = query;
    this.runtimeCache = null;
  }
  async query(text, params = []) {
    if (this.queryOverride) return this.queryOverride(text, params);
    if (!this.sql) this.sql = neon(this.config.databaseUrl);
    return this.sql.query(text, params, { fetchOptions: { signal: AbortSignal.timeout(8000) } });
  }
  async init() {
    if (!this.config.encryptionKey || (!this.config.databaseUrl && !this.queryOverride)) {
      throw publicError('Connect a Neon database and set SETTINGS_ENCRYPTION_KEY to enable the dashboard.', 503);
    }
    if (!this.initializing) this.initializing = (async () => {
      for (const statement of schema) await this.query(statement);
      const profile = { name: 'Shreeharsh Assistant', model: this.config.model, voice: this.config.voice,
        instructions: this.config.instructions, historyEnabled: true,
        keyCooldownMs: this.config.keyCooldownMs };
      await this.query('INSERT INTO assistant_config (id, revision, encrypted) VALUES (1, $1, $2) ON CONFLICT (id) DO NOTHING',
        [randomUUID(), encrypt(profile, this.config.encryptionKey)]);
      // Migrate older dashboard profiles: provider credentials now belong only
      // to the private server environment. Preserve all other saved settings.
      const [row] = await this.query('SELECT revision, encrypted FROM assistant_config WHERE id = 1');
      const stored = decrypt(row.encrypted, this.config.encryptionKey);
      if (Object.hasOwn(stored, 'apiKeys')) {
        delete stored.apiKeys;
        await this.query('UPDATE assistant_config SET revision = $1, encrypted = $2 WHERE id = 1 AND revision = $3',
          [randomUUID(), encrypt(stored, this.config.encryptionKey), row.revision]);
      }
    })().catch(error => { this.initializing = null; throw error; });
    await this.initializing;
  }
  async profile() {
    await this.init();
    const [row] = await this.query('SELECT revision, encrypted FROM assistant_config WHERE id = 1');
    const { apiKeys: _legacyKeys, ...profile } = decrypt(row.encrypted, this.config.encryptionKey);
    return { ...profile, revision: row.revision };
  }
  async runtime() {
    const profile = await this.profile();
    if (this.runtimeCache?.revision !== profile.revision) {
      this.runtimeCache = { ...this.config, ...profile, apiKeys: this.config.apiKeys };
    }
    return this.runtimeCache;
  }
  safeProfile(profile) {
    const { apiKeys, ...safe } = profile;
    return { ...safe, keys: this.config.apiKeys.map(key => ({ id: keyId(key), mask: '••••' + key.slice(-4) })) };
  }
  async saveProfile(body) {
    if (['apiKeys', 'addKeys', 'removeKeys'].some(field => Object.hasOwn(body, field))) {
      throw publicError('Manage Gemini keys only in private Vercel environment variables, then redeploy.');
    }
    const current = await this.profile();
    if (body.revision !== current.revision) throw publicError('Settings changed in another tab. Refresh before saving.', 409);
    for (const [field, limit] of [['name', 80], ['model', 120], ['voice', 40], ['instructions', 6000]]) {
      if (typeof body[field] !== 'string' || !body[field].trim() || body[field].length > limit) throw publicError(`Invalid ${field}`);
    }
    if (typeof body.historyEnabled !== 'boolean') throw publicError('Choose whether to save conversation history');
    const cooldown = Number(body.keyCooldownSeconds);
    if (!Number.isInteger(cooldown) || cooldown < 15 || cooldown > 3600) throw publicError('Key cooldown must be 15-3600 seconds');
    const next = { name: body.name.trim(), model: body.model.trim(), voice: body.voice.trim(),
      instructions: body.instructions.trim(), historyEnabled: body.historyEnabled,
      keyCooldownMs: cooldown * 1000 };
    const revision = randomUUID();
    const changed = await this.query('UPDATE assistant_config SET revision = $1, encrypted = $2 WHERE id = 1 AND revision = $3 RETURNING revision',
      [revision, encrypt(next, this.config.encryptionKey), current.revision]);
    if (!changed.length) throw publicError('Settings changed in another tab. Refresh before saving.', 409);
    this.runtimeCache = null;
    return this.safeProfile({ ...next, revision });
  }
  async devices() { await this.init(); return this.query('SELECT id, name, enabled, last_seen, state FROM assistant_devices ORDER BY name'); }
  async device(id) { await this.init(); return (await this.query('SELECT * FROM assistant_devices WHERE id = $1', [id]))[0]; }
  async authenticate(id, header) {
    if (!validDeviceId(id) || typeof header !== 'string' || !/^Bearer [A-Za-z0-9_-]{32,128}$/.test(header)) return null;
    await this.init();
    let device = await this.device(id);
    if (!device) {
      if (!authorized(header, this.config.token)) return null;
      await this.query("INSERT INTO assistant_devices (id, name) VALUES ($1, $2) ON CONFLICT (id) DO NOTHING", [id, 'Pocket Assistant']);
      device = await this.device(id);
    }
    if (!device.enabled) return null;
    if (device.token_hash && !authorized('Bearer ' + tokenHash(header.slice(7)), device.token_hash)) return null;
    if (!device.token_hash && !authorized(header, this.config.token)) return null;
    return { id, token: header.slice(7) };
  }
  async addDevice(id, name) {
    if (!validDeviceId(id) || typeof name !== 'string' || !name.trim() || name.length > 80) throw publicError('Enter a device ID and name');
    if (await this.device(id)) throw publicError('This device ID already exists', 409);
    const token = randomBytes(32).toString('hex');
    await this.query('INSERT INTO assistant_devices (id, name, token_hash) VALUES ($1, $2, $3)', [id, name.trim(), tokenHash(token)]);
    return { id, token };
  }
  async updateDevice(id, body) {
    if (!validDeviceId(id) || !(await this.device(id))) throw publicError('Device not found', 404);
    if (typeof body.name !== 'string' || !body.name.trim() || body.name.length > 80 || typeof body.enabled !== 'boolean') throw publicError('Invalid device settings');
    await this.query('UPDATE assistant_devices SET name = $1, enabled = $2 WHERE id = $3', [body.name.trim(), body.enabled, id]);
    return { ok: true };
  }
  async rotateToken(id) {
    if (!validDeviceId(id) || !(await this.device(id))) throw publicError('Device not found', 404);
    const token = randomBytes(32).toString('hex');
    await this.query('UPDATE assistant_devices SET token_hash = $1 WHERE id = $2', [tokenHash(token), id]);
    return { id, token };
  }
  async touchDevice(id, state) {
    await this.query('UPDATE assistant_devices SET last_seen = $1, state = $2 WHERE id = $3', [now(), state, id]);
  }
  async startSession(id, deviceId, model) {
    await this.query('INSERT INTO assistant_sessions (id, device_id, started_at, state, model) VALUES ($1, $2, $3, $4, $5)',
      [id, deviceId, now(), 'connected', model]);
    await this.touchDevice(deviceId, 'connected');
  }
  async endSession(id) { await this.query("UPDATE assistant_sessions SET ended_at = $1, state = 'ended' WHERE id = $2", [now(), id]); }
  async saveTurn(sessionId, deviceId, user, assistant) {
    for (const [role, text] of [['user', user], ['assistant', assistant]]) {
      if (!text.trim()) continue;
      await this.query('INSERT INTO assistant_messages (id, session_id, device_id, role, encrypted, created_at) VALUES ($1, $2, $3, $4, $5, $6)',
        [randomUUID(), sessionId, deviceId, role, encrypt(text.slice(0, 6000), this.config.encryptionKey), now()]);
    }
  }
  async conversations(deviceId = '') {
    if (deviceId && !validDeviceId(deviceId)) throw publicError('Invalid device ID');
    await this.init();
    const rows = deviceId ? await this.query('SELECT * FROM assistant_messages WHERE device_id = $1 ORDER BY created_at DESC LIMIT 100', [deviceId]) :
      await this.query('SELECT * FROM assistant_messages ORDER BY created_at DESC LIMIT 100');
    return rows.map(({ encrypted, ...row }) => ({ ...row, text: decrypt(encrypted, this.config.encryptionKey) }));
  }
  async deleteHistory(deviceId) {
    if (deviceId && !validDeviceId(deviceId)) throw publicError('Invalid device ID');
    await this.init();
    if (deviceId) await this.query('DELETE FROM assistant_messages WHERE device_id = $1', [deviceId]);
    else await this.query('DELETE FROM assistant_messages');
    return { ok: true };
  }
  async summary() {
    await this.init();
    const devices = await this.devices();
    const [count] = await this.query('SELECT COUNT(*) AS total FROM assistant_sessions');
    const sessions = await this.query('SELECT * FROM assistant_sessions ORDER BY started_at DESC LIMIT 8');
    return { devices: devices.length, connected: devices.filter(device => device.state !== 'offline' && device.last_seen &&
      Date.now() - Date.parse(device.last_seen) < 60000).length, sessions: Number(count.total), recentSessions: sessions };
  }
}
