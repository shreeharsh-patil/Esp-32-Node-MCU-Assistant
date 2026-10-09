import { createHash, timingSafeEqual } from 'node:crypto';

export function readConfig(env = process.env) {
  const base = env.PUBLIC_BASE_URL || '';
  if (base) {
    const parsed = new URL(base);
    if (parsed.protocol !== 'https:' || parsed.username || parsed.password ||
        parsed.search || parsed.hash || !['', '/'].includes(parsed.pathname)) {
      throw new Error('PUBLIC_BASE_URL must be an HTTPS origin');
    }
  }
  const token = env.DEVICE_TOKEN || '';
  if (token && !/^[A-Za-z0-9_-]{32,128}$/.test(token)) {
    throw new Error('DEVICE_TOKEN must be 32-128 URL-safe characters');
  }
  const seconds = Number(env.SESSION_SECONDS || 270);
  if (!Number.isInteger(seconds) || seconds < 30 || seconds > 270) {
    throw new Error('SESSION_SECONDS must be 30-270');
  }
  const apiKeys = [...new Set((env.GEMINI_API_KEYS || env.GEMINI_API_KEY || '')
    .split(',').map(value => value.trim()).filter(Boolean))];
  if (apiKeys.length > 32 || apiKeys.some(value => value.length > 256 || /\s/.test(value))) {
    throw new Error('Use at most 32 comma-separated Gemini keys without embedded whitespace');
  }
  const cooldown = Number(env.GEMINI_KEY_COOLDOWN_SECONDS || 60);
  if (!Number.isInteger(cooldown) || cooldown < 15 || cooldown > 3600) {
    throw new Error('GEMINI_KEY_COOLDOWN_SECONDS must be 15-3600');
  }
  const adminToken = env.ADMIN_TOKEN || '';
  if (adminToken && !/^[A-Za-z0-9_-]{32,128}$/.test(adminToken)) throw new Error('ADMIN_TOKEN must be 32-128 URL-safe characters');
  const encryptionKey = env.SETTINGS_ENCRYPTION_KEY || '';
  if (encryptionKey && !/^[a-fA-F0-9]{64}$/.test(encryptionKey)) throw new Error('SETTINGS_ENCRYPTION_KEY must be 64 hexadecimal characters');
  return {
    base: base.replace(/\/$/, ''), token,
    apiKeys,
    keyCooldownMs: cooldown * 1000,
    adminToken, encryptionKey, databaseUrl: env.DATABASE_URL || '',
    model: env.GEMINI_LIVE_MODEL || 'gemini-3.8-live',
    voice: env.GEMINI_VOICE || 'Kore',
    instructions: env.ASSISTANT_INSTRUCTIONS ||
      "You are Shreeharsh Assistant. Be friendly and concise. Answer in the user's language. Keep spoken replies short.",
    seconds,
  };
}

export function authorized(header, token) {
  if (!token || typeof header !== 'string' || !header.startsWith('Bearer ')) return false;
  const digest = value => createHash('sha256').update(value).digest();
  return timingSafeEqual(digest(header.slice(7)), digest(token));
}

export function discovery(config) {
  return {
    firmware: { version: '2.5.1', url: '' },
    websocket: { url: config.base.replace(/^https:/, 'wss:') + '/ws', token: config.token, version: 1 },
    server_time: { timestamp: Date.now(), timezone_offset: 0 },
  };
}
