// A site eval against the real Gemini: the live home page in Chromium (the
// DOM page adapter, as the widget has it), then "Read the site" for the
// pages named, each as its own audiences, at most three readings at once;
// every other page fetched as served and read with readHTML in the same
// tab. It prints the time of the whole and of each reading, the grid (each
// need's status on each page read), and where the readings disagree with
// what the site's sources claim (`answers=`, website/build/needs.json from
// `cd website && mise run build`): a need the content says a page answers
// that its reading did not find answered, and one a reading found answered
// on a page the content does not name.
//
//   mise run site-eval [-- --pages /,/chat/ --model M --thinking T]   (GEMINI_API_KEY through fnox)

import { parseArgs } from 'node:util';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { readFileSync, existsSync } from 'node:fs';
import * as esbuild from 'esbuild';
import { createReviewer } from '../src/core/reviewer.mjs';
import { createMemoryStore } from '../src/core/store.mjs';
import { fakeCredentials } from '../test/fakes.mjs';

const { values } = parseArgs({ options: {
  url: { type: 'string', default: 'https://www.tinyactors.dev/' },
  pages: { type: 'string', default: '/,/chat/,/guides/testing/,/performance/,/about/,/guides/snapshots/' },
  model: { type: 'string', default: 'gemini-4-hydrogen' },
  thinking: { type: 'string', default: 'high' },
} });
const key = process.env.GEMINI_API_KEY;
if (!key) { console.error('site-eval: GEMINI_API_KEY is not set (run through fnox)'); process.exit(2); }

const here = dirname(fileURLToPath(import.meta.url));
const { outputFiles } = await esbuild.build({
  stdin: {
    contents: `import { createDOMPage, readHTML } from './src/platform/dom.mjs'; window.cwPage = createDOMPage();
      window.cwFetchPage = async (url) => { const r = await fetch(url, { cache: 'no-cache' }); if (!r.ok) throw new Error(url + ': ' + r.status); return readHTML(await r.text(), url); };`,
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
const page = {
  info: () => call('info'), context: () => call('context'), purpose: () => call('purpose'), selection: () => null, onSelection() {},
  locate: (a) => call('locate', a), anchor: (t) => call('anchor', t), preview: () => false, revert: () => false, highlight: () => {},
};
const fetchPage = (url) => tab.evaluate((u) => window.cwFetchPage(u), url);
const reviewer = createReviewer({ page, credentials: fakeCredentials(key), store: createMemoryStore(), fetchPage, download() {}, clipboard() {} });
await reviewer.ready();
await reviewer.dispatch({ type: 'settings.save', model: values.model, thinking: values.thinking });
const origin = new URL(values.url).origin;
const urls = values.pages.split(',').map((p) => new URL(p, origin).href);

const started = performance.now();
const jobs = await reviewer.dispatch({ type: 'site.read', urls });
console.log(`site.read: ${jobs} jobs for ${urls.length} pages (${values.model}, readings at medium)`);
let last = '';
await new Promise((resolve) => {
  const off = reviewer.subscribe((s) => {
    const p = s.site.progress;
    const line = p ? `${p.done}/${p.total}${p.failed ? ` (${p.failed} failed)` : ''}` : '';
    if (line !== last) { last = line; console.log(`  ${((performance.now() - started) / 1000).toFixed(0)} s: ${line}`); }
    if (s.site.state !== 'reading') { off(); resolve(); }
  });
});
const seconds = (performance.now() - started) / 1000;
const s = reviewer.getState();
const each = s.readings.map((r) => r.seconds).filter(Boolean);
console.log(`\nread in ${seconds.toFixed(1)} s (the open page's readings: ${each.join(', ')} s); ${s.site.failures.length} failed${s.site.failures.map((f) => `\n  !! ${new URL(f.page).pathname} ${f.audienceID ?? ''}: ${f.error}`).join('')}`);

const short = (u) => new URL(u).pathname;
const needsFile = join(here, '../../website/build/needs.json');
const content = existsSync(needsFile) ? JSON.parse(readFileSync(needsFile, 'utf8')) : null;
if (!content) console.log('(no website/build/needs.json: build the website to compare with its answers=)');
const disagree = { claimedNotFound: [], foundNotClaimed: [] };
const glyph = { answered: '✓', partial: '◐', missing: '○' };
for (const a of s.site.audiences) {
  const read = a.pages.filter((u) => urls.includes(u) || s.site.pages.find((p) => p.url === u)?.read);
  if (!read.length) continue;
  console.log(`\n=== ${a.name} (${a.siteID}): ${a.answered}/${a.needs.length} needs answered on some page read; columns ${read.map(short).join(' ')}`);
  for (const u of read) {
    const cells = a.needs.map((n) => n.cells.find((c) => c.url === u));
    const count = (st) => cells.filter((c) => c?.status === st).length;
    console.log(`  ${short(u).padEnd(20)} ${count('answered')}/${a.needs.length} answered, ${count('partial')} partial, ${count('missing')} missing, ${cells.filter((c) => !c?.status).length} unknown`);
  }
  const claimedOf = (id) => content?.audiences.find((x) => x.id === a.siteID)?.needs.find((n) => n.id === id)?.answeredBy || [];
  console.log(`  ${'need'.padEnd(30)} ${read.map((u) => short(u).slice(0, 10).padEnd(11)).join('')}content answers=`);
  for (const n of a.needs) {
    const claimed = claimedOf(n.id);
    const row = read.map((u) => {
      const c = n.cells.find((x) => x.url === u);
      const claim = claimed.some((x) => x.path === short(u));
      if (content && claim && c?.status && c.status !== 'answered') disagree.claimedNotFound.push({ need: n.id, page: short(u), status: c.status, sections: claimed.filter((x) => x.path === short(u)).map((x) => x.section), why: c.explanation || '' });
      if (content && !claim && c?.status === 'answered') disagree.foundNotClaimed.push({ need: n.id, page: short(u), why: c.explanation || '' });
      return `${c?.status ? glyph[c.status] : '·'}${claim ? '*' : ' '}`.padEnd(11);
    }).join('');
    console.log(`  ${n.id.padEnd(30)} ${row}${claimed.map((x) => `${x.path}${x.section ? `#${x.section}` : ""}`).join(' ')}`);
  }
}
if (content) {
  console.log('\n=== where the readings and the content\'s answers= disagree (* in the grid: the content claims it)');
  console.log(`\nclaimed by the content, not found answered by the reading (${disagree.claimedNotFound.length}):`);
  for (const d of disagree.claimedNotFound) console.log(`  ${d.need} on ${d.page} (${d.sections.filter(Boolean).map((x) => `#${x}`).join(", ") || "the page"}): ${d.status}. ${d.why}`);
  console.log(`\nfound answered by the reading, not claimed by the content (${disagree.foundNotClaimed.length}):`);
  for (const d of disagree.foundNotClaimed) console.log(`  ${d.need} on ${d.page}: ${d.why}`);
}
reviewer.close();
await browser.close();
process.exit(0);
