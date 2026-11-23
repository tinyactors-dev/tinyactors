// Revising a page (charts/plan.scxml): the plan actor against a fake page,
// the published audience files, the site map of www.tinyactors.dev as its
// llms.txt and pages' heads gave it (test/fixtures/site/), and a fake
// Gemini that reads the page as the tech lead, then plans it.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync, writeFileSync, rmSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { setup, fakePage, fakeFetch, fakeFiles, geminiReply } from './fakes.mjs';
import { parseLLMS, parsePageHead, siteMap, htmlOf } from '../src/core/site.mjs';
import { planOf, bestPage } from '../src/core/plan.mjs';
import { unzip } from '../src/core/packet.mjs';

const SITE = 'https://www.tinyactors.dev';
const file = (id) => `${SITE}/audiences/${id}.xml`;
const TEXT = 'Actors as a library. Four million actors fit in 1.5 GB. When the process dies. A snapshot keeps every actor. Install it with npm.';
const HOME = { kind: 'pitch', audiences: [file('agent-platform'), file('tech-lead'), file('workflow-tester')] };
const LLMS = readFileSync(new URL('./fixtures/site/llms.txt', import.meta.url), 'utf8');
const HEADS = JSON.parse(readFileSync(new URL('./fixtures/site/heads.json', import.meta.url), 'utf8'));
const siteFiles = (more = {}) => fakeFiles({ [`${SITE}/llms.txt`]: LLMS, ...HEADS, ...more });
const system = (request) => request.body.systemInstruction.parts[0].text;
const projectDir = new URL('../', import.meta.url).pathname;
const decoder = new TextDecoder();

function sxml(...args) {
  const bin = execFileSync('mise', ['which', 'sxml'], { cwd: projectDir, encoding: 'utf8' }).trim();
  return execFileSync(bin, args, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] });
}

// The tech lead's reading: each need's kind of page, and what the page answers.
const READING = (n) => geminiReply('', [
  ['report_coverage', { item: n('decide'), status: 'partial', explanation: 'No limits stated.', kind: 'pitch' }],
  ['report_coverage', { item: n('failure-semantics'), status: 'partial', explanation: 'A snapshot, but no crash.', kind: 'explanation' }],
  ['report_coverage', { item: n('host-dies'), status: 'missing', explanation: 'Nothing on a crash.', kind: 'how-to' }],
  ['report_coverage', { item: n('when-to-use'), status: 'missing', explanation: 'No "when not to".', kind: 'explanation' }],
  ['report_finding', { kind: 'objection', original_text: 'When the process dies.', need: 'Say what is lost', explanation: 'What do I lose?', replacement: '' }],
]);

// The plan: a rewrite that fits the pitch, an add of an explanation's need
// (routed), a link the model made, a move to a new page, a cut, and
// calls the core drops.
const PLAN = geminiReply('', [
  ['plan_item', { action: 'rewrite', needs: ['tech-lead.decide'], target: 'here', original_text: 'Actors as a library.', text: 'Actors as a library you embed.', reason: 'Says what it is.', criteria: ['The home page says in one line what tinyactors is'] }],
  ['plan_item', { action: 'add', needs: ['tech-lead.failure-semantics', 'tech-lead.decide'], target: 'here', where: 'Production', text: 'One process holds every actor; a snapshot keeps them all.', reason: 'The lead asks what is lost.', criteria: ['The home page says what one process holds', 'The home page helps decide'] }],
  ['plan_item', { action: 'link', needs: ['tech-lead.host-dies'], target: '/guides/snapshots/', original_text: 'When the process dies.', text: 'When the process dies, a snapshot brings it back: see Snapshots.', target_text: 'Say what a snapshot keeps when the host dies.', reason: 'A how-to answers it.', criteria: ['The home page links to the snapshots guide for what is lost when the host dies', '/guides/snapshots/ answers tech-lead.host-dies'], conflict: 'The lead wants failure detail; the pitch stays short.' }],
  ['plan_item', { action: 'move', needs: ['tech-lead.when-to-use'], target: 'new', where: 'When to use tinyactors', text: 'Use it when one process is enough.', reason: 'No page says when not to.', criteria: ['A new page says when not to use tinyactors'] }],
  ['plan_item', { action: 'cut', needs: [], target: 'here', original_text: 'Install it with npm.', text: '', reason: 'The install block says it.', criteria: [] }],
  ['plan_item', { action: 'cut', needs: [], target: 'here', original_text: 'Not on the page.', text: '', reason: 'x', criteria: [] }],
  ['plan_item', { action: 'invent', needs: ['tech-lead.decide'], target: 'here', text: 'x', reason: 'x', criteria: [] }],
  ['plan_item', { action: 'rewrite', needs: ['tech-lead.nonsense'], target: 'here', original_text: 'Four million actors fit in 1.5 GB.', text: 'Four million actors in 1.5 GB.', reason: 'Shorter.', criteria: ['x'] }],
  ['new_page', { title: 'When to use tinyactors', kind: 'explanation', audiences: ['tech-lead'], outline: ['Use it when', 'Do not use it when'], needs: ['tech-lead.when-to-use'], reason: 'No page answers it.' }],
]);

async function planned({ plan = PLAN, fetchFile = siteFiles() } = {}) {
  const fetch = fakeFetch();
  const env = await setup({ page: fakePage({ url: `${SITE}/`, title: 'tinyactors', text: TEXT, purpose: HOME }), fetch, fetchFile });
  const lead = env.reviewer.getState().settings.audiences.find((a) => a.id === 'site-tech-lead');
  fetch.answer(READING((id) => lead.items.findIndex((x) => x.id === id) + 1));
  await env.reviewer.dispatch({ type: 'read', audienceIDs: ['site-tech-lead'] });
  if (plan) {
    fetch.answer(plan);
    await env.reviewer.dispatch({ type: 'plan' });
  }
  return { ...env, fetch, fetchFile };
}
const itemOf = (reviewer, action, target) => reviewer.getState().plan.items.find((x) => x.action === action && (!target || x.target === target));

test('the site map: llms.txt lists the pages, each page\'s Markdown says its kind and audiences', async () => {
  const listed = parseLLMS(LLMS, SITE);
  assert.equal(listed.length, 23, 'every page but the packages, audiences and links elsewhere');
  assert.deepEqual(listed[0], { title: 'tinyactors: like SQLite, but for actors', md: `${SITE}/index.md`, description: listed[0].description });
  assert.ok(listed.some((p) => p.md === `${SITE}/changelog/index.md`), 'the optional pages too');
  assert.equal(htmlOf(`${SITE}/guides/snapshots/index.md`), `${SITE}/guides/snapshots/`);
  assert.deepEqual(parsePageHead(HEADS[`${SITE}/guides/snapshots/index.md`]), {
    url: `${SITE}/guides/snapshots/`, title: 'Snapshots', kind: 'how-to',
    audiences: [
      { name: 'Tech lead who approves dependencies', url: file('tech-lead'), id: 'tech-lead' },
      { name: 'Agent-platform engineer', url: file('agent-platform'), id: 'agent-platform' },
    ],
  });
  assert.deepEqual(parsePageHead('# A page\n\nNo kind here.'), { title: 'A page', kind: null, audiences: [] });

  const fetchFile = siteFiles({ [`${SITE}/about/index.md`]: new Error('404') });
  const map = await siteMap(SITE, fetchFile);
  assert.equal(map.length, 23);
  const snapshots = map.find((p) => p.path === '/guides/snapshots/');
  assert.deepEqual([snapshots.kind, snapshots.audiences, snapshots.title], ['how-to', ['tech-lead', 'agent-platform'], 'Snapshots']);
  const about = map.find((p) => p.path === '/about/');
  assert.deepEqual([about.kind, about.title], [null, 'Why tinyactors exists'], 'a page whose Markdown fails keeps its title');
  // a site without llms.txt has no map, and nothing throws
  assert.deepEqual(await siteMap('https://example.com', fakeFiles({ 'https://example.com/llms.txt': new Error('404') })), []);
});

test('routing: a need of another kind than the page\'s becomes one line and a link here, and a task for the page of its kind', () => {
  const needs = new Map([
    ['tech-lead.decide', { kind: 'pitch', siteID: 'tech-lead', text: 'Decide yes or no', audience: 'Tech lead' }],
    ['tech-lead.host-dies', { kind: 'how-to', siteID: 'tech-lead', text: 'What happens to state when the host dies?', audience: 'Tech lead' }],
    ['agent-platform.memory', { kind: 'tutorial', siteID: 'agent-platform', text: 'What an actor costs', audience: 'Agent-platform engineer' }],
  ]);
  const pages = [
    { url: `${SITE}/guides/hibernation/`, path: '/guides/hibernation/', title: 'Hibernation', kind: 'how-to', audiences: ['agent-platform', 'tech-lead'] },
    { url: `${SITE}/guides/snapshots/`, path: '/guides/snapshots/', title: 'Snapshots', kind: 'how-to', audiences: ['tech-lead', 'agent-platform'] },
  ];
  assert.equal(bestPage(pages, { kind: 'how-to', siteID: 'tech-lead', here: `${SITE}/` }).title, 'Snapshots', 'its audience\'s main page first');
  let n = 0;
  const { items, newPages } = planOf({ calls: [{ name: 'plan_item', args: {
    action: 'add', needs: ['tech-lead.decide', 'tech-lead.host-dies', 'agent-platform.memory'], target: 'here', text: 'Everything, here.', reason: 'All of it.', criteria: ['decides', 'dies', 'costs'],
  } }] }, { url: `${SITE}/`, kind: 'pitch', needs, pages, id: () => `p${++n}` });
  assert.deepEqual(items.map((x) => [x.action, x.target, x.needs]), [
    ['add', 'here', ['tech-lead.decide']],
    ['link', `${SITE}/guides/snapshots/`, ['tech-lead.host-dies']],
    ['link', 'new', ['agent-platform.memory']],
  ], 'the need that fits stays; each other goes where its kind is');
  assert.deepEqual(items[0].criteria, [{ needID: 'tech-lead.decide', page: `${SITE}/`, text: 'decides' }]);
  const link = items[1];
  assert.equal(link.text, 'What happens to state when the host dies? See Snapshots.');
  assert.equal(link.target_text, 'Everything, here.', 'the target gets what was drafted');
  assert.equal(link.conflict, 'tech-lead.host-dies belongs on a how-to page: linked to /guides/snapshots/.');
  assert.ok(link.routed);
  assert.deepEqual(link.criteria.map((c) => [c.page, c.text]), [
    [`${SITE}/`, 'The home page links to /guides/snapshots/ for tech-lead.host-dies'],
    [`${SITE}/guides/snapshots/`, '/guides/snapshots/ answers tech-lead.host-dies'],
  ]);
  assert.deepEqual(newPages.map((p) => [p.kind, p.audiences, p.needs]), [['tutorial', ['agent-platform'], ['agent-platform.memory']]], 'no tutorial page: a new one');
});

test('plan the page: one request at the reviewer\'s level with everything gathered and the site map; the items routed and anchored', async () => {
  const { reviewer, fetch, fetchFile } = await planned();
  const request = fetch.requests[1];
  assert.deepEqual(request.body.tools[0].functionDeclarations.map((f) => f.name), ['plan_item', 'new_page']);
  assert.equal(request.body.generationConfig.thinkingConfig.thinkingLevel, 'high', 'the reviewer\'s level');
  const asked = system(request);
  assert.match(asked, /Its kind: pitch: the reader is deciding whether to adopt it/);
  assert.match(asked, /Its audiences, the main one first:\n- Agent-platform engineer/);
  assert.match(asked, /- tech-lead\.host-dies \[objections; belongs on: how-to; here: missing\] What happens to state when the host dies\?/);
  assert.match(asked, /- agent-platform\.memory \[wants; here: not read\]/, 'an audience not read yet');
  assert.match(asked, /Open findings of the readings:\n- objection \(Tech lead who approves dependencies\) at “When the process dies\.”: What do I lose\?/);
  assert.match(asked, /- \/guides\/snapshots\/ \(how-to; tech-lead, agent-platform\): Snapshots: /);
  assert.ok(!asked.includes('- / (pitch'), 'the page itself is not in the map');
  assert.ok(!/claim(ed|s) (by|to answer)/.test(asked), 'no section claims');

  const plan = reviewer.getState().plan;
  assert.equal(plan.state, 'planned');
  assert.equal(plan.sitePages, 22);
  assert.deepEqual(plan.items.map((x) => [x.action, x.target.replace(SITE, ''), x.needs.join(' ')]), [
    ['rewrite', 'here', 'tech-lead.decide'],
    ['add', 'here', 'tech-lead.decide'],
    ['link', '/about/', 'tech-lead.failure-semantics'],
    ['link', '/guides/snapshots/', 'tech-lead.host-dies'],
    ['move', 'new', 'tech-lead.when-to-use'],
    ['cut', 'here', ''],
    ['rewrite', 'here', ''],
  ], 'the explanation need of the add is linked to the explanation page; a cut not on the page and an unknown action are dropped, an unknown need id too');
  assert.ok(itemOf(reviewer, 'rewrite').anchored);
  assert.equal(itemOf(reviewer, 'link', `${SITE}/about/`).conflict, 'tech-lead.failure-semantics belongs on an explanation page: linked to /about/.');
  assert.equal(itemOf(reviewer, 'link', `${SITE}/guides/snapshots/`).conflict, 'The lead wants failure detail; the pitch stays short.');
  assert.deepEqual(plan.newPages.map((p) => [p.title, p.kind, p.needs]), [['When to use tinyactors', 'explanation', ['tech-lead.when-to-use']]]);

  // the site map is read once per origin this session
  const asks = fetchFile.asked.filter((u) => u.endsWith('/llms.txt')).length;
  fetch.answer(PLAN);
  await reviewer.dispatch({ type: 'plan' });
  assert.equal(fetchFile.asked.filter((u) => u.endsWith('/llms.txt')).length, asks);
  reviewer.close();
});

test('accepting: a rewrite is an annotation to preview, a link an annotation and a task, a move a task only; re-planning keeps what was decided', async () => {
  const { reviewer, page, fetch } = await planned();
  const rewrite = itemOf(reviewer, 'rewrite');
  await reviewer.dispatch({ type: 'plan.accept', id: rewrite.id });
  let item = itemOf(reviewer, 'rewrite');
  assert.equal(item.decision, 'accepted');
  const a = reviewer.getState().annotations.find((x) => x.id === item.annotationID);
  assert.equal(a.source, 'plan');
  assert.equal(a.state, 'suggested');
  assert.equal(a.original, 'Actors as a library.');
  assert.equal(a.suggestions[0].text, 'Actors as a library you embed.');
  assert.deepEqual(a.plan.needs, ['tech-lead.decide']);
  await reviewer.dispatch({ type: 'preview', id: a.id, suggestionID: 's1' });
  assert.match(page.text, /^Actors as a library you embed\. /, 'previewed as usual');
  await reviewer.dispatch({ type: 'keep', id: a.id });

  const link = itemOf(reviewer, 'link', `${SITE}/guides/snapshots/`);
  await reviewer.dispatch({ type: 'plan.accept', id: link.id });
  const linked = reviewer.getState().annotations.find((x) => x.id === itemOf(reviewer, 'link', `${SITE}/guides/snapshots/`).annotationID);
  assert.equal(linked.original, 'When the process dies.', 'the sentence becomes the line');
  assert.equal(linked.suggestions[0].text, 'When the process dies, a snapshot brings it back: see Snapshots.');

  const add = itemOf(reviewer, 'add');
  await reviewer.dispatch({ type: 'plan.accept', id: add.id });
  const added = reviewer.getState().annotations.find((x) => x.id === itemOf(reviewer, 'add').annotationID);
  assert.ok(added.pageLevel, 'an add is an open item of the page');
  assert.equal(added.plan.where, 'Production');

  const move = itemOf(reviewer, 'move');
  await reviewer.dispatch({ type: 'plan.accept', id: move.id });
  assert.equal(itemOf(reviewer, 'move').annotationID, undefined, 'a move is a task for its target');
  const cut = itemOf(reviewer, 'cut');
  await reviewer.dispatch({ type: 'plan.reject', id: cut.id });
  await reviewer.dispatch({ type: 'plan.accept', id: reviewer.getState().plan.newPages[0].id });
  await assert.rejects(reviewer.dispatch({ type: 'plan.accept', id: 'nope' }), /No such item/);

  // a new plan: the decided items stay, the untouched ones are replaced, repeats of decided ones dropped
  fetch.answer(geminiReply('', [
    PLAN.candidates[0].content.parts[0].functionCall && ['plan_item', PLAN.candidates[0].content.parts[0].functionCall.args],
    ['plan_item', { action: 'rewrite', needs: ['tech-lead.decide'], target: 'here', original_text: 'Four million actors fit in 1.5 GB.', text: 'Four million actors fit in 1.5 GB of memory.', reason: 'Names the unit.', criteria: ['The home page names what the 1.5 GB is'] }],
  ]));
  await reviewer.dispatch({ type: 'plan' });
  const plan = reviewer.getState().plan;
  assert.deepEqual(plan.items.map((x) => [x.action, x.decision]), [
    ['rewrite', 'accepted'], ['add', 'accepted'], ['link', 'accepted'], ['move', 'accepted'], ['cut', 'rejected'], ['rewrite', null],
  ]);
  assert.equal(plan.items.at(-1).original, 'Four million actors fit in 1.5 GB.');
  assert.match(system(fetch.requests.at(-1)), /Decided by the reviewer \(never propose again\):\n- accepted: rewrite here \[tech-lead\.decide\] Actors as a library you embed\./);
  assert.match(system(fetch.requests.at(-1)), /Kept changes \(keep them\):\n- “Actors as a library\.” → “Actors as a library you embed\.”/);
  assert.equal(plan.newPages[0].decision, 'accepted');

  // accept all: the rest at once
  await reviewer.dispatch({ type: 'plan.acceptAll' });
  assert.ok(reviewer.getState().plan.items.every((x) => x.decision));
  reviewer.close();
});

test('the plan is kept with the page: reopened, its decided items and their annotations come back', async () => {
  const { reviewer, store, page } = await planned();
  await reviewer.dispatch({ type: 'plan.accept', id: itemOf(reviewer, 'rewrite').id });
  reviewer.close();
  const again = await setup({ store, page: fakePage({ url: `${SITE}/`, title: 'tinyactors', text: TEXT, purpose: HOME }), fetchFile: siteFiles() });
  const plan = again.reviewer.getState().plan;
  assert.equal(plan.state, 'planned');
  assert.equal(plan.items.find((x) => x.action === 'rewrite').decision, 'accepted');
  assert.ok(again.reviewer.getState().annotations.some((x) => x.source === 'plan'));
  assert.ok(page);
  again.reviewer.close();
});

test('planning needs a reading; a reply with no items is an error the plan keeps', async () => {
  const { reviewer } = await setup({ page: fakePage({ url: `${SITE}/`, title: 'tinyactors', text: TEXT, purpose: HOME }), fetchFile: siteFiles() });
  await assert.rejects(reviewer.dispatch({ type: 'plan' }), /Read the page as an audience first/);
  reviewer.close();
  const env = await planned({ plan: null });
  env.fetch.answer(geminiReply('Nothing to do.'));
  await assert.rejects(env.reviewer.dispatch({ type: 'plan' }), /Gemini planned nothing/);
  assert.equal(env.reviewer.getState().plan.state, 'failed');
  env.reviewer.close();
});

test('export: each page\'s acceptance criteria, the tasks for other pages and the proposed pages; packet.xml stays valid', async () => {
  const { reviewer, downloads } = await planned();
  for (const action of ['rewrite', 'move']) await reviewer.dispatch({ type: 'plan.accept', id: itemOf(reviewer, action).id });
  await reviewer.dispatch({ type: 'plan.accept', id: itemOf(reviewer, 'link', `${SITE}/guides/snapshots/`).id });
  await reviewer.dispatch({ type: 'plan.accept', id: reviewer.getState().plan.newPages[0].id });
  const rewrite = reviewer.getState().annotations.find((x) => x.id === itemOf(reviewer, 'rewrite').annotationID);
  await reviewer.dispatch({ type: 'preview', id: rewrite.id, suggestionID: 's1' });
  await reviewer.dispatch({ type: 'keep', id: rewrite.id });
  await reviewer.dispatch({ type: 'export', to: 'download' });
  const files = Object.fromEntries(unzip(downloads[0].bytes).map((f) => [f.name, decoder.decode(f.data)]));
  const md = files['CHANGES.md'];
  assert.match(md, /### Acceptance criteria\n\n- \[ \] The home page says in one line what tinyactors is\. \(tech-lead\.decide\)\n- \[ \] The home page links to the snapshots guide for what is lost when the host dies\. \(tech-lead\.host-dies\)\n/);
  assert.match(md, /## Other pages\n[\s\S]*### https:\/\/www\.tinyactors\.dev\/guides\/snapshots\/ \(Snapshots\)\n\n- tech-lead\.host-dies: Say what a snapshot keeps when the host dies\.\n  From the plan of https:\/\/www\.tinyactors\.dev\/\. A how-to answers it\.\n  - \[ \] \/guides\/snapshots\/ answers tech-lead\.host-dies\./);
  assert.match(md, /## Proposed pages\n[\s\S]*### When to use tinyactors\n\nKind: explanation\. For: tech-lead\. Answers: tech-lead\.when-to-use\.\n[\s\S]*1\. Use it when\n2\. Do not use it when/);
  assert.match(md, /Plan: rewrite for tech-lead\.decide/);
  assert.ok(!md.includes('A how-to answers it.\n  - [ ] The home page links'), 'a criterion of this page is listed under it, not under the other page');
  assert.match(files['README.md'], /add the need's id to that section's `answers`/);
  assert.match(files['README.md'], /website\/README\.md/);

  const dir = mkdtempSync(join(tmpdir(), 'copywriter-plan-'));
  try {
    for (const [name, text] of Object.entries(files)) writeFileSync(join(dir, name), text);
    sxml('validate', `--schema=${join(dir, 'work-packet.xsd')}`, join(dir, 'packet.xml'));
    const tasks = sxml('xpath', `--context-item=${join(dir, 'packet.xml')}`,
      '/work-packet/payload/items/design-task[starts-with(@id, "task-") or starts-with(@id, "new-page-")]/summary/string()').trim().split('\n');
    assert.deepEqual(tasks, [
      '"Answer on https://www.tinyactors.dev/guides/snapshots/: tech-lead.host-dies"',
      '"A new explanation page: ""When to use tinyactors"", answering tech-lead.when-to-use"',
    ], 'a move to a new page is the new page\'s');
    const acceptance = sxml('xpath', `--context-item=${join(dir, 'packet.xml')}`, '/work-packet/payload/items/copy-change/acceptance/string()');
    assert.match(acceptance, /The home page says in one line what tinyactors is\./);
  } finally {
    rmSync(dir, { recursive: true, force: true });
  }
  reviewer.close();
});

test('a link\'s criteria go to their pages: the line\'s here, the answer\'s to the target or the new page; a need placed elsewhere says why', () => {
  const needs = new Map([['tech-lead.maintainer', { kind: 'reference', siteID: 'tech-lead', text: 'Who maintains it?', audience: 'Tech lead' }]]);
  let n = 0;
  const { items, newPages } = planOf({ calls: [
    { name: 'plan_item', args: { action: 'link', needs: ['tech-lead.maintainer'], target: 'new', where: 'Maintainers', text: 'See who maintains it.', reason: 'A reference answers it.',
      criteria: ['The home page links to \'Maintainers\' for who maintains tinyactors', '\'Maintainers\' names who maintains tinyactors'] } },
    { name: 'new_page', args: { title: 'Maintainers', kind: 'reference', audiences: ['tech-lead'], outline: ['Who'], needs: ['tech-lead.maintainer'] } },
  ] }, { url: `${SITE}/`, kind: 'pitch', needs, pages: [], id: () => `p${++n}` });
  assert.deepEqual(items[0].criteria.map((c) => [c.page, c.text]), [
    [`${SITE}/`, 'The home page links to \'Maintainers\' for who maintains tinyactors'],
    ['new', '\'Maintainers\' names who maintains tinyactors'],
  ]);
  assert.equal(items[0].conflict, 'A reference answers it.', 'no conflict given: the reason');
  assert.deepEqual(newPages[0].criteria.map((c) => c.text), ['\'Maintainers\' names who maintains tinyactors'], 'the new page takes the plan\'s criterion');
});
