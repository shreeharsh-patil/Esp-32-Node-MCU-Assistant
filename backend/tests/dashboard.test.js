import test from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { setTimeout as sleep } from 'node:timers/promises';
import WebSocket from 'ws';
import { createBackend } from '../src/server.js';
import { DatabaseStore, encrypt, decrypt } from '../src/store.js';
import { ConversationJournal } from '../src/history.js';
import { readConfig } from '../src/config.js';
import { databaseFixture, testConfig, profileBody } from './database-fixture.js';

async function until(condition, timeout = 3000) {
  const start = Date.now();
  while (!await condition()) { if (Date.now() - start > timeout) throw new Error('Timed out'); await sleep(10); }
}
async function httpFixture(t, withStore = true) {
  const fixture = databaseFixture(); let content; let providerConfig; let opens = 0;
  const backend = createBackend(fixture.config, async (config, onContent) => {
    opens++; providerConfig = config; content = onContent;
    return { sendAudio() {}, finishInput() {}, close() {} };
  }, withStore ? { store: fixture.store } : {});
  backend.server.listen(0, '127.0.0.1'); await once(backend.server, 'listening');
  const origin = `http://127.0.0.1:${backend.server.address().port}`;
  t.after(async () => {
    for (const ws of backend.wss.clients) ws.terminate();
    await new Promise(resolve => backend.wss.close(resolve));
    await new Promise(resolve => backend.server.close(resolve));
  });
  let cookie;
  const request = (path, method = 'GET', body, extra = {}) => fetch(origin + path, { method,
    headers: { Origin: origin, 'Content-Type': 'application/json', ...(cookie ? { Cookie: cookie } : {}), ...extra },
    ...(body === undefined ? {} : { body: JSON.stringify(body) }) });
  const login = async () => {
    const response = await request('/api/admin/login', 'POST', { token: fixture.config.adminToken });
    assert.equal(response.status, 200); cookie = response.headers.get('set-cookie').split(';')[0];
    return response;
  };
  return { ...fixture, backend, origin, request, login, content: value => content(value),
    get providerConfig() { return providerConfig; }, get opens() { return opens; } };
}

test('settings are encrypted at rest, survive a new instance and reject stale saves', async () => {
  const { store, query, config } = databaseFixture();
  const profile = await store.profile();
  const runtime = await store.runtime(); assert.equal(await store.runtime(), runtime);
  const updated = await store.saveProfile(profileBody(profile, { name: 'My companion', voice: 'Puck',
    instructions: 'Answer briefly in Marathi.' }));
  assert.equal(updated.name, 'My companion'); assert.equal(updated.keys.length, 2);
  assert.equal(JSON.stringify(updated).includes(config.apiKeys[0]), false);
  const [raw] = await query('SELECT encrypted FROM assistant_config');
  assert.equal(Object.hasOwn(decrypt(raw.encrypted, config.encryptionKey), 'apiKeys'), false);
  assert.equal(raw.encrypted.includes('Answer briefly'), false);
  const restarted = new DatabaseStore({ ...config, apiKeys: ['changed-environment'] }, query);
  const loaded = await restarted.runtime();
  assert.equal(loaded.voice, 'Puck'); assert.deepEqual(loaded.apiKeys, ['changed-environment']);
  assert.notEqual(await store.runtime(), runtime);
  await assert.rejects(store.saveProfile(profileBody(profile)), error => error.status === 409);
  await assert.rejects(new DatabaseStore({ ...config, encryptionKey: 'f'.repeat(64) }, query).profile());
});

test('dashboard rejects credential edits and validates fields, encryption settings and device IDs', async () => {
  const { store } = databaseFixture(); const profile = await store.profile();
  for (const changes of [{ addKeys: 'secret invalid' }, { addKeys: Array.from({ length: 33 }, (_, i) => 'key' + i).join(',') },
    { voice: '' }, { name: 'x'.repeat(81) }, { instructions: 'x'.repeat(6001) }, { historyEnabled: 'yes' },
    { keyCooldownSeconds: 1 }, { removeKeys: [] }, { apiKeys: ['injected-key'] }]) {
    await assert.rejects(store.saveProfile(profileBody(profile, changes)), error => error.status === 400 && !error.publicMessage.includes('secret invalid'));
  }
  await assert.rejects(store.addDevice('../bad', 'Device'), error => error.status === 400);
  assert.throws(() => readConfig({ ADMIN_TOKEN: 'short' }), /ADMIN_TOKEN/);
  assert.throws(() => readConfig({ SETTINGS_ENCRYPTION_KEY: 'invalid' }), /SETTINGS_ENCRYPTION_KEY/);
  await assert.rejects(new DatabaseStore(testConfig).profile(), error => error.status === 503);
});

test('legacy stored keys are removed and cannot override changed or empty environment keys', async () => {
  const { store, query, config } = databaseFixture();
  const profile = await store.profile();
  await query('UPDATE assistant_config SET encrypted = $1 WHERE id = 1',
    [encrypt({ ...profile, apiKeys: ['legacy-provider-key'], voice: 'Puck' }, config.encryptionKey)]);
  const redeployed = new DatabaseStore({ ...config, apiKeys: ['environment-key'] }, query);
  assert.deepEqual((await redeployed.runtime()).apiKeys, ['environment-key']);
  assert.equal((await redeployed.profile()).voice, 'Puck');
  const [row] = await query('SELECT encrypted FROM assistant_config WHERE id = 1');
  assert.equal(Object.hasOwn(decrypt(row.encrypted, config.encryptionKey), 'apiKeys'), false);
  assert.deepEqual((await new DatabaseStore({ ...config, apiKeys: [] }, query).runtime()).apiKeys, []);
});

test('device tokens are hashed, isolated, revocable and displayed only on creation or rotation', async () => {
  const { store, config } = databaseFixture();
  assert.equal(await store.authenticate('unknown', 'Bearer ' + 'x'.repeat(64)), null);
  assert.ok(await store.authenticate('aa:bb:cc:dd:ee:ff', 'Bearer ' + config.token));
  const created = await store.addDevice('desk', 'Desk assistant');
  assert.equal(created.token.length, 64);
  assert.notEqual((await store.device('desk')).token_hash, created.token);
  assert.ok(await store.authenticate('desk', 'Bearer ' + created.token));
  assert.equal(await store.authenticate('desk', 'Bearer ' + config.token), null);
  assert.equal(JSON.stringify(await store.devices()).includes(created.token), false);
  await assert.rejects(store.addDevice('desk', 'Duplicate'), error => error.status === 409);
  await store.updateDevice('desk', { name: 'Desk', enabled: false });
  assert.equal(await store.authenticate('desk', 'Bearer ' + created.token), null);
  await store.updateDevice('desk', { name: 'Desk', enabled: true });
  const rotated = await store.rotateToken('desk');
  assert.equal(await store.authenticate('desk', 'Bearer ' + created.token), null);
  assert.ok(await store.authenticate('desk', 'Bearer ' + rotated.token));
});

test('transcripts persist encrypted, filter by device and clear independently from session metrics', async () => {
  const { store, query } = databaseFixture();
  await store.addDevice('desk', 'Desk'); await store.addDevice('bed', 'Bed');
  await store.startSession('s1', 'desk', 'test-model'); await store.startSession('s2', 'bed', 'test-model');
  await store.saveTurn('s1', 'desk', 'A private question', 'A private reply');
  await store.saveTurn('s2', 'bed', 'Different device', '');
  const raw = await query('SELECT encrypted FROM assistant_messages');
  assert.equal(JSON.stringify(raw).includes('private'), false);
  assert.equal((await store.conversations('desk')).length, 2);
  assert.ok((await store.conversations()).some(message => message.text === 'A private reply'));
  await store.endSession('s1');
  const summary = await store.summary(); assert.equal(summary.sessions, 2); assert.equal(summary.devices, 2);
  assert.ok(summary.recentSessions.find(session => session.id === 's1').ended_at);
  await store.deleteHistory('desk'); assert.equal((await store.conversations()).length, 1);
  await store.deleteHistory(); assert.equal((await store.conversations()).length, 0);
  assert.equal((await store.summary()).sessions, 2);
});

test('history preference and queue bounds prevent unbounded writes without blocking audio', async () => {
  const writes = []; let release;
  const blocked = new Promise(resolve => { release = resolve; });
  const store = { async saveTurn(...args) { writes.push(args); await blocked; } };
  const disabled = new ConversationJournal(store, 's', 'd', false);
  disabled.observe({ inputTranscription: { text: 'ignored' }, turnComplete: true }); await disabled.finish();
  assert.equal(writes.length, 0);
  const enabled = new ConversationJournal(store, 's', 'd', true);
  for (let i = 0; i < 25; i++) enabled.observe({ inputTranscription: { text: 'x'.repeat(7000) }, turnComplete: true });
  assert.equal(enabled.pending, 20); assert.equal(enabled.failed, true);
  release(); await enabled.finish(); assert.equal(writes.length, 20); assert.equal(writes[0][2].length, 6000);
});

test('admin authentication uses a private cookie, rejects forged cookies and cross-origin mutations', async t => {
  const f = await httpFixture(t);
  assert.equal((await f.request('/api/admin/state')).status, 401);
  assert.equal((await f.request('/api/admin/login', 'POST', { token: f.config.token })).status, 401);
  assert.equal((await f.request('/api/admin/login', 'POST', { token: f.config.adminToken }, { Origin: 'https://evil.example' })).status, 403);
  assert.equal((await f.request('/api/admin/state', 'GET', undefined, { Cookie: 'assistant_admin=forged.cookie' })).status, 401);
  const login = await f.login();
  assert.match(login.headers.get('set-cookie'), /HttpOnly; SameSite=Strict/);
  assert.equal(login.headers.get('set-cookie').includes(f.config.adminToken), false);
  const result = await f.request('/api/admin/state'); const body = await result.json();
  assert.equal(result.headers.get('cache-control'), 'no-store'); assert.equal(body.profile.keys.length, 2);
  for (const secret of [...f.config.apiKeys, f.config.adminToken, f.config.encryptionKey, f.config.token]) assert.equal(JSON.stringify(body).includes(secret), false);
  assert.equal((await f.request('/api/admin/profile', 'POST', profileBody(await f.store.profile()), { Origin: 'https://evil.example' })).status, 403);
  assert.equal((await f.request('/api/admin/logout', 'POST', {})).status, 200);
  // A browser clears the expired cookie; signed cookies are stateless until expiry or ADMIN_TOKEN rotation.
  const anonymous = await fetch(f.origin + '/api/admin/state'); assert.equal(anonymous.status, 401);
});

test('owner HTTP flow updates settings, manages tokens, filters history and serves dashboard assets', async t => {
  const f = await httpFixture(t); await f.login();
  const index = await fetch(f.origin); assert.equal(index.status, 200);
  assert.match(index.headers.get('content-security-policy'), /frame-ancestors 'none'/);
  assert.match(await index.text(), /Your companion, connected/);
  for (const asset of ['/dashboard.css', '/dashboard.js']) assert.equal((await fetch(f.origin + asset)).status, 200);
  const profile = (await (await f.request('/api/admin/state')).json()).profile;
  const saved = await f.request('/api/admin/profile', 'POST', profileBody(profile, { voice: 'Aoede' }));
  assert.equal(saved.status, 200); assert.equal((await saved.json()).keys.length, 2);
  assert.equal((await f.request('/api/admin/profile', 'POST', profileBody(profile))).status, 409);
  for (const changes of [{ addKeys: 'new-key' }, { removeKeys: [] }, { apiKeys: ['new-key'] }]) {
    const invalid = await f.request('/api/admin/profile', 'POST', profileBody(await f.store.profile(), changes));
    assert.equal(invalid.status, 400); assert.match((await invalid.json()).error, /Vercel environment/);
  }
  const created = await f.request('/api/admin/devices', 'POST', { id: 'desk', name: 'Desk' });
  assert.equal(created.status, 201); const device = await created.json();
  assert.ok(device.token);
  const discovery = await f.request('/ota/', 'POST', {}, { Authorization: 'Bearer ' + device.token, 'Device-Id': 'desk' });
  assert.equal(discovery.status, 200); assert.equal((await discovery.json()).websocket.token, device.token);
  await f.store.startSession('session', 'desk', 'model'); await f.store.saveTurn('session', 'desk', 'Question', 'Answer');
  assert.equal((await (await f.request('/api/admin/conversations?device=desk')).json()).length, 2);
  assert.equal((await f.request('/api/admin/conversations?device=desk', 'DELETE')).status, 200);
  assert.equal((await f.store.conversations()).length, 0);
  const rotated = await (await f.request('/api/admin/devices/desk/token', 'POST', {})).json();
  assert.notEqual(rotated.token, device.token);
  await f.request('/api/admin/devices/desk', 'POST', { name: 'Renamed', enabled: false });
  assert.equal((await f.request('/ota/', 'POST', {}, { Authorization: 'Bearer ' + rotated.token, 'Device-Id': 'desk' })).status, 401);
});

test('missing database reports setup requirements instead of simulating persistent settings', async t => {
  const f = await httpFixture(t, false); await f.login();
  const response = await f.request('/api/admin/state'); assert.equal(response.status, 503);
  assert.match((await response.json()).error, /Neon database/);
});

test('saved voice settings reach a real WebSocket session and transcripts reach persistent history', async t => {
  const f = await httpFixture(t); const device = await f.store.addDevice('desk', 'Desk');
  await f.store.saveProfile(profileBody(await f.store.profile(), { voice: 'Puck', instructions: 'Use short replies.' }));
  const ws = new WebSocket(f.origin.replace('http:', 'ws:') + '/ws', { headers: { Authorization: 'Bearer ' + device.token, 'Device-Id': 'desk' } });
  t.after(() => ws.terminate()); const messages = [];
  ws.on('message', (data, binary) => { if (!binary) messages.push(JSON.parse(data.toString())); });
  await once(ws, 'open');
  ws.send(JSON.stringify({ type: 'hello', version: 1, transport: 'websocket', audio_params: { format: 'opus', sample_rate: 16000, channels: 1, frame_duration: 60 } }));
  await until(() => messages.some(message => message.type === 'hello'));
  assert.equal(f.providerConfig.voice, 'Puck'); assert.equal(f.providerConfig.instructions, 'Use short replies.');
  f.content({ inputTranscription: { text: 'Can you ' } });
  f.content({ inputTranscription: { text: 'hear me?' }, outputTranscription: { text: 'Yes.' }, turnComplete: true });
  await until(async () => (await f.store.conversations('desk')).length === 2);
  assert.ok((await f.store.conversations('desk')).some(message => message.text === 'Can you hear me?'));
  ws.close(); await once(ws, 'close');
  await until(async () => (await f.store.summary()).recentSessions[0].state === 'ended');
  assert.equal(f.opens, 1);
});

test('changing device access closes active sessions at the next authorization heartbeat', async t => {
  const f = await httpFixture(t); const device = await f.store.addDevice('desk', 'Desk');
  const ws = new WebSocket(f.origin.replace('http:', 'ws:') + '/ws', { headers: { Authorization: 'Bearer ' + device.token, 'Device-Id': 'desk' } });
  t.after(() => ws.terminate()); const messages = [];
  ws.on('message', (data, binary) => { if (!binary) messages.push(JSON.parse(data.toString())); });
  await once(ws, 'open');
  ws.send(JSON.stringify({ type: 'hello', version: 1, transport: 'websocket', audio_params: { format: 'opus', sample_rate: 16000, channels: 1, frame_duration: 60 } }));
  await until(() => messages.some(message => message.type === 'hello'));
  const closing = once(ws, 'close'); await f.store.rotateToken('desk');
  const [code] = await closing; assert.equal(code, 1008);
  const rejected = new WebSocket(f.origin.replace('http:', 'ws:') + '/ws', { headers: { Authorization: 'Bearer ' + device.token, 'Device-Id': 'desk' } });
  const [error] = await once(rejected, 'error'); assert.match(error.message, /401/);
  assert.equal(f.opens, 1);
});
