import OpusScript from 'opusscript';
import { setTimeout as sleep } from 'node:timers/promises';

const FRAME_SAMPLES = 1440; // 60 ms at the provider's 24 kHz output rate.
const FRAME_BYTES = FRAME_SAMPLES * 2;
const MAX_PACKET = OpusScript.MAX_PACKET_SIZE;
const MAX_PCM_CHUNK = 65536;
const TEXT_LIMIT = 400;

export function validHello(message) {
  const audio = message?.audio_params;
  return message?.type === 'hello' && message.version === 1 &&
    message.transport === 'websocket' && audio?.format === 'opus' &&
    audio.sample_rate === 16000 && audio.channels === 1 && audio.frame_duration === 60;
}

export class VoiceBridge {
  constructor(ws) {
    this.ws = ws;
    this.decoder = new OpusScript(16000, 1, OpusScript.Application.VOIP);
    this.encoder = new OpusScript(24000, 1, OpusScript.Application.VOIP);
    this.encoder.setBitrate(24000);
    this.queue = [];
    this.pendingBytes = 0;
    this.pcm = Buffer.alloc(0);
    this.listening = false;
    this.speaking = false;
    this.closed = false;
    this.provider = null;
    this.nextAudioAt = 0;
    this.inputWindow = Date.now();
    this.inputFrames = 0;
    this.worker = this.run();
  }

  sendJson(value) {
    if (!this.closed && this.ws.readyState === 1) this.ws.send(JSON.stringify(value));
  }

  handlePacket(packet) {
    if (this.closed || !this.listening || this.speaking || !this.provider) return;
    if (!packet.length || packet.length > MAX_PACKET) throw new Error('Invalid audio size');
    const now = Date.now();
    if (now - this.inputWindow >= 1000) { this.inputWindow = now; this.inputFrames = 0; }
    if (++this.inputFrames > 50) throw new Error('Audio rate limit');
    const pcm = this.decoder.decode(packet);
    if (pcm.length !== 1920) throw new Error('Expected a 60ms mono frame');
    this.provider.sendAudio(pcm);
  }

  handleControl(message) {
    if (this.closed) return;
    if (message?.type === 'listen') {
      if (message.state === 'start' && !this.speaking) this.listening = true;
      else if (message.state === 'stop') {
        this.listening = false;
        this.provider?.finishInput();
      }
    } else if (message?.type === 'abort') {
      // Half-duplex device: cancellation renews the session rather than sending
      // unsupported provider activity messages while automatic VAD is enabled.
      this.ws.close(1000, 'Conversation cancelled');
    }
  }

  enqueue(content) {
    if (!content || this.closed) return;
    const events = [];
    for (const [kind, transcription] of [['input', content.inputTranscription], ['output', content.outputTranscription]]) {
      if (!transcription?.text) continue;
      if (typeof transcription.text !== 'string' || transcription.text.length > 4096) throw new Error('Invalid provider text');
      events.push({ kind, text: transcription.text });
    }
    for (const part of content.modelTurn?.parts || []) {
      if (!part.inlineData?.data) continue;
      if (!/^audio\/pcm(?:;rate=24000)?$/.test(part.inlineData.mimeType || '')) {
        throw new Error('Unexpected provider audio format');
      }
      if (part.inlineData.data.length > Math.ceil(MAX_PCM_CHUNK / 3) * 4) throw new Error('Provider audio too large');
      const pcm = Buffer.from(part.inlineData.data, 'base64');
      if (!pcm.length || pcm.length > MAX_PCM_CHUNK || pcm.length % 2) throw new Error('Invalid provider PCM');
      events.push({ kind: 'audio', pcm });
    }
    if (content.interrupted) events.push({ kind: 'interrupted' });
    if (content.turnComplete) events.push({ kind: 'complete' });
    for (const event of events) {
      const bytes = event.pcm?.length || Buffer.byteLength(event.text || '');
      if (this.queue.length >= 64 || this.pendingBytes + bytes > 262144) throw new Error('Voice backpressure limit');
      this.pendingBytes += bytes;
      this.queue.push({ ...event, bytes });
    }
    this.wake?.();
  }

  startReply() {
    if (this.speaking) return;
    this.speaking = true;
    this.listening = false;
    this.provider?.finishInput();
    this.sendJson({ type: 'tts', state: 'start' });
  }

  async sendFrame(pcm) {
    if (this.closed) return;
    const wait = this.nextAudioAt - Date.now();
    if (wait > 0) await sleep(wait);
    if (this.closed) return;
    if (this.ws.bufferedAmount > 65536) throw new Error('Device output backpressure');
    this.ws.send(this.encoder.encode(pcm, FRAME_SAMPLES));
    this.nextAudioAt = Date.now() + 60;
  }

  async run() {
    try {
      while (!this.closed) {
        if (!this.queue.length) await new Promise(resolve => { this.wake = resolve; });
        this.wake = null;
        if (this.closed) break;
        const event = this.queue.shift();
        if (!event) continue;
        this.pendingBytes -= event.bytes;
        if (event.kind === 'input') this.sendJson({ type: 'stt', text: Array.from(event.text).slice(0, TEXT_LIMIT).join('') });
        else if (event.kind === 'output') {
          this.startReply();
          this.sendJson({ type: 'tts', state: 'sentence_start', text: Array.from(event.text).slice(0, TEXT_LIMIT).join('') });
        } else if (event.kind === 'audio') {
          this.startReply();
          this.pcm = Buffer.concat([this.pcm, event.pcm]);
          while (this.pcm.length >= FRAME_BYTES && !this.closed) {
            const frame = this.pcm.subarray(0, FRAME_BYTES);
            this.pcm = this.pcm.subarray(FRAME_BYTES);
            await this.sendFrame(frame);
          }
          // Do not retain a view into a large provider buffer between chunks.
          this.pcm = Buffer.from(this.pcm);
        } else if (event.kind === 'complete') {
          if (this.pcm.length) {
            const padded = Buffer.alloc(FRAME_BYTES);
            this.pcm.copy(padded);
            this.pcm = Buffer.alloc(0);
            await this.sendFrame(padded);
          }
          if (this.speaking) this.sendJson({ type: 'tts', state: 'stop' });
          this.speaking = false;
          // Firmware sends listen/start after its playback queue drains.
        } else if (event.kind === 'interrupted') {
          this.ws.close(1000, 'Conversation interrupted');
        }
      }
    } catch {
      this.ws.close(1011, 'Voice streaming failed');
    }
  }

  destroy() {
    if (this.closed) return;
    this.closed = true;
    this.wake?.();
    try { this.provider?.close(); } catch { /* Cleanup must still release both codecs. */ }
    this.queue.length = 0;
    this.pendingBytes = 0;
    this.pcm = Buffer.alloc(0);
    // Wait for the worker to finish any already-paced frame before freeing WASM.
    this.worker.finally(() => { this.decoder.delete(); this.encoder.delete(); });
  }
}
