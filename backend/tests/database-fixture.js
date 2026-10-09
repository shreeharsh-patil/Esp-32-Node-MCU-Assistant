// In-memory SQL adapter for local tests only. Production uses Neon PostgreSQL.
import { newDb } from 'pg-mem';
import { readConfig } from '../src/config.js';
import { DatabaseStore } from '../src/store.js';
export const testConfig = readConfig({ PUBLIC_BASE_URL: 'https://assistant.example.com',
  DEVICE_TOKEN: 'd'.repeat(64), ADMIN_TOKEN: 'a'.repeat(64), SETTINGS_ENCRYPTION_KEY: 'e'.repeat(64),
  GEMINI_API_KEYS: 'test-provider-one,test-provider-two' });
export function databaseFixture(config = testConfig) {
  // pg-mem leaves the constraints AST unread for CREATE IF NOT EXISTS on an
  // already existing table. Existing constraints still apply to row writes.
  const database = newDb({ noAstCoverageCheck: true });
  const { Pool } = database.adapters.createPg();
  const pool = new Pool();
  const query = async (text, params = []) => (await pool.query(text, params)).rows;
  return { database, query, store: new DatabaseStore(config, query), config };
}
export function profileBody(profile, changes = {}) {
  return { revision: profile.revision, name: profile.name, model: profile.model, voice: profile.voice,
    instructions: profile.instructions, historyEnabled: profile.historyEnabled,
    keyCooldownSeconds: profile.keyCooldownMs / 1000, ...changes };
}
