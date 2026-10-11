import test from 'node:test';
import assert from 'node:assert/strict';
import { readConfig } from '../src/config.js';
import { openGemini, instructionsWithMemory } from '../src/gemini.js';
import { firecrawlKeys, searchWeb } from '../src/search.js';
import { databaseFixture } from './database-fixture.js';

test('Firecrawl keys are separate, trimmed, and rotated when a key is rate limited', async () => {
  assert.deepEqual(firecrawlKeys({ FIRECRAWL_API_KEYS: ' fc-one,fc-two,fc-one ' }), ['fc-one', 'fc-two']);
  assert.deepEqual(firecrawlKeys({ FIRECRAWL_API_KEY: 'fc-old' }), ['fc-old']);
  const seen = [];
  const result = await searchWeb('current weather', ['fc-one', 'fc-two'], {
    fetch: async (_url, request) => {
      seen.push(request.headers.Authorization);
      return seen.length === 1 ? { status: 429, ok: false } : { status: 200, ok: true,
        json: async () => ({ data: { web: [{ title: 'Weather', description: 'Today is sunny', url: 'https://example.com/weather' }] } }) };
    },
  });
  assert.equal(new Set(seen).size, 2);
  assert.equal(result.results[0].url, 'https://example.com/weather');
  assert.equal(result.results[0].description, 'Today is sunny');
  assert.deepEqual(await searchWeb('', ['fc-one']), { error: 'Search query must be 1–240 characters.' });
});

test('recent conversation is scoped to one device and bounded for the voice prompt', async () => {
  const { store } = databaseFixture();
  await store.init();
  await store.saveTurn('session-a', 'pocket-a', 'My name is Asha', 'Nice to meet you');
  await store.saveTurn('session-b', 'pocket-b', 'My name is Ravi', 'Hello Ravi');
  const memory = await store.recentTurns('pocket-a');
  assert.deepEqual(memory.map(item => item.text), ['My name is Asha', 'Nice to meet you']);
  const instruction = instructionsWithMemory('Be concise.', memory);
  assert.match(instruction, /Asha/);
  assert.doesNotMatch(instruction, /Ravi/);
});

test('Gemini Live can call bounded current-web search and receive its tool response', async () => {
  const config = readConfig({ GEMINI_API_KEYS: 'provider', FIRECRAWL_API_KEYS: 'fc-one' });
  let callbacks;
  const responses = [];
  const provider = await openGemini(config, () => {}, () => assert.fail('Provider should stay open'), {
    memory: [{ role: 'user', text: 'Remember my city is Pune' }],
    search: async query => ({ results: [{ title: query, url: 'https://example.com', description: 'Current result' }] }),
    createClient: () => ({ live: { connect: async options => {
      callbacks = options.callbacks;
      assert.match(options.config.systemInstruction, /Pune/);
      assert.equal(options.config.tools[0].functionDeclarations[0].name, 'search_current_web');
      return { sendToolResponse: value => responses.push(value), sendRealtimeInput() {}, close() {} };
    } } }),
  });
  callbacks.onmessage({ toolCall: { functionCalls: [{ id: 'call-1', name: 'search_current_web', args: { query: 'Pune weather now' } }] } });
  for (let i = 0; i < 20 && !responses.length; i++) await new Promise(done => setTimeout(done, 10));
  assert.equal(responses[0].functionResponses[0].id, 'call-1');
  assert.equal(responses[0].functionResponses[0].response.results[0].title, 'Pune weather now');
  provider.close();
});
