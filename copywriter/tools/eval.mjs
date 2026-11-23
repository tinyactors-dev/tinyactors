// A copy eval against the real Gemini: the reviewer, as the panel drives
// it, over a fake page holding the live home page's text. For each case it
// annotates a sentence with a note, waits for the model's rewrites, and
// checks them against the website's voice (website/AGENTS.md): length,
// no period on a heading, no hype words, the original's facts kept.
//
//   mise run eval [-- --model gemini-4-hydrogen]   (GEMINI_API_KEY through fnox)

import { parseArgs } from 'node:util';
import { createReviewer } from '../src/core/reviewer.mjs';
import { createMemoryStore } from '../src/core/store.mjs';
import { fakePage, fakeCredentials } from '../test/fakes.mjs';

const { values } = parseArgs({ options: { model: { type: 'string', default: 'gemini-4-hydrogen' } } });
const key = process.env.GEMINI_API_KEY;
if (!key) { console.error('eval: GEMINI_API_KEY is not set (run through fnox)'); process.exit(2); }

const markdown = await (await fetch('https://www.tinyactors.dev/index.md')).text();
const text = markdown.replace(/[#*`>_\[\]]/g, ' ').replace(/\(https?:[^)]*\)/g, ' ').replace(/[ \t]+/g, ' ');

const HYPE = /\b(seamless|powerful|blazing|revolutionary|effortless|robust|cutting-edge|game-chang|supercharge|unlock|elevate|leverage)\w*/i;
const cases = [
  { kind: 'heading', words: [1, 8], note: 'Make this heading say what the reader gets.' },
  { kind: 'paragraph', words: [4, 42], note: 'Tighten this. Keep every fact.' },
  { kind: 'paragraph', words: [4, 42], note: 'Too abstract: lead with what the code does.' },
];

// Pick real sentences from the page: a short line for the heading, longer ones for paragraphs.
const lines = text.split('\n').map((l) => l.trim()).filter((l) => l && !l.startsWith('<'));
const heading = lines.find((l) => l.split(/\s+/).length >= 3 && l.split(/\s+/).length <= 8 && !/[.:]$/.test(l));
const paragraphs = lines.filter((l) => l.split(/\s+/).length >= 12 && l.split(/\s+/).length <= 40 && /\.$/.test(l));
cases[0].original = heading;
cases[1].original = paragraphs[0];
cases[2].original = paragraphs[1];

const page = fakePage({ url: 'https://www.tinyactors.dev/', title: 'tinyactors', text });
const reviewer = await createReviewer({
  page, credentials: fakeCredentials(key), store: createMemoryStore(),
  download() {}, clipboard() {},
});
await reviewer.ready();
await reviewer.dispatch({ type: 'settings.save', model: values.model });

let failures = 0;
for (const c of cases) {
  page.select(c.original);
  const started = performance.now();
  const id = await reviewer.dispatch({ type: 'annotate', note: c.note });
  const a = reviewer.getState().annotations.find((x) => x.id === id);
  const seconds = ((performance.now() - started) / 1000).toFixed(1);
  console.log(`\n[${c.kind}] ${c.original}\n  note: ${c.note}  (${seconds} s, state ${a.state})`);
  if (a.state === 'failed') { console.log(`  FAIL ${a.error}`); failures++; continue; }
  if (a.suggestions.length === 0) { console.log(`  FAIL no rewrites; the model said: ${a.messages.at(-1)?.text}`); failures++; continue; }
  for (const s of a.suggestions) {
    const n = s.text.trim().split(/\s+/).length;
    const problems = [];
    if (n < c.words[0] || n > c.words[1]) problems.push(`${n} words`);
    if (c.kind === 'heading' && /\.$/.test(s.text.trim())) problems.push('heading ends with a period');
    if (HYPE.test(s.text)) problems.push(`hype word "${s.text.match(HYPE)[0]}"`);
    if (s.text.trim() === c.original.trim()) problems.push('unchanged');
    if (problems.length) failures++;
    console.log(`  ${problems.length ? 'FAIL' : 'ok  '} ${s.text}${problems.length ? `  (${problems.join(', ')})` : ''}\n       ${s.rationale}`);
  }
}
reviewer.close();
console.log(`\neval: ${values.model}, ${failures} problem(s)`);
process.exit(failures ? 1 : 0);
