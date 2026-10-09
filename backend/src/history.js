// Bounded asynchronous journal. No database work runs inside codec/audio tasks.
export class ConversationJournal {
  constructor(store, sessionId, deviceId, enabled) {
    this.store = store; this.sessionId = sessionId; this.deviceId = deviceId;
    this.enabled = enabled; this.user = ''; this.assistant = ''; this.pending = 0;
    this.writes = Promise.resolve(); this.failed = false;
  }
  observe(content) {
    if (!this.enabled || !content) return;
    if (typeof content.inputTranscription?.text === 'string') this.user = (this.user + content.inputTranscription.text).slice(-6000);
    if (typeof content.outputTranscription?.text === 'string') this.assistant = (this.assistant + content.outputTranscription.text).slice(-6000);
    if (content.turnComplete || content.interrupted) this.flush();
  }
  flush() {
    if (!this.user && !this.assistant) return;
    const user = this.user, assistant = this.assistant;
    this.user = ''; this.assistant = '';
    if (this.pending >= 20) { this.failed = true; return; }
    this.pending++;
    this.writes = this.writes.then(() => this.store.saveTurn(this.sessionId, this.deviceId, user, assistant))
      .catch(() => { this.failed = true; console.error('Conversation history write failed'); })
      .finally(() => this.pending--);
  }
  async finish() { this.flush(); await this.writes; }
}
