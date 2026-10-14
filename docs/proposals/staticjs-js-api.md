# A JavaScript API for staticjs.wasm

**Status: Sketch.** Touches: a new package, `staticjs-js/`
(`@tinyactors/staticjs`), with a C layer of its own; staticjs's release
packaging; six small additions to staticjs's C API ("Requests to
staticjs").

The maintainer approved the direction (2026-10-14). This revision answers an adversarial review of the first draft; "The review, answered" lists each finding and where it went.

## Summary

A package that evaluates staticjs from JavaScript, in Node and in browsers,
with JavaScript values in and out:

```js
import { load } from '@tinyactors/staticjs';

const staticjs = await load();
const context = staticjs.createContext();
context.setClock({ epochMilliseconds: Date.now() });

const guard = context.compile('In("heating") && event.data.f > limit', { globals: ['In', 'event', 'limit'] });
guard.run({ In: (id) => active.has(id), event, limit: 25 }, { fuel: 10_000 });   // true

guard.dispose();
context.dispose();
```

Where the runtime has explicit resource management, `using guard = ...`
disposes at the end of the block. Node 22 does not: `using` is a
`SyntaxError` there (spike: `node u.mjs` with `{ using x = ... }` gives
"SyntaxError: Unexpected identifier 'x'"), though `Symbol.dispose` exists.

The package is built from the same amalgamation as `staticjs.wasm`, plus a
C layer of its own (`sjs_js_*`) whose exports take and return only
numbers. So JavaScript reads no C struct, and values cross by a handle per
element, not by text or a codec.

Measured on this repository's spikes (Measurements):

- a guard costs 200 to 215 ns per evaluation, counting the conversion of
  its input and its result; the raw exports alone, with globals built
  beforehand, take 90 to 101 ns;
- a result of 10,000 small objects reaches JavaScript in 1.9 to 2.8 ms
  (187 to 282 ns per row), 2.5 to 5.9 times faster than the writer's text
  plus `JSON.parse` across the runs recorded.

Every kind the package converts arrives exactly: BigInt, Map, Set,
`undefined`, `-0`, and an invalid Date. It refuses what it cannot carry
and never turns it into something else. In: a RegExp, a Temporal type
staticjs lacks, an array with holes. Out: a Temporal value where the
runtime has no `Temporal`.

The package needs six small additions to staticjs's C API. Each one also
serves C embedders, and each one replaces a workaround the first draft had
built into the binding.

## Whom it serves, and who pays

- **First: the application developer writing JavaScript** who evaluates
  programs written by someone else: rules, filters, a statechart's guards
  and assignments, computed fields. In a browser, an edge runtime, or Node
  without a native addon. They ask: what did this program give, as a value
  I can use? Why was it refused, and where? Will it end, and how much may
  it use? Where is a paused run? Today they have the C API through raw
  exports, or `examples/wasm/staticjs.mjs`, which returns text.
- **Then: C embedders of staticjs**, through the requests. Each request
  below names the C scenario it serves. They get calls of function values,
  Dates and Temporal values made and read without running a program,
  iterators and regular expressions read without the writer, natives that
  can free what they hold, and compile errors that keep their subject.
- **Then: a future browser host of tinyactors with the staticjs data
  model.** Its state would live in staticjs values that never cross into
  JavaScript (`Value` handles, below).
- **Who pays:**
  - per evaluation: the C API's own work, plus 2 to 4 wasm calls per
    element converted. Each function in the globals adds one native value
    per run: an allocation and a free (unmeasured);
  - per context: one evaluator, 101,616 B (99.2 KiB) with the default
    limits or 40,176 B (39.2 KiB) with `maxStack: 256` (spike
    `vmsize.mjs`), plus the C layer's tables of rows;
  - per package: about 1 MB of wasm (969,106 B; 316,388 B gzipped) and
    about 1,000 lines of JavaScript and C (an estimate);
  - API surface: `load`; the interfaces `StaticJS`, `Context`, `Program`,
    `Run`, `Value`, `Frame` and `FunctionSummary`; the class
    `StaticJSError`;
  - staticjs's library: six additions (one field each in two structs, a
    vtable entry, and about six functions; "Requests to staticjs"), with
    their tests and documentation;
  - the maintainers: a package with a build, tests and a changelog, and a
    second wasm artifact in the release (decision 1).
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
| F4 | Values are reference counted, and immutable once shared. Copying the struct does not retain it. `sjs_release` does not recurse. A run cannot change a global the embedder also holds | `staticjs.h:185`, `:225`, `:245`; spike `mutate.mjs`: `p.push(2)` on such a global faults ("staticjs changes a value only while one variable holds it"), the global stays `[1]` |
| F5 | A native function is a C vtable called synchronously from inside `sjs_vm_run`, which takes its result there. The module's C can call JavaScript only through an import (F1). Node 22 has no `WebAssembly.Function` to make a wasm function from a JavaScript one | `staticjs.h:313-333`; `src/vm.c:847-863`; spike: `typeof WebAssembly.Function` is `undefined` |
| F6 | A modules adapter (`sjs_modules_vtable.find`) is called during compilation and answers with exports the compiler borrows while it runs | `staticjs.h`, "Compiler and programs" |
| F7 | The value API reads arrays, objects, sets, maps, strings, numbers, booleans and BigInts (as text). It has nothing that reads or makes a Temporal value, a Date or a regular expression, and nothing that reads an iterator's values: `sjs_length` gives 0 for an iterator. It cannot call a staticjs function from C. Only the writer reads those kinds, and it writes them as JavaScript source | `staticjs.h`, "Context and values" and "Writer"; `src/value.c:631-643`; `src/writer.c:106-138` |
| F8 | This repository keeps C structs out of the languages: "Only C touches host structs". The rule is written for tinyactors' host structs; applying it to staticjs's structs is an analogy with the same reason (layouts copied into another language must match the module byte for byte) | `tinyactors-core/docs/bindings.md:265`; `tinyactors-core/docs/boundary.md:270-272` |
| F9 | A binding's direct layer is C, mirroring the C API, with a language layer above it. Scratch shared between calls is not reentrant, so a call takes a mark and releases to it | `tinyactors-napi/AGENTS.md`, "Layers and the resolver" and "The binding's memory"; napi commit `8d600f4c`, cited by `tinyactors-core/docs/boundary.md:532` and `.agents/skills/boundary-design/SKILL.md:78` |
| F10 | A call from JavaScript into wasm costs about 3 ns, and one from wasm into JavaScript 3.5 to 5.8 ns | ADR 0010:59 (2.9 ns); spikes below |
| F11 | A context has one evaluator per `sjs_vm_init`, and `sjs_vm_start` resets it: starting a program abandons the evaluation it held | `src/vm.c:1617-1628` |
| F12 | `sjs_native` has no release hook. The library stores the vtable and `self` in the function value and calls nothing when it frees them | `staticjs.h:326-333`; `src/value.c:687-694` |
| F13 | `sjs_compile` destroys the parser and compiler its error's `subject` points into before it returns. The example works around this by reporting a compile error without its subject | `src/compiler.c:3512-3532`; `staticjs.h:99-103`; `examples/eval.c:60-63` |
| F14 | An error's subject is cut to 64 bytes: the stage keeps it in a fixed slot | `src/internal.h:1320-1324`; `src/vm.c:34`, `:851` |
| F15 | staticjs can inspect a paused evaluation (its frames: kind, position, built-in, element progress) and a program (its functions: position, parameters, shape) | `staticjs.h:761-817`, "Inspection" |
| F16 | staticjs's library can be changed: the maintainer invited debugging and observability additions to it, and every caller of it is in this repository | the maintainer, 2026-10-14; root `AGENTS.md`, "The libraries are private" |
| F17 | A `FinalizationRegistry` callback may never run, and when it runs, it runs as a job between synchronous calls, so it can run between two steps of a paused run | ECMA-262, `HostEnqueueFinalizationRegistryCleanupJob`; MDN, [FinalizationRegistry](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/FinalizationRegistry), "Avoid where possible" (a conforming implementation is not required to call cleanup callbacks) |
| F18 | A wasm module's C cannot make JavaScript objects; something in JavaScript, or an emulated Node-API, must | `ROADMAP.md:840-845` |

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
- **The C API itself** lacks six things the package needs. Without them,
  the binding would have to work around each one. The first draft did
  this, and the review showed what it cost:
  - Helper programs (`new Date(t)`, `Temporal.X.from(s)`, `f(...a)`) ran
    on the context's one evaluator. A helper started while a `Run` was
    paused threw that run away (F11). One started while converting a
    native's result ran inside `sjs_vm_run` (F5), on the evaluator that
    was running.
  - Iterators had no route out (F7).
  - Natives could not be freed (F12).
  - A compile error's subject dangled (F13).

  "Requests to staticjs" replaces each workaround with a small addition
  to the library (F16).

## The API

```ts
// index.d.ts (sketch)
export function load(source?: BufferSource | URL | Response | WebAssembly.Module): Promise<StaticJS>;

export interface StaticJS {
  readonly version: string;                       // sjs_version
  createContext(options?: ContextOptions): Context;
}

export interface ContextOptions {
  limits?: Partial<Limits>;                       // over SJS_LIMITS_DEFAULT; names as in sjs_limits, camelCase
}

export interface Context {
  compile(source: string, options?: CompileOptions): Program;
  value(value: unknown): Value;                   // a JavaScript value made once, passed to many runs
  setClock(instant: { epochMilliseconds: number } | { epochNanoseconds: bigint }): void;
  advanceClock(duration: { milliseconds?: number; nanoseconds?: bigint }): void;
  setSeed(seed: number): void;                    // Math.random as `node --random-seed=<seed>`
  readonly liveBytes: number;                     // sjs_context_live_bytes
  dispose(): void;                                // also [Symbol.dispose]
}

export interface CompileOptions {
  module?: boolean;                               // SJS_GOAL_MODULE; default: an expression
  globals?: readonly string[];                    // the names each run gives values for
  modules?: Record<string, Value>;                // what `import ... from "name"` finds, for this compile
}

export interface RunOptions { fuel: number | bigint }   // required: sjs_vm_run takes it, with no default

export interface Program {
  readonly globals: readonly string[];
  functions(): FunctionSummary[];                 // sjs_program_function_count, sjs_program_function
  run(globals: Record<string, unknown>, options: RunOptions): unknown;     // the value, as JavaScript
  runValue(globals: Record<string, unknown>, options: RunOptions): Value;  // the value, kept in staticjs
  start(globals?: Record<string, unknown>): Run;  // run in slices of fuel
  dispose(): void;                                // also [Symbol.dispose]
}

export interface Run {
  step(fuel: number | bigint): boolean;           // true when done; throws StaticJSError on a fault
  readonly fuelUsed: bigint;                      // sjs_vm_fuel_used
  frames(): Frame[];                              // where a paused run is: sjs_vm_frame_count, sjs_vm_frame
  result(): unknown;                              // once done
  cancel(): void;                                 // sjs_vm_reset; also dispose() and [Symbol.dispose]
}

export interface Frame {                          // sjs_frame
  readonly kind: 'function' | 'loop';
  readonly line: number; readonly column: number; readonly offset: number;
  readonly object: string; readonly name: string; // a loop's built-in: "" and "map", "Array" and "from"
  readonly index: number; readonly length: number;// a loop's progress; 0 and 0 otherwise
}

export interface FunctionSummary {                // sjs_function_summary
  readonly line: number; readonly column: number; readonly offset: number;
  readonly params: number;
  readonly shape: string;                         // "PLUCK", "COMPARE_BY_KEY", ... or ""
}

export interface Value {
  readonly kind: 'undefined' | 'null' | 'boolean' | 'number' | 'string' | 'bigint' | 'array' | 'object'
    | 'set' | 'map' | 'iterator' | 'function' | 'regexp' | 'temporal';   // sjs_kind; a Date is 'temporal'
  toJS(): unknown;
  toString(): string;                             // the writer: JavaScript source that evaluates to it
  dispose(): void;                                // also [Symbol.dispose]
}

export class StaticJSError extends Error {
  readonly kind: 'syntax' | 'unsupported' | 'name' | 'limit' | 'memory' | 'fault';   // sjs_error_code, nothing added
  readonly line: number;                          // from 1
  readonly column: number;                        // from 1, in UTF-8 bytes, as sjs_position
  readonly offset: number;
  readonly subject: string;                       // at most 64 bytes (F14)
  readonly reason: string;                        // the library's message
  // `cause`: the error a native function threw, whole, when one did
}
```

`evaluate` from the first draft is dropped: it was `compile`, `run` and
`dispose` in one call, so it was a second way to do the same thing.

Choices, each against what JavaScript users expect:

- **Globals by name.** `compile` takes the names, as `sjs_compiler_config`
  does; `run` takes an object keyed by them. A missing name is a
  `TypeError` before the run; an extra key is ignored.
- **`fuel` is given to each run**, as `sjs_vm_run` takes it. There is no
  default, per context or otherwise. `step(fuel)` returns `false` when the
  fuel is spent, as `SJS_RUN_PAUSED` says. `run` and `runValue` are one
  call that returns a value. When the fuel is spent, a run has no value,
  so they reset the evaluator (`sjs_vm_reset`) and throw a `RangeError`
  that names the fuel used. This is not a `StaticJSError`, because staticjs
  reported no error. A caller who wants to resume uses `start`. This is
  the one place the package decides something the C API leaves to the
  embedder; the alternative, `run` returning a `Run` when paused, makes
  its result a union.
- **The clock and the seed are the host's** (`sjs_vm_set_clock`,
  `sjs_vm_set_seed`). Nothing sets them for you, as the C API does
  nothing for you, so a program that reads an unset one faults.
  `setClock` takes anything with `epochMilliseconds` or
  `epochNanoseconds`, so a `Temporal.Instant` works directly.
- **Disposal is explicit,** with `dispose()`, and `Symbol.dispose` for
  `using` where the runtime has it. A `FinalizationRegistry` is a fallback
  only. Its callbacks may never run, and they can run between two steps of
  a paused run (F17). So nothing a run reads depends on one: a `Run` holds
  its `Program` and the `Value`s it borrows, both as JavaScript references
  and as references in C (`sjs_vm_start` retains the program,
  `src/vm.c:1628`; the C layer retains each borrowed global). A callback
  only releases the C row of an object that has become unreachable, and
  it checks the row's generation first (Lifetimes).
- **Inspection** comes from staticjs (F15). `Run.frames()` says where a
  paused run is, down to the element a `map` has reached, so an operator
  can say where a program ran out of fuel. `Program.functions()` lists the
  functions and the shapes the compiler found. Both read on demand, so an
  evaluation pays nothing when they are not called.
- **Names** come from familiar APIs, with this library's meaning:
  `compile` and `run` as in `node:vm`'s `Script`; `dispose` as in
  quickjs-emscripten's handles and TC39's explicit resource management;
  `epochMilliseconds` and `epochNanoseconds` as in Temporal.

## Values

| JavaScript → staticjs | | staticjs → JavaScript | |
|---|---|---|---|
| number, string, boolean, `null`, `undefined`, bigint | the same | number (`-0` and `NaN` kept), string, boolean, `null`, `undefined`, BigInt | the same |
| `Array` without holes | array | array | `Array` |
| a plain object (prototype `Object.prototype` or `null`): own enumerable string keys, read with `[[Get]]` | object, keys in JavaScript's order | object | a plain object |
| `Set` of primitives, `Map` with primitive keys | set, map | set, map | `Set`, `Map` |
| a function | a native function for this run (below) | a function | a JavaScript function that calls it (below) |
| a `Value` of this context | itself, borrowed for the run: no copy | iterator | an Array Iterator over what it has left to yield (`[...].values()`, as the writer writes it); an entries iterator yields `[key, value]` arrays (request R3) |
| a `Date`, valid or not | a Date with its time value (request R2) | Date | `Date`; an invalid one is `new Date(NaN)` |
| a `Temporal.PlainDate`, `PlainTime`, `PlainDateTime`, `Duration` or `Instant` in the ISO 8601 calendar | the same type (request R2) | Temporal value | the runtime's `Temporal` type (request R2); refused where the runtime has none (decision 2) |
| | | regular expression | `RegExp` of the same source and flags (request R4) |

Refused with a `TypeError`, never turned into something else:

- a string that is not well formed (`String.prototype.isWellFormed`): a lone
  surrogate is refused, not replaced, as `TextEncoder` would replace it;
- an array with a hole, as staticjs refuses one (spike `holes.mjs`:
  `[1,,2]` gives "staticjs arrays have no holes");
- a `RegExp`. Making one from C would need the regular expression compiler
  behind a new call, and no scenario asks for it yet. A program writes its
  own literal;
- a `Temporal.ZonedDateTime`, `PlainYearMonth` or `PlainMonthDay`, and a
  Temporal value in a calendar other than `iso8601`. staticjs has none of
  these (`staticjs/docs/guide/language.md:370-383`; `src/internal.h:635-643`);
- an object of another class, a symbol key, an own `__proto__` key;
- a `Set` member or `Map` key that is not a primitive;
- nesting deeper than the context's `maxNesting`, which also stops a cycle;
- a `Value` of another context, or one already disposed.

**Shared parts grow.** staticjs has no identity (`staticjs.h:290`), so an
object that appears twice in the input is converted twice. Input that
shares parts heavily is converted to the tree the sharing unfolds to,
which can be far larger than the JavaScript graph. `maxValueItems` and
`maxNesting` bound each container and the depth, not the total.

**User code runs during conversion in.** A getter runs during `[[Get]]`. A
`Proxy` passes the plain-object and `Array.isArray` checks and runs its
traps (spike: `Object.getPrototypeOf(new Proxy({}, {}))` is
`Object.prototype`, and `Array.isArray(new Proxy([], {}))` is `true`). A
patched `Map.prototype` or `Set.prototype` would run too, so the package
calls the intrinsics it captured at `load` instead of looking methods up
on the value. Conversion in is guarded like a run:

- the context's running flag is set for the whole conversion, so a getter
  that calls the same context (`run`, `compile`, `value`, `dispose`) gets
  a `TypeError`;
- the builder keeps no state between calls: each container is a handle,
  and each append names its container (How it crosses);
- everything a conversion makes sits above the call's mark, so a getter
  that uses another context in the same instance allocates above it and
  gives back only its own.

Conversion out runs no user code: it calls the intrinsics captured at
`load` (`Map`, `Set`, `Date`, `RegExp`, the `Temporal` constructors).

**A function out** becomes a JavaScript function. Calling it converts its
arguments in, calls the staticjs function on the context's evaluator
(request R1), and converts the result out. Each call gets the fuel of the
run that returned the function, given anew. That run's `fuel` is the one
budget the caller stated for this code. The alternative is a
`Value.call(args, { fuel })` with no JavaScript function at all. A
function out keeps its program alive after `Program.dispose` (spike
`callf.mjs`: a closure called after its defining program was released
gives 43). A call throws a `TypeError` while its context is running, or
while a `Run` holds the evaluator (decision 3).

**Native functions.** For each JavaScript function in `globals`, a run
makes one `sjs_native` value, and the package keeps the function in a
table under the native's id. The function is called synchronously during
the run. Its arguments are converted to JavaScript, and its return value
converted back. A Date or Temporal value returned is made without running
anything on the evaluator (request R2). A function that throws faults the
run. The error's message, cut to 64 bytes, is the fault's subject (F14).
The whole thrown error is the `StaticJSError`'s `cause`. As in C, a
native must not change anything the program can see; the package cannot
check that.

The table entry is dropped when staticjs frees the native's value, which
request R5's release hook reports. Usually that happens when the run
ends. If the program returned the function or kept it in a `Value`, it
happens later. Without R5, natives are scoped to the run: at the run's end
the entry is dropped and the native's record changes generation, so a
native that escaped faults ("a native function of an ended run") when it
is called. Its 8-byte record then stays until the context is disposed,
because staticjs may still hold its `self`.

**Reentrancy.** A native function or a getter that calls its own context
(`run`, `compile`, `value`, `dispose`, a function out) gets a `TypeError`
("the context is running"). The C layer keeps that flag. Another context
in the same instance may be used: every call of the C layer takes a mark
on its handle stack and scratch at entry and releases to it at exit, so a
nested call never overwrites an outer one's (F9). A paused `Run` keeps
nothing on that stack (Lifetimes).

## How it crosses

### A C layer of the package's own

`staticjs-js/src/` holds C compiled into the package's module beside the
amalgamation. Its functions carry the prefix `sjs_js_`, take at most three
parameters, and return a number. JavaScript reads no struct (F8). F18 is
why the language layer exists at all: the module's C cannot make the
JavaScript objects, so JavaScript makes them, one accessor call per
element.

That is not the rejected "language layer ... decoding values"
(`.agents/skills/boundary-design/SKILL.md:75`). Its reason was a second
codec beside `values.c`, in a binding whose C can make the language's
objects. Here no byte format exists to decode: JavaScript calls accessors
and builds what they answer. The tape measured below would be such a
codec, and it is rejected for that reason (Alternatives).

**Bytes at the exports.** The rule that bytes cross as a view struct
(`buffers-are-ta-buffer`, `tinyactors-napi/AGENTS.md`, "C rules"; root
`AGENTS.md`, "At most three parameters") is kept inside the C layer: its
internal functions pass `sjs_str` and `sjs_buffer`, staticjs's own view
types. It is not kept at the exports, and that exemption is named here.
An export's signature is fixed by what JavaScript can pass without
writing a struct layout (F2, F8), as a Node-API callback's is fixed by its
typedef. Two things keep the exemption narrow:

- **In:** no pointer is passed in. JavaScript asks for room
  (`_scratch(n)` returns an offset into scratch the C layer owns, above
  the call's mark), writes UTF-8 there with `encodeInto`, and passes the
  length only. `_string(len)`, `_key(len)` and `_compile_global(c, len)`
  read their own scratch.
- **Out:** an offset and a length, in two calls, as a view whose owner is
  stated (the value, borrowed while the conversion's root is retained).

Sketch:

| Group | Functions |
|---|---|
| contexts | `sjs_js_context_new(limits)`, `_context_free(ctx)`, `_set_clock(ctx, seconds, nanoseconds)` (seconds as an f64, exact: Temporal's limit is 8.64 × 10^12 s, under 2^53), `_advance_clock(ctx, high, low)` (nanoseconds as two u32 halves, so no BigInt is made), `_set_seed(ctx, seed)` |
| compile | `_compile_begin(ctx)`, `_compile_global(c, name_len)`, `_compile_module(c, name_len, value)`, `_compile_end(c, goal, source_len)` → a program, or 0 and the error |
| run | `_run_begin(program)`, `_run_global(r, value)`, `_run(r, fuel)` → status, `_run_take(r)` → value, `_run_call(ctx, function, args_array)` (R1), `_frame_count(r)`, `_frame(r, i)` with field reads |
| reading a value | `_kind(v)`, `_number(v)`, `_boolean(v)`, `_length(v)`, `_at(v, i)`, `_entry(v, i)` then `_key_ptr()` and `_key_len()`, `_string_ptr(v)` then `_string_len()`, `_bigint_text(v)` then `_text_ptr()` and `_text_len()`, `_iterator_at(v, i)` (R3), `_time_read(v)` then `_time_field(i)` (R2), `_regexp_source(v)` and `_regexp_flags(v)` (R4), `_write(v)` (the writer, for `Value.toString`) |
| building a value | `_array_new(n)`, `_array_append(a, v)`, `_object_new(n)`, `_object_put(o, key_len, v)`, `_set_new(n)`, `_set_add(s, v)`, `_map_new(n)`, `_map_set(m, k, v)`, `_number(d)`, `_string(len)`, `_bigint(len)`, `_boolean(b)`, `_null()`, `_undefined()`, `_time_begin(type)`, `_time_put(i, d)`, `_time_end()` (R2), `_native(id)` |
| handles | `_keep(v)` → a persistent row; `_drop(row, generation)`; `_mark()`, `_release_to(mark)` |
| errors | `_error_code()`, `_error_line()`, `_error_column()`, `_error_offset()`, `_error_message_ptr/len()`, `_error_subject_ptr/len()` |
| imports | `staticjs_js.native(id, args, count)` → a value, or a fault; `staticjs_js.release(id)` (R5), which only deletes a table entry |

- **Values are handles:** indices into a stack of `sjs_value` slots in C.
  A slot is borrowed from its container and released to a mark. A
  `Value` is a row in a separate table, with a generation, and is
  retained until disposed. This is the boundary skill's "values are
  handles, metadata are views".
- **The builder has no current container.** Every append names its
  container's handle, and a key is written to scratch only after its
  value is converted, so a getter that runs during a conversion leaves
  nothing half built for the outer one.
- **Keys are decoded once per conversion.** Out, the C layer hands back a
  key's pointer, and JavaScript decodes each distinct pointer once, in a
  `Map` that lives for one conversion. The pointer stays valid only while
  the conversion's root is retained. Across conversions, the memory of a
  freed key is reused for another, and a cache kept by pointer returns
  the wrong key (spike `keyreuse.mjs`: the key `abc` at 3213468 is
  released, and `xyz` lands at 3213468).
- **ASCII text** is decoded by a loop over its bytes, and anything past
  ASCII by `TextDecoder`.
- **Modules without a callback:** the C layer's `find` looks the name up
  among the `(name, value)` pairs that `_compile_module` gave for this
  compile. No JavaScript runs during compilation, so `modules` is a plain
  object of `Value`s.
- **Compile errors** come from `sjs_compile` once request R6 gives its
  subject a home. Until then, the C layer drives the parser and compiler
  itself and reads the error before destroying them (F13).

### Lifetimes

| Object | Owner | Made | Read | Ends |
|---|---|---|---|---|
| the instance and its memory | `StaticJS` | `load` | every call | when unreachable; memory never shrinks |
| `sjs_context`, its `sjs_vm`, the context's tables (Values, Programs, Runs, natives) | `Context` | `createContext` | its calls | `dispose`: it resets a paused run, releases every row of every table, then calls `sjs_vm_destroy` and `sjs_context_destroy`, which needs everything gone first (`staticjs.h:198`). It then bumps the context's generation, so each wrapper's next call throws a `TypeError`. Refused while the context is running |
| `sjs_program` | `Program`, a row of its context | `compile` | `run`, `start`; retained by a run (`src/vm.c:1628`) and by functions it made (`callf.mjs`) | `dispose`, or its context's `dispose` |
| globals converted for `run` | the call: slots above its mark | `run`, before `sjs_vm_start` | during the run (borrowed until the evaluation ends, `staticjs.h:672-678`) | released to the call's mark when `run` returns |
| globals converted for `start` | the `Run`: an array it owns, allocated from the context's allocator | `start` | each `step` (borrowed) | done, a fault, `cancel`, or the context's `dispose` |
| a `Value` given as a global | its `Value`; the run also retains it in C and references it in JavaScript | `value()`, `runValue()` | during the run | `Value.dispose` drops the `Value`'s reference; the run's lasts until the run ends |
| a native, for a function in the globals | staticjs (the function value); a record in the C layer; the function in the package's table | the run's conversion in | by the program, inside `sjs_vm_run` (`src/vm.c:847-863`) | when staticjs frees the value (R5); without R5, the run's end |
| slots of a conversion out | the C layer's stack | `_at`, `_entry`, `_iterator_at` | during that conversion | `_release_to(mark)` at its end |
| the key cache of a conversion out | that conversion | its start | for each key | its end, never later (`keyreuse.mjs`) |
| scratch for text in | the C layer, above the call's mark | `_scratch(n)` | the next `_string`, `_key`, `_bigint` or `_compile_global` (copied by staticjs) | the call's mark |
| a native's arguments | the evaluator | inside `sjs_vm_run` | the JavaScript function, converted on entry | when it returns (borrowed, `staticjs.h:318-320`) |
| a native's result | the C layer, then the program | converted on return, with no evaluator (R2) | the program | the program's (it takes the reference) |
| a finalizer's work | the `FinalizationRegistry` | an object becomes unreachable | the C layer's tables only, never the context | releases one row if its generation still matches; nothing if the context was disposed |
| an error's text | the C layer (a compile's subject: R6's buffer) | at the failing call | copied into the `StaticJSError` at once | the next call |

A paused `Run`'s globals are not on the shared mark stack. Two contexts'
runs can interleave their steps; with globals on a shared stack, one run's
release would free the other's. A run that is abandoned without `cancel`
would also strand the slots above it. So the `Run` owns its globals, and
`cancel`, `dispose` or its context's `dispose` frees them.

## Requests to staticjs

Six additions, each the smallest that removes a binding workaround. Names
and exact shapes are the library's to choose; the sketches show what the
package needs. Every request also serves C embedders, who today have the
same gaps (F7, F12, F13). None of them costs an evaluation anything when
it is not used.

| # | Addition (sketch) | The binding's scenario | A C embedder's scenario | Removes from the binding | Costs staticjs |
|---|---|---|---|---|---|
| R1 | **Call a function value.** `sjs_run_params` gains `sjs_value function`. When `program` is NULL, `sjs_vm_start` calls `function` with `globals` as its arguments, on fuel, pausable like a program | a module's export called from JavaScript: `exports.net(rows)` | a host that compiles a module of handlers once and calls `onEvent(e)` for each event | the helper program `f(...a)` compiled per context, and the arguments array built for each call | a field; at start, the frame a call instruction builds (code size unmeasured) |
| R2 | **Make and read Dates and Temporal values.** A public struct with the type (PlainDate, PlainTime, PlainDateTime, Instant, Duration, Date) and its fields: the ISO date and time, an Instant's `sjs_epoch`, a Duration's ten numbers, a Date's time value (NaN for an invalid one). `sjs_time_new(context, fields, out)` checks the fields as the constructors do. `sjs_time_read(value, out)` reads them | a `Date` or `Temporal.Instant` in an event; a Date a program returns, or a native returns from inside `sjs_vm_run` | a host that passes an event's timestamp as a Date or an Instant, or reads a Date a program returns, without running `new Date(t)` on its evaluator or parsing the writer's text | the helper programs `new Date(t)` and `Temporal.X.from(s)`: one threw a paused run away (F11), and one started inside `sjs_vm_run` ran on the evaluator that was running (F5). Also the writer round trip and its reparse for these kinds out | two functions and a struct; the fields exist already (`src/internal.h:635-690`) |
| R3 | **Read an iterator's values.** `sjs_length` counts what an iterator has left to yield. `sjs_iterator_at(iterator, index, out)` gives the value at `index`, borrowed; for an entries iterator it gives the key and value without making the pair | a program that returns `map.keys()` or `xs.values().filter(f)` | a host that reads such a result | the only route out today, the writer (F7); without R3, iterators out are refused | one function and a case in `sjs_length`; the writer's `entries_of` and `iterated` compute both (`src/writer.c:106-138`) |
| R4 | **Read a regular expression's source and flags.** `sjs_regexp_source(value)` → `sjs_str`, and its flags | a program that returns a pattern | a host that stores results in a format of its own (CBOR, as a tinyactors data model would) | parsing the writer's `/…/flags` in JavaScript | one or two functions |
| R5 | **A release hook for natives.** `sjs_native_vtable` gains an optional `release(self)`, called when the native's function value is freed | the table that maps a native's id to a JavaScript function. Without the hook, the table holds each function strongly until the context is disposed, and a fresh arrow per run grows it with every evaluation | a host whose native's `self` is allocated per session (an `In()` bound to one session's configuration) and today cannot free it until the context is destroyed | the strong table, the per-context `WeakMap` cache, and the leak | a pointer per vtable (NULL allowed); a check when a native's value is freed |
| R6 | **A compile error whose subject outlives `sjs_compile`.** `sjs_compile_params` gains an optional `sjs_buffer *subject`. The subject is copied into it, cut to its capacity, and `error->subject` points there | every compile error the package reports | every C caller of `sjs_compile`: `examples/eval.c:60-63` drops the subject today | driving the parser and the compiler in eight calls to read the error before they are destroyed | a field and a copy on the error path |

Not requested:

- **A call outside an evaluator.** A staticjs function needs fuel and
  frames, so R1 runs it on one.
- **Making a regular expression from C.** No scenario asks for it, and it
  would put the regular expression compiler behind a new call.
- **More observability.** Fuel used (`sjs_vm_fuel_used`) and the new
  inspection calls (F15) answer the operator's questions: how much a run
  used, and where a paused one is.

**If a request is declined,** the package falls back as follows, and says
so in its guide:

- R1: the helper `f(...a)`, which is refused while a `Run` holds the
  evaluator or the context is running.
- R2: the helpers, refused in the same cases. A Date or Temporal value
  returned by a native faults the run.
- R3: iterators out are refused.
- R4: the writer's text is parsed from its last `/`.
- R5: natives are scoped to the run, as in "Native functions".
- R6: the C layer drives the stages itself.

## Measurements

Node 22.22, a 4-core x86_64 VM, the release flags (`zig cc -O2`);
10,000 rows of `{ id, name, total }` unless stated; medians of 7 runs.
The spikes are in this session's scratchpad: `crossing.mjs`, `tape.c`,
`load.mjs`, and the review's `review.mjs`, `fair.mjs`, `keyreuse.mjs`,
`vmsize.mjs`, `mutate.mjs`, `callf.mjs` and `holes.mjs`. Each range spans
the runs recorded: the first draft's two, the review's, and this
revision's. Some ran while a test suite used the same machine, so treat
differences under about 20% as noise.

| What | Result | Decides |
|---|---:|---|
| a call from JavaScript into wasm | 2.7 to 4.7 ns | a call per element is affordable (F10) |
| a call from wasm into a JavaScript function (an import) | 3.5 to 5.8 ns | natives cost their conversions, not the call |
| compiling `staticjs.wasm` (969,106 B; 316,388 B gzipped) | 2.5 to 7.2 ms | `load` once per page or process; share the `WebAssembly.Module` with workers |
| instantiating it | 0.07 to 0.77 ms | one instance per tenant is cheap |
| an evaluator (`sjs_vm_init`) | 101,616 B (99.2 KiB) by default; 40,176 B (39.2 KiB) with `maxStack: 256` | the cost of decision 3's options |
| a guard, raw exports, globals built beforehand: start, run, take | 90 to 101 ns | the floor |
| a guard as the API runs it: build `event`, start, run, take, read the boolean, release | 200 to 215 ns | the per-evaluation cost to keep |
| **out**, writer text + `JSON.parse` | 715 to 1,102 ns/row | lossy, and slowest |
| **out**, raw accessors reading layouts (a key cache) | 351 to 362 ns/row; 334 to 378 with the ASCII loop | ruled out by F8, not by speed |
| **out**, a tape (C encodes, keys as bytes; JavaScript decodes, ASCII loop) | 358 to 740 ns/row; the C encode alone 68 to 99 | — |
| **out**, handles, no key cache, `TextDecoder` for every string | 589 to 839 ns/row | the tricks, not the layout, made the difference |
| **out**, handles, key cache per conversion, ASCII loop | 187 to 282 ns/row | the design |
| **out**, a tape with key references, the same cache and ASCII loop | 218 to 286 ns/row (handles 187 to 197 in the same runs) | handles are no slower, and have no format |
| **out**, rows of `{ id: BigInt, tags: Set(2), m: Map(2) }`: handles / the tape with key references | 832 to 900 / 863 to 1,019 ns/row | even; BigInt, Set and Map cost about four times a small row |
| **in**, the C API's constructors reading layouts | 741 to 1,547 ns/row | ruled out by F8 |
| **in**, `JSON.stringify` + staticjs's `JSON.parse` | 497 to 542 ns/row | lossy (BigInt, Map, Set, `undefined`, `-0`) |
| **in**, a tape (JavaScript encodes, C decodes) | 481 to 638 ns/row | — |
| **in**, the scalar builder | 577 to 755 ns/row | the design; no consistent gap to the tape (one run each way) |

What these show, and what they do not:

- **The out side was won by two tricks, not by handles.** They are a key
  cache and an ASCII loop. Without them, handles took 589 to 839 ns per
  row and were the slowest structured path. With both tricks on both
  sides (`fair.mjs`), handles took 187 to 197 ns per row and a tape that
  emits key references took 218 to 286, in the same runs. On rows with a
  BigInt, a Set and a Map the two were even.
- **The first draft's key cache was keyed by pointer across
  conversions.** That was wrong (`keyreuse.mjs`). Its numbers stand only
  because the spike's value stayed alive throughout. The cache is now per
  conversion, and `fair.mjs` measured it that way.
- **The first conversion after load** took 308 to 946 ns per row in
  `fair.mjs`, against about 190 once warm. That is JIT warm-up. A
  one-shot conversion pays it; how much in a browser is unverified.
- **Not measured:** conversion in of BigInt, Set, Map, Date and Temporal
  values; strings past ASCII; a browser; and the code size of the C
  layer and of the requests.

## Costs

| Who | Pays |
|---|---|
| per evaluation | the C API's work, plus about 2 to 4 wasm calls per element crossed; one native value per function in the globals (an allocation, unmeasured) |
| per context | one `sjs_vm` (101,616 B with the default limits; 40,176 B with `maxStack: 256`), the C layer's tables, one handle stack shared by the instance |
| per package | the module (969,106 B, 316,388 B gzipped, before the C layer), the C layer (~400 lines, estimated), the JavaScript (~600 lines, estimated) |
| API surface | `load`, the interfaces `StaticJS`, `Context`, `Program`, `Run`, `Value`, `Frame`, `FunctionSummary`, the class `StaticJSError` |
| the release | a second wasm artifact, `staticjs-js.wasm`, or the npm package alone (decision 1) |
| staticjs's library | R1 to R6: two struct fields, a vtable entry, about six functions, their tests and their documentation in `staticjs.h` and the guide |

## Alternatives, and why not

- **Not JSON both ways,** because it changes values: BigInt throws in
  `JSON.stringify`; `-0`, `NaN`, `undefined`, Map and Set are lost. A
  binding refuses what it cannot carry and never turns it into something
  else (`tinyactors-napi/AGENTS.md`, "Values").
- **Not the writer's text out,** because it is JavaScript source. Reading
  it back needs `eval` or a parser of our own, and it was the slowest path
  measured.
- **Not struct layouts in JavaScript** (the example wrapper's way),
  because of F8. Layouts would ship as data (`staticjs.api.json`) that
  must match the module byte for byte. Speed does not decide it either
  way: with the same ASCII loop, layouts took 334 to 378 ns per row, which
  is between handles with the key cache and handles without it.
- **Not a tape (a binary codec) for every value.** With the same tricks
  it was no faster out (218 to 286 against 187 to 197 ns per row; even on
  BigInt, Set and Map rows). In, it showed no consistent gap (481 to 638
  against 577 to 755 ns per row). It is a format kept in two languages,
  the second codec that `.agents/skills/boundary-design/SKILL.md:75`
  rejects. This is **plausible**, not settled: the out difference is
  within the noise of a shared machine. Rerunning `fair.mjs` on an idle
  machine, plus an input workload with BigInt, Map and Set, would settle
  it.
- **Not helper programs on the context's evaluator** (the first draft's
  way for Dates, Temporal values and calls). A helper started while a
  `Run` was paused threw that run away (F11). One started while
  converting a native's result ran inside `sjs_vm_run` (F5). A second
  evaluator for helpers would avoid both, at 101,616 B per context with
  the default limits, but it keeps the helpers' compile per context and
  their reparse. Requests R1 and R2 cost the library less.
- **Not emnapi** (Node-API emulated in wasm, which the root roadmap leaves
  open for tinyactors' browser package, `ROADMAP.md:840-845`), because
  staticjs has no Node-API addon to reuse, and its gain is unverified. Its
  JavaScript runtime is 156,069 B minified, or 31,716 B with `gzip -9`
  (`@emnapi/core` 1.11.3, `emnapi-core.min.mjs`: 134,780 B; and
  `@emnapi/runtime` 1.11.3, `emnapi.min.mjs`: 21,289 B; measured from the
  npm tarballs). The C it adds to the module is not measured. Each object
  it makes is still a call from wasm into JavaScript (`napi_create_*`), as
  many crossings as handles, so a speed gain is unverified and not
  expected. Decision 5.
- **Not lazy `Proxy` values by default,** because a property read through
  a proxy costs a trap and calls per read, every time, while an eager copy
  costs 187 to 282 ns per row once. `Value` covers the case where data
  should stay in staticjs.
- **Not a Promise API,** because a run is synchronous and bounded by fuel;
  `start` and `step` let a caller yield between slices however its
  runtime does (`scheduler.yield`, `setImmediate`).
- **Not a callback clock,** because the library reads no clock
  (`staticjs.h`, the host clock) and a call per read would make a program's
  result depend on when it reads.
- **Not a Temporal fallback to the ISO string,** where the runtime has no
  `Temporal`. It turns a value into something else, which the package
  refuses everywhere else (decision 2).

## The API map

Each item and the C API behind it, as `tinyactors-napi/docs/api-map.md`
does for core. The C layer's `sjs_js_*` functions call these. Rows marked
R*n* need that request.

| Item | staticjs |
|---|---|
| `load` | `sjs_version`, `sjs_wasm_allocator`; the imports `staticjs_js.native` and `staticjs_js.release` |
| `StaticJS.version` | `sjs_version` |
| `createContext` | `sjs_context_init`, `sjs_vm_init` |
| `Context.value` | conversion in (below), `sjs_retain` |
| `Context.dispose` | `sjs_vm_reset`, `sjs_release` and `sjs_program_release` for every row, `sjs_vm_destroy`, `sjs_context_destroy` |
| `Context.liveBytes` | `sjs_context_live_bytes` |
| `Context.setClock`, `advanceClock` | `sjs_vm_set_clock`, `sjs_vm_advance_clock` |
| `Context.setSeed` | `sjs_vm_set_seed` |
| `Context.compile` | `sjs_compile` with R6; until then `sjs_parser_init`, `sjs_parser_push`, `sjs_parser_finish`, `sjs_parser_error`, `sjs_compiler_init`, `sjs_compiler_sink`, `sjs_compiler_finish`, `sjs_compiler_error`, the destroy calls; `modules`: an `sjs_modules_vtable` in C |
| `Program.run`, `runValue` | `sjs_vm_start`, `sjs_vm_run`, `sjs_vm_take_result`, `sjs_vm_error`, `sjs_vm_reset` |
| `Program.start`, `Run.step`, `fuelUsed`, `cancel` | `sjs_vm_start`, `sjs_vm_run`, `sjs_vm_fuel_used`, `sjs_vm_reset`, `sjs_retain` and `sjs_release` for the run's globals |
| `Run.result` | `sjs_vm_take_result`, conversion out |
| `Run.frames` | `sjs_vm_frame_count`, `sjs_vm_frame` |
| `Program.functions` | `sjs_program_function_count`, `sjs_program_function` |
| `Program.dispose` | `sjs_program_release` |
| `StaticJSError` | `sjs_vm_error`, `sjs_compiler_error`, `sjs_parser_error` (or `sjs_compile_params.error` with R6); the native's thrown error as `cause` |
| a function in `globals` (the native import) | `sjs_function_native`, `sjs_native_vtable.call`; `.release` (R5) |
| a function out, called | `sjs_vm_start` with `sjs_run_params.function` (R1), `sjs_vm_run`, `sjs_vm_take_result` |
| conversion in | `sjs_string_new`, `sjs_bigint_new`, `sjs_array_new`, `sjs_array_append`, `sjs_object_new`, `sjs_object_put`, `sjs_set_new`, `sjs_set_add`, `sjs_map_new`, `sjs_map_set`, `sjs_number`, `sjs_boolean`, `sjs_null`, `sjs_undefined`, `sjs_function_native`, `sjs_time_new` (R2) |
| conversion out | `sjs_length`, `sjs_array_at`, `sjs_object_at`, `sjs_set_at`, `sjs_map_size`, `sjs_map_at`, `sjs_string_text`, `sjs_bigint_text_bound`, `sjs_bigint_text`, `sjs_iterator_at` (R3), `sjs_time_read` (R2), `sjs_regexp_source` (R4) |
| `Value`, its `dispose` | `sjs_retain`, `sjs_release` |
| `Value.toString` | `sjs_writer_init`, `sjs_writer_write`, `sjs_writer_destroy` |

Not exposed: `sjs_vm_clock`, `sjs_vm_random` and `sjs_vm_set_random`
(saving a paused run elsewhere, which this package does not do); the event
stream to a sink of JavaScript's (a formatter or a linter in JavaScript
would need an event per token across the boundary; not asked for);
`sjs_map_get`, `sjs_object_get` (a conversion reads every entry);
`no_shapes`, `no_regex_leaf`, `no_regex_dfa` (for staticjs's own tests).

## Decisions for the maintainer

1. **Where natives' imports live.** Either the npm package ships its own
   module (the amalgamation, the C layer and two imports), and the
   release's `staticjs.wasm` keeps importing nothing (recommended: the raw
   module's contract stays as released). Or one module imports
   `staticjs_js.native` and `staticjs_js.release`, and raw users pass
   stubs.
2. **Temporal out, where the runtime has no `Temporal`** (Node 22, most
   browsers today). The options:
   - refuse with a `TypeError` that names the type; `Value.toString()`
     still gives the writer's text (recommended: nothing is turned into
     something else);
   - accept a `Temporal` implementation the application passes to `load`
     (a polyfill of its choosing);
   - the first draft's ISO string, which the review counted as a
     conversion into something else.
3. **One evaluator per context.** `start` holds it until the run is done
   or cancelled. While a `Run` holds it, `run`, `start` and calls of a
   function out on that context throw a `TypeError` (recommended: nothing
   is lost silently, but a `Run` abandoned without `cancel` blocks its
   context until `dispose`). The alternatives:
   - a later `start` abandons the paused run, as `sjs_vm_start` does in C
     (F11), and the abandoned `Run`'s next `step` throws;
   - `context.evaluator()` for several runs at once, at 101,616 B each
     with the default limits.
4. **The requests R1 to R6,** together or one by one. Each declined
   request has the fallback "Requests to staticjs" names.
5. **emnapi.** Not now (recommended): staticjs has no Node-API addon to
   reuse, and emnapi would add 31,716 B gzipped of runtime and C of
   unmeasured size for no measured gain. Revisit with tinyactors' own
   browser decision (`ROADMAP.md:840-845`): if that package uses emnapi
   anyway, a shared runtime changes the sum.

## The review, answered

| Finding | Answer |
|---|---|
| 1. Helpers on the single evaluator | R1 and R2 remove them; the fallbacks refuse while running or paused (Requests; decision 3) |
| 2. Iterators have no route out | F7 lists them; R3; refused without it |
| 3. The key cache keyed by pointer | per conversion, while the root is retained (How it crosses; `keyreuse.mjs`) |
| 4. Handles faster than the tape | rerun fairly (`fair.mjs`); restated as even, chosen for having no format; marked plausible (Measurements; Alternatives) |
| 5. Headline numbers | 200 to 215 ns per guard with conversions; 2.5 to 5.9 times; "every kind it converts", with refusals named; BigInt, Set and Map measured out, not in |
| 6. Requests to staticjs | the section of that name; the status line no longer says the library is unchanged |
| 7. Natives leak | R5, or scoped to the run with the cost stated (Native functions) |
| 8. `dispose` with live objects | releases every row, bumps the generation, refuses while running; finalizers touch tables only (Lifetimes) |
| 9. Paused runs' globals on a shared stack | the `Run` owns them (Lifetimes) |
| 10. User code during conversion in | running flag, intrinsics, a builder without state (Values) |
| 11. `FinalizationRegistry` | F17; a fallback only; a `Run` holds what it reads |
| 12. Repository rules | the exports' exemption named, text in through owned scratch, `_compile_module` takes a name length; no fuel default and no `fuel` kind; no ISO fallback (decision 2); holes refused; the rejected "language layer" idea and F18 addressed; emnapi measured in part, decision 5 |
| 13. Temporal input | the five types in the ISO calendar; the rest refused |
| 14. Invalid Dates out | `new Date(NaN)` (R2's time value) |
| 17. RegExp in, holes | both refused (Values) |
| 18. F8 and F9 evidence | cited as given, F8 marked an analogy |
| 19. KiB and timing ranges | bytes and KiB corrected; compile 2.5 to 7.2 ms, instantiate 0.07 to 0.77 ms |
| 20. Details | class count, the lifetimes reference, the API map rows, `_set_clock` packing, `using`, the 64-byte subject, shared parts growing: each fixed where it appears |

The new claims in this revision have not yet been refuted by a fresh
agent (reasoned-analysis, step 8). Until they are, they count as
plausible.

## If adopted

1. The requests R1 to R6 in staticjs, each with its tests and its
   documentation in `staticjs.h` and the embedding guide, before the
   package depends on them.
2. `staticjs-js/` with `AGENTS.md` (naming the exports' exemption from
   `buffers-are-ta-buffer`), `docs/api-map.md` (checked as napi's is) and
   the C layer, built with
   `Toolchain::Target.release('wasm32-freestanding')` from staticjs's
   amalgamation.
3. The value conversions, with tests that:
   - every row of the values table round-trips;
   - every refusal throws;
   - a getter or Proxy that reenters the context is refused;
   - `dispose` with live objects releases everything (`liveBytes` back to
     zero).

   Run the spec cases that expect a value through the package, so the
   package agrees with `diff-node`.
4. `load` for Node (`fs`) and browsers (`instantiateStreaming`), and a
   test in Chromium with the Playwright the environment has.
5. Packaging (the package's module, decision 1) and its changelog.
6. Replace `staticjs/examples/wasm/` with an example of the package, and
   `staticjs/docs/guide/wasm.md` with its guide.
