// The actors through voice transcription, thinking and personas, against
// a fake page, a fake microphone and a fake Gemini (fetch).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { setup, fakeFetch, fakePage, geminiReply, find } from './fakes.mjs';
import { createRecorder } from '../src/core/recorder.mjs';
import { createMemoryStore } from '../src/core/store.mjs';
import { thinkingConfig, parse } from '../src/core/gemini.mjs';

// A microphone whose MediaRecorder records three bytes.
function fakeRecorder() {
  const mediaDevices = { getUserMedia: async () => ({ getTracks: () => [{ stop() {} }] }) };
  class MediaRecorder {
    constructor() { this.mimeType = 'audio/webm'; }
    start() {}
    stop() {
      queueMicrotask(() => {
        this.ondataavailable({ data: new Blob([new Uint8Array([1, 2, 3])], { type: this.mimeType }) });
        this.onstop();
      });
    }
  }
  return createRecorder({ mediaDevices, MediaRecorder });
}

// A fetch that waits for `release()` before each answer it is told to hold.
function gatedFetch(...answers) {
  const inner = fakeFetch(...answers);
  const gates = [];
  const fetch = async (url, init) => {
    const n = inner.requests.length;
    if (gates[n]) await gates[n].promise;
    return inner(url, init);
  };
  fetch.requests = inner.requests;
  fetch.hold = (n) => {
    let release;
    const promise = new Promise((resolve) => { release = resolve; });
    gates[n] = { promise, release };
    return release;
  };
  return fetch;
}

const until = async (check) => {
  for (let i = 0; i < 200 && !check(); i++) await new Promise((resolve) => setTimeout(resolve, 5));
  assert.ok(check(), 'the condition came');
};

// A Gemini reply with a thought summary before its words.
const thinking = (thought, text, calls = [], finishReason = 'STOP') => {
  const body = geminiReply(text, calls);
  body.candidates[0].content.parts.unshift({ text: thought, thought: true });
  body.candidates[0].finishReason = finishReason;
  return body;
};

test('voice, annotation: transcribing, then the transcript shows as the message while the model answers', async () => {
  const fetch = gatedFetch(
    geminiReply('Shorter, please'),
    geminiReply('One shorter line.', [['propose_rewrite', { replacement: 'Ship better', rationale: 'Shorter.' }]]),
  );
  const holdTranscript = fetch.hold(0);
  const holdReply = fetch.hold(1);
  const { reviewer, page } = await setup({ fetch, recorder: fakeRecorder() });
  page.select('Build better products');
  const id = await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'record.start', target: id });
  const stopping = reviewer.dispatch({ type: 'record.stop' });

  await until(() => find(reviewer, id).state === 'transcribing');
  assert.deepEqual(reviewer.getState().recording, { state: 'transcribing', target: id });
  assert.equal(find(reviewer, id).messages.length, 0, 'no message before the transcript');
  assert.ok(reviewer.getState().totals.open >= 1, 'transcribing is open');

  holdTranscript();
  await until(() => find(reviewer, id).state === 'asking');
  const a = find(reviewer, id);
  assert.deepEqual(a.messages.map((m) => [m.role, m.text, m.spoken]), [['user', 'Shorter, please', true]]);
  assert.deepEqual(reviewer.getState().recording, { state: 'idle', target: null }, 'the mic is free once the words exist');

  holdReply();
  await stopping;
  assert.equal(find(reviewer, id).state, 'suggested');
  assert.equal(find(reviewer, id).suggestions[0].text, 'Ship better');
  // the request about the copy carries the transcript as text
  const asked = fetch.requests[1].body;
  assert.equal(asked.contents.at(-1).parts.at(-1).text, 'Shorter, please');
  assert.ok(!JSON.stringify(asked).includes('inlineData'));
  reviewer.close();
});

test('voice: nothing heard goes back with a short error; a failed transcription fails with its message', async () => {
  const fetch = fakeFetch(
    geminiReply(''),
    { status: 429, body: { error: { message: 'Slow down.' } } },
    geminiReply('   '),
    { status: 500, body: { error: { message: 'Broken.' } } },
  );
  const { reviewer, page } = await setup({ fetch, recorder: fakeRecorder() });
  page.select('Read the guide');
  const id = await reviewer.dispatch({ type: 'annotate' });

  await reviewer.dispatch({ type: 'record.start', target: id });
  await reviewer.dispatch({ type: 'record.stop' });
  let a = find(reviewer, id);
  assert.equal(a.state, 'draft', 'back to where it was');
  assert.equal(a.error, 'Nothing was heard.');
  assert.deepEqual(a.messages, []);

  await reviewer.dispatch({ type: 'record.start', target: id });
  await reviewer.dispatch({ type: 'record.stop' });
  a = find(reviewer, id);
  assert.equal(a.state, 'failed');
  assert.match(a.error, /429.*Slow down/);
  assert.equal(fetch.requests.length, 2, 'no rewrite asked');

  // the page chat: an empty transcript is the chat's error, no message
  await reviewer.dispatch({ type: 'record.start', target: 'page' });
  await reviewer.dispatch({ type: 'record.stop' });
  let chat = reviewer.getState().chat;
  assert.deepEqual(chat.messages, []);
  assert.equal(chat.error, 'Nothing was heard.');
  assert.equal(chat.busy, false);
  await reviewer.dispatch({ type: 'record.start', target: 'page' });
  await reviewer.dispatch({ type: 'record.stop' });
  chat = reviewer.getState().chat;
  assert.match(chat.error, /500/);
  assert.deepEqual(reviewer.getState().recording, { state: 'idle', target: null });
  reviewer.close();
});

test('voice while previewing: the original comes back, then the transcript is asked', async () => {
  const fetch = fakeFetch(
    geminiReply('Plainer.', [['propose_rewrite', { replacement: 'Ship it', rationale: 'r' }]]),
    geminiReply('Even shorter'),
    geminiReply('Shorter still.', [['propose_rewrite', { replacement: 'Ship', rationale: 'r' }]]),
  );
  const { reviewer, page } = await setup({ fetch, recorder: fakeRecorder() });
  page.select('Build better products');
  const id = await reviewer.dispatch({ type: 'annotate', note: 'Generic.' });
  await reviewer.dispatch({ type: 'preview', id, suggestionID: 's1' });
  assert.match(page.text, /^Ship it\./);
  await reviewer.dispatch({ type: 'record.start', target: id });
  await reviewer.dispatch({ type: 'record.stop' });
  const a = find(reviewer, id);
  assert.match(page.text, /^Build better products\./);
  assert.equal(a.state, 'suggested');
  assert.equal(a.chosenID, null);
  assert.deepEqual(a.messages.filter((m) => m.role === 'user').map((m) => m.text), ['Generic.', 'Even shorter']);
  reviewer.close();
});

test('voice: the export carries the transcript as the request', async () => {
  const fetch = fakeFetch(geminiReply('Name the guide'), geminiReply('Done.', [['propose_rewrite', { replacement: 'Read the hibernation guide', rationale: 'r' }]]));
  const { reviewer, page, clipboard } = await setup({ fetch, recorder: fakeRecorder() });
  page.select('Read the guide');
  const id = await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'record.start', target: id });
  await reviewer.dispatch({ type: 'record.stop' });
  await reviewer.dispatch({ type: 'preview', id, suggestionID: 's1' });
  await reviewer.dispatch({ type: 'keep', id });
  await reviewer.dispatch({ type: 'export', scope: 'page', to: 'clipboard' });
  assert.match(clipboard[0], /Name the guide/);
  assert.ok(!clipboard[0].includes('(voice message)'));
  reviewer.close();
});

test('thinking: the level goes in every request, is saved with the settings, and maps per model', async () => {
  const store = createMemoryStore();
  const fetch = fakeFetch(geminiReply('a'), geminiReply('b'));
  const { reviewer, page } = await setup({ fetch, store });
  assert.equal(reviewer.getState().settings.thinking, 'high');
  page.select('Read the guide');
  const id = await reviewer.dispatch({ type: 'annotate', note: 'x' });
  assert.deepEqual(fetch.requests[0].body.generationConfig, { maxOutputTokens: 65536, thinkingConfig: { thinkingLevel: 'high', includeThoughts: true } });

  await reviewer.dispatch({ type: 'settings.save', thinking: 'xhigh', model: 'gemini-4-carbon' });
  assert.equal(reviewer.getState().settings.thinking, 'xhigh');
  assert.equal((await store.settings()).thinking, 'xhigh');
  await reviewer.dispatch({ type: 'ask', id, text: 'again' });
  assert.deepEqual(fetch.requests[1].body.generationConfig.thinkingConfig, { thinkingLevel: 'high', includeThoughts: true }, 'Carbon takes no xhigh');
  await assert.rejects(reviewer.dispatch({ type: 'settings.save', thinking: 'max' }), /Thinking is one of low, medium, high, xhigh/);
  reviewer.close();

  const again = await setup({ store });
  assert.equal(again.reviewer.getState().settings.thinking, 'xhigh', 'stored with the settings');
  again.reviewer.close();

  assert.deepEqual(thinkingConfig('gemini-4-hydrogen', 'xhigh'), { thinkingLevel: 'xhigh', includeThoughts: true });
  assert.deepEqual(thinkingConfig('gemini-4-neon', 'xhigh'), { thinkingLevel: 'xhigh', includeThoughts: true });
  assert.deepEqual(thinkingConfig('gemini-3.8-flash', 'xhigh'), { thinkingLevel: 'high', includeThoughts: true });
  assert.deepEqual(thinkingConfig('gemini-3.8-flash', 'low', { includeThoughts: false }), { thinkingLevel: 'low' });
  assert.deepEqual(thinkingConfig('gemini-2.5-pro', 'medium'), { thinkingBudget: 8192, includeThoughts: true });
});

test('thinking: a model that refuses the level is asked once more at high', async () => {
  const fetch = fakeFetch(
    { status: 400, body: { error: { message: 'Thinking level XHIGH is not supported for this model. Please retry with other thinking level.' } } },
    geminiReply('Fine.'),
  );
  const { reviewer } = await setup({ fetch });
  await reviewer.dispatch({ type: 'settings.save', thinking: 'xhigh', model: 'gemini-9-new' });
  await reviewer.dispatch({ type: 'chat', text: 'Hi' });
  assert.equal(fetch.requests[1].body.generationConfig.thinkingConfig.thinkingLevel, 'high');
  assert.equal(reviewer.getState().chat.messages.at(-1).text, 'Fine.');
  reviewer.close();
});

test('thinking: thoughts are never the answer; the summary and the seconds stay on the message', async () => {
  const fetch = fakeFetch(
    thinking('**Weighing the headline**\nIt names no outcome.', 'It names no outcome.', [['propose_rewrite', { replacement: 'Ship it', rationale: 'r' }]]),
    thinking('**Reading the page**', 'The page is clear.'),
  );
  const { reviewer, page } = await setup({ fetch });
  page.select('Build better products');
  const id = await reviewer.dispatch({ type: 'annotate', note: 'Vague.' });
  const reply = find(reviewer, id).messages.at(-1);
  assert.equal(reply.text, 'It names no outcome.');
  assert.equal(reply.thoughts, '**Weighing the headline**\nIt names no outcome.');
  assert.ok(reply.seconds >= 1);
  await reviewer.dispatch({ type: 'chat', text: 'Overall?' });
  const said = reviewer.getState().chat.messages.at(-1);
  assert.equal(said.text, 'The page is clear.');
  assert.equal(said.thoughts, '**Reading the page**');
  reviewer.close();
});

test('thinking: an answer cut at MAX_TOKENS keeps its words; one cut before any is a clear error', async () => {
  const fetch = fakeFetch(
    thinking('long thought', 'Half an answer', [], 'MAX_TOKENS'),
    thinking('very long thought', '', [], 'MAX_TOKENS'),
  );
  const { reviewer } = await setup({ fetch });
  await reviewer.dispatch({ type: 'chat', text: 'One' });
  assert.equal(reviewer.getState().chat.messages.at(-1).text, 'Half an answer');
  await reviewer.dispatch({ type: 'chat', text: 'Two' });
  assert.match(reviewer.getState().chat.error, /whole output budget thinking.*lower thinking level/);
  assert.equal(parse(thinking('t', 'x', [], 'MAX_TOKENS')).cut, true);
  reviewer.close();
});

test('personas: the page chat defines one and uses it; the chip names it; the next request follows it', async () => {
  const store = createMemoryStore();
  const fetch = fakeFetch(
    geminiReply('Saved a persona for senior backend engineers and switched to it.', [
      ['define_persona', { name: 'Skeptical backend engineer', instructions: 'Readers are senior backend engineers. Prove every claim.', use: true }],
    ]),
    geminiReply('Rewritten for them.', [['propose_rewrite', { original_text: 'Actors are cheap', replacement: 'An actor costs 287 bytes', rationale: 'A number.' }]]),
  );
  const { reviewer } = await setup({ fetch, store });
  await reviewer.dispatch({ type: 'chat', text: 'Define a persona for a skeptical senior backend engineer and use it' });
  const s = reviewer.getState();
  const persona = s.settings.personas.find((p) => p.name === 'Skeptical backend engineer');
  assert.ok(persona, 'saved');
  assert.equal(persona.source, 'model');
  assert.equal(s.settings.personaID, persona.id, 'in use');
  assert.deepEqual(s.chat.messages.at(-1).personas, [{ name: 'Skeptical backend engineer', action: 'define', id: persona.id }]);
  assert.equal((await store.personas())[0].instructions, 'Readers are senior backend engineers. Prove every claim.');
  assert.equal((await store.settings()).personaID, persona.id);
  // the tools and the system instruction
  const first = fetch.requests[0].body;
  assert.deepEqual(first.tools[0].functionDeclarations.map((f) => f.name), ['propose_rewrite', 'set_note', 'define_persona', 'use_persona']);
  assert.match(first.systemInstruction.parts[0].text, /^Active persona: tinyactors voice/);
  assert.match(first.systemInstruction.parts[0].text, /define a persona for it with define_persona/);

  await reviewer.dispatch({ type: 'chat', text: 'Now rewrite the weakest line.' });
  const second = fetch.requests[1].body.systemInstruction.parts[0].text;
  assert.match(second, /^Active persona: Skeptical backend engineer\nReaders are senior backend engineers/);
  assert.match(second, /Saved personas: tinyactors voice\./);
  assert.equal(reviewer.getState().annotations[0].suggestions[0].text, 'An actor costs 287 bytes');
  reviewer.close();
});

test('personas: defined again by name it is updated; use_persona switches; an unknown name is the chat\'s error', async () => {
  const fetch = fakeFetch(
    geminiReply('', [['define_persona', { name: 'CTO', instructions: 'Short.' }]]),
    geminiReply(''), // the calls' results sent back: still no words
    geminiReply('Updated.', [['define_persona', { name: 'cto', instructions: 'Shorter.', use: true }]]),
    geminiReply('Back to the default.', [['use_persona', { name: 'tinyactors voice' }]]),
    geminiReply('Switching.', [['use_persona', { name: 'Nobody' }]]),
  );
  const { reviewer } = await setup({ fetch });
  await reviewer.dispatch({ type: 'chat', text: 'A CTO persona' });
  let s = reviewer.getState();
  assert.equal(s.chat.messages.at(-1).text, 'Persona “CTO” saved.', 'calls only: it says what was saved');
  assert.equal(s.settings.personaID, 'tinyactors-voice', 'not used: use was not true');
  const id = s.settings.personas.find((p) => p.name === 'CTO').id;

  await reviewer.dispatch({ type: 'chat', text: 'Shorter, and use it' });
  s = reviewer.getState();
  assert.equal(s.settings.personas.filter((p) => p.name.toLowerCase() === 'cto').length, 1);
  assert.equal(s.settings.personas.find((p) => p.id === id).instructions, 'Shorter.');
  assert.equal(s.settings.personaID, id);

  await reviewer.dispatch({ type: 'chat', text: 'Back' });
  assert.equal(reviewer.getState().settings.personaID, 'tinyactors-voice');
  await reviewer.dispatch({ type: 'chat', text: 'Use Nobody' });
  s = reviewer.getState();
  assert.equal(s.chat.error, 'No persona named “Nobody”.');
  assert.equal(s.settings.personaID, 'tinyactors-voice');
  reviewer.close();
});

test('personas: an annotation\'s conversation may define one too, through the same processor', async () => {
  const fetch = fakeFetch(geminiReply('For a CTO.', [
    ['define_persona', { name: 'CTO', instructions: 'Lead with cost.', use: true }],
    ['propose_rewrite', { replacement: 'Cut your costs', rationale: 'r' }],
  ]));
  const { reviewer, page } = await setup({ fetch });
  page.select('Build better products');
  const id = await reviewer.dispatch({ type: 'annotate', note: 'Write it for a CTO.' });
  const s = reviewer.getState();
  const cto = s.settings.personas.find((p) => p.name === 'CTO');
  assert.equal(s.settings.personaID, cto.id);
  const a = find(reviewer, id);
  assert.deepEqual(a.messages.at(-1).personas, [{ name: 'CTO', action: 'define', id: cto.id }]);
  assert.equal(a.suggestions[0].text, 'Cut your costs');
  reviewer.close();
});

test('personas: any but the default can be deleted; deleting the active one goes back to the default', async () => {
  const fetch = fakeFetch(geminiReply('Saved.', [['define_persona', { name: 'CTO', instructions: 'Lead with cost.', use: true }]]));
  const store = createMemoryStore();
  const { reviewer } = await setup({ fetch, store });
  await reviewer.dispatch({ type: 'chat', text: 'A CTO' });
  const s = reviewer.getState().settings;
  const cto = s.personas.find((p) => p.name === 'CTO');
  assert.equal(s.personas[0].builtIn, true);
  assert.equal(cto.builtIn, undefined);
  await assert.rejects(reviewer.dispatch({ type: 'settings.deletePersona', id: 'tinyactors-voice' }), /cannot be deleted/);
  // edited in Settings, it stays the model's
  await reviewer.dispatch({ type: 'settings.save', persona: { id: cto.id, name: 'CTO', instructions: 'Lead with cost and risk.' } });
  assert.equal(reviewer.getState().settings.personas.find((p) => p.id === cto.id).source, 'model');
  await reviewer.dispatch({ type: 'settings.deletePersona', id: cto.id });
  const after = reviewer.getState().settings;
  assert.deepEqual(after.personas.map((p) => p.name), ['tinyactors voice']);
  assert.equal(after.personaID, 'tinyactors-voice');
  assert.deepEqual(await store.personas(), []);
  assert.equal((await store.settings()).personaID, 'tinyactors-voice');
  reviewer.close();
});

test('pick mode: start, a tap is the selection, annotating it ends pick mode; stop and Escape end it too', async () => {
  const fetch = fakeFetch(geminiReply('Plainer.', [['propose_rewrite', { replacement: 'Ship it', rationale: 'r' }]]));
  const { reviewer, page } = await setup({ fetch });
  assert.equal(reviewer.getState().picking, false);
  await reviewer.dispatch({ type: 'pick.start' });
  assert.equal(reviewer.getState().picking, true);
  assert.equal(page.picking, true);

  page.tap('Actors are cheap');
  assert.deepEqual(reviewer.getState().selection, { text: 'Actors are cheap', fits: true });
  page.tap('Read the guide'); // another element moves the pick
  assert.equal(reviewer.getState().selection.text, 'Read the guide');
  const id = await reviewer.dispatch({ type: 'annotate', note: 'Which guide?' });
  let s = reviewer.getState();
  assert.equal(s.picking, false, 'the annotation ends pick mode');
  assert.equal(page.picking, false);
  assert.equal(s.activeID, id);
  assert.equal(find(reviewer, id).original, 'Read the guide');
  assert.equal(find(reviewer, id).state, 'suggested');

  await reviewer.dispatch({ type: 'pick.start' });
  await reviewer.dispatch({ type: 'pick.stop' });
  assert.equal(reviewer.getState().picking, false);
  assert.equal(page.picking, false);

  await reviewer.dispatch({ type: 'pick.start' });
  page.escape();
  assert.equal(reviewer.getState().picking, false, 'Escape on the page ends it');
  assert.deepEqual(page.calls.filter((c) => c[0] === 'pick').map((c) => c[1]), [true, false, true, false, true]);
  reviewer.close();
});

test('pick mode: a page without it refuses; another page ends it', async () => {
  const page = fakePage({ text: 'Build better products.' });
  page.pick = undefined;
  const { reviewer } = await setup({ page });
  await assert.rejects(reviewer.dispatch({ type: 'pick.start' }), /cannot pick/);
  reviewer.close();

  const second = await setup();
  await second.reviewer.dispatch({ type: 'pick.start' });
  second.page.navigate({ url: 'https://www.tinyactors.dev/guide/', title: 'Guide', text: 'A guide.' });
  await second.reviewer.dispatch({ type: 'deselect' });
  assert.equal(second.reviewer.getState().picking, false);
  second.reviewer.close();
});
