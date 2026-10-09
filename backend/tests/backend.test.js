import test from 'node:test';
import assert from 'node:assert/strict';
import { once } from 'node:events';
import { setTimeout as sleep } from 'node:timers/promises';
import WebSocket from 'ws';
import OpusScript from 'opusscript';
import { createBackend } from '../src/server.js';
import { readConfig, authorized } from '../src/config.js';
import { VoiceBridge } from '../src/bridge.js';

// Provider doubles are restricted to tests. Production always uses Google Live.
const config = readConfig({ PUBLIC_BASE_URL: 'https://assistant.example.com',
  DEVICE_TOKEN: 'a'.repeat(64), GEMINI_API_KEY: 'test-key-do-not-use' });
const hello = { type: 'hello', version: 1, transport: 'websocket',
  audio_params: { format: 'opus', sample_rate: 16000, channels: 1, frame_duration: 60 } };
const headers = { Authorization: `Bearer ${config.token}` };

function tone(samples, rate) {
  const pcm = Buffer.alloc(samples * 2);
  for (let i = 0; i < samples; i++) pcm.writeInt16LE(Math.round(4000 * Math.sin(2 * Math.PI * 440 * i / rate)), i * 2);
  return pcm;
}
async function until(condition, timeout = 3000) {
  const start = Date.now();
  while (!condition()) {
    if (Date.now() - start > timeout) throw new Error('Timed out waiting for transport');
    await sleep(10);
  }
}
async function fixture(t, options = {}) {
  let reply;
  let fail;
  let opened = 0;
  const capture = [];
  let finished = 0;
  let closed = 0;
  const provider = { sendAudio: pcm => capture.push(Buffer.from(pcm)),
    finishInput: () => finished++, close: () => closed++ };
  const backend = createBackend(options.config || config, async (_config, onMessage, onFailure) => {
    opened++; reply = onMessage; fail = onFailure; return provider;
  });
  backend.server.listen(0, '127.0.0.1');
  await once(backend.server, 'listening');
  const origin = `http://127.0.0.1:${backend.server.address().port}`;
  const clients = [];
  t.after(async () => {
    for (const ws of clients) ws.terminate();
    for (const ws of backend.wss.clients) ws.terminate();
    await new Promise(resolve => backend.wss.close(resolve));
    await new Promise(resolve => backend.server.close(resolve));
  });
  return { origin, capture, get opened() { return opened; }, get finished() { return finished; },
    get closed() { return closed; }, reply: content => reply(content), fail: () => fail(),
    async socket(auth = headers, greeting = hello) {
      const ws = new WebSocket(origin.replace('http:', 'ws:') + '/ws', { headers: auth });
      clients.push(ws);
      const messages = [];
      ws.on('message', (data, binary) => messages.push(binary ? Buffer.from(data) : JSON.parse(data.toString())));
      await once(ws, 'open');
      ws.send(JSON.stringify(greeting));
      if (greeting === hello) await until(() => messages.some(message => message.type === 'hello'));
      return { ws, messages };
    },
  };
}

test('configuration rejects insecure origins and invalid tokens without echoing secrets', () => {
  for (const base of ['http://assistant.example.com', 'https://user:pass@example.com', 'https://example.com/path', 'https://example.com?key=secret']) {
    assert.throws(() => readConfig({ PUBLIC_BASE_URL: base }), /HTTPS origin/);
  }
  assert.throws(() => readConfig({ DEVICE_TOKEN: 'secret' }), /32-128/);
  assert.throws(() => readConfig({ SESSION_SECONDS: '299' }), /30-270/);
  assert.equal(authorized(headers.Authorization, config.token), true);
  assert.equal(authorized('Bearer ' + 'b'.repeat(64), config.token), false);
  assert.equal(authorized(undefined, config.token), false);
  assert.equal(authorized('Bearer ', ''), false);
});

test('discovery requires authentication, supplies device WSS details, and omits the provider key', async t => {
  const f = await fixture(t);
  const rejected = await fetch(f.origin + '/ota/', { method: 'POST' });
  assert.equal(rejected.status, 401);
  const result = await fetch(f.origin + '/ota/', { method: 'POST', headers });
  assert.equal(result.status, 200);
  assert.equal(result.headers.get('cache-control'), 'no-store');
  const body = await result.json();
  assert.deepEqual(body.websocket, { url: 'wss://assistant.example.com/ws', token: config.token, version: 1 });
  assert.equal(body.firmware.url, '');
  for (const key of config.apiKeys) assert.equal(JSON.stringify(body).includes(key), false);
  assert.ok(Math.abs(body.server_time.timestamp - Date.now()) < 1000);
  assert.equal(f.opened, 0);
});

test('health exposes readiness only and missing keys fail closed', async t => {
  const f = await fixture(t, { config: { ...config, apiKeys: [] } });
  const result = await fetch(f.origin + '/health');
  assert.deepEqual(await result.json(), { status: 'ok', configured: false });
  const response = await fetch(f.origin + '/ota/', { method: 'POST', headers });
  assert.equal(response.status, 503);
  assert.equal(f.opened, 0);
});

test('oversized HTTP bodies receive a bounded generic error', async t => {
  const f = await fixture(t);
  const response = await fetch(f.origin + '/ota/', { method: 'POST',
    headers: { ...headers, 'Content-Type': 'application/json' }, body: JSON.stringify({ data: 'x'.repeat(10000) }) });
  assert.equal(response.status, 413);
  assert.deepEqual(await response.json(), { error: 'Invalid request' });
});

test('WebSocket rejects an unauthenticated client before opening the provider', async t => {
  const f = await fixture(t);
  const ws = new WebSocket(f.origin.replace('http:', 'ws:') + '/ws');
  const [error] = await once(ws, 'error');
  assert.match(error.message, /401/);
  assert.equal(f.opened, 0);
});

test('wrong audio format is rejected before provider activation', async t => {
  const f = await fixture(t);
  const { ws } = await f.socket(headers, { ...hello, audio_params: { ...hello.audio_params, channels: 2 } });
  const [code] = await once(ws, 'close');
  assert.equal(code, 1011);
  assert.equal(f.opened, 0);
});

test('real Opus capture and playback cross the bridge, stay half-duplex, and resume next turn', async t => {
  const f = await fixture(t);
  const { ws, messages } = await f.socket();
  const clientHello = messages[0];
  assert.equal(clientHello.audio_params.sample_rate, 24000);
  const microphone = new OpusScript(16000, 1, OpusScript.Application.VOIP);
  const speaker = new OpusScript(24000, 1, OpusScript.Application.VOIP);
  t.after(() => { microphone.delete(); speaker.delete(); });
  const input = microphone.encode(tone(960, 16000), 960);
  ws.send(input); // Audio before listen/start is ignored.
  ws.send(JSON.stringify({ type: 'listen', state: 'start', mode: 'auto' }));
  ws.send(input);
  await until(() => f.capture.length === 1);
  assert.equal(f.capture[0].length, 1920);
  assert.ok(f.capture[0].some(value => value !== 0));
  f.reply({ inputTranscription: { text: 'Hello' }, outputTranscription: { text: 'Hi Shreeharsh' },
    modelTurn: { parts: [{ inlineData: { mimeType: 'audio/pcm;rate=24000', data: tone(3500, 24000).toString('base64') } }] },
    turnComplete: true });
  await until(() => messages.some(message => message.type === 'tts' && message.state === 'start'));
  ws.send(JSON.stringify({ type: 'listen', state: 'start', mode: 'auto' }));
  ws.send(input); // No feedback capture while the assistant is speaking.
  await until(() => messages.some(message => message.type === 'tts' && message.state === 'stop'));
  assert.equal(f.capture.length, 1);
  const audio = messages.filter(Buffer.isBuffer);
  assert.equal(audio.length, 3); // Last partial 60ms frame is padded.
  for (const packet of audio) assert.equal(speaker.decode(packet).length, 2880);
  assert.equal(messages.at(-1).state, 'stop');
  assert.ok(messages.findIndex(message => message.type === 'tts' && message.state === 'start') < messages.findIndex(Buffer.isBuffer));
  assert.ok(messages.some(message => message.type === 'stt' && message.text === 'Hello'));
  assert.ok(f.finished >= 1);
  ws.send(JSON.stringify({ type: 'listen', state: 'start', mode: 'auto' }));
  ws.send(input);
  await until(() => f.capture.length === 2);
  ws.close();
  await once(ws, 'close');
  await until(() => f.closed === 1);
});

test('oversized WS packets close the socket instead of decoding unbounded input', async t => {
  const f = await fixture(t);
  const { ws } = await f.socket();
  ws.send(Buffer.alloc(20000));
  const [code] = await once(ws, 'close');
  assert.equal(code, 1009);
  assert.equal(f.capture.length, 0);
});

test('quiet sessions receive application heartbeats and provider failure releases the session', async t => {
  const f = await fixture(t);
  const { ws, messages } = await f.socket();
  await until(() => messages.some(message => message.type === 'ping'), 22000);
  assert.equal(f.capture.length, 0);
  const ended = once(ws, 'close');
  f.fail();
  const [code] = await ended;
  assert.equal(code, 1011);
  await until(() => f.closed === 1);
});

test('provider format, queue growth and text are bounded', async () => {
  const ws = { readyState: 1, bufferedAmount: 0, send() {}, close() {} };
  const bridge = new VoiceBridge(ws);
  assert.throws(() => bridge.enqueue({ modelTurn: { parts: [{ inlineData: { mimeType: 'audio/wav', data: 'AAAA' } }] } }), /format/);
  assert.throws(() => bridge.enqueue({ modelTurn: { parts: [{ inlineData: { mimeType: 'audio/pcm', data: 'AA==' } }] } }), /PCM/);
  assert.throws(() => bridge.enqueue({ outputTranscription: { text: 'x'.repeat(5000) } }), /text/);
  for (let i = 0; i < 64; i++) bridge.enqueue({ turnComplete: true });
  assert.throws(() => bridge.enqueue({ turnComplete: true }), /backpressure/);
  bridge.provider = { close() { throw new Error('Already closed'); } };
  bridge.destroy();
  bridge.destroy();
  await bridge.worker;
  bridge.handleControl({ type: 'listen', state: 'start' });
  bridge.handlePacket(Buffer.alloc(1)); // Late packets must never touch a freed codec.
  assert.equal(bridge.pendingBytes, 0);
});

test('malformed and excessive-rate microphone frames are rejected', async () => {
  const ws = { readyState: 1, bufferedAmount: 0, send() {}, close() {} };
  const bridge = new VoiceBridge(ws);
  const codec = new OpusScript(16000, 1, OpusScript.Application.VOIP);
  bridge.provider = { sendAudio() {}, close() {} };
  bridge.handleControl({ type: 'listen', state: 'start' });
  assert.throws(() => bridge.handlePacket(Buffer.alloc(0)), /size/);
  assert.throws(() => bridge.handlePacket(Buffer.alloc(3829)), /size/);
  assert.throws(() => bridge.handlePacket(codec.encode(tone(320, 16000), 320)), /60ms/);
  const packet = codec.encode(tone(960, 16000), 960);
  for (let i = 0; i < 49; i++) bridge.handlePacket(packet);
  assert.throws(() => bridge.handlePacket(packet), /rate limit/);
  bridge.destroy(); codec.delete();
  await bridge.worker;
});
