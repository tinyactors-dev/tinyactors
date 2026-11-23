// A reading eval against the real Gemini: the live home page in Chromium
// (the DOM page adapter's context() and anchor(), as the widget has them),
// read as each audience the page links (its <link rel="audience"> files,
// which seed the store as they would in the widget). It prints each
// reading's time, its findings per kind and placement, the checklist with
// each need's id and kind, the claims the page's sections make
// (data-answers) as confirmed or doubted, and checks that every anchored
// finding quotes text on the page and every rewrite adds no number the
// page lacks.
//
//   mise run read-eval [-- --url URL --model M --thinking T]   (GEMINI_API_KEY through fnox; READ_EVAL_RAW=1 prints the calls)

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
  thinking: { type: 'string', default: 'high' },
  only: { type: 'string' },
} });
const key = process.env.GEMINI_API_KEY;
if (!key) { console.error('read-eval: GEMINI_API_KEY is not set (run through fnox)'); process.exit(2); }

const here = dirname(fileURLToPath(import.meta.url));
const { outputFiles } = await esbuild.build({
  stdin: { contents: `import { createDOMPage } from './src/platform/dom.mjs'; window.cwPage = createDOMPage();`, resolveDir: join(here, '..') },
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
  locate: (a) => call('locate', a), anchor: (t) => call('anchor', t),
  preview: () => false, revert: () => false, highlight: () => {},
};
// READ_EVAL_RAW=1 prints each reading's function calls as Gemini made them
const raw = process.env.READ_EVAL_RAW ? async (url, init) => {
  const response = await fetch(url, init);
  const body = await response.clone().json().catch(() => null);
  for (const part of body?.candidates?.[0]?.content?.parts || []) if (part.functionCall) console.log('raw', JSON.stringify(part.functionCall));
  return response;
} : undefined;
const reviewer = createReviewer({ page, credentials: fakeCredentials(key), store: createMemoryStore(), download() {}, clipboard() {}, ...(raw ? { fetch: raw } : {}) });
await reviewer.ready();
await reviewer.dispatch({ type: 'settings.save', model: values.model, thinking: values.thinking });
const { text } = await call('context');
const pageNumbers = new Set(text.match(/\d[\d.,]*/g)?.map((n) => n.replace(/[.,]$/, '')));

let problems = 0;
const linkedIDs = reviewer.getState().settings.audienceIDs;
const audiences = linkedIDs.map((id) => reviewer.getState().settings.audiences.find((a) => a.id === id))
  .filter((a) => a && (!values.only || a.name.includes(values.only)));
console.log(`page kind: ${reviewer.getState().page.kind ?? '(none)'}; linked audiences: ${audiences.map((a) => `${a.siteID} (${a.items?.length ?? 0} needs)`).join(', ') || '(none)'}`);
if (!audiences.length) { console.error('read-eval: the page links no audience'); process.exit(2); }
const claims = { confirmed: [], doubted: [] };
const started = performance.now();
await reviewer.dispatch({ type: 'read', audienceIDs: audiences.map((a) => a.id) });
console.log(`read ${audiences.length} audience(s) side by side in ${((performance.now() - started) / 1000).toFixed(1)} s (${values.model}, thinking ${values.thinking})`);
const state = reviewer.getState();
for (const r of state.readings) {
  const name = audiences.find((a) => a.id === r.audienceID).name;
  console.log(`\n=== ${name}: ${r.state}${r.error ? ` (${r.error})` : ''}, ${r.seconds} s`);
  const found = state.annotations.filter((a) => r.findingIDs.includes(a.id));
  const kinds = {};
  for (const a of found) kinds[a.finding.kind] = (kinds[a.finding.kind] || 0) + 1;
  console.log(`findings: ${found.length} ${JSON.stringify(kinds)}; placed ${found.filter((a) => !a.pageLevel).length}, page-level ${found.filter((a) => a.pageLevel).length}`);
  for (const a of found) {
    const flags = [];
    if (!a.pageLevel && !text.includes(a.original)) flags.push('ANCHOR NOT IN TEXT');
    for (const s of a.suggestions) {
      const extra = (s.text.match(/\d[\d.,]*/g) || []).map((n) => n.replace(/[.,]$/, '')).filter((n) => !pageNumbers.has(n));
      if (extra.length) flags.push(`NEW NUMBER ${extra.join(',')}`);
    }
    if (flags.length) problems += 1;
    console.log(`- [${a.finding.kind}${a.pageLevel ? ', page' : ''}] ${a.original.slice(0, 110)}\n    ${a.finding.explanation}${a.finding.need ? `\n    need: ${a.finding.need}` : ''}${a.suggestions[0] ? `\n    → ${a.suggestions[0].text}` : ''}${flags.length ? `\n    !! ${flags.join('; ')}` : ''}`);
  }
  const statuses = { answered: 0, partial: 0, missing: 0 };
  for (const x of r.items) statuses[x.status]++;
  console.log(`checklist ${statuses.answered}/${r.items.length} answered, ${statuses.partial} partial, ${statuses.missing} missing; answered with an anchor: ${r.items.filter((x) => x.status === 'answered' && x.anchor).length}`);
  for (const x of r.items) console.log(`  ${{ answered: '✓', partial: '◐', missing: '○' }[x.status]} [${x.field}${x.kind ? `, ${x.kind}` : ''}] ${x.needID ?? ''} ${x.text}${x.claimed ? `  (claimed by ${x.claimed.join(', ')})` : ''}${x.anchor ? `  ⟶ “${x.anchor.quote.exact.slice(0, 60)}”` : ''}\n      ${x.explanation}`);
  const kinds2 = {};
  for (const x of r.items) kinds2[x.kind ?? '?'] = (kinds2[x.kind ?? '?'] || 0) + 1;
  console.log(`needs by kind of page: ${JSON.stringify(kinds2)}; without a kind: ${r.items.filter((x) => !x.kind).length}`);
  for (const x of r.items.filter((y) => y.claimed)) {
    const doubt = found.find((a) => a.finding.kind === 'doubt' && a.finding.needID === x.needID);
    const entry = { need: x.needID, sections: x.claimed, status: x.status, explanation: x.explanation, ...(doubt ? { doubt: doubt.finding.explanation } : {}) };
    (x.status === 'answered' && !doubt ? claims.confirmed : claims.doubted).push(entry);
  }
}
console.log(`\n=== claims (data-answers): ${claims.confirmed.length} confirmed, ${claims.doubted.length} doubted`);
for (const c of claims.confirmed) console.log(`  ✓ ${c.need} by ${c.sections.join(', ')}`);
for (const c of claims.doubted) console.log(`  ? ${c.need} by ${c.sections.join(', ')}: ${c.status}. ${c.explanation}${c.doubt ? `\n      doubt: ${c.doubt}` : ''}`);
reviewer.close();
await browser.close();
console.log(`\nread-eval: ${problems} problem(s)`);
process.exit(problems ? 1 : 0);
