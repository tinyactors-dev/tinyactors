// The guide's runs, in Node.js. Run: node main.mjs
import { readFileSync } from 'node:fs';
import { broken, compile, run, search, shrink } from './property.mjs';

const chart = readFileSync(new URL('./payment.scxml', import.meta.url), 'utf8');
const show = (answers) => `[${answers.map((yes) => (yes ? 'answer' : 'silence')).join(', ')}]`;

const correct = compile(chart);
const found = await search(correct, 1000);
console.log(found ? `seed ${found.seed} breaks ${broken(found.run).join(', ')}` : '1000 worlds: every property held');

const buggy = compile(chart.replace('attempts &lt; 3', 'attempts &lt; 4')); // one retry too many
const failure = await search(buggy, 1000);
console.log(`seed ${failure.seed}: ${show(failure.run.answers)} breaks ${broken(failure.run).join(', ')}`);
const smallest = await shrink(buggy, failure.run.answers);
const replay = await run(buggy, smallest, { trace: true });
console.log(`shrunk to ${show(smallest)}: ${replay.charges.length} charges, ${replay.outcome}`);
console.log(`its trace: ${replay.spans.filter((span) => span.name.startsWith('scxml.macrostep')).map((span) => span.name.replace('scxml.macrostep ', '')).join(' → ')}`);
correct.close();
buggy.close();
