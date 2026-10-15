// @tinyactors/staticjs in Node: values both ways, refusals, runs, natives,
// errors, the one evaluator of a context, budgets, and nothing left behind.
//   node --test test/
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { load, StaticJSError, DEFAULT_LIMITS } from '../index.mjs';

const staticjs = await load();

// A context, and a check that it gives back everything made in it: the
// body runs twice, and the second pass ends holding what the first did (the
// evaluator keeps an emptied array of globals for its next run's closures,
// so the first pass may end a few bytes up).
function withContext(options, body) {
  const context = staticjs.createContext(options);
  try {
    body(context);
    const after = context.liveBytes;
    body(context);
    assert.equal(context.liveBytes, after, 'the second pass holds no more than the first');
  } finally {
    context.dispose();
  }
}

// Evaluate to the end: the result as JavaScript.
function evaluate(context, source, globals = {}, { module = false } = {}) {
  const program = context.compile(source, { module, globals: Object.keys(globals) });
  const run = program.start(globals);
  try {
    assert.equal(run.step(1_000_000), true, `${source} finishes`);
    return run.result();
  } finally {
    run.dispose();
    program.dispose();
  }
}

test('values come out exactly', () => withContext({}, (context) => {
  assert.deepEqual(evaluate(context, '[undefined, null, true, 1.5, -0, NaN, "été😀", 2n ** 70n, -(2n ** 64n)]'),
    [undefined, null, true, 1.5, -0, NaN, 'été😀', 2n ** 70n, -(2n ** 64n)]);
  assert.deepEqual(evaluate(context, '({ b: [1, { c: new Set(["x"]) }], a: new Map([[1, "one"], ["k", [2]]]), 2: 0 })'),
    { 2: 0, b: [1, { c: new Set(['x']) }], a: new Map([[1, 'one'], ['k', [2]]]) });
  assert.deepEqual(evaluate(context, '/a+(b)/giu'), /a+(b)/giu);
  assert.deepEqual(evaluate(context, '[new Date(86400000), new Date(NaN)]').map((d) => d.getTime()), [86400000, NaN]);
  assert.deepEqual([...evaluate(context, '[1, 2, 3].values().filter((x) => x > 1)')], [2, 3]);
  assert.deepEqual([...evaluate(context, 'new Map([["k", 1]]).entries()')], [['k', 1]]);
  assert.deepEqual(evaluate(context, '"a1b".match(/\\d/)'), ['1']);
}));

test('values go in exactly, as globals', () => withContext({}, (context) => {
  const input = { n: -0, s: 'ü', b: 12345678901234567890n, a: [1, [null, undefined]], o: { x: { y: true } },
    m: new Map([[1n, 'big'], ['k', new Set([1, 'a'])]]), t: new Set(), d: new Date(5) };
  assert.deepEqual(evaluate(context, '[n, s, b, a, o, m, t, d]', input), Object.values(input));
  // An object with no prototype goes in; it comes out as a plain object.
  assert.deepEqual(evaluate(context, 'z', { z: Object.assign(Object.create(null), { k: 1 }) }), { k: 1 });
  assert.equal(evaluate(context, 'o.x.y && a[1].length', input), 2);
}));

test('what staticjs has no value for is refused, never turned into something else', () => withContext({}, (context) => {
  const refused = [Symbol('s'), [1, , 2], /x/, new (class Point {})(), new Set([[1]]), new Map([[{}, 1]]),
    'a\uD800b', { [Symbol('k')]: 1 }, JSON.parse('{"__proto__": 1}'), new Uint8Array(2)];
  const program = context.compile('v', { globals: ['v'] });
  for (const v of refused) assert.throws(() => program.start({ v }), TypeError, String(v?.constructor?.name ?? typeof v));
  program.dispose();
}));

test('a native: a JavaScript function a program calls, and its errors', () => withContext({}, (context) => {
  const rates = { EUR: 1, USD: 0.9 };
  assert.deepEqual(evaluate(context, '["EUR", "USD"].map((c) => rate(c) * 100)', { rate: (c) => rates[c] }), [100, 90]);
  const program = context.compile('rate("XYZ")', { globals: ['rate'] });
  const run = program.start({ rate: () => { throw new RangeError('no rate for XYZ'); } });
  assert.throws(() => run.step(100), (e) => e instanceof StaticJSError && e.kind === 'fault' &&
    e.subject === 'no rate for XYZ' && e.cause instanceof RangeError && e.frames.length === 1);
  assert.equal(run.state, 'faulted');
  program.dispose();
}));

test('a function a program made, called from JavaScript', () => withContext({}, (context) => {
  const exports = evaluate(context, 'export const k = 3; export const net = (rows) => rows.map((r) => r * k + limit);',
    { limit: 100 }, { module: true });
  assert.equal(exports.net.kind, 'function');
  const run = exports.net.start([[1, 2]]);
  let steps = 0;
  while (!run.step(2)) steps++;
  assert.ok(steps > 2);
  assert.deepEqual(run.result(), [103, 106]);
  run.dispose();
  exports.net.dispose();
}));

test('a module imports another, as the host gives it', () => withContext({}, (context) => {
  const units = context.compile('export const cm = 10; export const toMm = (x) => x * cm;', { module: true });
  const run = units.start();
  run.step(1000);
  const exports = run.resultValue();
  const program = context.compile('import { toMm } from "units"; export const total = [1, 2].map(toMm);',
    { module: true, modules: { units: exports } });
  const second = program.start();
  second.step(1000);
  assert.deepEqual(second.result(), { total: [10, 20] });
  assert.throws(() => context.compile('import { x } from "nowhere"; export const y = x;', { module: true }),
    (e) => e.kind === 'name');
  for (const x of [second, program, exports, run, units]) x.dispose();
}));

test('runs pause on fuel, say where they are, and one start abandons another', () => withContext({}, (context) => {
  const program = context.compile('[1, 2, 3, 4].map((x) => x * 2)');
  const first = program.start();
  let frames = [];
  while (!frames.some((f) => f.kind === 'loop')) {
    assert.equal(first.step(1), false);
    frames = first.frames();
  }
  assert.equal(frames[0].kind, 'function');
  assert.equal(frames[0].program, program);
  assert.ok(frames.some((f) => f.kind === 'loop' && f.name === 'map' && f.length === 4));
  const second = program.start();
  assert.throws(() => first.step(100), /abandoned/);
  assert.equal(second.step(1000), true);
  assert.deepEqual(second.result(), [2, 4, 6, 8]);
  assert.deepEqual(program.functions().map((f) => f.params), [0, 1]);
  second.dispose();
  first.dispose();
  program.dispose();
}));

test('errors: refused programs, faults, limits', () => withContext({ limits: { maxStringBytes: 16 } }, (context) => {
  assert.throws(() => context.compile('[1, 2'), (e) => e.kind === 'syntax' && e.line === 1 && e.column === 6);
  assert.throws(() => context.compile('rows.map((r) => r)'), (e) => e.kind === 'name' && e.subject === 'rows');
  const program = context.compile('null.x');
  const run = program.start();
  assert.throws(() => run.step(100), (e) => e.kind === 'fault' && e.frames.length === 1);
  program.dispose();
  const long = context.compile('s', { globals: ['s'] });
  assert.throws(() => long.start({ s: 'x'.repeat(17) }), (e) => e instanceof StaticJSError && e.kind === 'limit');
  long.dispose();
  assert.throws(() => staticjs.createContext({ limits: { maxNothing: 1 } }), TypeError);
}));

test('conversions are bounded by maxItems both ways, without the call stack', () => withContext({ maxItems: 1000 }, (context) => {
  const program = context.compile('Array.from({ length: 30 }).reduce((a) => [a, a], 0)');
  const run = program.start();
  run.step(10_000);
  assert.throws(() => run.result(), RangeError);
  run.dispose();
  program.dispose();
  let deep = 0;
  for (let i = 0; i < 2000; i++) deep = [deep];
  const keep = context.compile('v', { globals: ['v'] });
  assert.throws(() => keep.start({ v: deep }), RangeError);
  keep.dispose();
}));

test('deep values convert without recursion', () => withContext({}, (context) => {
  const program = context.compile('Array.from({ length: 200000 }).reduce((a) => [a], 0)');
  const run = program.start();
  run.step(10_000_000);
  let v = run.result();
  let depth = 0;
  while (Array.isArray(v)) {
    v = v[0];
    depth++;
  }
  assert.equal(depth, 200000);
  run.dispose();
  program.dispose();
}));

test('JSON and CBOR, in and out', () => withContext({}, (context) => {
  const value = context.fromJSON('{"a":[1,2.5,"x"],"b":null}');
  assert.deepEqual(value.toJS(), { a: [1, 2.5, 'x'], b: null });
  assert.equal(value.json(), '{"a":[1,2.5,"x"],"b":null}');
  const back = context.fromCBOR(value.cbor());
  assert.deepEqual(back.toJS(), value.toJS());
  const big = context.fromCBOR(new Uint8Array([0x1b, 0, 0x20, 0, 0, 0, 0, 0, 0]));
  assert.equal(big.toJS(), 2n ** 53n);
  assert.throws(() => context.fromJSON('[1,'), (e) => e.kind === 'syntax' && e.offset === 3);
  assert.throws(() => context.fromCBOR(new Uint8Array([0x9f, 0xff])), (e) => e.kind === 'syntax');
  const result = context.value({ when: new Date(0), sizes: new Set([1]), ratio: NaN, big: 1n });
  assert.throws(() => result.json(), (e) => e.kind === 'unsupported');
  const plain = context.value({ when: new Date(0), ratio: NaN });
  assert.equal(plain.json(), '{"when":"1970-01-01T00:00:00.000Z","ratio":null}');
  assert.equal(result.toString(), '{"when":new Date("1970-01-01T00:00:00.000Z"),"sizes":new Set([1]),"ratio":NaN,"big":1n}');
  for (const v of [value, back, big, result, plain]) v.dispose();
}));

test('a program that reads an unset clock faults', () => {
  const context = staticjs.createContext();
  const program = context.compile('new Date().toISOString()');
  assert.throws(() => program.start().step(100), (e) => e.kind === 'fault');
  context.dispose();
});

test('the clock and the seed are the host\'s', () => withContext({}, (context) => {
  context.setClock({ epochMilliseconds: Date.UTC(2024, 0, 31, 10) });
  assert.equal(evaluate(context, 'new Date().toISOString()'), '2024-01-31T10:00:00.000Z');
  context.advanceClock({ milliseconds: 1500 });
  assert.equal(evaluate(context, 'Date.now()'), Date.UTC(2024, 0, 31, 10) + 1500);
  context.setSeed(7);
  const a = evaluate(context, 'Math.random()');
  context.setSeed(7);
  assert.equal(evaluate(context, 'Math.random()'), a);
}));

test('a Temporal value needs the runtime\'s Temporal', () => withContext({}, (context) => {
  const program = context.compile('Temporal.PlainDate.from("2024-01-31")');
  const run = program.start();
  run.step(100);
  if (globalThis.Temporal === undefined) assert.throws(() => run.result(), TypeError);
  else assert.equal(String(run.result()), '2024-01-31');
  const kept = run.resultValue();
  assert.equal(kept.toString(), 'Temporal.PlainDate.from("2024-01-31")');
  kept.dispose();
  run.dispose();
  program.dispose();
}));

test('reentrancy: a native or a getter that uses its context is refused', () => withContext({}, (context) => {
  const program = context.compile('f()', { globals: ['f'] });
  const run = program.start({ f: () => context.compile('1') });
  assert.throws(() => run.step(100), (e) => e.kind === 'fault' && /running/.test(e.subject));
  const getter = { get x() { return context.compile('1'); } };
  const keep = context.compile('v', { globals: ['v'] });
  assert.throws(() => keep.start({ v: getter }), /running/);
  keep.dispose();
  program.dispose();
}));

test('dispose gives everything back; a disposed object refuses', () => {
  const context = staticjs.createContext();
  const baseline = context.liveBytes;
  const program = context.compile('[x, y]', { globals: ['x', 'y'] });
  const run = program.start({ x: [1, 2], y: context.value({ k: 'v' }) });
  run.step(1);
  const kept = context.value(new Map([[1, [2]]]));
  assert.ok(context.liveBytes > baseline);
  context.dispose();
  assert.throws(() => program.start({ x: 1, y: 2 }), TypeError);
  assert.throws(() => kept.toJS(), TypeError);
  assert.throws(() => context.compile('1'), TypeError);
  assert.equal(DEFAULT_LIMITS.maxValueItems, 1024 * 1024);
});
