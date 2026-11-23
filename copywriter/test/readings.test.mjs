// Reading the page as an audience: the reading actors (charts/reading.scxml)
// against a fake page and a fake Gemini that answers each reader with
// report_finding and report_coverage calls.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setup, fakePage, geminiReply, find } from './fakes.mjs';
import { createMemoryStore } from '../src/core/store.mjs';
import { checklist, STARTER_AUDIENCES } from '../src/core/audiences.mjs';
import { readingOf } from '../src/core/gemini.mjs';
import { unzip } from '../src/core/packet.mjs';

const TEXT = 'Actors as a library. Four million actors fit in 1.5 GB. Install it with npm. Statecharts run the same everywhere. Licensed under MIT.';

const LEAD = {
  id: 'lead', name: 'Tech lead', description: 'Approves dependencies.',
  wants: 'An honest "use it when"; the license',
  tasks: 'Decide yes or no, check the license',
  objections: '"Who maintains it?" "What happens when the host dies?"',
};
const DEV = { id: 'dev', name: 'Workflow developer', description: 'Tests workflows.', wants: 'A simulated clock' };

// The checklist of LEAD: 1 use it when, 2 the license, 3 decide, 4 check the license, 5 who maintains, 6 host dies.
const finding = (args) => ['report_finding', args];
const coverage = (item, status, explanation, answer_text) => ['report_coverage', { item, status, explanation, ...(answer_text ? { answer_text } : {}) }];

const LEAD_READING = [
  finding({ kind: 'doubt', original_text: 'Four million actors fit in 1.5 GB.', need: 'Link the benchmark', explanation: 'Measured how, on what machine?', replacement: 'Four million actors fit in 1.5 GB in the published benchmark.' }),
  finding({ kind: 'jargon', original_text: 'Statecharts run the same everywhere.', need: 'Say what a statechart is', explanation: 'I do not know what a statechart is.' }),
  finding({ kind: 'missing', need: 'Add a "when not to use it" section', explanation: 'I cannot tell when to say no.', section: 'Actors as a library' }),
  finding({ kind: 'objection', original_text: 'Text that is not on the page.', need: 'Say who maintains it', explanation: 'Nobody is named as the maintainer.' }),
  finding({ kind: 'nonsense', explanation: 'dropped' }),
  coverage(1, 'missing', 'No such section.'),
  coverage(2, 'answered', 'The MIT license is named.', 'Licensed under MIT.'),
  coverage(3, 'partial', 'Some facts, no limits.'),
  coverage(4, 'answered', 'MIT.', 'Licensed under MIT.'),
  coverage(5, 'missing', 'No maintainer.'),
  // item 6 left out: missing
];

// A fetch that answers each reader by the name in "Read the page as …",
// in the order given per reader, holding an answer until it is released
// when the reader's queue entry is a function returning a promise.
function readerFetch(answers) {
  const requests = [];
  const fetch = async (url, init = {}) => {
    const body = init.body ? JSON.parse(init.body) : undefined;
    requests.push({ url: String(url), body });
    const asked = JSON.stringify(body?.contents || []);
    const name = Object.keys(answers).find((n) => asked.includes(`Read the page as ${n}.`)) ?? 'chat';
    const queue = answers[name];
    if (!queue?.length) throw new Error(`readerFetch: no answer left for ${name}`);
    let answer = queue.shift();
    if (typeof answer === 'function') answer = await answer();
    const { status = 200, body: reply } = answer && answer.status ? answer : { body: answer };
    return new Response(JSON.stringify(reply), { status, headers: { 'content-type': 'application/json' } });
  };
  fetch.requests = requests;
  return fetch;
}

async function withAudiences(options) {
  const env = await setup({ text: TEXT, ...options });
  await env.reviewer.dispatch({ type: 'settings.save', audience: LEAD });
  await env.reviewer.dispatch({ type: 'settings.save', audience: DEV });
  await env.reviewer.dispatch({ type: 'settings.save', audienceIDs: ['lead', 'dev'] });
  return env;
}

const readingOfLead = (reviewer) => reviewer.getState().readings.find((r) => r.audienceID === 'lead');
const findings = (reviewer, audienceID = 'lead') => reviewer.getState().annotations.filter((a) => a.finding?.audienceID === audienceID);

test('checklist: one item per quoted question, per semicolon part and sentence; tasks part at commas', () => {
  assert.deepEqual(checklist(LEAD).map((x) => [x.field, x.text]), [
    ['wants', 'An honest "use it when"'], ['wants', 'The license'],
    ['tasks', 'Decide yes or no'], ['tasks', 'check the license'],
    ['objections', '"Who maintains it?"'], ['objections', '"What happens when the host dies?"'],
  ]);
  const lead = STARTER_AUDIENCES.find((a) => a.id === 'starter-tech-lead');
  assert.equal(checklist(lead).length, 11, 'the starting tech lead: four wants, three tasks, four objections');
  // an item left out is missing; a number out of range is dropped
  const { coverage: c } = readingOf({ calls: [{ name: 'report_coverage', args: { item: 9, status: 'answered', explanation: 'x' } }] }, checklist(DEV));
  assert.deepEqual(c, [{ field: 'wants', text: 'A simulated clock', status: 'missing', explanation: '' }]);
});

test('read as an audience: findings become annotations where the reader gets stuck, the checklist comes with the reading', async () => {
  const fetch = readerFetch({ 'Tech lead': [geminiReply('', LEAD_READING)] });
  const { reviewer, page } = await withAudiences({ fetch });
  await reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] });

  // one request: only the reading's functions, the reader, the numbered checklist
  assert.equal(fetch.requests.length, 1);
  const asked = fetch.requests[0].body;
  assert.deepEqual(asked.tools[0].functionDeclarations.map((f) => f.name), ['report_finding', 'report_coverage']);
  const system = asked.systemInstruction.parts[0].text;
  assert.match(system, /- Tech lead: Approves dependencies\./);
  assert.match(system, /1\. \[Wants\] An honest "use it when"\n2\. \[Wants\] The license\n3\. \[Came to\] Decide yes or no/);
  assert.match(system, /6\. \[Objections\] "What happens when the host dies\?"/);
  assert.match(system, /Licensed under MIT\./, 'the page text');

  const r = readingOfLead(reviewer);
  assert.equal(r.state, 'done');
  assert.ok(r.at);
  assert.deepEqual(r.items.map((x) => [x.status, Boolean(x.anchor)]), [
    ['missing', false], ['answered', true], ['partial', false], ['answered', true], ['missing', false], ['missing', false],
  ]);
  assert.equal(r.items[1].anchor.quote.exact, 'Licensed under MIT.');
  assert.equal(r.items[1].explanation, 'The MIT license is named.');

  const made = findings(reviewer);
  assert.equal(made.length, 4, 'the finding of an unknown kind is dropped');
  assert.deepEqual(r.findingIDs.slice().sort(), made.map((a) => a.id).sort());
  const byKind = Object.fromEntries(made.map((a) => [a.finding.kind, a]));
  // anchored, with its rewrite as the first suggestion, written for the audience
  assert.equal(byKind.doubt.source, 'audience');
  assert.equal(byKind.doubt.original, 'Four million actors fit in 1.5 GB.');
  assert.equal(byKind.doubt.state, 'suggested');
  assert.equal(byKind.doubt.suggestions[0].text, 'Four million actors fit in 1.5 GB in the published benchmark.');
  assert.equal(byKind.doubt.suggestions[0].rationale, 'Measured how, on what machine?');
  assert.deepEqual(byKind.doubt.audiences, ['Tech lead']);
  assert.equal(byKind.doubt.pageLevel, undefined);
  assert.equal(byKind.jargon.state, 'draft', 'no rewrite: a draft');
  // page-level: what the page lacks, and text the model named that is not on the page
  assert.equal(byKind.missing.pageLevel, true);
  assert.equal(byKind.missing.original, 'Add a "when not to use it" section');
  assert.equal(byKind.missing.finding.section, 'Actors as a library');
  assert.equal(byKind.missing.anchorFound, true, 'page-level is not "no longer on the page"');
  assert.equal(byKind.objection.pageLevel, true);
  assert.equal(byKind.objection.original, 'Say who maintains it');

  // the keep flow works unchanged: preview, keep
  await reviewer.dispatch({ type: 'preview', id: byKind.doubt.id, suggestionID: 's1' });
  assert.match(page.text, /in the published benchmark/);
  await reviewer.dispatch({ type: 'keep', id: byKind.doubt.id });
  assert.equal(find(reviewer, byKind.doubt.id).state, 'kept');
  // a page-level one is never previewed
  await reviewer.dispatch({ type: 'edit', id: byKind.missing.id, text: 'Use it when one process holds your state.' });
  assert.equal(find(reviewer, byKind.missing.id).state, 'suggested');
  assert.ok(!page.calls.some((c) => c[0] === 'preview' && c[1] === byKind.missing.id));
  reviewer.close();
});

test('read as all active: the readings run side by side', async () => {
  const gates = [];
  const held = () => new Promise((resolve) => gates.push(resolve));
  const fetch = readerFetch({
    'Tech lead': [() => held().then(() => geminiReply('', LEAD_READING))],
    'Workflow developer': [() => held().then(() => geminiReply('', [coverage(1, 'missing', 'No clock.')]))],
  });
  const { reviewer } = await withAudiences({ fetch });
  const reading = reviewer.dispatch({ type: 'read' });
  for (let i = 0; i < 200 && gates.length < 2; i++) await new Promise((resolve) => setTimeout(resolve, 5));
  assert.equal(gates.length, 2, 'both requests are out before either answers');
  assert.deepEqual(reviewer.getState().readings.map((r) => [r.audienceID, r.state]), [['lead', 'reading'], ['dev', 'reading']]);
  await assert.rejects(reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] }), /Already reading as Tech lead/);
  for (const release of gates) release();
  await reading;
  assert.deepEqual(reviewer.getState().readings.map((r) => [r.audienceID, r.state]), [['lead', 'done'], ['dev', 'done']], 'in the order of the audiences');
  assert.deepEqual(reviewer.getState().readings[1].items.map((x) => x.status), ['missing']);
  reviewer.close();
});

test('reading again replaces the untouched findings and keeps those the reviewer acted on', async () => {
  const again = [
    finding({ kind: 'doubt', original_text: 'Four million actors fit in 1.5 GB.', need: 'Link the benchmark', explanation: 'Still no source.' }),
    finding({ kind: 'jargon', original_text: 'Statecharts run the same everywhere.', need: 'Define statechart', explanation: 'Still jargon.' }),
    finding({ kind: 'task', original_text: 'Install it with npm.', need: 'Give the command', explanation: 'Which command?' }),
    coverage(1, 'answered', 'Now there.', 'Actors as a library.'),
  ];
  const fetch = readerFetch({ 'Tech lead': [geminiReply('', LEAD_READING), geminiReply('', again)] });
  const { reviewer } = await withAudiences({ fetch });
  await reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] });
  const first = Object.fromEntries(findings(reviewer).map((a) => [a.finding.kind, a]));
  await reviewer.dispatch({ type: 'dismiss', id: first.doubt.id });                         // acted on
  await reviewer.dispatch({ type: 'note', id: first.missing.id, text: 'We need this.' });  // acted on
  // jargon and objection stay untouched

  await reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] });
  const now = findings(reviewer);
  const ids = new Set(now.map((a) => a.id));
  assert.ok(ids.has(first.doubt.id), 'the dismissed one stays');
  assert.ok(ids.has(first.missing.id), 'the noted one stays');
  assert.ok(!ids.has(first.jargon.id) && !ids.has(first.objection.id), 'the untouched ones went');
  assert.deepEqual(now.map((a) => [a.finding.kind, a.finding.explanation]).sort(), [
    ['doubt', 'Measured how, on what machine?'], ['jargon', 'Still jargon.'], ['missing', 'I cannot tell when to say no.'], ['task', 'Which command?'],
  ], 'the doubt the reviewer dismissed is not reported again');
  const r = readingOfLead(reviewer);
  assert.equal(r.findingIDs.length, 2, 'the new reading names its own findings');
  assert.equal(r.items[0].status, 'answered');
  assert.equal(reviewer.getState().readings.length, 1, 'one reading per audience');
  reviewer.close();
});

test('a reading that fails says why; reading again retries it', async () => {
  const fetch = readerFetch({ 'Tech lead': [{ status: 500, body: { error: { message: 'Broken.' } } }, geminiReply('', LEAD_READING)] });
  const { reviewer } = await withAudiences({ fetch });
  await reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] });
  let r = readingOfLead(reviewer);
  assert.equal(r.state, 'failed');
  assert.match(r.error, /500.*Broken/);
  assert.equal(findings(reviewer).length, 0);
  await reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] });
  r = readingOfLead(reviewer);
  assert.equal(r.state, 'done');
  assert.equal(r.error, undefined);
  assert.equal(findings(reviewer).length, 4);
  reviewer.close();
});

test('read refuses with no audience or no key; a reply that reports nothing fails the reading', async () => {
  const fetch = readerFetch({ 'Tech lead': [geminiReply('I read it.')] });
  const { reviewer, credentials } = await withAudiences({ fetch });
  await assert.rejects(reviewer.dispatch({ type: 'read', audienceIDs: [] }), /Choose an audience first/);
  await reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] });
  assert.match(readingOfLead(reviewer).error, /reported nothing/);
  credentials.forget();
  await assert.rejects(reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] }), /No Gemini key/);
  reviewer.close();
});

test('the checklist: an answered item shows its text on the page; a gap becomes a page-level open item', async () => {
  const fetch = readerFetch({ 'Tech lead': [geminiReply('', LEAD_READING)] });
  const { reviewer, page } = await withAudiences({ fetch });
  await reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] });

  await reviewer.dispatch({ type: 'reading.show', audienceID: 'lead', index: 1 });
  assert.equal(page.highlighted.at(-1).quote.exact, 'Licensed under MIT.');
  await assert.rejects(reviewer.dispatch({ type: 'reading.show', audienceID: 'lead', index: 0 }), /no text for it/);
  await assert.rejects(reviewer.dispatch({ type: 'reading.open', audienceID: 'lead', index: 1 }), /answers it already/);

  const id = await reviewer.dispatch({ type: 'reading.open', audienceID: 'lead', index: 4 });
  const a = find(reviewer, id);
  assert.equal(a.pageLevel, true);
  assert.equal(a.original, '"Who maintains it?"');
  assert.deepEqual(a.finding, { audienceID: 'lead', kind: 'missing', explanation: 'No maintainer.', item: { field: 'objections', status: 'missing' } });
  assert.equal(a.state, 'draft');
  assert.equal(readingOfLead(reviewer).items[4].annotationID, id);
  assert.equal(await reviewer.dispatch({ type: 'reading.open', audienceID: 'lead', index: 4 }), id, 'once');
  // deleted, the gap is a gap again
  await reviewer.dispatch({ type: 'delete', id });
  assert.equal(readingOfLead(reviewer).items[4].annotationID, undefined);
  reviewer.close();
});

test('the page chat reads as an audience when asked (read_as); an unknown name is said', async () => {
  const fetch = readerFetch({
    chat: [
      geminiReply('', [['read_as', { audience: 'tech lead' }], ['read_as', { audience: 'Nobody' }]]),
      geminiReply('I will read it as the tech lead.'),
    ],
    'Tech lead': [geminiReply('', LEAD_READING)],
  });
  const { reviewer } = await withAudiences({ fetch });
  await reviewer.dispatch({ type: 'chat', text: 'How would the tech lead read this page?' });
  const reply = reviewer.getState().chat.messages.at(-1);
  assert.equal(reply.role, 'model');
  assert.deepEqual(reply.audiences, [{ name: 'Tech lead', action: 'read', id: 'lead' }]);
  assert.match(reply.text, /No audience named “Nobody”/);
  assert.equal(readingOfLead(reviewer).state, 'done');
  assert.equal(findings(reviewer).length, 4);
  reviewer.close();
});

test('export: page-level findings are items to add; the gaps no item was made of are listed per audience', async () => {
  const fetch = readerFetch({ 'Tech lead': [geminiReply('', LEAD_READING)] });
  const { reviewer, downloads } = await withAudiences({ fetch });
  await reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] });
  await reviewer.dispatch({ type: 'reading.open', audienceID: 'lead', index: 4 });
  await reviewer.dispatch({ type: 'export', scope: 'page', to: 'download' });
  const files = Object.fromEntries(unzip(downloads[0].bytes).map((f) => [f.name, new TextDecoder().decode(f.data)]));
  const changes = files['CHANGES.md'];
  assert.match(changes, /### Open item \d+: add\n\nAdd:\n\n```text\nAdd a "when not to use it" section\n```/);
  assert.match(changes, /Found reading as: Tech lead \(missing\)/);
  assert.match(changes, /- under the heading "Actors as a library"/);
  const gaps = changes.slice(changes.indexOf('### Gaps by audience'));
  assert.match(gaps, /^### Gaps by audience\n\nTech lead \(read /);
  assert.match(gaps, /- Give them: An honest "use it when" \(not answered\)\. No such section\./);
  assert.match(gaps, /- Give them a next step to: Decide yes or no \(partly answered\)/);
  assert.match(gaps, /- Answer: "What happens when the host dies\?" \(not answered\)/);
  assert.doesNotMatch(gaps, /Who maintains it/, 'made into an open item, it is no gap');
  assert.doesNotMatch(gaps, /The license/, 'answered');
  assert.match(files['README.md'], /3 gap\(s\) by audience/);

  const dir = mkdtempSync(join(tmpdir(), 'copywriter-'));
  try {
    for (const [name, text] of Object.entries(files)) writeFileSync(join(dir, name), text);
    const bin = execFileSync('mise', ['which', 'sxml'], { cwd: new URL('../', import.meta.url).pathname, encoding: 'utf8' }).trim();
    execFileSync(bin, ['validate', `--schema=${join(dir, 'work-packet.xsd')}`, join(dir, 'packet.xml')], { stdio: ['ignore', 'pipe', 'pipe'] });
    const tasks = execFileSync(bin, ['xpath', `--context-item=${join(dir, 'packet.xml')}`, 'count(/work-packet/payload/items/design-task[starts-with(summary, "Answer") or starts-with(summary, "Give")])'], { encoding: 'utf8' }).trim();
    assert.equal(tasks, '3', 'each gap is a design task in packet.xml');
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
  reviewer.close();
});

test('readings come back with the page: a done one keeps its checklist, one cut off while reading has failed', async () => {
  const store = createMemoryStore();
  let release;
  const fetch = readerFetch({
    'Tech lead': [geminiReply('', LEAD_READING)],
    'Workflow developer': [() => new Promise((resolve) => { release = resolve; })],
  });
  const first = await withAudiences({ fetch, store });
  await first.reviewer.dispatch({ type: 'read', audienceIDs: ['lead'] });
  first.reviewer.dispatch({ type: 'read', audienceIDs: ['dev'] }).catch(() => {});
  for (let i = 0; i < 200 && !release; i++) await new Promise((resolve) => setTimeout(resolve, 5));
  first.reviewer.close();

  const page = fakePage({ text: TEXT });
  const { reviewer } = await setup({ store, page, fetch: readerFetch({}) });
  const readings = reviewer.getState().readings;
  assert.deepEqual(readings.map((r) => [r.audienceID, r.state]), [['lead', 'done'], ['dev', 'failed']]);
  assert.match(readings[1].error, /cut off/);
  assert.equal(readings[0].items.length, 6);
  assert.equal(readings[0].findingIDs.length, 4);
  // another page has none; back again, it is there
  page.navigate({ url: 'https://www.tinyactors.dev/other/', text: 'Other.' });
  await reviewer.dispatch({ type: 'deselect' });
  assert.deepEqual(reviewer.getState().readings, []);
  page.navigate({ url: 'https://www.tinyactors.dev/', text: TEXT });
  await reviewer.dispatch({ type: 'deselect' });
  assert.equal(reviewer.getState().readings.length, 2);
  release?.(geminiReply('', []));
  reviewer.close();
});
