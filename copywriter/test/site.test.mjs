// Pages that say what they are for (website/README.md, "Audiences and page
// kinds: the convention"): the audience files a page links seed the store
// and are its set, their needs are the checklist with their ids, a reading
// judges each from the page's text, the page's kind goes into the
// instructions, and the export names needs, kinds and the audiences the
// reviewer edited. Against a fake page, the published audience files
// (test/fixtures/audiences/) and a fake Gemini.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { setup, fakePage, fakeFetch, fakeFiles, audienceFile, geminiReply } from './fakes.mjs';
import { createMemoryStore } from '../src/core/store.mjs';
import { parseAudience, parseVoice, voiceInstructions, parseXML, itemsOf, differences, AUDIENCE_NS } from '../src/core/site.mjs';
import { buildPacket } from '../src/core/packet.mjs';

const SITE = 'https://www.tinyactors.dev';
const file = (id) => `${SITE}/audiences/${id}.xml`;
const TEXT = 'Actors as a library. Four million actors fit in 1.5 GB. When the process dies. A snapshot keeps every actor. Install it with npm.';
const HOME = {
  kind: 'pitch',
  audiences: [file('agent-platform'), file('tech-lead'), file('workflow-tester')],
};
const TESTING = { kind: 'how-to', audiences: [file('workflow-tester'), file('agent-platform')] };

const homePage = (purpose = HOME) => fakePage({ url: `${SITE}/`, title: 'tinyactors', text: TEXT, purpose });
const system = (request) => request.body.systemInstruction.parts[0].text;
const ids = (reviewer) => reviewer.getState().settings.audiences.map((a) => a.id);

test('an audience file parses into its name, description, knowledge and needs with their ids', () => {
  const lead = parseAudience(audienceFile('tech-lead'), { DOMParser: null });
  assert.equal(lead.id, 'tech-lead');
  assert.equal(lead.name, 'Tech lead who approves dependencies');
  assert.match(lead.knows, /sqlite\.org's "when to use" page/, 'entities and quotes read');
  assert.deepEqual(lead.items.slice(0, 2), [
    { id: 'when-to-use', field: 'wants', text: 'An honest "use it when, do not use it when"' },
    { id: 'failure-semantics', field: 'wants', text: 'The failure semantics of one process: what is lost, what a snapshot or migration keeps' },
  ]);
  assert.deepEqual(lead.items.map((x) => x.field).filter((f, i, all) => all.indexOf(f) === i), ['wants', 'tasks', 'objections']);
  for (const name of ['agent-platform', 'workflow-tester']) assert.ok(parseAudience(audienceFile(name), { DOMParser: null }).items.length >= 10, name);
  // namespaces are resolved, not matched by prefix
  const prefixed = `<a:audience xmlns:a="${AUDIENCE_NS}" id="x"><a:name>X &amp; Y</a:name><a:want id="w">W</a:want><b:want xmlns:b="urn:other" id="no">No</b:want></a:audience>`;
  assert.deepEqual(parseAudience(prefixed, { DOMParser: null }).items, [{ id: 'w', field: 'wants', text: 'W' }]);
  assert.equal(parseAudience(prefixed, { DOMParser: null }).name, 'X & Y');
  assert.throws(() => parseAudience(readFileSync(new URL('./fixtures/audiences/index.xml', import.meta.url), 'utf8'), { DOMParser: null }), /Not an audience file/);
  assert.throws(() => parseAudience('<audience id="x"><name>X</name></audience>', { DOMParser: null }), /Not an audience file/, 'no namespace');
  assert.throws(() => parseXML('<a><b></a>'), /Not well-formed/);
});

test('opening a page that links audiences adds them to the store, in its order, as its set; nothing is asked of the model', async () => {
  const fetch = fakeFetch();
  const fetchFile = fakeFiles();
  const store = createMemoryStore();
  const { reviewer } = await setup({ page: homePage(), fetch, fetchFile, store });
  const state = reviewer.getState();
  assert.deepEqual(fetchFile.asked.filter((u) => u.endsWith('.xml')), HOME.audiences);
  assert.deepEqual(state.settings.audienceIDs, ['site-agent-platform', 'site-tech-lead', 'site-workflow-tester'], 'the links\' order');
  assert.equal(state.page.kind, 'pitch');
  assert.equal(state.page.mainAudienceID, 'site-agent-platform');
  assert.equal(state.page.audienceSuggestion, null);
  assert.equal(fetch.requests.length, 0, 'no suggestion asked');
  const lead = state.settings.audiences.find((a) => a.id === 'site-tech-lead');
  assert.equal(lead.source, 'site');
  assert.equal(lead.url, file('tech-lead'));
  assert.equal(lead.siteID, 'tech-lead');
  assert.equal(lead.diverged, false);
  assert.equal(lead.items.length, 13);
  assert.equal(lead.items[1].id, 'failure-semantics');
  assert.match(lead.wants, /^An honest "use it when, do not use it when"\nThe failure semantics/, 'one need a line');
  assert.equal(lead.site, undefined, 'the file\'s copy stays out of ViewState');
  const saved = (await store.audiences()).find((a) => a.id === 'site-tech-lead');
  assert.match(saved.hash, /^[0-9a-f]{16}$/);
  assert.deepEqual((await store.pages())[0].from, 'site');
  assert.deepEqual((await store.settings()).audienceIDs ?? [], [], 'the last set used is the reviewer\'s, not the site\'s');
  reviewer.close();
});

test('another page links its own set, main audience first; a set the reviewer chose is kept against the links', async () => {
  const page = homePage();
  const { reviewer } = await setup({ page });
  await reviewer.dispatch({ type: 'settings.save', audienceIDs: ['site-tech-lead'] });
  page.navigate({ url: `${SITE}/guides/testing/`, title: 'Testing', declared: TESTING });
  await reviewer.dispatch({ type: 'deselect' });
  let state = reviewer.getState();
  assert.deepEqual(state.settings.audienceIDs, ['site-workflow-tester', 'site-agent-platform']);
  assert.equal(state.page.kind, 'how-to');
  assert.equal(state.page.mainAudienceID, 'site-workflow-tester');
  page.navigate({ url: `${SITE}/`, title: 'tinyactors', declared: HOME });
  await reviewer.dispatch({ type: 'deselect' });
  state = reviewer.getState();
  assert.deepEqual(state.settings.audienceIDs, ['site-tech-lead'], 'the reviewer\'s choice wins');
  reviewer.close();
});

test('a page without links keeps the store as it is and is asked for a suggestion', async () => {
  const store = createMemoryStore();
  await store.putAudience({ id: 'mine', name: 'My reader', description: 'Mine.' });
  const fetch = fakeFetch(geminiReply('', [['page_audiences', { names: ['My reader'] }]]));
  const fetchFile = fakeFiles();
  const { reviewer } = await setup({ store, fetch, fetchFile, text: TEXT });
  for (let i = 0; i < 200 && reviewer.getState().page.audienceSuggestion?.state !== 'ready'; i++) await new Promise((r) => setTimeout(r, 5));
  assert.deepEqual(reviewer.getState().page.audienceSuggestion, { state: 'ready', audienceIDs: ['mine'] });
  assert.deepEqual(fetchFile.asked.filter((u) => u.endsWith('.xml')), []);
  assert.deepEqual(ids(reviewer), ['mine']);
  assert.equal(reviewer.getState().page.kind, null);
  reviewer.close();
});

test('a changed file updates the stored copy; an edited one keeps the edit, is marked diverged, and the export asks to update the site', async () => {
  const store = createMemoryStore();
  const first = await setup({ page: homePage(), store });
  first.reviewer.close();

  // the site rewords a need: the stored copy follows
  const reworded = audienceFile('tech-lead').replace('<task id="decide">Decide yes or no</task>', '<task id="decide">Decide yes or no, this week</task>');
  const second = await setup({ page: homePage(), store, fetchFile: fakeFiles({ [file('tech-lead')]: reworded }) });
  let lead = second.reviewer.getState().settings.audiences.find((a) => a.id === 'site-tech-lead');
  assert.match(lead.tasks, /^Decide yes or no, this week$/m);
  assert.equal(lead.diverged, false);

  // the reviewer edits it: one need reworded, one added
  await second.reviewer.dispatch({
    type: 'settings.save',
    audience: { ...lead, tasks: `${lead.tasks.replace('Write the case for the team', 'Write a one-page case for the team')}\nAsk the security team` },
  });
  lead = second.reviewer.getState().settings.audiences.find((a) => a.id === 'site-tech-lead');
  assert.equal(lead.diverged, true);
  assert.equal(lead.source, 'site', 'still the site\'s');
  const tasks = lead.items.filter((x) => x.field === 'tasks');
  assert.deepEqual(tasks.map((x) => x.id), ['decide', 'write-case', 'check-portability', 'ask-the-security-team'], 'a reworded need keeps its id');
  second.reviewer.close();

  // the file changes again: the edit stays
  const third = await setup({ page: homePage(), store, fetchFile: fakeFiles({ [file('tech-lead')]: reworded.replace('Who maintains it?', 'Who maintains it, and for how long?') }) });
  lead = third.reviewer.getState().settings.audiences.find((a) => a.id === 'site-tech-lead');
  assert.equal(lead.diverged, true);
  assert.match(lead.tasks, /Write a one-page case/);
  assert.match(lead.objections, /^Who maintains it\?$/m, 'the reviewer\'s copy, not the new file');
  const stored = (await store.audiences()).find((a) => a.id === 'site-tech-lead');
  assert.deepEqual(differences(stored).map((d) => d.field), ['tasks', 'objections'], 'against the newest file');

  await third.reviewer.dispatch({ type: 'export', to: 'clipboard' });
  const changes = third.clipboard[0];
  assert.match(changes, /## Audiences to update/);
  assert.match(changes, /Update the audience "tech-lead" in website\/content\/site\.xml \(published as https:\/\/www\.tinyactors\.dev\/audiences\/tech-lead\.xml\): task, objection\./);
  assert.match(changes, /Write a one-page case for the team\nCheck the license and portability\nAsk the security team/);
  third.reviewer.close();
});

test('a starting audience of the same name is replaced by the site\'s, and the sets that named it name the site\'s', async () => {
  const store = createMemoryStore();
  await store.putAudience({ id: 'starter-tech-lead', name: 'Tech lead who approves dependencies', description: 'Old.', source: 'starter' });
  await store.putAudience({ id: 'starter-other', name: 'Someone else', description: 'Stays.', source: 'starter' });
  await store.putSettings({ starters: 1, audienceIDs: ['starter-tech-lead', 'starter-other'] });
  await store.putPage({ url: `${SITE}/about/`, audienceIDs: ['starter-tech-lead'], suggestion: null, state: 'set' });
  const { reviewer } = await setup({ page: homePage(), store });
  assert.deepEqual(ids(reviewer).sort(), ['site-agent-platform', 'site-tech-lead', 'site-workflow-tester', 'starter-other']);
  assert.deepEqual((await store.settings()).audienceIDs, ['site-tech-lead', 'starter-other']);
  assert.equal((await store.settings()).starters, undefined);
  assert.deepEqual((await store.pages()).find((p) => p.url === `${SITE}/about/`).audienceIDs, ['site-tech-lead']);
  reviewer.close();
});

test('a file that cannot be fetched keeps the stored copy; one never stored is left out', async () => {
  const store = createMemoryStore();
  const first = await setup({ page: homePage({ ...HOME, audiences: [file('tech-lead')] }), store });
  first.reviewer.close();
  const fetchFile = fakeFiles({ [file('tech-lead')]: new Error('offline'), [file('agent-platform')]: new Error('offline') });
  const { reviewer } = await setup({ page: homePage({ ...HOME, audiences: [file('agent-platform'), file('tech-lead')] }), store, fetchFile });
  assert.deepEqual(reviewer.getState().settings.audienceIDs, ['site-tech-lead']);
  reviewer.close();
});

// A reading as the tech lead on the home page: one need answered, one
// missing, and a doubt about the missing one.
const READING = (n) => geminiReply('', [
  ['report_coverage', { item: n('failure-semantics'), status: 'answered', answer_text: 'A snapshot keeps every actor.', explanation: 'It says what a snapshot keeps.', kind: 'explanation' }],
  ['report_coverage', { item: n('same-everywhere'), status: 'missing', explanation: 'The section shows memory, not portability.', kind: 'reference' }],
  ['report_coverage', { item: n('decide'), status: 'partial', explanation: 'No limits stated.', kind: 'pitch' }],
  ['report_finding', { kind: 'doubt', item: n('same-everywhere'), original_text: 'Four million actors fit in 1.5 GB.', need: 'Show the chart running in each runtime', explanation: 'Memory says nothing of running the same everywhere.', replacement: '' }],
]);

test('a reading checks the file\'s needs with their ids and the page\'s kind, from the text alone; a doubt names its need', async () => {
  const fetch = fakeFetch();
  const { reviewer } = await setup({ page: homePage(), fetch });
  const lead = reviewer.getState().settings.audiences.find((a) => a.id === 'site-tech-lead');
  const number = (id) => lead.items.findIndex((x) => x.id === id) + 1;
  fetch.answer(READING(number));
  await reviewer.dispatch({ type: 'read', audienceIDs: ['site-tech-lead'] });
  const asked = system(fetch.requests[0]);
  assert.match(asked, /The page is a pitch: the reader is deciding whether to adopt it/);
  assert.match(asked, /2\. \[Wants\] The failure semantics of one process: what is lost, what a snapshot or migration keeps \(tech-lead\.failure-semantics\)/);
  assert.ok(!/claim(s|ed) (to answer|by)/.test(asked), 'the page lists no claims: the reading judges the text');
  const declared = fetch.requests[0].body.tools[0].functionDeclarations;
  assert.ok(declared.find((f) => f.name === 'report_coverage').parameters.required.includes('kind'));

  const reading = reviewer.getState().readings.find((r) => r.audienceID === 'site-tech-lead');
  assert.equal(reading.items.length, 13, 'one item per need of the file');
  const byNeed = Object.fromEntries(reading.items.map((x) => [x.needID, x]));
  assert.deepEqual(Object.keys(byNeed).slice(0, 3), ['tech-lead.when-to-use', 'tech-lead.failure-semantics', 'tech-lead.same-everywhere'], 'in the file\'s order');
  assert.equal(byNeed['tech-lead.failure-semantics'].status, 'answered');
  assert.equal(byNeed['tech-lead.failure-semantics'].kind, 'explanation');
  assert.equal(byNeed['tech-lead.failure-semantics'].claimed, undefined, 'no item is claimed');
  assert.equal(byNeed['tech-lead.same-everywhere'].status, 'missing');
  assert.equal(byNeed['tech-lead.decide'].kind, 'pitch');
  const doubt = reviewer.getState().annotations.find((a) => a.finding?.kind === 'doubt');
  assert.equal(doubt.finding.needID, 'tech-lead.same-everywhere');
  reviewer.close();
});

test('the export carries the page\'s kind and need ids; open items made of gaps keep theirs', async () => {
  const fetch = fakeFetch();
  const { reviewer, clipboard } = await setup({ page: homePage(), fetch });
  const lead = reviewer.getState().settings.audiences.find((a) => a.id === 'site-tech-lead');
  fetch.answer(READING((id) => lead.items.findIndex((x) => x.id === id) + 1));
  await reviewer.dispatch({ type: 'read', audienceIDs: ['site-tech-lead'] });
  const index = reviewer.getState().readings[0].items.findIndex((x) => x.needID === 'tech-lead.decide');
  const opened = await reviewer.dispatch({ type: 'reading.open', audienceID: 'site-tech-lead', index });
  assert.equal(reviewer.getState().annotations.find((a) => a.id === opened).finding.needID, 'tech-lead.decide');
  await reviewer.dispatch({ type: 'export', to: 'clipboard' });
  const md = clipboard[0];
  assert.match(md, /Kind: pitch\. For: Agent-platform engineer, Tech lead who approves dependencies, Workflow developer who tests/);
  assert.match(md, /Need: tech-lead\.same-everywhere/);
  assert.match(md, /Need: tech-lead\.decide/);
  assert.match(md, /- Answer: Who maintains it\? \(not answered, tech-lead\.maintainer\)/);
  assert.match(md, /- Give them: One chart running the same everywhere \(not answered, tech-lead\.same-everywhere, belongs on a reference\)/);
  reviewer.close();
});

test('rewrites and the page chat are told the page\'s kind, what it requires, its main audience, and that the kind wins', async () => {
  const fetch = fakeFetch(geminiReply('Sure.', []));
  const { reviewer, page } = await setup({ page: homePage(), fetch });
  page.select('Install it with npm.');
  await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'chat', text: 'Is the page clear?' });
  const asked = system(fetch.requests.at(-1));
  assert.match(asked, /The page is a pitch: the reader is deciding whether to adopt it: a claim, a demonstration that runs, proof with its source, a next step\./);
  assert.match(asked, /Its main audience: Agent-platform engineer\./);
  assert.match(asked, /a need that belongs on another kind of page is answered here with a link to where it is answered/);
  reviewer.close();
});

test('itemsOf keeps the ids of reworded and moved needs, and gives a new one an id of its words', () => {
  const site = parseAudience(audienceFile('workflow-tester'), { DOMParser: null });
  const audience = { source: 'site', site, tasks: 'Write one workflow as a chart\nRun its timeouts in CI\nShow the team the speedup\nDemo it on Friday' };
  const tasks = itemsOf(audience).filter((x) => x.field === 'tasks');
  assert.deepEqual(tasks.map((x) => x.id), ['one-workflow', 'test-timeouts', 'show-speedup', 'demo-it-on-friday']);
});

test('a packet with only an audience to update is valid to build', async () => {
  const { xml, changes } = await buildPacket({
    annotations: [],
    audiences: [{ siteID: 'tech-lead', url: file('tech-lead'), name: 'Tech lead', differences: [{ field: 'knows', site: 'A', yours: 'B' }] }],
    at: '2026-11-23T12:00:00.000Z',
  });
  assert.match(xml, /<design-task id="audience-item-1" observationRef="audience-observation-1" status="open"><summary>Update the audience "tech-lead"/);
  assert.match(changes, /knows:\n\nThe site:/);
});

const voiceFile = (id) => `${SITE}/voices/${id}.xml`;

test('a voice file parses into its name, kinds and rules, and a page that links one is written in it', async () => {
  const tutorial = parseVoice(readFileSync(new URL('./fixtures/voices/tutorial.xml', import.meta.url), 'utf8'));
  assert.equal(tutorial.name, 'Tutorial');
  assert.deepEqual(tutorial.kinds, ['tutorial']);
  assert.ok(tutorial.rules.some((r) => r.id === 'paragraphs' && /passages of two or three sentences/.test(r.text)), 'the tutorial\'s own paragraphs rule');
  assert.match(voiceInstructions(tutorial), /^This page is written in the site's "Tutorial" voice: /);
  assert.throws(() => parseVoice(audienceFile('tech-lead')), /Not a voice file/);

  const fetch = fakeFetch(geminiReply('Sure.', []));
  const fetchFile = fakeFiles();
  const { reviewer, page } = await setup({ page: homePage({ ...HOME, kind: 'tutorial', voice: voiceFile('tutorial') }), fetch, fetchFile });
  assert.deepEqual(reviewer.getState().settings.voice.page, { name: 'Tutorial', url: voiceFile('tutorial') });
  page.select('Install it with npm.');
  await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'chat', text: 'Is the page clear?' });
  const asked = system(fetch.requests.at(-1));
  assert.match(asked, /The voice \(how to write\):\nThis page is written in the site's "Tutorial" voice/);
  assert.match(asked, /passages of two or three sentences/);
  assert.doesNotMatch(asked, /A paragraph is one or two sentences of at most 21 words/, 'the default voice\'s rule is replaced');
  reviewer.close();
});

test('a page whose voice file cannot be read, or that links none, is written in the reviewer\'s voice', async () => {
  const fetch = fakeFetch(geminiReply('Sure.', []));
  const fetchFile = fakeFiles({ [voiceFile('gone')]: new Error('404') });
  const { reviewer, page } = await setup({ page: homePage({ ...HOME, voice: voiceFile('gone') }), fetch, fetchFile });
  assert.equal(reviewer.getState().settings.voice.page, null);
  page.select('Install it with npm.');
  await reviewer.dispatch({ type: 'annotate' });
  await reviewer.dispatch({ type: 'chat', text: 'Is the page clear?' });
  assert.match(system(fetch.requests.at(-1)), /The voice \(how to write\):\nYou edit the copy of www\.tinyactors\.dev/);
  reviewer.close();
});
