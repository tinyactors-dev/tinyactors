# @tinyactors/staticjs

staticjs is a small, embeddable JavaScript engine for a pure, total subset
of the language. This package is staticjs as WebAssembly, for Node and
browsers: values go in and come out as JavaScript values, and an
evaluation runs on fuel the host gives it, so it takes turns with the
event loop.

```js
import { load } from '@tinyactors/staticjs';

const staticjs = await load();
const context = staticjs.createContext();

const net = context.compile('order.items.reduce((sum, item) => sum + item.price * item.count, 0)',
  { globals: ['order'] });
const run = net.start({ order: { items: [{ price: 2.5, count: 2 }, { price: 1, count: 3 }] } });
run.step(10_000);   // true: done
run.result();       // 8
```

A program staticjs accepts is valid JavaScript, and evaluates to the value
JavaScript gives, or to a fault. Like JavaScript, the language has arrow
functions, destructuring, Map, Set, BigInt, JSON and Temporal. Unlike
JavaScript, it has no loops, recursion, classes, exceptions, async or I/O,
and every evaluation ends. The language is described at
[staticjs.tinyactors.dev](https://staticjs.tinyactors.dev/guide/language).

## Install

```sh
npm install @tinyactors/staticjs
```

The package is ES modules, with types. It needs Node 22 or a browser with
WebAssembly. `load()` fetches the bundled `staticjs.wasm` (about 1 MB,
about 330 KB compressed) beside `index.mjs`; a bundler that copies assets
serves it, or `load(url)` takes another place.

## Contexts, programs and runs

- **A context** holds values, programs and runs, under its own limits
  (`createContext({ limits })`, staticjs's `SJS_LIMITS_DEFAULT` by
  default). Contexts share nothing but the module's memory.
- **A program** is compiled once: an expression, or a module
  (`{ module: true }`) whose value is the object of its exports. `globals`
  names the values each run is given.
- **A run** is an evaluation. `program.start(globals)` starts one, and
  `run.step(fuel)` runs it on that much fuel: `true` when it is done,
  `false` when the fuel is spent, and a `StaticJSError` when it faults. A
  context has one evaluator, so a `start` abandons a paused run of the
  same context.
- **`dispose()`** gives a context, a program, a run or a value back;
  `using` works where the runtime has it. A context's `dispose()` gives
  back everything made in it.

## Fuel and the event loop

Fuel is the evaluator's count of work: a unit per instruction, and per
element for the built-ins that loop. The same program on the same inputs
uses the same fuel on every machine. A run stops when its fuel is spent
and resumes on the next `step`, inside a callback of `map` too, so a long
evaluation shares the thread with everything else:

```js
const run = program.start(globals);
while (!run.step(10_000)) {
  await new Promise((resolve) => setTimeout(resolve));   // timers, input and I/O run here
  if (run.fuelUsed > 50_000_000) run.cancel();
}
run.result();
```

`run.frames()` says where a paused run is: the program, each callback, and
each looping built-in with how far it has got (`map`, element 2 of 4).

## Values

Values cross exactly, both ways: numbers (`-0` and `NaN` kept), strings,
booleans, `null`, `undefined`, BigInts, arrays, plain objects, Maps, Sets,
Dates, and Temporal values where the runtime has `Temporal`. A regular
expression comes out as a `RegExp`, an iterator as an array iterator over
what it has left, and a function as a `Value` that stays in staticjs and
can be called (`value.start(args)`). What staticjs has no value for is a
`TypeError`, never turned into something else: a symbol, a class instance,
an array with a hole, a string with a lone surrogate, an object key
`__proto__`.

A function among the globals is a native: the program calls it, and what
it returns goes back in. What it throws faults the run, and is the
error's `cause`.

```js
const priced = context.compile('items.map((i) => i.count * rate(i.currency))', { globals: ['items', 'rate'] });
const run = priced.start({ items, rate: (currency) => rates[currency] });
```

Every conversion is bounded by `maxItems` (the context's
`limits.maxValueItems` by default): values share parts, so a result made on
little fuel can stand for a much larger tree.

**JSON and CBOR.** `context.fromJSON(text)` and `context.fromCBOR(bytes)`
read a value without running anything; `value.json()` writes what
`JSON.stringify` gives in staticjs, `value.cbor()` one CBOR neutral value,
and `value.toString()` JavaScript source that evaluates to an equal value.

## Errors

A program staticjs refuses, an evaluation that faults, and bytes that are
not a value throw a `StaticJSError`: its `kind` (`syntax`, `unsupported`,
`name`, `limit`, `memory` or `fault`), `line` and `column` (in bytes),
`reason`, `subject`, and for a fault the `frames` where it faulted.

## The clock and the seed

The engine reads no clock and no entropy. `context.setClock(instant)` (a
`Temporal.Instant`, or `{ epochMilliseconds }`) sets what `Date.now()`,
`new Date()` and `Temporal.Now` read, and `context.setSeed(n)` what
`Math.random` draws from. A program that reads either before it is set
faults.

## Limitations

- The language leaves out loops, recursion, classes, exceptions and
  asynchronous code; a program written for Node usually needs rewriting
  with `map`, `filter` and `reduce`.
- A Temporal value comes out only where the runtime has `Temporal`
  (Node 22 has none); `value.toString()` still writes it.
- The module is about 1 MB.
- staticjs is at version 0.9, and the API may change.

## License

MIT.
