# A JavaScript API for staticjs.wasm

**Status: Sketch** (an independent refutation of its claims is in progress; it becomes Proposed when they hold). Touches: a new package, `staticjs-js/`
(`@tinyactors/staticjs`), with a C layer of its own; staticjs's release
packaging. staticjs's library is not changed.

## Summary

A package that evaluates staticjs from JavaScript, in Node and in browsers,
with JavaScript values in and out:

```js
import { load } from '@tinyactors/staticjs';

const staticjs = await load();
using context = staticjs.createContext({ fuel: 100_000 });
context.setClock({ epochMilliseconds: Date.now() });

using guard = context.compile('In("heating") && event.data.f > limit', { globals: ['In', 'event', 'limit'] });
guard.run({ In: (id) => active.has(id), event, limit: 25 });   // true
```

It is built from the same amalgamation as `staticjs.wasm`, plus a C layer
of its own (`sjs_js_*`) whose exports take and return only numbers. So
JavaScript reads no C struct, and values cross by a handle per element, not
by text or a codec. Measured on this repository's spikes, a guard
evaluates in about 0.1 µs, and a result of 10,000 small objects reaches
JavaScript in about 2 ms. That is five times faster than the writer's text
plus `JSON.parse`, and exact for every kind (BigInt, Map, Set, `undefined`,
`-0`).

## Whom it serves, and who pays

- **First: the application developer writing JavaScript** who evaluates
  programs written by someone else: rules, filters, a statechart's guards
  and assignments, computed fields. In a browser, an edge runtime, or Node
  without a native addon. They ask: what did this program give, as a value
  I can use? Why was it refused, and where? Will it end, and how much may
  it use? Today they have the C API through raw exports, or
  `examples/wasm/staticjs.mjs`, which returns text.
- **Then: a future browser host of tinyactors with the staticjs data
  model.** Its state would live in staticjs values that never cross into
  JavaScript (`Value` handles, below).
- **Who pays:**
  - per evaluation: nothing beyond the C API's own work and converting
    what crosses;
  - per context: one evaluator, about 101 KiB with the default limits;
  - per package: about 1 MB of wasm (316 KB gzipped) and about 1,000 lines
    of JavaScript and C (an estimate);
  - API surface: five classes;
  - the maintainers: a package with a build, tests and a changelog, and a
    second wasm artifact in the release (below);
  - staticjs's library: nothing.
- **Out of scope:**
  - a sandbox beyond what staticjs already is (it reaches only what it is
    given);
  - running in a worker (the package works in one; it does not manage
    one);
  - TypeScript types for programs;
  - a REPL;
  - a staticjs data model for tinyactors.

## The frame

The facts the design rests on, each checked:

| # | Fact | Evidence |
|---|---|---|
| F1 | `staticjs.wasm` imports nothing and exports the C API and its memory, nothing else: no function table | spike: `WebAssembly.Module.exports` lists 65 functions and `memory`; 0 imports |
| F2 | The wasm32 C ABI passes a struct by value as a pointer to a copy, returns one through a pointer given first, and passes a 64-bit integer as a BigInt | `staticjs/docs/guide/wasm.md`; `staticjs/tests/wasm/release_test.mjs` |
| F3 | A context, and everything made with it, is used by one thread at a time. A JavaScript realm is one thread, and a wasm instance belongs to one realm | `staticjs.h:187` |
| F4 | Values are reference counted and immutable once shared. Copying the struct does not retain it. `sjs_release` does not recurse | `staticjs.h:185`, `:225`, `:247` |
| F5 | A native function is a C vtable (`sjs_native_vtable`) called synchronously during `sjs_vm_run`. The module's C can call JavaScript only through an import (F1); Node 22 has no `WebAssembly.Function` to make a wasm function from a JavaScript one | `staticjs.h:313-333`; spike: `typeof WebAssembly.Function` is `undefined` |
| F6 | A modules adapter (`sjs_modules_vtable.find`) is called during compilation and answers with exports the compiler borrows while it runs | `staticjs.h:588-600` |
| F7 | The value API reads arrays, objects, sets, maps, strings, numbers, booleans and BigInts (as text). It has no accessor for a Temporal value, a Date or a regular expression, and no call of a staticjs function from C. Only the writer writes those, as JavaScript source | `staticjs.h`, "Context and values" and "Writer" |
| F8 | This repository keeps C structs out of the languages: "Only C touches host structs"; struct layouts read from JavaScript are a rejected idea | ADR 0009; `.agents/skills/boundary-design/SKILL.md:82` |
| F9 | A binding's direct layer is C, mirroring the C API, with a language layer above it; scratch shared between calls is not reentrant, so a call takes a mark and releases to it | `tinyactors-napi/AGENTS.md`, "Layers and the resolver" and "The binding's memory"; napi commit `8d600f4c` |
| F10 | A call from JavaScript into wasm costs about 3 ns | ADR 0010:59 (2.9 ns); spike below (2.7 to 4.7 ns) |

## What exists, and why it is not enough

- **The raw exports** (`tests/wasm/release_test.mjs`). They work, but
  every call places structs in memory at the offsets of
  `staticjs.api.json`'s `layout.wasm32`, which F8 rules out for a
  package. A caller writes 30 lines to evaluate one expression.
- **`staticjs/examples/wasm/staticjs.mjs`**, the example wrapper. It reads
  layouts too (F8). It returns the writer's text, which is JavaScript
  source, not JSON (`10n`, `new Map(...)`, `undefined`), so a caller gets
  a value back only by evaluating the text or parsing a grammar of its
  own. It allocates and frees every struct per call. It has no globals,
  natives or modules. It is a starting point to copy, as its comment says.

## The API

```ts
// index.d.ts (sketch)
export function load(source?: BufferSource | URL | Response | WebAssembly.Module): Promise<StaticJS>;

export interface StaticJS {
  readonly version: string;                       // sjs_version
  createContext(options: ContextOptions): Context;
}

export interface ContextOptions {
  fuel: number | bigint;                          // the budget of each run, unless a run gives its own
  limits?: Partial<Limits>;                       // over SJS_LIMITS_DEFAULT; names as in sjs_limits, camelCase
}

export interface Context extends Disposable {
  compile(source: string, options?: CompileOptions): Program;
  evaluate(source: string, options?: CompileOptions & RunOptions & { globals?: Record<string, unknown> }): unknown;
  value(value: unknown): Value;                   // a JavaScript value made once, passed to many runs
  setClock(instant: { epochMilliseconds: number } | { epochNanoseconds: bigint }): void;
  advanceClock(duration: { milliseconds?: number; nanoseconds?: bigint }): void;
  setSeed(seed: number): void;                    // Math.random as `node --random-seed=<seed>`
  readonly liveBytes: number;                     // sjs_context_live_bytes
  dispose(): void;
}

export interface CompileOptions {
  module?: boolean;                               // SJS_GOAL_MODULE; default: an expression
  globals?: readonly string[];                    // the names each run gives values for
  modules?: Record<string, Value>;                // what `import ... from "name"` finds, for this compile
}

export interface RunOptions { fuel?: number | bigint }

export interface Program extends Disposable {
  readonly globals: readonly string[];
  run(globals?: Record<string, unknown>, options?: RunOptions): unknown;   // the value, as JavaScript
  runValue(globals?: Record<string, unknown>, options?: RunOptions): Value; // the value, kept in staticjs
  start(globals?: Record<string, unknown>): Run;  // run in slices of fuel
  dispose(): void;
}

export interface Run {
  step(fuel: number | bigint): boolean;           // true when done; throws StaticJSError on a fault
  readonly fuelUsed: bigint;
  result(): unknown;
  cancel(): void;                                 // sjs_vm_reset
}

export interface Value extends Disposable {
  readonly kind: 'undefined' | 'null' | 'boolean' | 'number' | 'string' | 'bigint' | 'array' | 'object'
    | 'set' | 'map' | 'iterator' | 'function' | 'regexp' | 'temporal';
  toJS(): unknown;
  toString(): string;                             // the writer: JavaScript source that evaluates to it
  dispose(): void;
}

export class StaticJSError extends Error {
  readonly kind: 'syntax' | 'unsupported' | 'name' | 'limit' | 'memory' | 'fault' | 'fuel';
  readonly line: number;                          // from 1; 0 for 'fuel'
  readonly column: number;                        // from 1, in UTF-8 bytes, as sjs_position
  readonly offset: number;
  readonly subject: string;
  readonly reason: string;                        // the library's message
  // `cause`: the error a native function threw, when one did
}
```

Choices, each against what JavaScript users expect:

- **Globals by name.** `compile` takes the names, as `sjs_compiler_config`
  does; `run` takes an object keyed by them. A missing name is a
  `TypeError` before the run; an extra key is ignored.
- **`fuel` is required.** The C API has no default (`sjs_vm_run` takes
  it), and a binding adds no defaults of its own (F9). It is given once,
  per context, and a run may override it. Running out is a
  `StaticJSError` of kind `fuel`, after `sjs_vm_reset`.
- **The clock and the seed are the host's** (`sjs_vm_set_clock`,
  `sjs_vm_set_seed`). Nothing sets them for you, as the C API does
  nothing for you, so a program that reads an unset one faults.
  `setClock` takes anything with `epochMilliseconds` or
  `epochNanoseconds`, so a `Temporal.Instant` works directly.
- **Disposal is explicit,** with `dispose()` and `Symbol.dispose` (for
  `using`, in TypeScript 5.2 and in runtimes that have it). A
  `FinalizationRegistry` releases what was forgotten. Its callbacks run
  as jobs of their own, never inside the synchronous call that runs a
  program (ECMA-262, `HostEnqueueFinalizationRegistryCleanupJob`), so a
  release never lands in the middle of a run.
- **Names** come from familiar APIs, with this library's meaning:
  `compile` and `run` as in `node:vm`'s `Script`; `dispose` as in
  quickjs-emscripten's handles and TC39's explicit resource management;
  `epochMilliseconds` and `epochNanoseconds` as in Temporal.

## Values

| JavaScript → staticjs | | staticjs → JavaScript | |
|---|---|---|---|
| number, string, boolean, `null`, `undefined`, bigint | the same | number (`-0` and `NaN` kept), string, boolean, `null`, `undefined`, BigInt | the same |
| `Array` (a hole is `undefined`) | array | array | `Array` |
| a plain object (prototype `Object.prototype` or `null`): own enumerable string keys, read with `[[Get]]` | object, keys in JavaScript's order | object | a plain object |
| `Set` of primitives, `Map` with primitive keys | set, map | set, map | `Set`, `Map` |
| a function | a native function (below), made once per context and function | a function | a JavaScript function that runs it (below) |
| a `Value` | itself, borrowed for the run: no copy | iterator | an array's iterator over its values, as the writer writes one |
| a `Date` | a Date, made by a program (below) | Date | `Date` |
| a `Temporal` value, when the runtime has Temporal | the same type, made by a program | Temporal value | the runtime's `Temporal` type; without one, the ISO string |
| | | regular expression | `RegExp` of the same source and flags |

Refused with a `TypeError`, never turned into something else:

- a string that is not well formed (`String.prototype.isWellFormed`): a lone
  surrogate is refused, not replaced, as `TextEncoder` would replace it;
- an object of another class, a symbol key, an own `__proto__` key;
- a `Set` member or `Map` key that is not a primitive;
- nesting deeper than the context's `maxNesting`, which also stops a cycle;
- a `Value` of another context.

**Kinds the value API cannot reach (F7)** are made and read by staticjs
itself, never decoded in JavaScript:

- In: a `Date` goes in by running a program compiled once per context,
  `new Date(t)`, with `t` its time value. A Temporal value runs
  `Temporal.<Type>.from(s)` with its ISO string. These are the library's
  own constructors.
- Out: a Date, a Temporal value or a regular expression is written by the
  writer (`sjs_writer_*`), and its one known form is read back
  (`new Date("…")`, `Temporal.PlainDate.from("…")`, `/…/flags`).

**A function out** becomes a JavaScript function. Calling it runs a
program compiled once per context, `f(...a)`, with globals `f` and `a`, on
the context's fuel. So a module's exports are callable from JavaScript:
`context.evaluate('export const net = (xs) => …', { module: true }).net(rows)`.
It throws while its context is running (below).

**Native functions.** A JavaScript function in `globals` becomes an
`sjs_native` the context keeps, one per function (a `WeakMap`). It is
called synchronously during the run, with its arguments converted to
JavaScript, and its return value converted back. A function that throws
faults the run: the error's message is the fault's subject, and the thrown
error is the `StaticJSError`'s `cause`. As in C, it must not change
anything the program can see; the package cannot check that.

**Reentrancy.** A native function that calls its own context (`run`,
`compile`, `value`) gets a `TypeError` ("the context is running"). The C
layer keeps that flag. Another context in the same instance may be used:
every call of the C layer takes a mark on its handle stack and scratch at
entry and releases to it at exit, so a nested call never overwrites an
outer one's (F9).

## How it crosses

### A C layer of the package's own

`staticjs-js/src/` holds C compiled into the package's module beside the
amalgamation. Its functions carry the prefix `sjs_js_`, take at most three
parameters (all numbers: `uint32_t` handles and lengths, `double`, a
pointer as an offset), and return a number. JavaScript reads no struct
(F8). Sketch:

| Group | Functions |
|---|---|
| contexts, evaluators | `sjs_js_context_new(limits_handle)`, `_context_free`, `_set_clock(ctx, seconds_hi_lo, ns)`, `_advance_clock`, `_set_seed` |
| compile | `_compile_begin(ctx)`, `_compile_global(c, ptr, len)`, `_compile_module(c, name_ptr, value)`, `_compile_end(c, goal, source_len)` → program or error |
| run | `_run_begin(program)`, `_run_global(r, value)`, `_run(r, fuel)` → status, `_run_take(r)` → value |
| reading a value | `_kind(v)`, `_number(v)`, `_boolean(v)`, `_length(v)`, `_at(v, i)`, `_entry(v, i)` with `_key_ptr()` and `_key_len()`, `_string_ptr(v)` with `_string_len()`, `_bigint_text(v)`, `_write(v)` (the writer, for F7's kinds) |
| building a value | `_begin_array(n)`, `_begin_object(n)`, `_begin_set(n)`, `_begin_map(n)`, `_key(ptr, len)`, `_number(d)`, `_string(ptr, len)`, `_bigint(ptr, len)`, `_boolean(b)`, `_null()`, `_undefined()`, `_put(value)`, `_end()` |
| handles | `_keep(v)` → a persistent handle; `_drop(h)`; `_mark()`, `_release_to(mark)` |
| errors | `_error_code()`, `_error_line()`, `_error_column()`, `_error_offset()`, `_error_message_ptr/len()`, `_error_subject_ptr/len()` |
| natives | one import, `staticjs_js.native(native_id, args_handle, count)` → a handle or a fault |

- **Values are handles:** indices into a stack of `sjs_value` slots in C.
  A slot is borrowed from its container and released to a mark. A
  `Value` is a slot in a separate table, with a generation, and is
  retained until disposed. This is the boundary skill's "values are
  handles, metadata are views".
- **Modules without a callback:** the C layer's `find` looks the name up
  among the `(name, value)` pairs that `_compile_module` gave for this
  compile. No JavaScript runs during compilation, so `modules` is a plain
  object of `Value`s.
- **Natives need one import** (F5). The package's module imports
  `staticjs_js.native`, and `load` provides it. The release's
  `staticjs.wasm` keeps importing nothing (decision 1).

### Lifetimes

| Object | Owner | Made | Read | Ends |
|---|---|---|---|---|
| the instance and its memory | `StaticJS` | `load` | every call | when unreachable; memory never shrinks |
| `sjs_context`, its `sjs_vm` | `Context` | `createContext` | its calls | `dispose`, or its finalizer |
| `sjs_program` | `Program` | `compile` | `run`, `start` | `dispose`, or its finalizer (after the context's, never) |
| a global's value converted from JavaScript | the run | `run`, before `sjs_vm_start` | during the run (borrowed, F6 of `sjs_run_params`) | released after the run, to the run's mark |
| a `Value` given as a global | its `Value` | `value()` or `runValue()` | during the run, borrowed | `dispose`; the `Run` holds it until done |
| slots of a conversion out | the C layer's stack | `_at`, `_entry` | during that conversion | `_release_to(mark)` at its end |
| scratch for strings in | the C layer | per string (`encodeInto`) | during `_string` or `_key` (copied by `sjs_string_new`) | the next string at that mark |
| a native's arguments | the evaluator | during `sjs_vm_run` | the JavaScript function, converted on entry | when it returns (borrowed, `sjs_native_call`) |
| a native's result | the C layer, then the program | converted on return | the program | the program's (it takes the reference) |
| an error's text | the C layer | at the failing call | copied into the `StaticJSError` at once | the next call |

**Compile errors do not need `sjs_compile`'s dangling `subject`.** The C
layer drives the parser and compiler itself, and reads the error before
destroying them.

## Measurements

Node 22.22, a 4-core x86_64 VM, the release flags (`zig cc -O2`);
10,000 rows of `{ id, name, total }`; the medians of 7 runs. The spikes
are in this session's scratchpad (`crossing.mjs`, `tape.c`, `load.mjs`).

| What | Result | Decides |
|---|---:|---|
| a call from JavaScript into wasm | 2.7 to 4.7 ns | a call per element is affordable (F10) |
| a call from wasm into a JavaScript function (an import) | 4.2 ns | natives cost their conversions, not the call |
| compiling `staticjs.wasm` (969 KB; 316 KB gzipped) | 2.5 ms | `load` once per page or process; share the `WebAssembly.Module` with workers |
| instantiating it | 0.18 ms | one instance per tenant is cheap |
| a guard: start, run, take, through the exports | 90 to 101 ns | the per-evaluation cost to keep |
| **out**, writer text + `JSON.parse` | 715 to 1,102 ns/row | lossy, and slowest |
| **out**, the raw accessors reading layouts | 351 to 362 ns/row | ruled out by F8 |
| **out**, a tape (C encodes, JavaScript decodes) | 361 to 740 ns/row | no faster than handles, and a codec to keep |
| **out**, handles through the scalar C layer | **195 ns/row** | the design |
| **in**, the C API's constructors reading layouts | 741 to 1,547 ns/row | ruled out by F8 |
| **in**, `JSON.stringify` + staticjs's `JSON.parse` | 519 to 542 ns/row | lossy (BigInt, Map, Set, `undefined`, `-0`) |
| **in**, a tape (JavaScript encodes, C decodes) | 481 to 486 ns/row | 30% faster than the builder, at the cost of a codec |
| **in**, the scalar C layer's builder | **675 ns/row** | the design; the tape stays an option (open question 4) |

The handles were fastest on the way out because the C layer hands back an
interned key's pointer, so JavaScript decodes each key once, and ASCII
text is decoded without `TextDecoder`. The spikes ran while a test suite
used the same machine; the ranges are two runs.

## Costs

| Who | Pays |
|---|---|
| per evaluation | the C API's work, plus about 2 to 4 wasm calls per element crossed |
| per context | one `sjs_vm` (about 101 KiB at the default limits; 41 KiB with `maxStack: 256`), one handle stack, three cached programs (`new Date`, `Temporal.*.from`, `f(...a)`) made on first use |
| per package | the module (969 KB, 316 KB gzipped), the C layer (~400 lines, estimated), the JavaScript (~600 lines, estimated) |
| API surface | `load`, `StaticJS`, `Context`, `Program`, `Run`, `Value`, `StaticJSError` |
| the release | a second wasm artifact, `staticjs-js.wasm`, or the npm package alone (decision 1) |
| staticjs's library | nothing |

## Alternatives, and why not

- **Not JSON both ways,** because it changes values: BigInt throws in
  `JSON.stringify`; `-0`, `NaN`, `undefined`, Map and Set are lost. A
  binding refuses what it cannot carry and never turns it into something
  else (F9; `tinyactors-napi/AGENTS.md`, "Values").
- **Not the writer's text out,** because it is JavaScript source. Reading
  it back needs `eval` or a parser of our own, and it was the slowest path
  measured.
- **Not struct layouts in JavaScript** (the example wrapper's way),
  because of F8. Layouts would ship as data (`staticjs.api.json`) that must
  match the module byte for byte, and the measured gain is none: the
  scalar layer was faster.
- **Not a tape (a binary codec) for every value,** because handles were
  faster out. In, a tape was 30% faster, but it is a format to keep in two
  languages. Revisit with a workload where input dominates.
- **Not emnapi** (Node-API emulated in wasm, which the root roadmap leaves
  open for tinyactors' browser package), because staticjs has no
  Node-API addon to reuse. It would add emnapi's runtime to a package
  whose C makes no JavaScript objects; its size was not measured
  (unverified).
- **Not lazy `Proxy` values by default,** because a property read through
  a proxy costs a trap and calls per read, every time, while an eager copy
  costs 195 ns per row once. `Value` covers the case where data should
  stay in staticjs.
- **Not a Promise API,** because a run is synchronous and bounded by fuel;
  `start` and `step` let a caller yield between slices however its
  runtime does (`scheduler.yield`, `setImmediate`).
- **Not a callback clock,** because the library reads no clock
  (`staticjs.h`, the host clock) and a call per read would make a program's
  result depend on when it reads.

## The API map

Each item and the C API behind it, as `tinyactors-napi/docs/api-map.md`
does for core. The C layer's `sjs_js_*` functions call these.

| Item | staticjs |
|---|---|
| `load` | `sjs_version`, `sjs_wasm_allocator` |
| `createContext` | `sjs_context_init`, `sjs_vm_init` |
| `Context.dispose` | `sjs_vm_destroy`, `sjs_context_destroy` |
| `Context.liveBytes` | `sjs_context_live_bytes` |
| `Context.setClock`, `advanceClock` | `sjs_vm_set_clock`, `sjs_vm_advance_clock` |
| `Context.setSeed` | `sjs_vm_set_seed` |
| `Context.compile` | `sjs_parser_init`, `sjs_parser_push`, `sjs_parser_finish`, `sjs_parser_error`, `sjs_compiler_init`, `sjs_compiler_sink`, `sjs_compiler_finish`, `sjs_compiler_error`, the destroy calls; `modules`: an `sjs_modules_vtable` in C |
| `Program.run`, `runValue` | `sjs_vm_start`, `sjs_vm_run`, `sjs_vm_take_result`, `sjs_vm_error`, `sjs_vm_reset` |
| `Program.start`, `Run.step`, `fuelUsed`, `cancel` | `sjs_vm_start`, `sjs_vm_run`, `sjs_vm_fuel_used`, `sjs_vm_reset` |
| `Program.dispose` | `sjs_program_release` |
| conversion in | `sjs_string_new`, `sjs_bigint_new`, `sjs_array_new`, `sjs_array_append`, `sjs_object_new`, `sjs_object_put`, `sjs_set_new`, `sjs_set_add`, `sjs_map_new`, `sjs_map_set`, `sjs_number`, `sjs_boolean`, `sjs_null`, `sjs_undefined`, `sjs_function_native` |
| conversion out | `sjs_length`, `sjs_array_at`, `sjs_object_at`, `sjs_set_at`, `sjs_map_size`, `sjs_map_at`, `sjs_string_text`, `sjs_bigint_text_bound`, `sjs_bigint_text`, `sjs_writer_init`, `sjs_writer_write`, `sjs_writer_destroy` |
| `Value`, its `dispose` | `sjs_retain`, `sjs_release` |
| `Value.toString` | `sjs_writer_*` |

Not exposed: `sjs_vm_clock`, `sjs_vm_random` and `sjs_vm_set_random`
(saving a paused run elsewhere, which this package does not do); the event
stream to a sink of JavaScript's (a formatter or a linter in JavaScript
would need an event per token across the boundary; not asked for);
`sjs_map_get`, `sjs_object_get` (a conversion reads every entry);
`no_shapes`, `no_regex_leaf`, `no_regex_dfa` (for staticjs's own tests).

## Decisions for the maintainer

1. **Where natives' import lives.** Either the npm package ships its own
   module (the amalgamation, the C layer and one import), and the
   release's `staticjs.wasm` keeps importing nothing (recommended: the raw
   module's contract stays as released). Or one module imports
   `staticjs_js.native`, and raw users pass a stub.
2. **Temporal out, where the runtime has no `Temporal`** (Node 22, most
   browsers today): the ISO string (recommended: no polyfill bundled), or a
   small frozen object `{ type, iso }`.
3. **One evaluator per context,** taken by `start` until the run ends
   (recommended: it is the C API's shape). Or `context.evaluator()` for
   several runs at once, at about 101 KiB each.
4. **The tape for input,** if a workload shows input dominating (30%
   measured on 10,000 rows). Not now.

## If adopted

1. `staticjs-js/` with `AGENTS.md`, `docs/api-map.md` (checked as napi's
   is) and the C layer, built with `Toolchain::Target.release('wasm32-freestanding')`
   from staticjs's amalgamation.
2. The value conversions, with tests that every row of the values table
   round-trips and every refusal throws. Run the spec cases that expect a
   value through the package, so the package agrees with `diff-node`.
3. `load` for Node (`fs`) and browsers (`instantiateStreaming`), and a
   test in Chromium with the Playwright the environment has.
4. Packaging (the package's module, decision 1) and its changelog.
5. Replace `staticjs/examples/wasm/` with an example of the package, and
   `staticjs/docs/guide/wasm.md` with its guide.
