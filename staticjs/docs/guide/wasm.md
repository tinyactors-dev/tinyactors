# staticjs in the browser, and in Node

`@tinyactors/staticjs` is staticjs from JavaScript, in Node and in
browsers ([`staticjs-js/`](../../../staticjs-js/) in this repository). It ships a
WebAssembly module of its own, about 1 MB (330 KB compressed); values go
in and come out as JavaScript values, and a run takes the fuel it is
given, so it takes turns with the event loop.

## Install

```sh
npm install @tinyactors/staticjs
```

It needs Node 22 or a browser with WebAssembly. `load()` compiles the
bundled `staticjs.wasm` beside the package's `index.mjs`; `load(url)`
takes it from another place.

```js
import { load } from '@tinyactors/staticjs';

const staticjs = await load();
const context = staticjs.createContext();
const program = context.compile('[{ age: 36 }, { age: 85 }].map((p) => p.age * 2)');
const run = program.start();
run.step(10_000);   // true: done
run.result();       // [72, 170]
```

## Contexts, programs and runs

- **A context** (`createContext({ limits })`) holds the values, programs
  and runs made in it, under its own limits, staticjs's defaults unless
  given. Contexts share nothing but the module's memory.
- **A program** is compiled once: `context.compile(source, { module,
  globals, modules })`. `globals` names the values each run is given; a
  module's value is the object of its exports, and `modules` gives what
  its imports find.
- **A run** is one evaluation: `program.start(globals)`, then
  `run.step(fuel)`, which returns `true` when it is done and `false` when
  the fuel is spent. `run.result()` is the result. A context has one
  evaluator: a `start` abandons a paused run of the same context.
- **`dispose()`** gives a context, a program, a run or a value back, and a
  context's gives back everything made in it.

```js
const total = context.compile('order.items.reduce((sum, i) => sum + i.price * i.count, 0)',
  { globals: ['order'] });
const run = total.start({ order: { items: [{ price: 2.5, count: 2 }, { price: 1, count: 3 }] } });
run.step(10_000);
run.result();   // 8
```

## Values

Values cross exactly both ways: numbers (`-0` and `NaN` kept), strings,
booleans, `null`, `undefined`, BigInts, arrays, plain objects, Maps, Sets,
Dates, and Temporal values where the runtime has `Temporal`. A regular
expression comes out as a `RegExp`, an iterator as an array iterator over
what it has left, a function as a `Value` that stays in staticjs. What
staticjs has no value for is a `TypeError`: a symbol, a class instance, an
array with a hole, a string with a lone surrogate.

- **A native.** A function among the globals is called by the program;
  what it returns goes back in, and what it throws faults the run and is
  the error's `cause`.
- **A function a program made**, such as a module's export, is called with
  `value.start(args)`, a run like a program's.
- **Bounded.** Every conversion stops at `maxItems` (the context's
  `limits.maxValueItems` by default), with a `RangeError`: values share
  parts, so a result made on little fuel can stand for a much larger
  tree.
- **JSON and CBOR.** `context.fromJSON(text)` and `context.fromCBOR(bytes)`
  read a value without running anything; `value.json()` writes what
  `JSON.stringify` gives in staticjs, `value.cbor()` one CBOR neutral
  value, `value.toString()` JavaScript source that evaluates to an equal
  value.

```js
const priced = context.compile('items.map((i) => i.count * rate(i.currency))', { globals: ['items', 'rate'] });
const run = priced.start({ items: [{ count: 2, currency: 'EUR' }], rate: (currency) => ({ EUR: 1.1 })[currency] });
run.step(10_000);
run.result();   // [2.2]
```

## Modules

`context.compile(source, { module: true, modules })` compiles a module, and
`modules` maps a name to a Value: the `resultValue()` of a module's run.

```js
// Evaluate a module and keep its exports, a Value, under the name others import.
const modules = {};
function define(name, source, globals = {}) {
  const run = context.compile(source, { module: true, modules, globals: Object.keys(globals) }).start(globals);
  if (!run.step(100_000)) throw new Error(`${name} ran out of fuel`);
  modules[name] = run.resultValue();
}

// In dependency order: a module imports only what is already defined.
define('fx', 'export const rate = (currency) => rates[currency];', { rates: { EUR: 1, USD: 0.92 } });
define('money', 'export const round2 = (x) => Math.round(x * 100) / 100;');
define('pricing', `
import { round2 } from "money";
import { rate } from "fx";
export const net = (gross, vat) => round2(gross / (1 + vat));
export const inEUR = (amount, currency) => round2(amount * rate(currency));
`);

// A user's plugin imports them by name.
const plugin = context.compile(`
import { net, inEUR } from "pricing";
export default (order) => inEUR(net(order.gross, order.vat), order.currency);
`, { module: true, modules });
const run = plugin.start();
run.step(100_000);
const { default: price } = run.result();           // a function staticjs made: a Value
const call = price.start([{ gross: 120, vat: 0.2, currency: 'USD' }]);
call.step(100_000);
call.result();                                       // 92
```

To use a module's exports in an expression, pass them as a global:
`pricing.net(120, 0.2)` compiled with `globals: ['pricing']` and run with
`{ pricing: modules.pricing }` gives 100. A module can also be built from
host functions with `context.value({ ... })`: `modules: { fx:
context.value({ rate: (c) => rates[c], base: 'EUR' }) }` serves `import {
rate, base } from "fx";`.

## Fuel and the event loop

The evaluator runs on the fuel it is given
([what fuel counts](embedding.md#evaluating-on-fuel)), and when that is spent it pauses and
returns; the next `step` resumes it where it stopped, inside a callback of
`map` or `reduce` too. An evaluation that runs to its end in one call
holds the thread: the page does not paint, and a server answers no other
request. Between steps, `await` a timer, and the event loop runs:

```js
const run = program.start(globals);
while (!run.step(10_000)) {
  await new Promise((resolve) => setTimeout(resolve));   // timers, input and I/O run here
  if (run.fuelUsed > 50_000_000) run.cancel();
}
run.result();
```

- **The size of a step.** Fuel counts work. To bound a turn by time, step
  in small amounts until it is up:

  ```js
  const until = performance.now() + 4;   // milliseconds of this turn
  let done;
  do done = run.step(1_000); while (!done && performance.now() < until);
  ```

- **Where it is.** `run.frames()` gives a paused run's frames: the program,
  each callback, and each looping built-in with how far it has got (`map`,
  element 2 of 4).
- **One run per context.** Runs of different contexts take turns on one
  thread; a context per tenant, or per kind of program, keeps them apart.

## Errors

A program staticjs refuses, a run that faults, and bytes that are not a
value throw a `StaticJSError`: its `kind` (`syntax`, `unsupported`,
`name`, `limit`, `memory` or `fault`), `line` and `column` (in bytes),
`reason`, `subject`, and for a fault the `frames` where it faulted.

## The clock and the seed

`context.setClock(instant)` (a `Temporal.Instant`, or
`{ epochMilliseconds }`) sets what `Date.now()`, `new Date()` and
`Temporal.Now` read, and `context.setSeed(n)` what `Math.random` draws
from, as `node --random-seed=<seed>` does. A program that reads either
before it is set faults.

## The raw module

Each release also ships `staticjs.wasm`, the library as a module that
imports nothing and exports the whole C API, its memory, and an allocator
of its own, for a host that calls the C API itself.

### Calling the C API

Every function in `staticjs.h` is an export of the same name. The C ABI for
wasm32 decides how arguments cross:

- **Pointers** are offsets into `exports.memory`, as numbers. Read and write
  through a `DataView` on `memory.buffer`, and make a fresh view after any
  call: the module's memory grows as it allocates, and a grown memory has a
  new buffer.
- **Structs** are laid out as `staticjs.api.json` says under
  `types.<name>.layout.wasm32`: its `size`, `align` and each field's
  `offsets`. A struct passed by value is passed as a pointer to a copy. A
  struct returned by value, such as `sjs_wasm_allocator()`'s, is written
  through a pointer passed first.
- **64-bit integers** passed or returned (`sjs_vm_run`'s fuel,
  `sjs_vm_set_seed`'s seed, `sjs_vm_advance_clock`'s nanoseconds,
  `sjs_vm_fuel_used`) are BigInts. A 64-bit field of a struct, such as
  `sjs_epoch.seconds`, is written with `DataView.setBigInt64`.
- **Constants** (`SJS_OK`, `SJS_GOAL_MODULE`, `SJS_RUN_PAUSED`, ...) are in
  `staticjs.api.json` under `constants`.

**Memory.** Pass `sjs_wasm_allocator()` as a context's allocator. Place
what a call reads (a config, a source's bytes, an out-parameter) with
`sjs_wasm_alloc(size, alignment)`, and give it back with
`sjs_wasm_free(pointer, size, alignment)`. The module's allocator reuses
freed blocks. Linear memory never shrinks, so a module's memory stays at its
peak.

[`tests/wasm/release_test.mjs`](../../tests/wasm/release_test.mjs) drives the module through the raw API: it
compiles, runs on fuel 16 units at a time, writes the result, and checks
that every allocation is given back.

### When to use it

Use the wasm module where native code cannot go: a browser, an edge
runtime, a plugin host. In Node and in browsers, the package is the simplest way to
evaluate staticjs. A program evaluates to the same
value in the module as in the native library: the module's math is the
same code, built freestanding.
