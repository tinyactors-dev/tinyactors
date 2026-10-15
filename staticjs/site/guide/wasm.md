---
title: Browser and Node
description: "staticjs.wasm from JavaScript, in a browser or in Node."
---

# Browser and Node

`staticjs.wasm` is the library as a WebAssembly module. It imports
nothing, so it runs wherever WebAssembly does, with no JavaScript glue
generated for it. It exports the whole C API, its memory, and an allocator
of its own. `examples/wasm/staticjs.mjs` wraps it for a page or for Node.

## Quick start

You need two files from a release: `staticjs.wasm` from
`staticjs-<version>-wasm32.tar.gz`, and `staticjs.api.json` from
`staticjs-<version>-src.tar.gz`. The API description gives the wrapper
the module's struct layouts and constants. Put them beside the three files
of [`examples/wasm/`](https://github.com/dhamidi/tinyactors/tree/main/staticjs/examples/wasm/) in this repository (no release
ships them):

```
staticjs.mjs        the wrapper
index.html          a page that evaluates the source in its text box, in slices
node.mjs            the wrapper's calls, from Node
staticjs.wasm
staticjs.api.json
```

**In Node:**

```sh
$ node node.mjs staticjs.wasm staticjs.api.json
[ 1, 2, 3 ]
Map(2) { 'ada' => 36n, 'grace' => 85n }
{"when":"1970-01-01T00:00:00.000Z","sizes":{},"ratio":null}
new Map([["ada",36n],["grace",85n]])
{ total: 6 }
'2024-01-31T10:00:00.000Z'
8
0
fault at 1:14: cannot read a property of undefined (c)
syntax error at 1:6: expected , or ]
name error at 1:1: this name is not bound (a const is visible after its declaration)
out of fuel: the evaluation used its 10000 units
fault at 1:11: Array.from length is past the limit of an array
5000
a: 199990000 in 100014 units, b: 12497500 in 25014 units
slices: a b a b a a a a a a a
bytes held: 0
```

**In a browser:** serve the directory (`python3 -m http.server`) and
open `index.html`. A page loads the module so:

```js
import { load } from './staticjs.mjs';

const [wasm, api] = await Promise.all([
  fetch('staticjs.wasm').then((r) => r.arrayBuffer()),
  fetch('staticjs.api.json').then((r) => r.json()),
]);
const staticjs = await load(wasm, api);
const context = staticjs.createContext();
context.evaluate('[{ age: 36 }, { age: 85 }].map((p) => p.age * 2)');   // [72, 170]
```

## Contexts and programs

`load(wasm, api)` instantiates the module. `createContext({ limits })`
makes a context: the values, programs and evaluations made in it, under
its own limits. `limits` overrides any of `DEFAULT_LIMITS`, the header's
`SJS_LIMITS_DEFAULT`. Contexts share nothing but the module's memory, so a
page can give each tenant, or each kind of program, one of its own:

```js
const context = staticjs.createContext();
const small = staticjs.createContext({ limits: { max_value_items: 1000 } });
small.evaluate('Array.from({ length: 5000 }, (_, i) => i).length');     // throws: fault, past the limit of an array
context.evaluate('Array.from({ length: 5000 }, (_, i) => i).length');   // 5000
```

`context.compile(source, { module, globals })` compiles a program once.
`globals` names the values each evaluation binds, and
`program.evaluate(values, options)` evaluates it on them:

```js
const total = context.compile('order.items.reduce((sum, item) => sum + item.price * item.count, 0)',
  { globals: ['order'] });
total.evaluate({ order: { items: [{ price: 2.5, count: 2 }, { price: 1, count: 3 }] } });   // 8
total.evaluate({ order: { items: [] } });                                                    // 0
```

`context.evaluate(source, options)` does the three steps at once: it
compiles `source` with the names of `options.globals` bound, evaluates it
on their values, and releases the program. `module: true` compiles a
module, whose value is the object of its exports; the wrapper gives no
modules adapter, so a module cannot import.

`program.release()` gives a program back, and `context.close()` ends every
evaluation, releases every program and destroys the context.
`context.liveBytes` is what the context holds: 0 once everything made in
it is given back.

The options of an evaluation are:

| Option | Default | Means |
|---|---|---|
| `fuel` | 10,000,000 | the units the evaluation may use (`evaluate` only) |
| `output` | `'value'` | how the result comes out: `'value'`, `'json'` or `'source'` |
| `now` | `Date.now()` | the clock, in milliseconds since the epoch, that `Temporal.Now`, `Date.now()` and `new Date()` read |
| `seed` | 1 | what `Math.random` draws from, as `node --random-seed=<seed>` does |

## Values

A global goes in as a JavaScript value: `undefined`, `null`, a boolean, a
number, a string, a BigInt, an array, a plain object, a `Map` or a `Set`.
The wrapper builds it in the context with the value API
(`sjs_array_new`, `sjs_object_put`, ...); anything else, such as a
function or a `Date`, is a `TypeError`. A `Map`'s keys and a `Set`'s
elements are primitives, as in staticjs.

A result comes out in one of three outputs:

- **`'value'`** reads the result through the value API (`sjs_length`,
  `sjs_array_at`, `sjs_object_at`, `sjs_map_at`, `sjs_string_text`, ...)
  into a JavaScript value. Nothing is encoded or parsed on the way. A
  regular expression comes out as a `RegExp`, a `Date` as a `Date`, and a
  Temporal value as the host's Temporal (a `TypeError` where the host has
  none). An array gives its elements, as JSON does: a match result's
  `index` and `groups` stay behind. An iterator or a function cannot leave
  the evaluation: return `[...iterator]`, or what the function computes.
- **`'json'`** gives JSON text, the same as `JSON.stringify` of the result
  in staticjs: `NaN` is `null`, a `Set` is `{}`, a Date or a Temporal value
  its string, and a BigInt a `TypeError`.
- **`'source'`** gives what the C writer writes: JavaScript source that
  evaluates to an equal value, such as `10n`, `new Set([1])` or
  `Temporal.PlainDate.from("2024-01-31")`. Write it to a log or a file;
  reading it back means evaluating it.

```js
context.evaluate('new Map([["ada", 36n]])');                         // Map(1) { 'ada' => 36n }
context.evaluate('({ when: new Date(0), ratio: NaN })', { output: 'json' });
                                                                    // '{"when":"1970-01-01T00:00:00.000Z","ratio":null}'
context.evaluate('new Map([["ada", 36n]])', { output: 'source' });   // 'new Map([["ada",36n]])'
```

A result read as a value or as JSON makes at most the context's
`max_value_items` values, and past them throws a `StaticJSError` of kind
`limit`. Values share parts, so a result made on little fuel can stand for
a much larger tree: `Array.from({ length: 30 }).reduce((a) => [a, a], 0)`
is a tree of about a billion arrays, made on about 250 units of fuel.

## Fuel and the event loop

The evaluator runs on the fuel it is given ([what fuel counts](./embedding.md#evaluating-on-fuel)),
and when that is spent it pauses and returns. The next run resumes where
it stopped, inside a callback of `map` or `reduce` too.

An evaluation that runs to its end in one call holds the thread: the page
does not paint, and a server answers no other request.
`program.start(values, options)` gives an evaluation that runs in slices
instead. `evaluation.run(fuel)` runs one slice and returns `'paused'` or
`'done'`; between slices, `await` a timer, and the event loop runs:

```js
const evaluation = total.start({ order }, { output: 'json' });
while (evaluation.run(10_000) === 'paused') {
  await new Promise((resolve) => setTimeout(resolve));   // timers, input and I/O run here
  if (evaluation.fuelUsed > 50_000_000) evaluation.cancel();
}
evaluation.result;   // '8'
```

- **`evaluation.result`** is the result, in the evaluation's `output`, once
  `run` returned `'done'`. A fault throws a `StaticJSError` from `run`.
  The evaluation then ends and releases what it held.
- **`evaluation.fuelUsed`** is the fuel used so far: check a budget
  against it. `evaluation.cancel()` abandons a
  paused evaluation; `evaluation.state` is `'paused'`, `'done'`,
  `'faulted'` or `'cancelled'`.
- **Several at once.** Each evaluation has an evaluator of its own, so
  evaluations take turns on one thread. `node.mjs` runs two; they
  alternate slice by slice (`a b a b`) until the shorter one ends.
- **The size of a slice.** Fuel counts work. To bound a turn by time, run
  small slices until it is up:

  ```js
  const until = performance.now() + 4;   // milliseconds of this turn
  let status;
  do status = evaluation.run(1_000); while (status === 'paused' && performance.now() < until);
  ```

`index.html` runs its source in slices of 100,000 units and yields
between them, so a counter of the fuel used keeps painting and Stop
cancels the evaluation. In a Web Worker, slices let the worker answer a
message to stop.

## Errors

A refusal, a fault or running out of fuel throws a `StaticJSError`. Its
`kind` is `syntax`, `unsupported`, `name`, `limit`, `memory`, `fault` or
`fuel`. It also has `line`, `column` (in bytes), `reason` (the library's
message) and `subject`. A global the context cannot hold, such as a string
past `max_string_bytes`, throws one of kind `limit`.

The wrapper gives no natives and no modules adapter: for those, call the
C API as the wrapper does.

## Calling the C API from JavaScript

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

[`tests/wasm/release_test.mjs`](https://github.com/dhamidi/tinyactors/blob/main/staticjs/tests/wasm/release_test.mjs) drives the module through the raw API: it
compiles, runs on fuel 16 units at a time, writes the result, and checks
that every allocation is given back.

## When to use it

Use the wasm module where native code cannot go: a browser, an edge
runtime, a plugin host. In Node, the module is the simplest way to
evaluate staticjs without a native addon. A program evaluates to the same
value in the module as in the native library: the module's math is the
same code, built freestanding.
