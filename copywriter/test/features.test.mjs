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

const ENGINEER = {
  name: 'Skeptical backend engineer',
  description: 'A senior backend engineer evaluating actor runtimes.',
  knows: 'Queues, threads, Erlang.',
  wants: 'Numbers: memory per actor, throughput.',
  tasks: 'Decide whether to try tinyactors.',
  objections: 'Another framework to learn.',
};

test('audiences: the page chat proposes one and uses it; the chip names it; rewrites are written for it and say so', async () => {
  const store = createMemoryStore();
  const fetch = fakeFetch(
    geminiReply('This page is for backend engineers evaluating runtimes.', [['define_audience', { ...ENGINEER, use: true }]]),
    geminiReply('The second line gives no number.', [['propose_rewrite', { original_text: 'Actors are cheap', replacement: 'An actor costs 287 bytes', rationale: 'Serves the skeptical backend engineer\'s want for numbers.' }]]),
  );
  const { reviewer, page, clipboard } = await setup({ fetch, store });
  await reviewer.dispatch({ type: 'chat', text: 'Who is this page for?' });
  let s = reviewer.getState();
  const engineer = s.settings.audiences.find((a) => a.name === ENGINEER.name);
  assert.deepEqual({ ...engineer, id: undefined }, { ...ENGINEER, id: undefined, source: 'model' });
  assert.deepEqual(s.settings.audienceIDs, [engineer.id], 'in use');
  assert.deepEqual(s.chat.messages.at(-1).audiences, [{ name: ENGINEER.name, action: 'define', id: engineer.id }]);
  assert.deepEqual((await store.settings()).audienceIDs, [engineer.id]);
  // the tools and the system instruction: the voice is how, the audiences who
  const first = fetch.requests[0].body;
  assert.deepEqual(first.tools[0].functionDeclarations.map((f) => f.name), ['propose_rewrite', 'set_note', 'define_audience', 'read_as', 'talk_to', 'use_audiences']);
  assert.match(first.systemInstruction.parts[0].text, /^The voice \(how to write\):\nYou edit the copy/);
  assert.match(first.systemInstruction.parts[0].text, /No audience is chosen\./);
  assert.match(first.systemInstruction.parts[0].text, /who the page is for.*define_audience/);

  await reviewer.dispatch({ type: 'chat', text: 'What does a backend engineer want here that is missing?' });
  const second = fetch.requests[1].body.systemInstruction.parts[0].text;
  assert.match(second, /The audiences \(who reads\):\n- Skeptical backend engineer: A senior backend engineer evaluating actor runtimes\.\n {2}Knows: Queues, threads, Erlang\.\n {2}Wants: Numbers/);
  assert.match(second, /Objections: Another framework to learn\./);
  assert.match(second, /rationale which need of which active audience/);
  assert.doesNotMatch(first.systemInstruction.parts[0].text, /rationale which need/, 'no audience, nothing to name');
  s = reviewer.getState();
  const made = s.annotations[0];
  assert.deepEqual(made.audiences, [ENGINEER.name], 'the annotation was written for it');
  // the export says what it was written for
  await reviewer.dispatch({ type: 'preview', id: made.id, suggestionID: 's1' });
  await reviewer.dispatch({ type: 'keep', id: made.id });
  await reviewer.dispatch({ type: 'export', to: 'clipboard' });
  assert.match(clipboard[0], /Written for: Skeptical backend engineer/);
  assert.match(page.text, /An actor costs 287 bytes/);
  reviewer.close();
});

test('audiences: defined again by name it is updated; use_audiences sets the active ones; an unknown name is the chat\'s error', async () => {
  const fetch = fakeFetch(
    geminiReply('Two readers.', [['define_audience', { name: 'CTO', description: 'Buys it.' }], ['define_audience', { name: 'Student', description: 'Learns it.' }]]),
    geminiReply('Updated.', [['define_audience', { name: 'cto', description: 'Signs the budget.', use: true }]]),
    geminiReply('', [['use_audiences', { names: ['Student', 'CTO'] }]]),
    geminiReply(''), // the calls' results sent back: still no words
    geminiReply('Switching.', [['use_audiences', { names: ['Nobody'] }]]),
  );
  const { reviewer } = await setup({ fetch });
  await reviewer.dispatch({ type: 'chat', text: 'Who reads this?' });
  let s = reviewer.getState();
  assert.deepEqual(s.settings.audiences.map((a) => a.name), ['CTO', 'Student']);
  assert.deepEqual(s.settings.audienceIDs, [], 'defined, not in use');
  const cto = s.settings.audiences.find((a) => a.name === 'CTO').id;

  await reviewer.dispatch({ type: 'chat', text: 'Write for the CTO' });
  s = reviewer.getState();
  assert.equal(s.settings.audiences.length, 2);
  assert.equal(s.settings.audiences.find((a) => a.id === cto).description, 'Signs the budget.');
  assert.deepEqual(s.settings.audienceIDs, [cto]);

  await reviewer.dispatch({ type: 'chat', text: 'Both' });
  s = reviewer.getState();
  assert.equal(s.chat.messages.at(-1).text, 'Writing for “Student”, “CTO”.', 'calls only: it says what changed');
  assert.equal(s.settings.audienceIDs.length, 2);
  await reviewer.dispatch({ type: 'chat', text: 'Nobody' });
  s = reviewer.getState();
  assert.equal(s.chat.error, 'No audience named “Nobody”.');
  assert.equal(s.settings.audienceIDs.length, 2, 'unchanged');
  reviewer.close();
});

test('audiences: an annotation\'s conversation may define one too; Settings edits, chooses and deletes them', async () => {
  const fetch = fakeFetch(geminiReply('For a CTO.', [
    ['define_audience', { name: 'CTO', description: 'Signs the budget.', use: true }],
    ['propose_rewrite', { replacement: 'Cut your costs', rationale: 'Serves the CTO.' }],
  ]));
  const store = createMemoryStore();
  const { reviewer, page } = await setup({ fetch, store });
  page.select('Build better products');
  const id = await reviewer.dispatch({ type: 'annotate', note: 'Write it for a CTO.' });
  let s = reviewer.getState();
  const cto = s.settings.audiences.find((a) => a.name === 'CTO');
  assert.deepEqual(s.settings.audienceIDs, [cto.id]);
  assert.deepEqual(find(reviewer, id).messages.at(-1).audiences, [{ name: 'CTO', action: 'define', id: cto.id }]);
  assert.deepEqual(find(reviewer, id).audiences, [], 'written before the audience was in use');

  await reviewer.dispatch({ type: 'settings.save', audience: { id: cto.id, name: 'CTO', description: 'Signs the budget.', objections: 'Lock-in.' } });
  s = reviewer.getState();
  assert.equal(s.settings.audiences[0].objections, 'Lock-in.');
  assert.equal(s.settings.audiences[0].source, 'model', 'still the model\'s');
  await reviewer.dispatch({ type: 'settings.save', audience: { name: 'Student', description: 'Learns it.' } });
  const student = reviewer.getState().settings.audiences.find((a) => a.name === 'Student');
  await reviewer.dispatch({ type: 'settings.save', audienceIDs: [student.id, cto.id] });
  assert.deepEqual(reviewer.getState().settings.audienceIDs, [student.id, cto.id]);
  await assert.rejects(reviewer.dispatch({ type: 'settings.save', audience: { name: ' ' } }), /needs a name/);
  await reviewer.dispatch({ type: 'settings.deleteAudience', id: cto.id });
  s = reviewer.getState().settings;
  assert.deepEqual(s.audiences.map((a) => a.name), ['Student']);
  assert.deepEqual(s.audienceIDs, [student.id]);
  assert.deepEqual((await store.settings()).audienceIDs, [student.id]);
  reviewer.close();
});

test('conversations: a new one starts empty, the model reads only it; the earlier ones are listed and reopen', async () => {
  const store = createMemoryStore();
  const fetch = fakeFetch(geminiReply('One.'), geminiReply('Two.'), geminiReply('Three.'));
  const { reviewer, page } = await setup({ fetch, store });
  await reviewer.dispatch({ type: 'chat', text: 'First question about the headline' });
  const first = reviewer.getState().chat.id;
  assert.deepEqual(reviewer.getState().chat.conversations.map((c) => c.title), ['First question about the headline']);

  await reviewer.dispatch({ type: 'chat.new' });
  let chat = reviewer.getState().chat;
  assert.notEqual(chat.id, first);
  assert.deepEqual(chat.messages, []);
  await reviewer.dispatch({ type: 'chat.new' }); // already new: nothing
  assert.equal(reviewer.getState().chat.id, chat.id);
  await reviewer.dispatch({ type: 'chat', text: 'A'.repeat(80) });
  chat = reviewer.getState().chat;
  assert.deepEqual(chat.messages.map((m) => m.text), ['A'.repeat(80), 'Two.']);
  assert.ok(!JSON.stringify(fetch.requests[1].body.contents).includes('First question'), 'the model reads this conversation only');
  assert.deepEqual(chat.conversations.map((c) => c.id), [chat.id, first], 'newest first');
  assert.equal(chat.conversations[0].title, `${'A'.repeat(59)}…`);

  await reviewer.dispatch({ type: 'chat.open', id: first });
  chat = reviewer.getState().chat;
  assert.equal(chat.id, first);
  assert.deepEqual(chat.messages.map((m) => m.text), ['First question about the headline', 'One.']);
  await reviewer.dispatch({ type: 'chat', text: 'And again?' });
  assert.match(JSON.stringify(fetch.requests[2].body.contents), /First question/, 'its own history');
  await assert.rejects(reviewer.dispatch({ type: 'chat.open', id: 'nope' }), /No such conversation/);
  reviewer.close();

  // a new panel opens the newest conversation; the others are listed
  const again = await setup({ store, page });
  chat = again.reviewer.getState().chat;
  assert.equal(chat.id, first);
  assert.equal(chat.conversations.length, 2);
  again.reviewer.close();
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

test('conversations: while the model answers, the conversation stays open', async () => {
  const fetch = gatedFetch(geminiReply('Later.'));
  const release = fetch.hold(0);
  const { reviewer } = await setup({ fetch });
  const asking = reviewer.dispatch({ type: 'chat', text: 'Slow?' });
  await until(() => reviewer.getState().chat.busy);
  await assert.rejects(reviewer.dispatch({ type: 'chat.new' }), /Wait for the answer/);
  release();
  await asking;
  await reviewer.dispatch({ type: 'chat.new' });
  assert.deepEqual(reviewer.getState().chat.messages, []);
  reviewer.close();
});

// A Gemini reply grounded on web pages.
const grounded = (text, calls, pages) => {
  const body = geminiReply(text, calls);
  body.candidates[0].groundingMetadata = {
    webSearchQueries: ['actor runtimes'],
    groundingChunks: [...pages.map(([title, uri]) => ({ web: { title, uri } })), { web: { title: pages[0][0], uri: pages[0][1] } }],
  };
  return body;
};

test('length: the model\'s answers are short by default, longer on request; the rewrites keep the voice', async () => {
  const store = createMemoryStore();
  const fetch = fakeFetch(geminiReply('a'), geminiReply('b'));
  const { reviewer } = await setup({ fetch, store });
  assert.equal(reviewer.getState().settings.length, 'short');
  await reviewer.dispatch({ type: 'chat', text: 'One' });
  let system = fetch.requests[0].body.systemInstruction.parts[0].text;
  assert.match(system, /answer the reviewer in one or two sentences/);
  assert.match(system, /the rewrites keep the length the voice asks for/);
  assert.equal(fetch.requests[0].body.generationConfig.maxOutputTokens, 65536, 'no cap: thinking counts against it');
  await reviewer.dispatch({ type: 'settings.save', length: 'long' });
  await reviewer.dispatch({ type: 'chat', text: 'Two' });
  system = fetch.requests[1].body.systemInstruction.parts[0].text;
  assert.match(system, /a thorough answer with your reasons/);
  assert.doesNotMatch(system, /answer the reviewer in one or two sentences/);
  assert.equal((await store.settings()).length, 'long');
  await assert.rejects(reviewer.dispatch({ type: 'settings.save', length: 'epic' }), /Length is one of short, medium, long/);
  reviewer.close();
});

test('built-in tools: URL context by default, search and code on request, beside the functions; none in a transcription', async () => {
  const fetch = fakeFetch(geminiReply('a'), geminiReply('b'), geminiReply('c'), geminiReply('heard'), geminiReply('d'));
  const { reviewer, store } = await setup({ fetch, recorder: fakeRecorder() });
  assert.deepEqual(reviewer.getState().settings.tools, { search: false, url: true, code: false });
  await reviewer.dispatch({ type: 'chat', text: 'One' });
  let body = fetch.requests[0].body;
  assert.deepEqual(body.tools.slice(1), [{ urlContext: {} }]);
  assert.equal(body.tools[0].functionDeclarations[0].name, 'propose_rewrite');
  assert.deepEqual(body.toolConfig, { includeServerSideToolInvocations: true });

  await reviewer.dispatch({ type: 'settings.save', tools: { search: true, code: true } });
  await reviewer.dispatch({ type: 'chat', text: 'Two' });
  body = fetch.requests[1].body;
  assert.deepEqual(body.tools.slice(1), [{ googleSearch: {} }, { urlContext: {} }, { codeExecution: {} }]);
  assert.deepEqual((await store.settings()).tools, { search: true, url: true, code: true });

  await reviewer.dispatch({ type: 'settings.save', tools: { search: false, url: false, code: false, other: true } });
  await reviewer.dispatch({ type: 'chat', text: 'Three' });
  body = fetch.requests[2].body;
  assert.equal(body.tools.length, 1, 'the functions only');
  assert.equal(body.toolConfig, undefined);
  assert.deepEqual(reviewer.getState().settings.tools, { search: false, url: false, code: false });

  await reviewer.dispatch({ type: 'settings.save', tools: { search: true } });
  await reviewer.dispatch({ type: 'record.start', target: 'page' });
  await reviewer.dispatch({ type: 'record.stop' });
  assert.equal(fetch.requests[3].body.tools, undefined, 'a transcription uses no tool');
  reviewer.close();
});

test('grounded answers keep their sources: on the page chat\'s message, an annotation\'s, the follow-up\'s, and in the export', async () => {
  const fetch = fakeFetch(
    grounded('Akka calls itself a platform; Orleans a framework.', [], [['akka.io', 'https://example.com/akka'], ['learn.microsoft.com', 'https://example.com/orleans']]),
    geminiReply('', [['propose_rewrite', { replacement: 'Run actors in your program', rationale: 'r' }]]),
    grounded('Akka leads with what it runs.', [], [['akka.io', 'https://example.com/akka']]),
  );
  const { reviewer, page, clipboard } = await setup({ fetch });
  await reviewer.dispatch({ type: 'chat', text: 'How do other actor runtimes describe themselves? Use search.' });
  assert.deepEqual(reviewer.getState().chat.messages.at(-1).sources, [
    { title: 'akka.io', uri: 'https://example.com/akka' }, { title: 'learn.microsoft.com', uri: 'https://example.com/orleans' },
  ], 'once each');

  page.select('Build better products');
  const id = await reviewer.dispatch({ type: 'annotate', note: 'Compare with Akka' });
  await reviewer.dispatch({ type: 'ask', id, text: 'And now?' });
  const a = find(reviewer, id);
  assert.deepEqual(a.messages.at(-1).sources, [{ title: 'akka.io', uri: 'https://example.com/akka' }]);
  await reviewer.dispatch({ type: 'preview', id, suggestionID: 's1' });
  await reviewer.dispatch({ type: 'keep', id });
  await reviewer.dispatch({ type: 'export', to: 'clipboard' });
  assert.match(clipboard[0], /Sources:\n1\. \[akka\.io\]\(https:\/\/example\.com\/akka\)/);
  reviewer.close();
});

test('grounded: a page chat reply of calls only takes the sources of its follow-up, with each call\'s id', async () => {
  const calls = geminiReply('', [['propose_rewrite', { original_text: 'Actors are cheap', replacement: 'An actor costs 287 bytes', rationale: 'r' }]]);
  calls.candidates[0].content.parts[0].functionCall.id = 'call-1';
  const fetch = fakeFetch(calls, grounded('Akka gives numbers; so should we.', [], [['akka.io', 'https://example.com/akka']]));
  const { reviewer } = await setup({ fetch });
  await reviewer.dispatch({ type: 'chat', text: 'Compare with Akka' });
  const reply = reviewer.getState().chat.messages.at(-1);
  assert.equal(reply.text, 'Akka gives numbers; so should we.');
  assert.deepEqual(reply.sources, [{ title: 'akka.io', uri: 'https://example.com/akka' }]);
  assert.equal(fetch.requests[1].body.contents.at(-1).parts[0].functionResponse.id, 'call-1');
  reviewer.close();
});
