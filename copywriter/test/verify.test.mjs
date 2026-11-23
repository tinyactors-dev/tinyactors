// The site view and verification: the grid assembled from stored readings
// and plans and the site map (test/fixtures/site/), the site reading's
// coordinator (charts/site.scxml: at most three readings at once, cancel,
// a failed page), a reading's history and the comparison after a page
// changed ("Check changes"), the criteria check's request and answer, and
// the export ticking the criteria already met. Gemini is a fake that
// answers by what each request asks.

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { setup, fakePage, fakeFiles, geminiReply } from './fakes.mjs';
import { createMemoryStore } from '../src/core/store.mjs';
import { siteGrid, compare, historyOf, criteriaFor, textHash } from '../src/core/grid.mjs';
import { checksOf, CHECK_FUNCTIONS } from '../src/core/plan.mjs';
import { siteMap, parseAudience } from '../src/core/site.mjs';
import { unzip } from '../src/core/packet.mjs';

const SITE = 'https://www.tinyactors.dev';
const file = (id) => `${SITE}/audiences/${id}.xml`;
const LLMS = readFileSync(new URL('./fixtures/site/llms.txt', import.meta.url), 'utf8');
const HEADS = JSON.parse(readFileSync(new URL('./fixtures/site/heads.json', import.meta.url), 'utf8'));
const siteFiles = () => fakeFiles({ [`${SITE}/llms.txt`]: LLMS, ...HEADS });
const HOME = { kind: 'pitch', audiences: [file('agent-platform'), file('tech-lead'), file('workflow-tester')] };
const HOME_TEXT = 'Actors as a library. When the process dies. Read the snapshots guide.';
const decoder = new TextDecoder();
const lead = parseAudience(readFileSync(new URL('./fixtures/audiences/tech-lead.xml', import.meta.url), 'utf8'));
const needID = (n) => `tech-lead.${lead.items[n].id}`;

// The pages as served (fetchPage): text, kind and audiences by URL; a page may fail.
function servedPages(pages) {
  const asked = [];
  const fetchPage = async (url) => {
    asked.push(url);
    const p = pages[url];
    if (!p) throw new Error(`${url}: 404`);
    if (p instanceof Error) throw p;
    return { url, title: p.title || url, lang: 'en', headings: [], text: p.text, links: p.links || [], kind: p.kind || null, audiences: p.audiences || [] };
  };
  fetchPage.asked = asked;
  fetchPage.pages = pages;
  return fetchPage;
}

const bodyOf = (init) => JSON.parse(init.body);
const systemOf = (body) => body.systemInstruction.parts[0].text;
const functionsOf = (body) => (body.tools?.[0]?.functionDeclarations || []).map((f) => f.name);
const pageOf = (body) => /^Page: (\S+)/m.exec(systemOf(body))?.[1];

/**
 * A fake Gemini that answers each request by what it asks: a reading by
 * `statuses(page, items)` (an array of statuses, one per checklist item),
 * a criteria check by `met(page, n)`. `hold` keeps every reading waiting
 * until `release()`; `inFlight` and `most` count the readings under way.
 */
function smartGemini({ statuses = () => [], met = () => true, fail = () => false, hold = false } = {}) {
  const requests = [];
  const waiting = [];
  let inFlight = 0;
  const fetch = async (url, init) => {
    const body = bodyOf(init);
    requests.push({ url, body });
    const tools = functionsOf(body);
    const page = pageOf(body);
    let reply;
    if (tools.includes('report_coverage')) {
      inFlight += 1;
      fetch.most = Math.max(fetch.most, inFlight);
      try {
        if (fetch.hold) await new Promise((resolve) => waiting.push(resolve));
        if (fail(page)) return new Response(JSON.stringify({ error: { message: 'Bad request' } }), { status: 400 });
        const items = [...systemOf(body).matchAll(/^(\d+)\. \[/gm)].length;
        const s = statuses(page, items, body);
        reply = geminiReply('', Array.from({ length: items }, (_, i) => ['report_coverage', { item: i + 1, status: s[i] || 'missing', explanation: `item ${i + 1}`, kind: 'pitch' }]));
      } finally { inFlight -= 1; }
    } else if (tools.includes('report_criterion')) {
      const n = [...systemOf(body).matchAll(/^(\d+)\. /gm)].length;
      reply = geminiReply('', Array.from({ length: n }, (_, i) => ['report_criterion', { number: i + 1, met: met(page, i), quote: met(page, i) ? 'Read the snapshots guide.' : '' }]));
    } else {
      reply = geminiReply('No.');
    }
    return new Response(JSON.stringify(reply), { status: 200, headers: { 'content-type': 'application/json' } });
  };
  fetch.requests = requests;
  fetch.hold = hold;
  fetch.most = 0;
  fetch.release = () => { while (waiting.length) waiting.shift()(); };
  fetch.waiting = () => waiting.length;
  return fetch;
}

// Resolves when the reviewer's state meets `pred`.
function until(reviewer, pred, ms = 5000) {
  return new Promise((resolve, reject) => {
    if (pred(reviewer.getState())) { resolve(reviewer.getState()); return; }
    const timer = setTimeout(() => { off(); reject(new Error('timed out')); }, ms);
    const off = reviewer.subscribe((s) => { if (pred(s)) { clearTimeout(timer); off(); resolve(s); } });
  });
}
const tick = () => new Promise((r) => setTimeout(r, 20));

async function home({ gemini = smartGemini(), served = {}, text = HOME_TEXT } = {}) {
  const page = fakePage({ url: `${SITE}/`, title: 'tinyactors', text, purpose: HOME });
  const fetchPage = servedPages({ [`${SITE}/`]: { text, kind: 'pitch', title: 'tinyactors' }, ...served });
  const fetchFile = siteFiles();
  const env = await setup({ page, fetch: gemini, fetchFile, fetchPage });
  await until(env.reviewer, (s) => s.site.state !== 'none');
  return { ...env, fetchPage, fetchFile, gemini };
}

test('the grid: every need of the site audiences against their pages, from the readings, the plans and the site map', async () => {
  const map = await siteMap(SITE, siteFiles());
  const audiences = [{ id: 'site-tech-lead', source: 'site', siteID: 'tech-lead', name: 'Tech lead', items: lead.items }];
  const item = (n, status) => ({ field: lead.items[n].field, text: lead.items[n].text, needID: needID(n), status, explanation: `why ${n}` });
  const readings = [
    { page: `${SITE}/`, audienceID: 'site-tech-lead', state: 'done', at: '2026-11-23T10:00:00Z', items: [item(0, 'answered'), item(1, 'partial'), item(2, 'missing')] },
    { page: `${SITE}/guides/snapshots/`, audienceID: 'site-tech-lead', state: 'done', at: '2026-11-23T11:00:00Z', items: [item(2, 'answered')] },
    { page: `${SITE}/about/`, audienceID: 'site-tech-lead', state: 'reading', items: [] },
  ];
  const plans = [{
    page: `${SITE}/`,
    items: [
      { id: 'p1', action: 'link', target: `${SITE}/guides/snapshots/`, needs: [needID(1)], decision: 'accepted', criteria: [] },
      { id: 'p2', action: 'link', target: `${SITE}/performance/`, needs: [needID(1)], decision: null, criteria: [] },
      { id: 'p3', action: 'move', target: 'new', where: 'When to use tinyactors', needs: [needID(3)], decision: 'accepted', criteria: [] },
    ],
    newPages: [{ id: 'n1', title: 'When to use tinyactors', kind: 'explanation', needs: [needID(3)], decision: null }, { id: 'n2', title: 'Rejected', needs: [needID(3)], decision: 'rejected' }],
  }];
  const grid = siteGrid({ map, audiences, readings, plans, here: `${SITE}/` });
  assert.equal(grid.pages.length, 23);
  const homePage = grid.pages.find((p) => p.path === '/');
  assert.deepEqual([homePage.read, homePage.here, homePage.audiences], [true, true, ['site-tech-lead']], 'the site\'s other audiences are not in the store');
  assert.equal(grid.pages.find((p) => p.path === '/about/').read, false, 'a reading under way is not a reading');

  const [a] = grid.audiences;
  assert.equal(a.needs.length, lead.items.length);
  assert.ok(a.pages.indexOf(`${SITE}/guides/snapshots/`) < a.pages.indexOf(`${SITE}/`), 'its main pages first, then those that name it');
  const cell = (n, path) => a.needs[n].cells.find((c) => c.url === `${SITE}${path}`);
  assert.deepEqual(cell(0, '/'), { url: `${SITE}/`, status: 'answered', explanation: 'why 0' });
  assert.equal(cell(1, '/').status, 'partial');
  assert.equal(cell(2, '/').status, 'missing');
  assert.equal(cell(2, '/guides/snapshots/').status, 'answered');
  assert.equal(cell(0, '/guides/snapshots/').status, null, 'not in that reading: unknown');
  assert.equal(cell(0, '/about/').status, null, 'never read: unknown');
  assert.deepEqual(a.needs[2].answered, [`${SITE}/guides/snapshots/`]);
  assert.equal(a.answered, 2, 'needs answered somewhere');
  assert.deepEqual(a.needs[1].routes, [{ url: `${SITE}/guides/snapshots/`, title: 'Snapshots', from: `${SITE}/` }], 'an accepted route; an undecided one is not');
  assert.deepEqual(a.needs[3].routes.map((r) => r.url), ['new']);
  assert.deepEqual(a.needs[3].newPages, [{ title: 'When to use tinyactors', decision: null }], 'a rejected new page is left out');
});

test('history and comparison: a reading keeps the ones it replaced; improved, regressed and unchanged', () => {
  const r1 = { state: 'done', at: 't1', hash: 'h1', items: [
    { field: 'wants', text: 'a', needID: 'x.a', status: 'missing' }, { field: 'wants', text: 'b', needID: 'x.b', status: 'answered' },
    { field: 'wants', text: 'c', needID: 'x.c', status: 'partial' }, { field: 'wants', text: 'd', status: 'partial' }] };
  const history = historyOf(r1);
  assert.deepEqual(history[0], { at: 't1', hash: 'h1', items: r1.items });
  assert.deepEqual(historyOf({ state: 'failed', history }), history, 'a failed reading passes its history on');
  assert.equal(historyOf({ state: 'done', at: 't', items: [], history: Array(9).fill(history[0]) }).length, 5, 'at most five');
  const r2 = { state: 'done', history, items: [
    { field: 'wants', text: 'a', needID: 'x.a', status: 'answered' }, { field: 'wants', text: 'b', needID: 'x.b', status: 'partial' },
    { field: 'wants', text: 'c', needID: 'x.c', status: 'partial' }, { field: 'wants', text: 'd', status: 'answered' }, { field: 'wants', text: 'new', status: 'missing' }] };
  assert.deepEqual(compare(r2), {
    before: { at: 't1', answered: 1, total: 4 }, after: { answered: 2, total: 5 },
    improved: [{ needID: 'x.a', text: 'a', from: 'missing', to: 'answered' }, { text: 'd', from: 'partial', to: 'answered' }],
    regressed: [{ needID: 'x.b', text: 'b', from: 'answered', to: 'partial' }],
    unchanged: 1,
  });
  assert.equal(compare({ state: 'done', items: [] }), null, 'nothing before it');
});

test('criteria: those the accepted plans set for a page, checked with one cheap request; a quote not on the page is dropped', async () => {
  const plans = [
    { page: `${SITE}/`, items: [
      { id: 'p1', decision: 'accepted', criteria: [{ needID: 'x.a', page: `${SITE}/`, text: 'The home page links to /guides/snapshots/' }, { needID: 'x.a', page: `${SITE}/guides/snapshots/`, text: '/guides/snapshots/ answers x.a' }] },
      { id: 'p2', decision: 'rejected', criteria: [{ page: `${SITE}/`, text: 'rejected' }] },
      { id: 'p3', decision: 'accepted', annotationID: 'gone', criteria: [{ page: `${SITE}/`, text: 'its annotation was dismissed' }] },
    ] },
    { page: `${SITE}/about/`, items: [{ id: 'q1', decision: 'accepted', criteria: [{ page: '/', text: 'The home page says why', needID: 'x.b' }, { page: 'new', text: 'a new page' }] }] },
  ];
  const here = criteriaFor(`${SITE}/`, plans, (page, id) => id !== 'gone');
  assert.deepEqual(here.map((c) => [c.text, c.from.page]), [['The home page links to /guides/snapshots/', `${SITE}/`], ['The home page says why', `${SITE}/about/`]]);
  assert.deepEqual(criteriaFor(`${SITE}/guides/snapshots/`, plans).map((c) => c.text), ['/guides/snapshots/ answers x.a']);

  const results = checksOf({ calls: [
    { name: 'report_criterion', args: { number: 1, met: true, quote: 'Snapshots   → /guides/snapshots/' } },
    { name: 'report_criterion', args: { number: 2, met: false, quote: 'Not on the page.' } },
    { name: 'report_criterion', args: { number: 9, met: true, quote: '' } },
  ] }, [...here, { id: 'z', text: 'silent' }], 'Actors. Snapshots → /guides/snapshots/');
  assert.deepEqual(results.map((r) => [r.met, r.quote]), [[true, 'Snapshots   → /guides/snapshots/'], [false, undefined], [null, undefined]]);
  assert.deepEqual(CHECK_FUNCTIONS.map((f) => f.name), ['report_criterion']);
});

test('read the site: each page as its own audiences, three readings at once; the grid fills; a page that fails is named', async () => {
  const gemini = smartGemini({ hold: true, statuses: (page) => (page.endsWith('/chat/') ? ['answered', 'answered'] : ['partial']), fail: (page) => page.endsWith('/about/') });
  const urls = ['/', '/chat/', '/guides/testing/', '/about/'].map((p) => `${SITE}${p}`);
  const served = Object.fromEntries(urls.slice(1).map((u) => [u, { text: `The page ${u}.`, kind: 'how-to' }]));
  const { reviewer, fetchPage, fetchFile } = await home({ gemini, served });
  const n = await reviewer.dispatch({ type: 'site.read', urls });
  const s0 = reviewer.getState();
  assert.equal(s0.site.state, 'reading', 'the command returns at once: the site is read in the background');
  const jobs = s0.site.progress.total;
  assert.equal(n, jobs);
  // the home page's three audiences, then each other page's own (heads.json)
  const expected = 3 + ['/chat/', '/guides/testing/', '/about/'].reduce((k, p) => k + HEADS[`${SITE}${p}index.md`].match(/\]\(https[^)]+audiences/g).length, 0);
  assert.equal(jobs, expected);
  assert.ok(fetchFile.asked.some((u) => u.endsWith('/audiences/workflow-tester.xml')), 'the pages\' audiences are fetched as a page\'s links are');
  // only three readings at a time
  await until(reviewer, () => gemini.waiting() === 3);
  await tick();
  assert.equal(gemini.waiting(), 3);
  assert.equal(reviewer.getState().site.running.length > 0, true);
  while (reviewer.getState().site.state === 'reading') { gemini.release(); await tick(); }
  assert.equal(gemini.most, 3, 'never more than three under way');
  const s = reviewer.getState();
  assert.equal(s.site.state, 'done');
  assert.equal(s.site.progress.done, jobs);
  assert.ok(s.site.failures.length > 0 && s.site.failures.every((f) => f.page === `${SITE}/about/`), 'the failed page is named; the others went on');
  assert.ok(fetchPage.asked.includes(`${SITE}/chat/`), 'another page is fetched as served');
  const reads = gemini.requests.filter((r) => functionsOf(r.body).includes('report_coverage'));
  assert.ok(reads.some((r) => systemOf(r.body).includes('The page https://www.tinyactors.dev/chat/.')), 'its served text is read');
  // the grid: /chat/ answered for its audiences, unknown where never read
  const ap = s.site.audiences.find((a) => a.siteID === 'agent-platform');
  const chatCell = ap.needs[0].cells.find((c) => c.url === `${SITE}/chat/`);
  assert.equal(chatCell.status, 'answered');
  assert.equal(ap.needs[0].cells.find((c) => c.url === `${SITE}/performance/`)?.status ?? null, null);
  assert.ok(s.site.pages.find((p) => p.path === '/chat/').read);
  // no findings for a page the panel does not show; the home page's readings are its own
  assert.equal(s.annotations.length, 0);
  assert.equal(s.readings.length, 3);
  reviewer.close();
});

test('read the site: cancel stops starting readings; the ones under way end and the readings they replaced are back', async () => {
  const gemini = smartGemini({ statuses: () => ['answered'] });
  const { reviewer, store } = await home({ gemini, served: { [`${SITE}/chat/`]: { text: 'Chat.', kind: 'tutorial' }, [`${SITE}/about/`]: { text: 'About.' } } });
  await reviewer.dispatch({ type: 'read', audienceIDs: ['site-tech-lead'] });
  const before = reviewer.getState().readings.find((r) => r.audienceID === 'site-tech-lead');
  assert.equal(before.state, 'done');

  gemini.hold = true;
  await reviewer.dispatch({ type: 'site.read', urls: [`${SITE}/`, `${SITE}/chat/`, `${SITE}/about/`] });
  await until(reviewer, () => gemini.waiting() === 3);
  await assert.rejects(reviewer.dispatch({ type: 'site.read' }), /Already reading the site/);
  assert.equal(reviewer.getState().readings.find((r) => r.audienceID === 'site-tech-lead').state, 'reading');
  await reviewer.dispatch({ type: 'site.cancel' });
  const asked = gemini.requests.length;
  gemini.hold = false;
  gemini.release();
  await tick(); await tick();
  const s = reviewer.getState();
  assert.equal(s.site.state, 'cancelled');
  assert.equal(gemini.requests.length, asked, 'no reading starts after the cancel');
  const back = s.readings.find((r) => r.audienceID === 'site-tech-lead');
  assert.deepEqual([back.state, back.at], ['done', before.at], 'the reading it replaced is back');
  const stored = (await store.readings(`${SITE}/`)).find((r) => r.audienceID === 'site-tech-lead');
  assert.equal(stored.at, before.at, 'in the store too');
  assert.ok(s.readings.every((r) => r.state !== 'reading'), 'an answer that came late is dropped');
  await assert.rejects(reviewer.dispatch({ type: 'site.cancel' }), /not being read/);
  reviewer.close();
});

const PLAN = {
  page: `${SITE}/`, lang: 'en', state: 'planned', at: '2026-11-23T09:00:00Z', items: [
    { id: 'p1', action: 'link', target: `${SITE}/guides/snapshots/`, needs: ['tech-lead.host-dies'], text: 'See Snapshots.', reason: 'r', decision: 'accepted',
      criteria: [{ needID: 'tech-lead.host-dies', page: `${SITE}/`, text: 'The home page links to /guides/snapshots/ for what is lost' }, { needID: 'tech-lead.host-dies', page: `${SITE}/guides/snapshots/`, text: '/guides/snapshots/ answers tech-lead.host-dies' }] },
    { id: 'p2', action: 'move', target: `${SITE}/about/`, needs: ['tech-lead.decide'], text: 'x', reason: 'r', decision: 'accepted', criteria: [{ needID: 'tech-lead.decide', page: `${SITE}/`, text: 'The home page says what it is' }] },
  ], newPages: [],
};

test('check changes: the page as served changed since its readings; read again as the same audiences, compared, its criteria checked', async () => {
  let answered = 1;
  const gemini = smartGemini({ statuses: (page, n) => Array.from({ length: n }, (_, i) => (i < answered ? 'answered' : i === answered ? 'partial' : 'missing')), met: (page, i) => i === 0 });
  const store = createMemoryStore();
  await store.putPlan(PLAN);
  const page = fakePage({ url: `${SITE}/`, title: 'tinyactors', text: HOME_TEXT, purpose: HOME });
  const fetchPage = servedPages({ [`${SITE}/`]: { text: HOME_TEXT, kind: 'pitch' } });
  const { reviewer } = await setup({ page, store, fetch: gemini, fetchFile: siteFiles(), fetchPage });
  await until(reviewer, (s) => s.site.state !== 'none');
  assert.equal(reviewer.getState().page.changed, false, 'no reading yet');
  assert.deepEqual(reviewer.getState().criteria.items.map((c) => [c.text, c.met]), [
    ['The home page links to /guides/snapshots/ for what is lost', null], ['The home page says what it is', null],
  ], 'the accepted plan\'s criteria for this page, not checked yet');
  await reviewer.dispatch({ type: 'read', audienceIDs: ['site-tech-lead'] });
  assert.equal(reviewer.getState().page.changed, false, 'read as served');

  // deployed: the page as served changed, and the panel opens it again
  const deployed = `${HOME_TEXT} A snapshot keeps every actor.`;
  fetchPage.pages[`${SITE}/`] = { text: deployed, kind: 'pitch' };
  page.navigate({ text: deployed });
  await until(reviewer, (s) => s.page.changed === true);

  answered = 3;
  await reviewer.dispatch({ type: 'verify' });
  const s = await until(reviewer, (x) => x.site.state === 'done');
  assert.equal(s.page.changed, false, 'read again: the readings read this text');
  const r = s.readings.find((x) => x.audienceID === 'site-tech-lead');
  assert.equal(r.history.length, 1);
  assert.deepEqual([r.compared.before.answered, r.compared.after.answered, r.compared.after.total], [1, 3, lead.items.length]);
  assert.deepEqual(r.compared.improved.map((c) => [c.needID, c.from, c.to]), [[needID(1), 'partial', 'answered'], [needID(2), 'missing', 'answered'], [needID(3), 'missing', 'partial']]);
  assert.deepEqual(r.compared.regressed, []);
  // only the audiences read before are read again; the criteria are checked in one cheap request
  const reads = gemini.requests.filter((x) => functionsOf(x.body).includes('report_coverage'));
  assert.equal(reads.length, 2);
  const check = gemini.requests.find((x) => functionsOf(x.body).includes('report_criterion')).body;
  assert.equal(check.generationConfig.thinkingConfig.thinkingLevel, 'low');
  assert.equal(check.generationConfig.thinkingConfig.includeThoughts, undefined);
  assert.equal(check.toolConfig.functionCallingConfig.mode, 'ANY');
  assert.match(systemOf(check), /1\. The home page links to \/guides\/snapshots\/ for what is lost\n2\. The home page says what it is/);
  assert.match(systemOf(check), /A snapshot keeps every actor\./, 'the page as served');
  assert.deepEqual(s.criteria.items.map((c) => [c.met, c.quote ?? null]), [[true, 'Read the snapshots guide.'], [false, null]]);

  // a regression: read again with fewer answered
  answered = 0;
  await reviewer.dispatch({ type: 'read', audienceIDs: ['site-tech-lead'] });
  const worse = reviewer.getState().readings.find((x) => x.audienceID === 'site-tech-lead');
  assert.equal(worse.history.length, 2);
  assert.deepEqual(worse.compared.regressed.map((c) => [c.from, c.to]), [['answered', 'partial'], ['answered', 'missing'], ['answered', 'missing'], ['partial', 'missing']]);

  reviewer.close();
});

test('export: criteria the last check found met are ticked, with their quote; the others stay open', async () => {
  const store = createMemoryStore();
  await store.putPlan(PLAN);
  await store.putCheck({ page: `${SITE}/`, at: 't', results: [
    { id: 'a', text: 'The home page links to /guides/snapshots/ for what is lost', page: `${SITE}/`, met: true, quote: 'Read the snapshots guide.' },
    { id: 'b', text: 'The home page says what it is', page: `${SITE}/`, met: false },
  ] });
  await store.putCheck({ page: `${SITE}/guides/snapshots/`, at: 't', results: [{ id: 'c', text: '/guides/snapshots/ answers tech-lead.host-dies', page: `${SITE}/guides/snapshots/`, met: true }] });
  const page = fakePage({ url: `${SITE}/`, title: 'tinyactors', text: HOME_TEXT, purpose: HOME });
  const env = await setup({ page, store, fetch: smartGemini(), fetchFile: siteFiles(), fetchPage: servedPages({ [`${SITE}/`]: { text: HOME_TEXT } }) });
  await env.reviewer.dispatch({ type: 'export', scope: 'all', to: 'download' });
  const files = unzip(env.downloads[0].bytes);
  const changes = decoder.decode(files.find((f) => f.name === 'CHANGES.md').data);
  assert.match(changes, /- \[x\] The home page links to \/guides\/snapshots\/ for what is lost\. \(tech-lead\.host-dies\) Met already: “Read the snapshots guide\.”\n/);
  assert.match(changes, /- \[ \] The home page says what it is\. \(tech-lead\.decide\)\n/);
  assert.match(changes, /  - \[x\] \/guides\/snapshots\/ answers tech-lead\.host-dies\. Met already\./, 'a task\'s criterion for another page too');
  const xml = decoder.decode(files.find((f) => f.name === 'packet.xml').data);
  assert.match(xml, /Met already: \/guides\/snapshots\/ answers tech-lead\.host-dies/);
  const readme = decoder.decode(files.find((f) => f.name === 'README.md').data);
  assert.match(readme, /Met already/);
  env.reviewer.close();
});

test('the hash of a page\'s text ignores whitespace; a page without fetchPage hashes its live text', async () => {
  assert.equal(await textHash('a  b\n c'), await textHash('a b c'));
  assert.notEqual(await textHash('a b'), await textHash('a c'));
  const { reviewer } = await setup({ page: fakePage({ text: 'One. Two.' }) });
  assert.equal(reviewer.getState().page.changed, false);
  await assert.rejects(reviewer.dispatch({ type: 'site.read' }), /cannot read other pages/);
  reviewer.close();
});
