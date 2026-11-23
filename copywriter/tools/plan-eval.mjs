// A plan eval against the real Gemini: the live page in Chromium (the DOM
// page adapter, as the widget has it), read as each audience it links,
// then planned ("Revise page") with the site map of its origin (llms.txt).
// It prints the plan's items by action and target, the conflicts it
// resolved and how, the proposed pages, each criterion, and flags every
// number in the plan's copy that neither the page nor the site map states
// (an invented fact), with the times of the readings and the plan.
//
//   mise run plan-eval [-- --url URL --model M --thinking T]   (GEMINI_API_KEY through fnox)

import { parseArgs } from 'node:util';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import * as esbuild from 'esbuild';
import { createReviewer } from '../src/core/reviewer.mjs';
import { createMemoryStore } from '../src/core/store.mjs';
import { siteMap } from '../src/core/site.mjs';
import { fakeCredentials } from '../test/fakes.mjs';

const { values } = parseArgs({ options: {
  url: { type: 'string', default: 'https://www.tinyactors.dev/' },
  model: { type: 'string', default: 'gemini-4-hydrogen' },
  thinking: { type: 'string', default: 'high' },
} });
const key = process.env.GEMINI_API_KEY;
if (!key) { console.error('plan-eval: GEMINI_API_KEY is not set (run through fnox)'); process.exit(2); }

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
const reviewer = createReviewer({ page, credentials: fakeCredentials(key), store: createMemoryStore(), download() {}, clipboard() {} });
await reviewer.ready();
await reviewer.dispatch({ type: 'settings.save', model: values.model, thinking: values.thinking });
const ids = reviewer.getState().settings.audienceIDs;
if (!ids.length) { console.error('plan-eval: the page links no audience'); process.exit(2); }
const name = (id) => reviewer.getState().settings.audiences.find((a) => a.id === id)?.name ?? id;

let started = performance.now();
await reviewer.dispatch({ type: 'read', audienceIDs: ids });
console.log(`read as ${ids.map(name).join(', ')} in ${((performance.now() - started) / 1000).toFixed(1)} s`);
for (const r of reviewer.getState().readings) {
  const n = (s) => r.items.filter((x) => x.status === s).length;
  console.log(`  ${name(r.audienceID)}: ${r.state}, ${r.seconds} s, ${n('answered')}/${r.items.length} answered, ${n('partial')} partial, ${n('missing')} missing, ${r.findingIDs.length} findings`);
}

started = performance.now();
try {
  await reviewer.dispatch({ type: 'plan' });
} catch (e) {
  console.error(`plan-eval: the plan failed: ${e.message}`);
  process.exit(1);
}
const seconds = (performance.now() - started) / 1000;
const plan = reviewer.getState().plan;
const short = (t) => (t === 'here' || t === 'new' ? t : new URL(t).pathname);
console.log(`\nplanned in ${seconds.toFixed(1)} s (${values.model}, thinking ${values.thinking}), with ${plan.sitePages} pages of the site map: ${plan.items.length} items, ${plan.newPages.length} new pages`);

// the facts the plan may use: the page's text and the site map's titles and descriptions
const { text } = await call('context');
const map = await siteMap(new URL(values.url).origin, async (u) => { const r = await fetch(u); if (!r.ok) throw new Error(r.status); return r.text(); });
const known = `${text}\n${map.map((p) => `${p.title} ${p.description}`).join('\n')}`;
const numbers = (t) => (String(t).match(/\d[\d.,]*/g) || []).map((n) => n.replace(/[.,]$/, ''));
const knownNumbers = new Set(numbers(known));
let invented = 0;

const byAction = {};
for (const x of plan.items) byAction[`${x.action} → ${short(x.target)}`] = (byAction[`${x.action} → ${short(x.target)}`] || 0) + 1;
console.log(`by action and target: ${JSON.stringify(byAction)}`);
for (const x of plan.items) {
  const extra = [...new Set([...numbers(x.text), ...numbers(x.targetText || '')].filter((n) => !knownNumbers.has(n)))];
  if (extra.length) invented += 1;
  console.log(`\n- [${x.action} → ${short(x.target)}${x.targetTitle ? ` (${x.targetTitle})` : ''}${x.routed ? ', routed by the core' : ''}] ${x.needs.join(' ') || '(no need)'}`);
  if (x.original) console.log(`    on the page: “${x.original.slice(0, 120)}”${x.anchored ? '' : ' (NOT FOUND ON THE PAGE)'}`);
  console.log(`    text: ${x.text.slice(0, 240)}`);
  if (x.targetText) console.log(`    for the target: ${x.targetText.slice(0, 240)}`);
  if (x.where) console.log(`    where: ${x.where}`);
  console.log(`    why: ${x.reason}`);
  if (x.conflict) console.log(`    conflict: ${x.conflict}`);
  for (const c of x.criteria) console.log(`    ✓? ${c.text}${c.page && c.page !== values.url ? `  [${short(c.page)}]` : ''}`);
  if (extra.length) console.log(`    !! numbers neither the page nor the site map states: ${extra.join(', ')}`);
}
for (const p of plan.newPages) {
  console.log(`\n+ new ${p.kind} page “${p.title}” for ${p.audiences.join(', ')}: ${p.needs.join(' ')}\n    ${p.outline.join(' / ')}${p.reason ? `\n    why: ${p.reason}` : ''}`);
}
const conflicts = plan.items.filter((x) => x.conflict);
console.log(`\nconflicts resolved: ${conflicts.length} (${conflicts.filter((x) => x.routed).length} routed by the core, ${conflicts.filter((x) => !x.routed).length} by the model)`);
console.log(`items with an original not found on the page: ${plan.items.filter((x) => x.original && !x.anchored).length}`);
console.log(`plan-eval: ${invented} item(s) with numbers the site does not state`);
reviewer.close();
await browser.close();
process.exit(0);
