import { test } from 'node:test';
import assert from 'node:assert/strict';
import { setup, fakeFetch, geminiReply, find } from './fakes.mjs';
import { createRecorder } from '../src/core/recorder.mjs';
import { listModels, MODELS } from '../src/core/models.mjs';
import { contents, parse, reading } from '../src/core/gemini.mjs';

const listed = {
  models: [
    { name: 'models/gemini-4-carbon', displayName: 'Gemini 4 Carbon', supportedGenerationMethods: ['generateContent', 'countTokens'] },
    { name: 'models/gemini-4-hydrogen', displayName: 'Gemini 4 Hydrogen', supportedGenerationMethods: ['generateContent'] },
    { name: 'models/gemini-embedding-2', displayName: 'Embedding', supportedGenerationMethods: ['embedContent'] },
    { name: 'models/imagen-5', displayName: 'Imagen', supportedGenerationMethods: ['generateContent'] },
  ],
};

test('models.refresh: the generateContent gemini-* models, the default first', async () => {
  const fetch = fakeFetch(listed);
  const { reviewer } = await setup({ fetch });
  await reviewer.dispatch({ type: 'models.refresh' });
  assert.deepEqual(reviewer.getState().settings.models, [
    { id: 'gemini-4-hydrogen', label: 'Gemini 4 Hydrogen' },
    { id: 'gemini-4-carbon', label: 'Gemini 4 Carbon' },
  ]);
  assert.match(fetch.requests[0].url, /\/v1beta\/models\?/);
  assert.equal(fetch.requests[0].headers['x-goog-api-key'], 'AIza-test-key-123');
  reviewer.close();
});

test('models.refresh falls back to the hard-coded list on a refusal or with no key', async () => {
  const { reviewer } = await setup({ fetch: fakeFetch({ status: 403, body: { error: { message: 'no' } } }) });
  await reviewer.dispatch({ type: 'models.refresh' });
  assert.deepEqual(reviewer.getState().settings.models.map((m) => m.id), MODELS.map((m) => m.id));
  reviewer.close();
  assert.deepEqual(await listModels(null), { models: MODELS, fallback: true });
  assert.equal((await listModels('k', { fetch: async () => { throw new Error('offline'); } })).fallback, true);
});

test('settings: the key saved and forgotten, the model, a persona of the user\'s own', async () => {
  const fetch = fakeFetch(listed, geminiReply('ok', [['propose_rewrite', { replacement: 'x', rationale: '' }]]));
  const { reviewer, credentials, store, page } = await setup({ key: null, fetch });
  assert.equal(reviewer.getState().settings.hasKey, false);
  await reviewer.dispatch({ type: 'settings.save', apiKey: ' AIza-new ', rememberKey: true });
  let s = reviewer.getState().settings;
  assert.equal(s.hasKey, true);
  assert.equal(s.rememberKey, true);
  assert.equal(credentials.get(), 'AIza-new');
  assert.equal(s.models.length, 2, 'a saved key refreshes the models');

  await reviewer.dispatch({ type: 'settings.save', model: 'gemini-4-carbon', persona: { id: 'terse', name: 'Terse', instructions: 'Use five words.' }, personaID: 'terse' });
  s = reviewer.getState().settings;
  assert.equal(s.model, 'gemini-4-carbon');
  assert.equal(s.personaID, 'terse');
  assert.deepEqual(s.personas.map((p) => p.name), ['tinyactors voice', 'Terse']);
  assert.deepEqual(await store.settings(), { model: 'gemini-4-carbon', personaID: 'terse', thinking: 'high' });

  page.select('Read the guide');
  await reviewer.dispatch({ type: 'annotate', note: 'x' });
  const request = fetch.requests[1];
  assert.match(request.url, /gemini-4-carbon:generateContent/);
  assert.match(request.body.systemInstruction.parts[0].text, /^Active persona: Terse\nUse five words\./);

  await reviewer.dispatch({ type: 'settings.forgetKey' });
  assert.equal(reviewer.getState().settings.hasKey, false);
  assert.equal(credentials.get(), null);
  reviewer.close();
});

test('settings: a key Google refuses is not saved, and says so', async () => {
  const refused = { status: 400, body: { error: { code: 400, message: 'API key not valid. Please pass a valid API key.', status: 'INVALID_ARGUMENT', details: [{ reason: 'API_KEY_INVALID' }] } } };
  const { reviewer, credentials } = await setup({ key: null, fetch: fakeFetch(refused) });
  await assert.rejects(reviewer.dispatch({ type: 'settings.save', apiKey: 'AIza-wrong', rememberKey: true }), /^Error: That key was not accepted by Google\.$/);
  assert.equal(reviewer.getState().settings.hasKey, false);
  assert.equal(credentials.get(), null);
  reviewer.close();
});

test('settings: a key is saved when Google cannot be asked (offline)', async () => {
  const offline = async () => { throw new TypeError('fetch failed'); };
  const { reviewer, credentials } = await setup({ key: null, fetch: offline });
  await reviewer.dispatch({ type: 'settings.save', apiKey: 'AIza-later' });
  assert.equal(reviewer.getState().settings.hasKey, true);
  assert.equal(credentials.get(), 'AIza-later');
  reviewer.close();
});

// A microphone and a MediaRecorder that record three bytes.
function fakeMedia() {
  const tracks = [];
  const mediaDevices = {
    async getUserMedia(constraints) {
      assert.deepEqual(constraints, { audio: true });
      const track = { stopped: false, stop() { this.stopped = true; } };
      tracks.push(track);
      return { getTracks: () => [track] };
    },
  };
  class MediaRecorder {
    constructor(stream) { this.stream = stream; this.mimeType = 'audio/webm;codecs=opus'; }
    start() { this.state = 'recording'; }
    stop() {
      queueMicrotask(() => {
        this.ondataavailable({ data: new Blob([new Uint8Array([1, 2, 3])], { type: this.mimeType }) });
        this.onstop();
      });
    }
  }
  return { recorder: createRecorder({ mediaDevices, MediaRecorder }), tracks };
}

test('push-to-talk: the clip is transcribed first, and the transcript is the message, for an annotation and for the page', async () => {
  const { recorder, tracks } = fakeMedia();
  const fetch = fakeFetch(
    geminiReply('Make it shorter'),
    geminiReply('Heard you.', [['propose_rewrite', { replacement: 'Spoken rewrite', rationale: 'r' }]]),
    geminiReply('What is vague here?'),
    geminiReply('About the page.'),
  );
  const { reviewer, page, store } = await setup({ fetch, recorder });
  page.select('Build better products');
  const id = await reviewer.dispatch({ type: 'annotate' });

  await reviewer.dispatch({ type: 'record.start', target: id });
  assert.deepEqual(reviewer.getState().recording, { state: 'recording', target: id });
  await reviewer.dispatch({ type: 'record.stop' });
  assert.deepEqual(reviewer.getState().recording, { state: 'idle', target: null });
  assert.ok(tracks[0].stopped, 'the microphone is released');
  const a = find(reviewer, id);
  assert.equal(a.state, 'suggested');
  assert.deepEqual(a.messages[0], { role: 'user', text: 'Make it shorter', at: a.messages[0].at, spoken: true });
  assert.equal(a.suggestions[0].text, 'Spoken rewrite');
  // the transcription: the audio inline, no tools, low thinking
  const heard = fetch.requests[0].body;
  assert.deepEqual(heard.contents[0].parts.at(-1), { inlineData: { mimeType: 'audio/webm', data: 'AQID' } });
  assert.equal(heard.tools, undefined);
  assert.deepEqual(heard.generationConfig.thinkingConfig, { thinkingLevel: 'low' });
  assert.match(heard.systemInstruction.parts[0].text, /verbatim/);
  // the rewrite: the transcript as text, no audio
  assert.ok(!JSON.stringify(fetch.requests[1].body).includes('AQID'));
  assert.match(JSON.stringify(fetch.requests[1].body.contents), /Make it shorter/);

  await reviewer.dispatch({ type: 'record.start', target: 'page' });
  await reviewer.dispatch({ type: 'record.stop' });
  assert.deepEqual(reviewer.getState().chat.messages.map((m) => [m.text, m.spoken]), [['What is vague here?', true], ['About the page.', undefined]]);
  assert.ok(!store.dump().includes('AQID'), 'the audio is not stored');
  await assert.rejects(reviewer.dispatch({ type: 'record.stop' }), /Not recording/);
  reviewer.close();
});

test('gemini: contents merge turns, a reply\'s text and calls are read, bad calls dropped', () => {
  const turns = contents([{ role: 'model', text: 'hi' }, { role: 'user', text: 'a' }, { role: 'user', text: 'b' }], { prompt: 'c' });
  assert.deepEqual(turns, [
    { role: 'user', parts: [{ text: '(start)' }] },
    { role: 'model', parts: [{ text: 'hi' }] },
    { role: 'user', parts: [{ text: 'a' }, { text: 'b' }, { text: 'c' }] },
  ]);
  const read = reading(parse(geminiReply('Done.', [
    ['propose_rewrite', { original_text: ' Old ', replacement: ' New ', rationale: 'why' }],
    ['propose_rewrite', { replacement: 42 }],
    ['set_note', { note: 'Vague.' }],
    ['delete_page', {}],
  ])));
  assert.deepEqual(read, { text: 'Done.', rewrites: [{ original_text: 'Old', replacement: 'New', rationale: 'why' }], note: 'Vague.', personas: [], thoughts: '' });
  assert.throws(() => parse({ promptFeedback: { blockReason: 'SAFETY' } }), /blocked the request \(SAFETY\)/);
});

test('set_note: the model writes the note when the user gave none', async () => {
  const fetch = fakeFetch(geminiReply('', [['set_note', { note: 'Says nothing measurable.' }], ['propose_rewrite', { replacement: 'Run 1M actors', rationale: 'A number.' }]]));
  const { reviewer, page } = await setup({ fetch });
  page.select('Actors are cheap');
  const id = await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'suggest', id });
  const a = find(reviewer, id);
  assert.equal(a.note, 'Says nothing measurable.');
  assert.equal(a.messages.at(-1).text, 'A number.', 'no text from the model: the rewrite\'s reason');
  assert.equal(a.suggestions[0].after, a.messages.length, 'the card comes after the reply that brought it');
  reviewer.close();
});
