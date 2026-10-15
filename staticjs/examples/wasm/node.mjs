// Evaluate staticjs programs through staticjs.wasm in Node.
//
//   node node.mjs staticjs.wasm staticjs.api.json
import { readFileSync } from 'node:fs';
import { inspect } from 'node:util';
import { load, StaticJSError } from './staticjs.mjs';

const [wasmPath = 'staticjs.wasm', apiPath = 'staticjs.api.json'] = process.argv.slice(2);
const staticjs = await load(readFileSync(wasmPath), JSON.parse(readFileSync(apiPath, 'utf8')));
const show = (value) => console.log(inspect(value, { depth: null, breakLength: Infinity }));

// A context holds values, programs and evaluations, under limits of its own.
const context = staticjs.createContext();

// A result comes out as a JavaScript value (the default), as JSON, or as
// JavaScript source that evaluates to it.
show(context.evaluate('[3, 1, 2].toSorted((a, b) => a - b)'));
show(context.evaluate('new Map([["ada", 36n], ["grace", 85n]])'));
console.log(context.evaluate('({ when: new Date(0), sizes: new Set([1, 2]), ratio: NaN })', { output: 'json' }));
console.log(context.evaluate('new Map([["ada", 36n], ["grace", 85n]])', { output: 'source' }));
show(context.evaluate('export const total = [1, 2, 3].reduce((a, b) => a + b, 0);', { module: true }));
show(context.evaluate('new Date().toISOString()', { now: Date.UTC(2024, 0, 31, 10) }));

// Values go in as globals. A program compiled once is evaluated per record.
const total = context.compile('order.items.reduce((sum, item) => sum + item.price * item.count, 0)',
  { globals: ['order'] });
for (const order of [{ items: [{ price: 2.5, count: 2 }, { price: 1, count: 3 }] }, { items: [] }]) {
  show(total.evaluate({ order }));
}
total.release();

// Refusals, faults and running out of fuel throw a StaticJSError.
const attempt = (run) => {
  try {
    run();
  } catch (error) {
    if (!(error instanceof StaticJSError)) throw error;
    console.log(error.message);
  }
};
for (const source of ['({ a: 1 }).b.c', '[1, 2', 'xs.map((x) => x * 2)', 'Array.from({ length: 1e6 }, (_, i) => i)']) {
  attempt(() => context.evaluate(source, { fuel: 10_000 }));
}

// Each context has its own limits: this one holds 1,000 items in a value.
const small = staticjs.createContext({ limits: { max_value_items: 1000 } });
attempt(() => small.evaluate('Array.from({ length: 5000 }, (_, i) => i).length'));
show(context.evaluate('Array.from({ length: 5000 }, (_, i) => i).length'));
small.close();

// Run on slices of fuel, and let the event loop run between them: two
// evaluations take turns, and timers, I/O and other requests go on meanwhile.
const sum = context.compile('Array.from({ length: n }, (_, i) => i).reduce((a, b) => a + b, 0)',
  { globals: ['n'] });
const turns = [];
async function inSlices(name, n) {
  const evaluation = sum.start({ n });
  while (evaluation.run(10_000) === 'paused') {
    turns.push(name);
    await new Promise((resolve) => setImmediate(resolve));
  }
  return `${name}: ${evaluation.result} in ${evaluation.fuelUsed} units`;
}
console.log((await Promise.all([inSlices('a', 20_000), inSlices('b', 5_000)])).join(', '));
console.log(`slices: ${turns.join(' ')}`);
sum.release();

console.log(`bytes held: ${context.liveBytes}`);
context.close();
