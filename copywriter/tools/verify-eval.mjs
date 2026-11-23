// A verification eval against the real Gemini: the live home page in
// Chromium, read as its audiences, planned, two items accepted (the first
// rewrite here and the first link) and kept, so the page shows them; then
// the page "as served" is that changed page (a deploy simulated), the panel
// opens it again, sees it changed, and checks the changes ("Check changes":
// read again, compared, the accepted items' criteria checked). It prints
// before → after per audience, what rose and fell, and each criterion met
// or not with its quote, with the times.
//
//   node tools/verify-eval.mjs [--url URL --model M]   (GEMINI_API_KEY through fnox)

import { parseArgs } from 'node:util';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import * as esbuild from 'esbuild';
import { createReviewer } from '../src/core/reviewer.mjs';
import { createMemoryStore } from '../src/core/store.mjs';
import { fakeCredentials } from '../test/fakes.mjs';

const { values } = parseArgs({ options: {
  url: { type: 'string', default: 'https://www.tinyactors.dev/' },
  model: { type: 'string', default: 'gemini-4-hydrogen' },
} });
const key = process.env.GEMINI_API_KEY;
if (!key) { console.error('verify-eval: GEMINI_API_KEY is not set (run through fnox)'); process.exit(2); }

const here = dirname(fileURLToPath(import.meta.url));
const { outputFiles } = await esbuild.build({
  stdin: {
    contents: `import { createDOMPage, readHTML, purposeOf } from './src/platform/dom.mjs'; window.cwPage = createDOMPage();
      window.cwFetchPage = async (url) => { const r = await fetch(url, { cache: 'no-cache' }); if (!r.ok) throw new Error(url + ': ' + r.status); return readHTML(await r.text(), url); };
      window.cwServedNow = () => ({ url: location.href, title: document.title, lang: 'en', ...window.cwPage.context(), ...purposeOf(document) });`,
    resolveDir: join(here, '..'),
  },
  bundle: true, format: 'iife', write: false, logLevel: 'silent',
});
const { chromium } = createRequire(import.meta.url)('playwright');
const browser = await chromium.launch();
const tab = await browser.newPage({ viewport: { width: 412, height: 915 } });
await tab.goto(values.url, { waitUntil: 'networkidle' });
await tab.addScriptTag({ content: outputFiles[0].text });
const call = (method, ...args) => tab.evaluate(([m, a]) => window.cwPage[m](...a), [method, args]);
let navigate = () => {};
let deployed = false;
const page = {
  info: () => call('info'), context: () => call('context'), purpose: () => call('purpose'), selection: () => null, onSelection() {},
  locate: (a) => call('locate', a), anchor: (t) => call('anchor', t), highlight: () => {},
  preview: (r) => call('preview', r), revert: (r) => call('revert', r),
  onNavigate: (fn) => { navigate = fn; },
};
// as served: the live page, until the deploy is simulated: then the page as it shows the kept changes
const fetchPage = (url) => (deployed && url === values.url ? tab.evaluate(() => window.cwServedNow()) : tab.evaluate((u) => window.cwFetchPage(u), url));
const reviewer = createReviewer({ page, credentials: fakeCredentials(key), store: createMemoryStore(), fetchPage, download() {}, clipboard() {} });
await reviewer.ready();
await reviewer.dispatch({ type: 'settings.save', model: values.model });
const s0 = reviewer.getState();
const name = (id) => s0.settings.audiences.find((a) => a.id === id)?.name ?? id;
const clock = () => performance.now();
let t = clock();
await reviewer.dispatch({ type: 'read', audienceIDs: s0.settings.audienceIDs });
console.log(`read as ${s0.settings.audienceIDs.length} audiences in ${((clock() - t) / 1000).toFixed(1)} s`);
t = clock();
await reviewer.dispatch({ type: 'plan' });
const plan = reviewer.getState().plan;
console.log(`planned in ${((clock() - t) / 1000).toFixed(1)} s (medium): ${plan.items.length} items`);
const rewrite = plan.items.find((x) => x.action === 'rewrite' && x.anchored);
const link = plan.items.find((x) => x.action === 'link' && x.anchored) || plan.items.find((x) => x.action === 'link');
for (const x of [rewrite, link].filter(Boolean)) {
  await reviewer.dispatch({ type: 'plan.accept', id: x.id });
  const made = reviewer.getState().plan.items.find((y) => y.id === x.id).annotationID;
  const a = reviewer.getState().annotations.find((y) => y.id === made);
  console.log(`\naccepted ${x.action}: “${x.original ?? ''}” → “${x.text}”`);
  for (const c of x.criteria) console.log(`  criterion [${new URL(c.page).pathname}] ${c.text}`);
  if (a && !a.pageLevel) {
    await reviewer.dispatch({ type: 'preview', id: a.id, suggestionID: a.suggestions[0].id });
    await reviewer.dispatch({ type: 'keep', id: a.id });
    console.log(`  kept: ${reviewer.getState().annotations.find((y) => y.id === a.id).state}`);
  }
}

// the deploy: the page as served is the page with the kept changes; the panel opens it again
deployed = true;
navigate();
const changed = await new Promise((resolve) => {
  const end = setTimeout(() => resolve(false), 15000);
  const off = reviewer.subscribe((s) => { if (s.page.changed) { clearTimeout(end); off(); resolve(true); } });
});
console.log(`\nthe page changed since its readings: ${changed}`);
t = clock();
await reviewer.dispatch({ type: 'verify' });
await new Promise((resolve) => {
  const off = reviewer.subscribe((s) => { if (s.site.state !== 'reading') { off(); resolve(); } });
});
const s = reviewer.getState();
console.log(`checked in ${((clock() - t) / 1000).toFixed(1)} s; ${s.site.failures.length} failed`);
for (const r of s.readings) {
  if (!r.compared) { console.log(`  ${name(r.audienceID)}: no comparison (${r.state})`); continue; }
  const { before, after, improved, regressed, unchanged } = r.compared;
  console.log(`\n${name(r.audienceID)}: ${before.answered}/${before.total} → ${after.answered}/${after.total} (${regressed.length} regressed, ${improved.length} improved, ${unchanged} unchanged)`);
  for (const x of regressed) console.log(`  ↓ ${x.needID ?? ''} ${x.from} → ${x.to}: ${x.text}`);
  for (const x of improved) console.log(`  ↑ ${x.needID ?? ''} ${x.from} → ${x.to}: ${x.text}`);
}
console.log(`\ncriteria of this page (${s.criteria?.items.length ?? 0}):`);
for (const c of s.criteria?.items ?? []) console.log(`  ${c.met === true ? '✓' : c.met === false ? '✗' : '?'} ${c.text}${c.quote ? `\n      “${c.quote}”` : ''}`);
reviewer.close();
await browser.close();
process.exit(0);
