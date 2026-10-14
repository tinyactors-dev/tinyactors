---
title: In the browser
description: "staticjs.wasm from JavaScript, in a browser or in Node."
---

# staticjs in the browser, and in Node

`staticjs.wasm` is the library as a WebAssembly module. It imports
nothing, so it runs wherever WebAssembly does, with no JavaScript glue
generated for it. It exports the whole C API, its memory, and an allocator
of its own. `examples/wasm/staticjs.mjs` wraps it in one function,
`evaluate(source)`, for a page or for Node.

## Quick start

You need two files from a release: `staticjs.wasm` from
`staticjs-<version>-wasm32.tar.gz`, and `staticjs.api.json` from
`staticjs-<version>-src.tar.gz`. The API description gives the wrapper
the module's struct layouts and constants. Put them beside the three files
of [`examples/wasm/`](https://github.com/dhamidi/tinyactors/tree/main/staticjs/examples/wasm/) in this repository (no release
ships them):

```
staticjs.mjs        the wrapper
index.html          a page that evaluates what you type
node.mjs            the same from Node
staticjs.wasm
staticjs.api.json
```

**In Node:**

```sh
$ node node.mjs staticjs.wasm staticjs.api.json
[1,2,3]
{"total":6}
"2024-01-31T10:00:00.000Z"
fault at 1:14: cannot read a property of undefined (c)
syntax error at 1:6: expected , or ]
name error at 1:1: this name is not bound (a const is visible after its declaration)
out of fuel: the evaluation used its 10000 units
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
staticjs.evaluate('[{ age: 36 }, { age: 85 }].map((p) => p.age * 2)');   // '[72,170]'
```

## The wrapper

`load(wasm, api, { limits })` instantiates the module and makes one context
and one evaluator. `limits` overrides any of `DEFAULT_LIMITS`, which are
the header's `SJS_LIMITS_DEFAULT`.

`evaluate(source, options)` compiles and evaluates `source`, and returns
its value as JavaScript source text: what the C writer writes, such as
`{"a":1}`, `10n`, `new Set([1])` or `Temporal.PlainDate.from("2024-01-31")`.
The options are:

| Option | Default | Means |
|---|---|---|
| `module` | `false` | compile a module, whose value is the object of its exports (the wrapper gives no modules adapter, so it cannot import) |
| `fuel` | 10,000,000 | the units the evaluation may use |
| `now` | `Date.now()` | the clock, in milliseconds since the epoch, that `Temporal.Now`, `Date.now()` and `new Date()` read |
| `seed` | 1 | what `Math.random` draws from, as `node --random-seed=<seed>` does |

A refusal, a fault or running out of fuel throws a `StaticJSError`. Its
`kind` is `syntax`, `unsupported`, `name`, `limit`, `memory`, `fault` or
`fuel`. It also has `line`, `column` (in bytes), `reason` (the library's
message) and `subject`.

`close()` gives back the evaluator and the context.

The wrapper is about 200 lines, meant to be copied and changed. To pass
globals, keep the result as a value instead of text, or run on slices of
fuel, call the C API as the wrapper does.

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
