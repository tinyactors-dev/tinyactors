// examples/wasm/staticjs.mjs against staticjs.wasm (mise run wasm-test):
//   node tests/wasm/example_test.mjs <staticjs.wasm> <staticjs.api.json> <staticjs.h>
//
// Its default limits are the header's SJS_LIMITS_DEFAULT; values cross both
// ways and come out as values, JSON and source; evaluations run on slices,
// are cancelled and are ended by close(); and every one gives back what it
// held.
import { readFileSync } from 'node:fs';
import assert from 'node:assert/strict';
import { DEFAULT_LIMITS, load, StaticJSError } from '../../examples/wasm/staticjs.mjs';

const [wasmPath, apiPath, headerPath] = process.argv.slice(2);
const header = readFileSync(headerPath, 'utf8');
const defaults = header.match(/#define SJS_LIMITS_DEFAULT[\s\S]*?\}\)/)[0];
const limits = Object.fromEntries([...defaults.matchAll(/\.(\w+) = ([\du *]+?)\s*[,}]/g)]
  .map(([, name, value]) => [name, value.replace(/u/g, '').split('*').reduce((a, b) => a * Number(b), 1)]));
assert.deepEqual({ ...DEFAULT_LIMITS }, limits, 'examples/wasm/staticjs.mjs gives the header\'s default limits');

const staticjs = await load(readFileSync(wasmPath), JSON.parse(readFileSync(apiPath, 'utf8')));
const context = staticjs.createContext();
const evaluate = (source, options) => context.evaluate(source, options);
let checks = 0;
const same = (actual, expected, what) => {
  assert.deepEqual(actual, expected, what);
  checks++;
};

// Out, as values.
same(evaluate('[undefined, null, true, 1.5, -0, "été", 2n ** 70n]'),
  [undefined, null, true, 1.5, -0, 'été', 2n ** 70n], 'primitives');
same(evaluate('({ b: [1, { c: new Set(["x"]) }], a: new Map([[1, "one"], ["k", [2]]]) })'),
  { b: [1, { c: new Set(['x']) }], a: new Map([[1, 'one'], ['k', [2]]]) }, 'containers, in their order');
same(Object.keys(evaluate('({ b: 1, a: 2, 1: 3 })')), ['1', 'b', 'a'], 'keys in JavaScript\'s order');
same(evaluate('/a+(b)/giu'), /a+(b)/giu, 'a regular expression');
same(evaluate('new Date(86400000)'), new Date(86400000), 'a Date');
same(evaluate('new Date(NaN)').getTime(), NaN, 'an invalid Date');
same(evaluate('"a1b".match(/\\d/)'), ['1'], 'a match result, by its elements');
for (const source of ['[1].values()', '(x) => x']) assert.throws(() => evaluate(source), TypeError, source);
if (globalThis.Temporal === undefined) {
  assert.throws(() => evaluate('Temporal.PlainDate.from("2024-01-31")'), TypeError, 'no Temporal here');
}

const tree = staticjs.createContext({ limits: { max_value_items: 1000 } });
same(tree.evaluate('Array.from({ length: 999 }, (_, i) => i)').length, 999, 'a result of as many values as the limit');
assert.throws(() => tree.evaluate('Array.from({ length: 12 }).reduce((a) => [a, a], 0)'),
  (e) => e instanceof StaticJSError && e.kind === 'limit', 'a result whose tree is past the limit');
assert.throws(() => tree.evaluate('Array.from({ length: 12 }).reduce((a) => [a, a], 0)', { output: 'json' }),
  (e) => e.kind === 'limit', 'and as JSON');
same(tree.liveBytes, 0, 'a result past the limit is given back');
tree.close();

// Out, as JSON and as source.
for (const source of ['({ a: [1, -0, NaN, undefined, "\\u2028"], d: new Date(0), s: new Set([1]), m: new Map() })',
  '"a1b".match(/\\d/)', '({ f: (x) => x, r: /a/, i: [1].values(), t: Temporal.PlainDate.from("2024-01-31") })',
  'JSON.parse("[1e400]")']) {
  same(evaluate(source, { output: 'json' }), evaluate(`JSON.stringify(${source})`), `JSON of ${source}`);
}
assert.throws(() => evaluate('1n', { output: 'json' }), TypeError, 'JSON has no BigInt');
same(evaluate('new Set([1n, "a"])', { output: 'source' }), 'new Set([1n,"a"])', 'source');
assert.throws(() => evaluate('1', { output: 'cbor' }), TypeError, 'an output that is not one');

// In, as globals.
const input = { n: -0, s: 'ü', b: 12345678901234567890n, a: [1, [null, undefined]], o: { x: { y: true } },
  m: new Map([[1n, 'big'], ['k', new Set([1, 'a'])]]), t: new Set() };
same(evaluate('[n, s, b, a, o, m, t]', { globals: input }), Object.values(input), 'values in and out again');
same(evaluate('o.x.y && a[1].length', { globals: input }), 2, 'globals read by a program');
for (const bad of [{ v: new Date() }, { v: () => 1 }, { v: Symbol('s') }, { v: new Set([[1]]) }, { v: new Map([[{}, 1]]) },
  { v: JSON.parse('{"__proto__":1}') }]) {
  assert.throws(() => evaluate('v', { globals: bad }), TypeError, `no value for ${String(Object.values(bad)[0])}`);
}
const named = context.compile('a + b', { globals: ['a', 'b'] });
assert.throws(() => named.evaluate({ a: 1 }), TypeError, 'a global without a value');
same(named.evaluate({ a: 1, b: 2 }), 3, 'a program evaluated again');
same(named.evaluate({ a: 'x', b: 'y' }), 'xy', 'and again');

// On slices of fuel.
const evaluation = named.start({ a: [1, 2, 3].join(), b: '' });
const squares = context.compile('Array.from({ length: 1000 }, (_, i) => i * i).reduce((a, b) => a + b, 0)');
const slices = squares.start();
squares.release();
let runs = 0;
while (slices.run(100) === 'paused') runs++;
same([slices.result, runs > 10, slices.fuelUsed > 1000], [332833500, true, true], 'run on slices');
assert.throws(() => slices.run(100), /done/, 'a finished evaluation does not run');
named.release();
same(evaluation.run(1_000), 'done', 'an evaluation outlives its released program');
same(evaluation.result, '1,2,3', 'and gives its result');

// Errors end what they began.
assert.throws(() => evaluate('[1, 2'), (e) => e instanceof StaticJSError && e.kind === 'syntax' && e.column === 6);
assert.throws(() => evaluate('null.x'), (e) => e instanceof StaticJSError && e.kind === 'fault');
assert.throws(() => evaluate('Array.from({ length: 1e6 }, (_, i) => i)', { fuel: 1000 }),
  (e) => e instanceof StaticJSError && e.kind === 'fuel');
const numbers = context.compile('Array.from({ length: 1000 }, (_, i) => i)');
const cancelled = numbers.start();
numbers.release();
same(cancelled.run(10), 'paused', 'a paused evaluation');
cancelled.cancel();
same(cancelled.state, 'cancelled', 'cancelled');
same(context.liveBytes, 0, 'the context holds nothing once every evaluation ended and every program was released');

// Contexts are apart, and close() ends what is left in one.
const small = staticjs.createContext({ limits: { max_string_bytes: 8 } });
assert.throws(() => small.evaluate('"a".repeat(9)'), (e) => e.kind === 'fault' || e.kind === 'limit');
assert.throws(() => small.evaluate('s', { globals: { s: 'a'.repeat(9) } }), (e) => e.kind === 'limit');
same(evaluate('"a".repeat(9).length'), 9, 'the other context has its own limits');
small.compile('[1, 2]').start({}).run(1);
small.close();
assert.throws(() => small.compile('1'), /closed/, 'a closed context compiles nothing');
assert.throws(() => staticjs.createContext({ limits: { max_nothing: 1 } }), TypeError, 'a limit that is not one');

context.close();
console.log(`examples/wasm/staticjs.mjs: the header's ${Object.keys(limits).length} default limits; ${checks} checks`);
