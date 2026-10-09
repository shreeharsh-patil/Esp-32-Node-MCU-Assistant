import { once } from 'node:events';
import WebSocket from 'ws';
import { readConfig } from '../src/config.js';

const config = readConfig();
if (!config.base || !config.token) throw new Error('Set PUBLIC_BASE_URL and DEVICE_TOKEN privately in .env or .env.local');
const headers = { Authorization: `Bearer ${config.token}` };
const response = await fetch(config.base + '/ota/', { method: 'POST', headers, signal: AbortSignal.timeout(15000) });
if (!response.ok) throw new Error(`Discovery returned HTTP ${response.status}; check production settings, token and deployment protection`);
const discovery = await response.json();
if (discovery.websocket?.url !== config.base.replace('https:', 'wss:') + '/ws') throw new Error('Discovery returned an unexpected WebSocket address');
console.log('Authenticated discovery: PASS');
const ws = new WebSocket(discovery.websocket.url, { headers, handshakeTimeout: 15000, maxPayload: 16384 });
const timer = setTimeout(() => ws.terminate(), 20000);
let valid = false;
let failure = 'Provider hello was not received; check Live model/key/quota and runtime logs';
ws.on('message', (data, binary) => {
  if (binary) return;
  let message;
  try { message = JSON.parse(data.toString()); } catch { failure = 'Invalid server response'; ws.close(); return; }
  if (message.type !== 'hello') return;
  valid = message.transport === 'websocket' && message.audio_params?.format === 'opus' &&
    message.audio_params?.sample_rate === 24000 && message.audio_params?.frame_duration === 60 && message.audio_params?.channels === 1;
  ws.close(1000, 'Deployment check complete');
});
try {
  await once(ws, 'open');
  const closed = once(ws, 'close');
  ws.send(JSON.stringify({ type: 'hello', version: 1, transport: 'websocket',
    audio_params: { format: 'opus', sample_rate: 16000, channels: 1, frame_duration: 60 } }));
  await closed;
  if (!valid) throw new Error(failure);
  console.log('Real provider session and audio negotiation: PASS');
  console.log('Physical speech recognition and audible replies still require the ESP32 test.');
} finally { clearTimeout(timer); ws.terminate(); }
