import { test } from 'node:test';
import assert from 'node:assert/strict';
import { setup, fakeFetch, fakePage, geminiReply, find } from './fakes.mjs';
import { createMemoryStore } from '../src/core/store.mjs';

const twoRewrites = geminiReply('Two plainer headlines.', [
  ['propose_rewrite', { replacement: 'Ship reliable software', rationale: 'Names the outcome.' }],
  ['propose_rewrite', { replacement: 'Run statecharts as actors', rationale: 'Says what it is.' }],
]);

test('boots: ready, the page, default settings, no annotations', async () => {
  const { reviewer } = await setup();
  const s = reviewer.getState();
  assert.equal(s.status, 'ready');
  assert.equal(s.page.url, 'https://www.tinyactors.dev/');
  assert.equal(s.settings.model, 'gemini-4-hydrogen');
  assert.equal(s.settings.models[0].id, 'gemini-4-hydrogen');
  assert.equal(s.settings.hasKey, true);
  assert.match(s.settings.voice.instructions, /short and literal/);
  assert.equal(s.settings.voice.custom, false);
  assert.deepEqual(s.settings.audiences, []);
  assert.deepEqual(s.settings.audienceIDs, []);
  assert.deepEqual(s.annotations, []);
  assert.ok(Object.isFrozen(s) && Object.isFrozen(s.settings));
  reviewer.close();
});

test('annotate, ask, two rewrites, preview, keep; a reload re-applies the kept text', async () => {
  const store = createMemoryStore();
  const fetch = fakeFetch(twoRewrites);
  const { reviewer, page } = await setup({ store, fetch });
  page.select('Build better products');
  assert.deepEqual(reviewer.getState().selection, { text: 'Build better products', fits: true });

  const id = await reviewer.dispatch({ type: 'annotate' });
  assert.equal(find(reviewer, id).state, 'draft');
  assert.equal(reviewer.getState().activeID, id);

  await reviewer.dispatch({ type: 'ask', id, text: 'Too generic.' });
  let a = find(reviewer, id);
  assert.equal(a.state, 'suggested');
  assert.deepEqual(a.suggestions.map((s) => s.text), ['Ship reliable software', 'Run statecharts as actors']);
  assert.deepEqual(a.messages.map((m) => [m.role, m.text]), [['user', 'Too generic.'], ['model', 'Two plainer headlines.']]);

  // What went to Gemini: the key in its header, the model, tools, the voice, the selection.
  const request = fetch.requests[0];
  assert.match(request.url, /\/v1beta\/models\/gemini-4-hydrogen:generateContent$/);
  assert.equal(request.headers['x-goog-api-key'], 'AIza-test-key-123');
  assert.equal(request.body.tools[0].functionDeclarations[0].name, 'propose_rewrite');
  assert.match(request.body.systemInstruction.parts[0].text, /short and literal/);
  assert.match(JSON.stringify(request.body.contents), /Build better products/);

  await reviewer.dispatch({ type: 'preview', id, suggestionID: 's2' });
  a = find(reviewer, id);
  assert.equal(a.state, 'previewing');
  assert.equal(a.chosenID, 's2');
  assert.match(page.text, /^Run statecharts as actors\./);

  await reviewer.dispatch({ type: 'preview', id, suggestionID: 's1' });
  assert.match(page.text, /^Ship reliable software\./);

  await reviewer.dispatch({ type: 'keep', id });
  assert.equal(find(reviewer, id).state, 'kept');
  assert.deepEqual(reviewer.getState().totals, { kept: 1, open: 0, pages: 1 });
  reviewer.close();

  // A new panel on the same page, which shows the original again.
  const fresh = fakePage({ text: 'Build better products. Actors are cheap. Read the guide.' });
  const again = await setup({ store, page: fresh });
  const b = find(again.reviewer, id);
  assert.equal(b.state, 'kept');
  assert.equal(b.chosenID, 's1');
  assert.equal(b.anchorFound, true);
  assert.match(fresh.text, /^Ship reliable software\./);
  again.reviewer.close();
});

test('undo puts the original back; dismiss, reopen and delete', async () => {
  const { reviewer, page } = await setup({ fetch: fakeFetch(twoRewrites) });
  page.select('Actors are cheap');
  const id = await reviewer.dispatch({ type: 'annotate', note: 'Says nothing measurable.' });
  let a = find(reviewer, id);
  assert.equal(a.state, 'suggested', 'a note asks at once');
  assert.equal(a.note, 'Says nothing measurable.');
  assert.equal(a.messages[0].text, 'Says nothing measurable.');

  await reviewer.dispatch({ type: 'preview', id, suggestionID: 's1' });
  await reviewer.dispatch({ type: 'keep', id });
  assert.match(page.text, /Ship reliable software/);
  await reviewer.dispatch({ type: 'undo', id });
  a = find(reviewer, id);
  assert.equal(a.state, 'suggested');
  assert.equal(a.chosenID, null);
  assert.match(page.text, /Actors are cheap/);

  await reviewer.dispatch({ type: 'preview', id, suggestionID: 's2' });
  await reviewer.dispatch({ type: 'dismiss', id });
  assert.equal(find(reviewer, id).state, 'dismissed');
  assert.match(page.text, /Actors are cheap/, 'dismiss reverts the preview');
  await reviewer.dispatch({ type: 'preview', id, suggestionID: 's1' });
  assert.equal(find(reviewer, id).state, 'dismissed', 'a dismissed annotation takes no preview');
  await reviewer.dispatch({ type: 'reopen', id });
  assert.equal(find(reviewer, id).state, 'suggested');

  await reviewer.dispatch({ type: 'delete', id });
  assert.equal(find(reviewer, id), undefined);
  assert.deepEqual(reviewer.getState().totals, { kept: 0, open: 0, pages: 0 });
  await assert.rejects(reviewer.dispatch({ type: 'keep', id }), /No annotation/);
  reviewer.close();
});

test('edit: the user\'s own text is a suggestion, previewed at once; note sets the note', async () => {
  const { reviewer, page } = await setup();
  page.select('Read the guide');
  const id = await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'note', id, text: 'Which guide?' });
  await reviewer.dispatch({ type: 'edit', id, text: 'Read the hibernation guide' });
  const a = find(reviewer, id);
  assert.equal(a.state, 'previewing');
  assert.equal(a.note, 'Which guide?');
  assert.deepEqual(a.suggestions, [{ id: 's1', text: 'Read the hibernation guide', rationale: 'Your own text', own: true, after: 0 }]);
  assert.match(page.text, /Read the hibernation guide\./);
  await reviewer.dispatch({ type: 'keep', id });
  assert.equal(find(reviewer, id).state, 'kept');
  reviewer.close();
});

test('select highlights the text; deselect clears it', async () => {
  const { reviewer, page } = await setup();
  page.select('Read the guide');
  const id = await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'deselect' });
  assert.equal(reviewer.getState().activeID, null);
  await reviewer.dispatch({ type: 'select', id });
  assert.equal(reviewer.getState().activeID, id);
  assert.equal(page.highlighted.at(-2), null);
  assert.equal(page.highlighted.at(-1).quote.exact, 'Read the guide');
  reviewer.close();
});

test('a model error fails the annotation with a clear message; asking again recovers', async () => {
  const fetch = fakeFetch({ status: 429, body: { error: { message: 'Resource exhausted.' } } }, twoRewrites);
  const { reviewer, page } = await setup({ fetch });
  page.select('Build better products');
  const id = await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'ask', id, text: 'Shorter.' });
  let a = find(reviewer, id);
  assert.equal(a.state, 'failed');
  assert.match(a.error, /429/);
  assert.match(a.error, /Resource exhausted/);
  await reviewer.dispatch({ type: 'suggest', id });
  a = find(reviewer, id);
  assert.equal(a.state, 'suggested');
  assert.equal(a.error, undefined);
  assert.match(JSON.stringify(fetch.requests[1].body.contents), /Propose two more alternatives/);
  reviewer.close();
});

test('no key: the annotation fails and says where to add one', async () => {
  const { reviewer, page, fetch } = await setup({ key: null });
  assert.equal(reviewer.getState().settings.hasKey, false);
  page.select('Build better products');
  const id = await reviewer.dispatch({ type: 'annotate', note: 'Generic.' });
  assert.equal(find(reviewer, id).state, 'failed');
  assert.match(find(reviewer, id).error, /Settings/);
  assert.equal(fetch.requests.length, 0);
  reviewer.close();
});

test('a refused key, a 403 and a 400 say what to check', async () => {
  const fetch = fakeFetch(
    { status: 400, body: { error: { code: 400, message: 'API key not valid. Please pass a valid API key.', status: 'INVALID_ARGUMENT', details: [{ reason: 'API_KEY_INVALID' }] } } },
    { status: 403, body: { error: { message: 'Permission denied.' } } },
    { status: 400, body: { error: { message: 'Bad model.' } } });
  const { reviewer, page } = await setup({ fetch });
  page.select('Build better products');
  const id = await reviewer.dispatch({ type: 'annotate', note: 'x' });
  assert.equal(find(reviewer, id).error, 'That key was not accepted by Google. Check it in Settings.', 'not "check the model"');
  await reviewer.dispatch({ type: 'ask', id, text: 'again' });
  assert.match(find(reviewer, id).error, /refused the key \(403\)/);
  await reviewer.dispatch({ type: 'ask', id, text: 'again' });
  assert.match(find(reviewer, id).error, /\(400\): check the model/);
  reviewer.close();
});

test('the page chat: the model\'s rewrites of the page\'s text become annotations', async () => {
  const fetch = fakeFetch(geminiReply('Two lines are vague.', [
    ['propose_rewrite', { original_text: 'Actors are cheap', replacement: 'An actor costs 287 bytes', rationale: 'A number.' }],
    ['propose_rewrite', { original_text: 'Not on this page', replacement: 'Something', rationale: '' }],
    ['propose_rewrite', { replacement: 'No original: skipped', rationale: '' }],
  ]));
  const { reviewer, page, store } = await setup({ fetch });
  await reviewer.dispatch({ type: 'chat', text: 'What is vague here?' });
  const s = reviewer.getState();
  assert.deepEqual(s.chat.messages.map((m) => [m.role, m.text]), [['user', 'What is vague here?'], ['model', 'Two lines are vague.']]);
  assert.equal(s.chat.busy, false);
  assert.equal(s.annotations.length, 2);
  const found = s.annotations.find((a) => a.original === 'Actors are cheap');
  assert.equal(found.state, 'suggested');
  assert.equal(found.source, 'model');
  assert.equal(found.anchorFound, true);
  assert.equal(found.suggestions[0].text, 'An actor costs 287 bytes');
  const missing = s.annotations.find((a) => a.original === 'Not on this page');
  assert.equal(missing.anchorFound, false);
  assert.deepEqual((await store.conversations(page.url))[0].messages.map((m) => m.text), ['What is vague here?', 'Two lines are vague.']);

  await reviewer.dispatch({ type: 'preview', id: found.id, suggestionID: 's1' });
  assert.match(page.text, /An actor costs 287 bytes/);
  reviewer.close();
});

test('another page: its own annotations and chat; back again, the kept change is re-applied', async () => {
  const fetch = fakeFetch();
  const { reviewer, page } = await setup({ fetch });
  page.select('Read the guide');
  const id = await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'edit', id, text: 'Read the guides' });
  await reviewer.dispatch({ type: 'keep', id });

  page.navigate({ url: 'https://www.tinyactors.dev/guide/', title: 'Guide', text: 'A guide page.' });
  await reviewer.dispatch({ type: 'deselect' }); // runs after the navigation
  let s = reviewer.getState();
  assert.equal(s.page.url, 'https://www.tinyactors.dev/guide/');
  assert.deepEqual(s.annotations, []);
  assert.deepEqual(s.totals, { kept: 1, open: 0, pages: 1 });

  page.navigate({ url: 'https://www.tinyactors.dev/', title: 'tinyactors', text: 'Build better products. Actors are cheap. Read the guide.' });
  await reviewer.dispatch({ type: 'deselect' });
  s = reviewer.getState();
  assert.equal(s.annotations[0].state, 'kept');
  assert.match(page.text, /Read the guides\./);
  reviewer.close();
});

test('a kept change whose text left the page: anchorFound is false', async () => {
  const store = createMemoryStore();
  const first = await setup({ store });
  first.page.select('Read the guide');
  const id = await first.reviewer.dispatch({ type: 'annotate' });
  await first.reviewer.dispatch({ type: 'edit', id, text: 'Read on' });
  await first.reviewer.dispatch({ type: 'keep', id });
  first.reviewer.close();
  const second = await setup({ store, page: fakePage({ text: 'The page was rewritten.' }) });
  const a = find(second.reviewer, id);
  assert.equal(a.state, 'kept');
  assert.equal(a.anchorFound, false);
  second.reviewer.close();
});

test('annotate without a selection is refused; an unknown command too', async () => {
  const { reviewer } = await setup();
  await assert.rejects(reviewer.dispatch({ type: 'annotate' }), /Select some text/);
  await assert.rejects(reviewer.dispatch({ type: 'fly' }), /Unknown command/);
  reviewer.close();
});

test('commands do not wait for each other: a note and a selection go through while the model works', async () => {
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  const fetch = fakeFetch(twoRewrites);
  const slow = async (url, init) => { await gate; return fetch(url, init); };
  const { reviewer, page } = await setup({ fetch: slow });
  page.select('Build better products');
  const id = await reviewer.dispatch({ type: 'annotate' });
  const asking = reviewer.dispatch({ type: 'ask', id, text: 'Shorter.' });
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(find(reviewer, id).state, 'asking');
  await reviewer.dispatch({ type: 'deselect' });
  reviewer.dispatch({ type: 'note', id, text: 'Typed while it works.' });
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(find(reviewer, id).note, 'Typed while it works.');
  assert.equal(reviewer.getState().activeID, null);
  release();
  await asking;
  assert.equal(find(reviewer, id).state, 'suggested');
  reviewer.close();
});

test('a selection across blocks, or over 600 characters, is not annotated', async () => {
  const long = `${'A sentence of copy that goes on. '.repeat(20)}End.`;
  const { reviewer, page } = await setup({ text: `Build better products. ${long}` });
  page.select('Build better products', { fits: false });
  assert.deepEqual(reviewer.getState().selection, { text: 'Build better products', fits: false });
  await assert.rejects(reviewer.dispatch({ type: 'annotate' }), /^Error: Select one sentence, heading or paragraph\.$/);
  page.select(long);
  assert.equal(reviewer.getState().selection.fits, false, `${long.length} characters`);
  await assert.rejects(reviewer.dispatch({ type: 'annotate' }), /Select one sentence/);
  assert.deepEqual(reviewer.getState().annotations, []);
  reviewer.close();
});

test('the page chat: a reply of function calls only shows the proposals, and names the annotations it made', async () => {
  const fetch = fakeFetch(geminiReply('', [
    ['propose_rewrite', { original_text: 'Actors are cheap', replacement: 'An actor costs 287 bytes', rationale: 'A number.' }],
  ]));
  const { reviewer, page } = await setup({ fetch });
  await reviewer.dispatch({ type: 'chat', text: 'Weakest sentence?' });
  const s = reviewer.getState();
  const reply = s.chat.messages.at(-1);
  assert.equal(reply.text, '“Actors are cheap” → An actor costs 287 bytes');
  assert.equal(reply.proposed, true);
  assert.equal(reply.annotations.length, 1);
  const made = find(reviewer, reply.annotations[0]);
  assert.equal(made.original, 'Actors are cheap');
  assert.equal(made.suggestions[0].after, 0);
  assert.match(JSON.stringify(fetch.requests[0].body.systemInstruction), /Always answer the reviewer in one or two sentences/);
  // the anchor is the page adapter's (page.anchor): the text around it too
  await reviewer.dispatch({ type: 'select', id: made.id });
  assert.deepEqual(page.highlighted.at(-1), page.anchor('Actors are cheap'));
  assert.notEqual(page.highlighted.at(-1).quote.prefix, '');
  reviewer.close();
});

test('the page chat: a failure is the chat\'s error, not a message; the next message clears it', async () => {
  const fetch = fakeFetch({ status: 429, body: { error: { message: 'Slow down.' } } }, geminiReply('Fine now.'));
  const { reviewer } = await setup({ fetch });
  await reviewer.dispatch({ type: 'chat', text: 'Hello?' });
  let chat = reviewer.getState().chat;
  assert.deepEqual(chat.messages.map((m) => m.role), ['user']);
  assert.match(chat.error, /429/);
  await reviewer.dispatch({ type: 'chat', text: 'Again?' });
  chat = reviewer.getState().chat;
  assert.equal(chat.error, undefined);
  assert.equal(chat.messages.at(-1).text, 'Fine now.');
  assert.equal(chat.messages.at(-1).proposed, undefined);
  reviewer.close();
});

test('the page chat: function calls only, then the calls\' results go back and the model says it in words', async () => {
  const fetch = fakeFetch(
    geminiReply('', [['propose_rewrite', { original_text: 'Actors are cheap', replacement: 'An actor costs 287 bytes', rationale: 'A number.' }]]),
    geminiReply('“Actors are cheap” is the vaguest line: the page measures an actor, so say the number.'));
  const { reviewer } = await setup({ fetch });
  await reviewer.dispatch({ type: 'chat', text: 'Weakest sentence?' });
  const reply = reviewer.getState().chat.messages.at(-1);
  assert.equal(reply.text, '“Actors are cheap” is the vaguest line: the page measures an actor, so say the number.');
  assert.equal(reply.proposed, undefined);
  assert.equal(reply.annotations.length, 1);
  const again = fetch.requests[1].body.contents;
  assert.deepEqual(again.at(-2).parts[0].functionCall.name, 'propose_rewrite', 'the model\'s turn as it came');
  assert.equal(again.at(-1).parts[0].functionResponse.name, 'propose_rewrite');
  reviewer.close();
});

test('a request Google turns away for load (503) is retried, and answers the second time', async () => {
  const { retrying } = await import('../src/core/gemini.mjs');
  const answers = [new Response('{}', { status: 503 }), new Response('{"ok":true}', { status: 200 })];
  const waits = [];
  const fetch = retrying(async () => answers.shift(), [10, 20], async (ms) => { waits.push(ms); });
  const response = await fetch('https://example.invalid', {});
  assert.equal(response.status, 200);
  assert.deepEqual(waits, [10]);
  const refused = retrying(async () => new Response('{}', { status: 400 }), [10], async (ms) => waits.push(ms));
  assert.equal((await refused('x', {})).status, 400, 'a 400 is not retried');
  assert.deepEqual(waits, [10]);
});
