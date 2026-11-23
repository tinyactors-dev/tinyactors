// Pages remember their audiences (charts/audiences.scxml: each page's own
// set, and the one suggestion asked of the model for a page that has none)
// and rewrites side by side (an annotation's per-audience request), against
// a fake page and a fake Gemini.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { setup, fakePage, fakeFetch, geminiReply, find } from './fakes.mjs';
import { createMemoryStore } from '../src/core/store.mjs';
import { readingThinking } from '../src/core/reviewer.mjs';

const TEXT = 'Actors as a library. Four million actors fit in 1.5 GB. Install it with npm. Licensed under MIT.';
const HOME = 'https://www.tinyactors.dev/';
const ABOUT = 'https://www.tinyactors.dev/about/';

const LEAD = { id: 'lead', name: 'Tech lead', description: 'Approves dependencies.', wants: 'The license' };
const DEV = { id: 'dev', name: 'Workflow developer', description: 'Tests workflows.', wants: 'A simulated clock' };

const suggest = (...names) => geminiReply('', [['page_audiences', { names }]]);
const system = (request) => request.body.systemInstruction.parts[0].text;
const functions = (request) => (request.body.tools || []).flatMap((t) => (t.functionDeclarations || []).map((f) => f.name));

// A store that has the two audiences already, so the first page opened can be asked about.
async function seeded() {
  const store = createMemoryStore();
  await store.putAudience(LEAD);
  await store.putAudience(DEV);
  await store.putSettings({ starters: 1, audienceIDs: [] });
  return store;
}

test('a page with no set is asked once which audiences read it: a suggestion, not applied until accepted', async () => {
  const store = await seeded();
  const fetch = fakeFetch(suggest('Tech lead', 'Nobody we know'));
  const { reviewer } = await setup({ store, fetch, text: TEXT });
  await reviewer.dispatch({ type: 'deselect' });  // waits for the page to open
  assert.equal(fetch.requests.length, 1, 'asked at the open');
  const request = fetch.requests[0];
  assert.deepEqual(functions(request), ['page_audiences']);
  assert.deepEqual(request.body.toolConfig, { functionCallingConfig: { mode: 'ANY' } }, 'a call, never words');
  assert.deepEqual(request.body.generationConfig.thinkingConfig, { thinkingLevel: 'low' }, 'cheap: low thinking, no thoughts');
  assert.match(system(request), /- Tech lead: Approves dependencies\.\n- Workflow developer: Tests workflows\./);
  assert.match(system(request), /Licensed under MIT\./, 'the page');

  let state = reviewer.getState();
  assert.deepEqual(state.page.audienceSuggestion, { state: 'ready', audienceIDs: ['lead'] }, 'an unknown name is dropped');
  assert.deepEqual(state.settings.audienceIDs, [], 'not applied');

  await reviewer.dispatch({ type: 'audiences.accept' });
  state = reviewer.getState();
  assert.equal(state.page.audienceSuggestion, null);
  assert.deepEqual(state.settings.audienceIDs, ['lead']);
  assert.deepEqual((await store.pages()).find((p) => p.url === HOME).audienceIDs, ['lead']);
  assert.deepEqual((await store.settings()).audienceIDs, ['lead'], 'the last set used');
  await assert.rejects(reviewer.dispatch({ type: 'audiences.accept' }), /No suggestion/);
  reviewer.close();

  // opened again: its set, and nothing asked
  const again = fakeFetch();
  const second = await setup({ store, fetch: again, text: TEXT });
  await second.reviewer.dispatch({ type: 'deselect' });
  assert.deepEqual(second.reviewer.getState().settings.audienceIDs, ['lead']);
  assert.equal(again.requests.length, 0);
  second.reviewer.close();
});

test('each page has its own set; a page with none writes for the last set used', async () => {
  const store = await seeded();
  const fetch = fakeFetch(suggest(), suggest());  // home and about: none suggested
  const { reviewer, page } = await setup({ store, fetch, text: TEXT });
  await reviewer.dispatch({ type: 'settings.save', audienceIDs: ['lead'] });
  assert.deepEqual(reviewer.getState().settings.audienceIDs, ['lead']);

  page.navigate({ url: ABOUT, title: 'About' });
  await reviewer.dispatch({ type: 'deselect' });
  assert.equal(reviewer.getState().page.url, ABOUT);
  assert.deepEqual(reviewer.getState().settings.audienceIDs, ['lead'], 'no set of its own: the last used');
  assert.equal(reviewer.getState().page.audienceSuggestion, null, 'the model named none');
  await reviewer.dispatch({ type: 'settings.save', audienceIDs: ['dev', 'lead'] });

  page.navigate({ url: HOME, title: 'tinyactors' });
  await reviewer.dispatch({ type: 'deselect' });
  assert.deepEqual(reviewer.getState().settings.audienceIDs, ['lead'], 'home keeps its own');
  page.navigate({ url: ABOUT, title: 'About' });
  await reviewer.dispatch({ type: 'deselect' });
  assert.deepEqual(reviewer.getState().settings.audienceIDs, ['dev', 'lead']);
  assert.equal(fetch.requests.length, 2, 'each page asked once');

  // deleting an audience takes it out of every page's set
  await reviewer.dispatch({ type: 'settings.deleteAudience', id: 'dev' });
  assert.deepEqual(reviewer.getState().settings.audienceIDs, ['lead']);
  reviewer.close();
});

test('a dismissed suggestion keeps the set in use as the page\'s own; a page opened with no key is asked when a key is saved', async () => {
  const store = await seeded();
  await store.putSettings({ starters: 1, audienceIDs: ['dev'] });
  const fetch = fakeFetch({ models: [{ name: 'models/gemini-4-hydrogen', supportedGenerationMethods: ['generateContent'] }] }, suggest('Tech lead', 'Workflow developer'));
  const { reviewer } = await setup({ store, fetch, key: null, text: TEXT });
  await reviewer.dispatch({ type: 'deselect' });
  assert.equal(fetch.requests.length, 0, 'no key: nothing asked');
  assert.equal(reviewer.getState().page.audienceSuggestion, null);

  await reviewer.dispatch({ type: 'settings.save', apiKey: 'AIza-new-key-456' });
  assert.equal(fetch.requests.length, 2, 'the key checked, then the page asked');
  assert.deepEqual(reviewer.getState().page.audienceSuggestion, { state: 'ready', audienceIDs: ['lead', 'dev'] });
  assert.deepEqual(reviewer.getState().settings.audienceIDs, ['dev']);

  await reviewer.dispatch({ type: 'audiences.dismiss' });
  assert.equal(reviewer.getState().page.audienceSuggestion, null);
  assert.deepEqual((await store.pages())[0].audienceIDs, ['dev'], 'the set in use is the page\'s now');
  reviewer.close();
});

test('the reviewer adjusts a suggestion by choosing the set: the suggestion goes', async () => {
  const store = await seeded();
  const { reviewer } = await setup({ store, fetch: fakeFetch(suggest('Tech lead')), text: TEXT });
  await reviewer.dispatch({ type: 'deselect' });
  assert.equal(reviewer.getState().page.audienceSuggestion.state, 'ready');
  await reviewer.dispatch({ type: 'settings.save', audienceIDs: ['lead', 'dev'] });
  assert.equal(reviewer.getState().page.audienceSuggestion, null);
  assert.deepEqual(reviewer.getState().settings.audienceIDs, ['lead', 'dev']);
  reviewer.close();
});

test('use_audiences in the page chat sets the open page\'s set; the export says which audiences each page is for', async () => {
  const store = await seeded();
  const fetch = fakeFetch(suggest(), geminiReply('Writing for the tech lead.', [['use_audiences', { names: ['Tech lead'] }]]));
  const { reviewer, page, clipboard } = await setup({ store, fetch, text: TEXT });
  await reviewer.dispatch({ type: 'chat', text: 'Write for the tech lead.' });
  assert.deepEqual(reviewer.getState().settings.audienceIDs, ['lead']);
  assert.deepEqual((await store.pages()).find((p) => p.url === HOME).audienceIDs, ['lead']);

  page.select('Licensed under MIT.');
  await reviewer.dispatch({ type: 'annotate', note: '' });
  const id = reviewer.getState().annotations[0].id;
  await reviewer.dispatch({ type: 'note', id, text: 'Name the license' });
  await reviewer.dispatch({ type: 'export', to: 'clipboard' });
  assert.match(clipboard[0], /## https:\/\/www\.tinyactors\.dev\/ \(tinyactors\)\n\nFor: Tech lead\n/);
  reviewer.close();
});

test('rewrites side by side: with several audiences active, one rewrite per audience, each tagged', async () => {
  const store = await seeded();
  const fetch = fakeFetch(suggest(), geminiReply('One for each of you.', [
    ['propose_rewrite', { replacement: 'MIT licensed, every file.', rationale: 'For everyone.', audience: 'everyone' }],
    ['propose_rewrite', { replacement: 'Licensed under MIT: no dependency review needed.', rationale: 'The lead checks the license.', audience: 'Tech lead' }],
    ['propose_rewrite', { replacement: 'MIT: use it in your tests freely.', rationale: 'The developer tests.', audience: 'workflow developer' }],
  ]));
  const { reviewer, page, clipboard } = await setup({ store, fetch, text: TEXT });
  await reviewer.dispatch({ type: 'settings.save', audienceIDs: ['lead', 'dev'] });
  page.select('Licensed under MIT.');
  const id = await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'ask', id, text: 'Make it count' });

  const request = fetch.requests.at(-1);
  assert.match(system(request), /one rewrite per audience, a call of propose_rewrite each, with audience set to its name exactly \("Tech lead", "Workflow developer"\)/);
  assert.ok(request.body.tools[0].functionDeclarations.find((f) => f.name === 'propose_rewrite').parameters.properties.audience);
  const a = find(reviewer, id);
  assert.deepEqual(a.suggestions.map((s) => [s.audienceID, s.text]), [
    ['lead', 'Licensed under MIT: no dependency review needed.'],
    ['dev', 'MIT: use it in your tests freely.'],
    [null, 'MIT licensed, every file.'],
  ], 'in the audiences\' order, the one for everyone last');

  // preview and keep work per card
  await reviewer.dispatch({ type: 'preview', id, suggestionID: a.suggestions[1].id });
  assert.match(page.text, /MIT: use it in your tests freely\./);
  await reviewer.dispatch({ type: 'keep', id });
  assert.equal(find(reviewer, id).state, 'kept');
  await reviewer.dispatch({ type: 'export', to: 'clipboard' });
  assert.match(clipboard[0], /Written for: Workflow developer/);

  // more suggestions: one more per audience
  await reviewer.dispatch({ type: 'undo', id });
  fetch.answer(geminiReply('', [['propose_rewrite', { replacement: 'MIT.', rationale: 'Short.', audience: 'Tech lead' }]]));
  await reviewer.dispatch({ type: 'suggest', id });
  assert.match(JSON.stringify(fetch.requests.at(-1).body.contents), /Propose one more rewrite per audience/);
  assert.equal(find(reviewer, id).suggestions.at(-1).audienceID, 'lead');
  reviewer.close();
});

test('one active audience, or a finding, keeps one rewrite at a time: no audienceID', async () => {
  const store = await seeded();
  const fetch = fakeFetch(suggest(), geminiReply('Shorter.', [['propose_rewrite', { replacement: 'MIT licensed.', rationale: 'Short.' }]]));
  const { reviewer, page } = await setup({ store, fetch, text: TEXT });
  await reviewer.dispatch({ type: 'settings.save', audienceIDs: ['lead'] });
  page.select('Licensed under MIT.');
  const id = await reviewer.dispatch({ type: 'annotate', note: 'Shorter' });
  assert.doesNotMatch(system(fetch.requests.at(-1)), /one rewrite per audience/);
  assert.equal('audienceID' in find(reviewer, id).suggestions[0], false);
  reviewer.close();
});

test('a note that asks at once, with two audiences active, asks per audience', async () => {
  const store = await seeded();
  const fetch = fakeFetch(suggest(), geminiReply('', [
    ['propose_rewrite', { replacement: 'A', rationale: 'a', audience: 'Tech lead' }],
    ['propose_rewrite', { replacement: 'B', rationale: 'b', audience: 'Workflow developer' }],
  ]));
  const { reviewer, page } = await setup({ store, fetch, text: TEXT });
  await reviewer.dispatch({ type: 'settings.save', audienceIDs: ['lead', 'dev'] });
  page.select('Install it with npm.');
  const id = await reviewer.dispatch({ type: 'annotate', note: 'Say the command' });
  assert.match(system(fetch.requests.at(-1)), /one rewrite per audience/);
  assert.deepEqual(find(reviewer, id).suggestions.map((s) => s.audienceID), ['lead', 'dev']);
  reviewer.close();
});

test('readings think at medium unless the reviewer picked low', () => {
  assert.equal(readingThinking('high'), 'medium');
  assert.equal(readingThinking('xhigh'), 'medium');
  assert.equal(readingThinking('medium'), 'medium');
  assert.equal(readingThinking('low'), 'low');
});

test('the store\'s pages survive a new reviewer and are listed for every page', async () => {
  const store = await seeded();
  const { reviewer, page } = await setup({ store, fetch: fakeFetch(suggest(), suggest()), page: fakePage({ text: TEXT }) });
  await reviewer.dispatch({ type: 'settings.save', audienceIDs: ['dev'] });
  page.navigate({ url: ABOUT });
  await reviewer.dispatch({ type: 'deselect' });
  reviewer.close();
  const pages = await store.pages();
  assert.deepEqual(pages.map((p) => [p.url, p.audienceIDs, p.state]).sort(), [[HOME, ['dev'], 'set'], [ABOUT, null, 'unset']]);
});
