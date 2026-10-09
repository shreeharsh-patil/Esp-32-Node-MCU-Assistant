import test from 'node:test';
import assert from 'node:assert/strict';
import { readConfig } from '../src/config.js';
import { nextGeminiKey, markRateLimited, isRateLimited, openGemini } from '../src/gemini.js';

test('comma-separated Gemini keys are trimmed, deduplicated and backwards compatible', () => {
  assert.deepEqual(readConfig({ GEMINI_API_KEYS: ' key-one, key-two, key-one, ,key-three ' }).apiKeys,
    ['key-one', 'key-two', 'key-three']);
  assert.deepEqual(readConfig({ GEMINI_API_KEY: 'legacy-one,legacy-two' }).apiKeys, ['legacy-one', 'legacy-two']);
  assert.deepEqual(readConfig({ GEMINI_API_KEY: 'old', GEMINI_API_KEYS: 'new-one,new-two' }).apiKeys, ['new-one', 'new-two']);
  assert.throws(() => readConfig({ GEMINI_API_KEYS: Array.from({ length: 33 }, (_, i) => `key-${i}`).join(',') }), /at most 32/);
  assert.throws(() => readConfig({ GEMINI_API_KEYS: 'key with spaces' }), /whitespace/);
  assert.deepEqual(readConfig({}).apiKeys, []);
});

test('rate-limited keys are skipped until cooldown and all-exhausted pools fail without requests', () => {
  const config = readConfig({ GEMINI_API_KEYS: 'one,two', GEMINI_KEY_COOLDOWN_SECONDS: '15' });
  const first = nextGeminiKey(config, 1000);
  markRateLimited(config, first.slot, 1000);
  const second = nextGeminiKey(config, 1001);
  assert.notEqual(first.apiKey, second.apiKey);
  markRateLimited(config, second.slot, 1001);
  assert.throws(() => nextGeminiKey(config, 1002), /cooling down/);
  assert.equal(nextGeminiKey(config, 16000).apiKey, first.apiKey);
  assert.equal(isRateLimited({ status: 429 }), true);
  assert.equal(isRateLimited({ reason: 'RESOURCE_EXHAUSTED' }), true);
  assert.equal(isRateLimited({ message: 'You exceeded your current quota' }), true);
  assert.equal(isRateLimited({ reason: 'Resource has been exhausted (e.g. check quota).' }), true);
  assert.equal(isRateLimited({ status: 403, message: 'Invalid API key' }), false);
});

test('setup rate limits try the next key even when the SDK reports close without rejecting connect', async () => {
  const config = readConfig({ GEMINI_API_KEYS: 'one,two' });
  const attempted = [];
  let closed = 0;
  const adapter = await openGemini(config, () => {}, () => assert.fail('Setup failover must not close the device'), apiKey => ({
    live: { connect(options) {
      attempted.push(apiKey);
      if (attempted.length === 1) {
        queueMicrotask(() => options.callbacks.onclose({ reason: 'Rate limit exceeded' }));
        return new Promise(() => {});
      }
      return Promise.resolve({ close: () => closed++, sendRealtimeInput() {} });
    } },
  }));
  assert.equal(attempted.length, 2);
  assert.notEqual(attempted[0], attempted[1]);
  adapter.close();
  assert.equal(closed, 1);
});

test('active 429 triggers one reconnect and the next session avoids the exhausted key', async () => {
  const config = readConfig({ GEMINI_API_KEYS: 'one,two' });
  const clients = [];
  let failures = 0;
  const factory = apiKey => ({ live: { connect: async options => {
    clients.push({ apiKey, callbacks: options.callbacks });
    return { close() {}, sendRealtimeInput() {} };
  } } });
  const current = await openGemini(config, () => {}, () => failures++, factory);
  clients[0].callbacks.onerror({ message: 'Unexpected server response: 429' });
  clients[0].callbacks.onclose({ reason: 'Quota exceeded' });
  const next = await openGemini(config, () => {}, () => failures++, factory);
  assert.equal(failures, 1);
  assert.notEqual(clients[0].apiKey, clients[1].apiKey);
  current.close(); next.close();
});

test('all rate-limited keys stop after one pass and no requests are made during cooldown', async () => {
  const config = readConfig({ GEMINI_API_KEYS: 'one,two' });
  let attempts = 0;
  const factory = () => ({ live: { connect: async () => {
    attempts++;
    throw Object.assign(new Error('RESOURCE_EXHAUSTED'), { status: 429 });
  } } });
  await assert.rejects(() => openGemini(config, () => {}, () => {}, factory), /rate limited/);
  assert.equal(attempts, 2);
  await assert.rejects(() => openGemini(config, () => {}, () => {}, factory), /cooling down/);
  assert.equal(attempts, 2);
});

test('cancelled provider opening is bounded and closes a late SDK session', async () => {
  const config = readConfig({ GEMINI_API_KEYS: 'one' });
  const abort = new AbortController();
  let resolve;
  let closed = 0;
  const opening = openGemini(config, () => {}, () => {}, { signal: abort.signal,
    createClient: () => ({ live: { connect: () => new Promise(done => { resolve = done; }) } }) });
  abort.abort();
  await assert.rejects(opening, /connection failed/);
  resolve({ close: () => closed++ });
  await new Promise(done => setImmediate(done));
  assert.equal(closed, 1);
});

test('each configured key is used once per cycle and independent pools do not share counters', () => {
  const config = readConfig({ GEMINI_API_KEYS: 'one,two,three' });
  const cycle = Array.from({ length: 3 }, () => nextGeminiKey(config));
  assert.deepEqual(new Set(cycle.map(value => value.apiKey)), new Set(config.apiKeys));
  const again = Array.from({ length: 3 }, () => nextGeminiKey(config));
  assert.deepEqual(again, cycle);
  assert.equal(nextGeminiKey(readConfig({ GEMINI_API_KEYS: 'only' })).apiKey, 'only');
  assert.throws(() => nextGeminiKey(readConfig({})), /No Gemini key/);
});

test('Live connections use different keys, retain one key per session, and cleanup is idempotent', async () => {
  const config = readConfig({ GEMINI_API_KEYS: 'one,two,three' });
  const clients = [];
  const factory = apiKey => {
    const record = { apiKey, sent: [], closes: 0 };
    clients.push(record);
    return { live: { async connect(options) {
      assert.equal(options.config.realtimeInputConfig.automaticActivityDetection.disabled, false);
      return { sendRealtimeInput: message => record.sent.push(message), close: () => record.closes++ };
    } } };
  };
  const a = await openGemini(config, () => {}, () => {}, factory);
  a.finishInput(); // No spurious stream-end before capture.
  a.sendAudio(Buffer.alloc(1920));
  a.sendAudio(Buffer.alloc(1920));
  a.finishInput();
  a.finishInput();
  const b = await openGemini(config, () => {}, () => {}, factory);
  assert.notEqual(clients[0].apiKey, clients[1].apiKey);
  assert.equal(clients[0].sent.length, 3);
  assert.equal(clients[0].sent[0].audio.mimeType, 'audio/pcm;rate=16000');
  assert.deepEqual(clients[0].sent[2], { audioStreamEnd: true });
  a.close(); a.close(); a.sendAudio(Buffer.alloc(1920));
  assert.equal(clients[0].closes, 1);
  assert.equal(clients[0].sent.length, 3);
  b.close();
  let rejectedKey;
  await assert.rejects(() => openGemini(config, () => {}, () => {}, apiKey => {
    rejectedKey = apiKey;
    return { live: { connect: async () => { throw new Error('Provider rejected test key'); } } };
  }), /rejected test key/);
  const retry = await openGemini(config, () => {}, () => {}, factory);
  assert.notEqual(clients.at(-1).apiKey, rejectedKey);
  retry.close();
});
