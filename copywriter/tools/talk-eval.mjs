// A talk eval against the real Gemini: the live home page in Chromium (the
// DOM page adapter's context(), as the widget has it), and a short
// conversation with each saved starting audience: "What would make you
// close this tab?", then a correction that should make the reader call
// update_audience. It prints each answer, the audience's changes and gaps,
// and flags an answer with a number the page lacks (an invented fact) and
// a correction that changed nothing.
//
//   mise run talk-eval [-- --url URL --model M --thinking T --only NAME]   (GEMINI_API_KEY through fnox)

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
if (!key) { console.error('talk-eval: GEMINI_API_KEY is not set (run through fnox)'); process.exit(2); }

// The correction each starting audience gets, by id.
const CORRECTIONS = {
  'site-agent-platform': 'You\'d actually know Erlang and OTP well: you ran an Elixir backend for years.',
  'site-workflow-tester': 'Actually, you work in TypeScript on Node.js now, not Python.',
  'site-tech-lead': 'You\'d also care about supply-chain security: signed releases and an SBOM.',
};
const FIRST = 'What would make you close this tab?';

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
const { text } = await call('context');
const numbers = new Set(text.match(/\d[\d.,]*/g)?.map((n) => n.replace(/[.,]$/, '')));

let problems = 0;
const audiences = reviewer.getState().settings.audiences.filter((a) => CORRECTIONS[a.id] && (!values.only || a.name.includes(values.only)));
for (const audience of audiences) {
  console.log(`\n=== ${audience.name}`);
  await reviewer.dispatch({ type: 'talk', audienceID: audience.id });
  for (const said of [FIRST, CORRECTIONS[audience.id]]) {
    const started = performance.now();
    await reviewer.dispatch({ type: 'chat', text: said });
    const { messages, error } = reviewer.getState().chat;
    const reply = messages.at(-1);
    console.log(`> ${said}`);
    if (error || reply.role !== 'model') { console.log(`!! ${error ?? 'no reply'}`); problems += 1; continue; }
    console.log(`< (${((performance.now() - started) / 1000).toFixed(0)} s) ${reply.text}`);
    const invented = (reply.text.match(/\d[\d.,]*/g) || []).map((n) => n.replace(/[.,]$/, '')).filter((n) => !numbers.has(n));
    if (invented.length) { console.log(`!! a number the page lacks: ${invented.join(', ')}`); problems += 1; }
    for (const c of reply.audiences ?? []) {
      if (c.action !== 'update') continue;
      for (const x of c.changes) console.log(`  updated ${x.field}: “${x.before}” → “${x.after}”`);
    }
    for (const id of reply.annotations ?? []) {
      const a = reviewer.getState().annotations.find((y) => y.id === id);
      if (a) console.log(`  gap: ${a.original} (${a.finding.explanation})`);
    }
    if (said !== FIRST && !(reply.audiences ?? []).some((c) => c.action === 'update')) { console.log('!! the correction changed nothing'); problems += 1; }
  }
}
reviewer.close();
await browser.close();
console.log(`\ntalk-eval: ${problems} problem(s)`);
process.exit(problems ? 1 : 0);
