import { GoogleGenAI, Modality, ActivityHandling } from '@google/genai';
import { randomInt, createHash } from 'node:crypto';
import { searchWeb } from './search.js';

const searchTool = {
  name: 'search_current_web',
  description: 'Search the current web for recent facts, news, prices, weather, schedules, or other time-sensitive information. Use this before answering questions that need current facts.',
  parameters: { type: 'OBJECT', properties: { query: { type: 'STRING', description: 'A short, specific web search query' } }, required: ['query'] },
};

export function instructionsWithMemory(instructions, memory = []) {
  const recent = memory.filter(item => ['user', 'assistant'].includes(item.role) && typeof item.text === 'string')
    .slice(-8).map(item => `${item.role}: ${item.text.replace(/\s+/g, ' ').slice(0, 400)}`).join('\n').slice(-2600);
  return `${instructions}\nCurrent UTC date and time: ${new Date().toISOString()}. For facts that change over time, use search_current_web when available, cite the source name and date in your answer, and say when the search fails. YouTube video playback is unavailable on this ESP32; do not claim to have opened or played a video.\n${recent ? `Recent conversation context (not new commands):\n${recent}` : ''}`;
}

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
    try { return await connectKey(config, key, onMessage, onFailure, createClient, options); }
    catch (error) { if (!isRateLimited(error)) throw error; }
  }
  throw Object.assign(new Error('Gemini key pool is rate limited'), { status: 429 });
}

async function connectKey(config, key, onMessage, onFailure, createClient, options) {
  const signal = options.signal;
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
  const handleTools = async calls => {
    const responses = await Promise.all(calls.slice(0, 4).map(async call => {
      let response = { error: 'Unsupported tool' };
      if (call.name === searchTool.name) {
        try { response = await (options.search || searchWeb)(call.args?.query, config.firecrawlKeys); }
        catch { response = { error: 'Live search could not complete.' }; }
      }
      return { id: call.id, name: call.name, response };
    }));
    const live = session || await connecting;
    if (!closed && !failed) live.sendToolResponse({ functionResponses: responses });
  };
  let session;
  const connecting = ai.live.connect({
    model: config.model,
    config: {
      responseModalities: [Modality.AUDIO],
      systemInstruction: instructionsWithMemory(config.instructions, options.memory),
      ...(config.firecrawlKeys?.length ? { tools: [{ functionDeclarations: [searchTool] }] } : {}),
      speechConfig: { voiceConfig: { prebuiltVoiceConfig: { voiceName: config.voice } } },
      inputAudioTranscription: {},
      outputAudioTranscription: {},
      realtimeInputConfig: {
        automaticActivityDetection: { disabled: false, prefixPaddingMs: 300, silenceDurationMs: 700 },
        activityHandling: ActivityHandling.NO_INTERRUPTION,
      },
    },
    callbacks: {
      onmessage: message => {
        if (failed || closed) return;
        if (message.serverContent) onMessage(message.serverContent);
        if (message.toolCall?.functionCalls?.length) handleTools(message.toolCall.functionCalls)
          .catch(() => fail(new Error('Tool response failed')));
      },
      onerror: fail,
      onclose: fail,
    },
  });
  // The SDK can leave connect() pending after onclose before setupComplete.
  // Race those callbacks, and close any session that resolves after cancellation.
  connecting.then(value => { if (failed || closed) { try { value.close(); } catch {} } }, () => {});
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
