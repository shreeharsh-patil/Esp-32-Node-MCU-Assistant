import test from 'node:test';
import assert from 'node:assert/strict';
import { currentClockTime, clockInstructions, CLOCK_TIMEZONES } from '../src/clock.js';
import { VoiceBridge, validHello } from '../src/bridge.js';

test('current clock uses the device timezone, 12h midnight and real weekday', () => {
  const time = currentClockTime('Asia/Kolkata', new Date('2026-10-12T16:58:00Z'));
  assert.equal(time.spoken, '10:28 PM');
  assert.match(time.local, /Monday/);
  const midnight = currentClockTime('Asia/Kolkata', new Date('2026-10-12T18:30:00Z'));
  assert.equal(midnight.spoken, '12:00 AM');
  assert.match(midnight.local, /Tuesday, 13 October/);
  assert.equal(currentClockTime('bad-zone').timezone, 'Asia/Kolkata');
  for (const timezone of CLOCK_TIMEZONES) assert.equal(currentClockTime(timezone).timezone, timezone);
  assert.equal(currentClockTime('America/New_York', new Date('2026-07-01T16:00:00Z')).spoken, '12:00 PM');
  assert.equal(currentClockTime('America/New_York', new Date('2026-01-01T16:00:00Z')).spoken, '11:00 AM');
  assert.match(clockInstructions('Europe/London'), /Europe\/London/);
  assert.match(clockInstructions(), /always call get_current_time/);
});
test('optional clock zone keeps legacy hello valid and rejects invalid metadata', () => {
  const hello = { type: 'hello', version: 1, transport: 'websocket', audio_params: {
    format: 'opus', sample_rate: 16000, channels: 1, frame_duration: 60 } };
  assert.ok(validHello(hello));
  assert.ok(validHello({ ...hello, timezone: 'Asia/Kolkata' }));
  assert.equal(validHello({ ...hello, timezone: ['Asia/Kolkata'] }), false);
  assert.equal(validHello({ ...hello, timezone: 'invalid' }), false);
});
test('fragmented user clock command is assembled and reset before the next turn', async () => {
  const messages = [];
  const bridge = new VoiceBridge({ readyState: 1, send: value => messages.push(JSON.parse(value)), close() {} });
  bridge.enqueue({ inputTranscription: { text: 'Show me' } });
  bridge.enqueue({ inputTranscription: { text: ' the time.' }, turnComplete: true });
  bridge.enqueue({ inputTranscription: { text: 'What is the weather?' }, turnComplete: true });
  for (let i = 0; i < 50 && messages.length < 3; i++) await new Promise(done => setTimeout(done, 5));
  assert.deepEqual(messages.filter(value => value.type === 'stt').map(value => value.text),
    ['Show me', 'Show me the time.', 'What is the weather?']);
  bridge.destroy();
  await bridge.worker;
});
