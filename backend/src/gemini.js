import { GoogleGenAI, Modality, ActivityHandling } from '@google/genai';
import { randomInt, createHash } from 'node:crypto';

const pools = new WeakMap();
export function keyPoolStatus(config) {
  return config.apiKeys.map((key, index) => ({
    id: createHash('sha256').update(key).digest('hex').slice(0, 16),
    cooldownSeconds: Math.max(0, Math.ceil(((pools.get(config)?.cooldowns[index] || 0) - Date.now()) / 1000)),
  }));
}
function pool(config) {
  if (!pools.has(config)) pools.set(config, { cursor: randomInt(config.apiKeys.length),
    cooldowns: config.apiKeys.map(() => 0) });
  return pools.get(config);
}
export function isRateLimited(error) {
  return [error?.status, error?.code, error?.error?.code].some(value => Number(value) === 429 || value === 'RESOURCE_EXHAUSTED') ||
    /RESOURCE_EXHAUSTED|resource[^.]{0,80}exhaust|quota[^.]{0,80}(?:exceed|exhaust|limit)|(?:exceeded|exhausted)[^.]{0,80}quota|rate.?limit|\b429\b/i.test(
      String(error?.message || error?.reason || '').slice(0, 2048));
}
export function markRateLimited(config, slot, now = Date.now()) {
  pool(config).cooldowns[slot - 1] = now + config.keyCooldownMs;
}
export function nextGeminiKey(config, now = Date.now()) {
  const keys = config.apiKeys;
  if (!keys?.length) throw new Error('No Gemini key configured');
  const state = pool(config);
  for (let offset = 0; offset < keys.length; offset++) {
    const index = (state.cursor + offset) % keys.length;
    if (state.cooldowns[index] > now) continue;
    state.cursor = (index + 1) % keys.length;
    return { apiKey: keys[index], slot: index + 1, total: keys.length };
  }
  throw Object.assign(new Error('All Gemini keys are cooling down'), { status: 429 });
}

// Provider boundary: firmware and transport do not know or receive the API key.
export async function openGemini(config, onMessage, onFailure,
    options = {}) {
  const createClient = typeof options === 'function' ? options :
    options.createClient || (apiKey => new GoogleGenAI({ apiKey }));
  for (let attempt = 0; attempt < config.apiKeys.length; attempt++) {
    if (options.signal?.aborted) throw new Error('Voice connection cancelled');
    const key = nextGeminiKey(config);
    try { return await connectKey(config, key, onMessage, onFailure, createClient, options.signal); }
    catch (error) { if (!isRateLimited(error)) throw error; }
  }
  throw Object.assign(new Error('Gemini key pool is rate limited'), { status: 429 });
}

async function connectKey(config, key, onMessage, onFailure, createClient, signal) {
  console.info(`Opening Gemini Live with key slot ${key.slot}/${key.total}`);
  const ai = createClient(key.apiKey);
  let active = false;
  let failed = false;
  let closed = false;
  let rejectOpening;
  const openingFailure = new Promise((_resolve, reject) => { rejectOpening = reject; });
  const fail = error => {
    if (closed || failed) return;
    failed = true;
    const limited = isRateLimited(error);
    if (limited) {
      markRateLimited(config, key.slot);
      console.info(`Gemini key slot ${key.slot}/${key.total} rate limited; cooling down`);
    }
    if (active) onFailure();
    else rejectOpening(Object.assign(new Error(limited ? 'Gemini rate limited' : 'Gemini connection failed'),
      { status: limited ? 429 : 503 }));
  };
  const cancelled = () => fail({});
  signal?.addEventListener('abort', cancelled, { once: true });
  const connecting = ai.live.connect({
    model: config.model,
    config: {
      responseModalities: [Modality.AUDIO],
      systemInstruction: config.instructions,
      speechConfig: { voiceConfig: { prebuiltVoiceConfig: { voiceName: config.voice } } },
      inputAudioTranscription: {},
      outputAudioTranscription: {},
      realtimeInputConfig: {
        automaticActivityDetection: { disabled: false, prefixPaddingMs: 300, silenceDurationMs: 1000 },
        activityHandling: ActivityHandling.NO_INTERRUPTION,
      },
    },
    callbacks: {
      onmessage: message => { if (!failed && !closed) onMessage(message.serverContent); },
      onerror: fail,
      onclose: fail,
    },
  });
  // The SDK can leave connect() pending after onclose before setupComplete.
  // Race those callbacks, and close any session that resolves after cancellation.
  connecting.then(value => { if (failed || closed) { try { value.close(); } catch {} } }, () => {});
  let session;
  try { session = await Promise.race([connecting, openingFailure]); }
  catch (error) {
    closed = true;
    if (!failed && isRateLimited(error)) markRateLimited(config, key.slot);
    throw error;
  } finally { signal?.removeEventListener('abort', cancelled); }
  if (failed) { session.close(); throw new Error('Gemini connection failed'); }
  active = true;
  let streaming = false;
  return {
    sendAudio(pcm) {
      if (closed || failed) return;
      streaming = true;
      session.sendRealtimeInput({ audio: { data: pcm.toString('base64'), mimeType: 'audio/pcm;rate=16000' } });
    },
    finishInput() {
      if (!streaming || closed || failed) return;
      streaming = false;
      session.sendRealtimeInput({ audioStreamEnd: true });
    },
    close() { if (!closed) { closed = true; session.close(); } },
  };
}
