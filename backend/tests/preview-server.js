// Test-only browser fixture; never selected by dev.js or the production entry.
import { createBackend } from '../src/server.js';
import { databaseFixture } from './database-fixture.js';
const { store, config } = databaseFixture();
await store.init();
await store.addDevice('fixture-device', 'Browser test device');
await store.startSession('fixture-session', 'fixture-device', config.model);
await store.saveTurn('fixture-session', 'fixture-device', 'UI test: hello assistant.', 'UI test: hello, Shreeharsh.');
await store.endSession('fixture-session');
await store.touchDevice('fixture-device', 'offline');
const backend = createBackend(config, async () => { throw new Error('No live provider in browser fixture'); }, { store, localPreview: true });
backend.server.listen(8081, '127.0.0.1', () => console.log('Local UI verification fixture: http://127.0.0.1:8081 (test data only)'));
