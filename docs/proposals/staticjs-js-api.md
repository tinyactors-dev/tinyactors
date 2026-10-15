# A JavaScript API for staticjs.wasm

**Status: Adopted → [ADR 0013](../adr/0013-staticjs-javascript-package.md)**
(2026-10-15), which records the maintainer's decisions and adds request
R8 (values read from and written as JSON and CBOR). Touches: a new
package, `staticjs-js/` (`@tinyactors/staticjs`), with a C layer of its
own; staticjs's release packaging; additions to staticjs's C API.

The maintainer approved the direction (2026-10-14). This revision answers a second adversarial review; "The review, answered" lists each finding of both reviews and where it went.

## Summary

A package that evaluates staticjs from JavaScript, in Node and in browsers,
with JavaScript values in and out:

```js
import { load } from '@tinyactors/staticjs';

const staticjs = await load();
const context = staticjs.createContext({ conversion: { maxItems: 100_000 } });
context.setClock({ epochMilliseconds: Date.now() });

const guard = context.compile('In("heating") && event.data.f > limit', { globals: ['In', 'event', 'limit'] });
const run = guard.start({ In: (id) => active.has(id), event, limit: 25 });
if (run.step(10_000)) console.log(run.result());   // true
else console.log(run.frames());                    // where it stopped; run.step(more) resumes

run.cancel();
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

- the guard above costs 615 to 689 ns per evaluation in a spike of the C
  layer, counting the conversion of its input, the native `In` and the
  result; 382 to 394 ns without the native. These are the spike's numbers,
  not the package's;
- a result of 10,000 small objects reaches JavaScript in 1.9 to 3.0 ms
  (187 to 303 ns per row), 2.0 to 5.9 times faster than the writer's text
  plus `JSON.parse` across the runs recorded.

Every kind the package converts arrives exactly: BigInt, Map, Set,
`undefined`, `-0`, and an invalid Date. It refuses what it cannot carry
and never turns it into something else. In: a RegExp, a Temporal type
staticjs lacks, an array with holes. Out: a Temporal value where the
runtime has no `Temporal`. A conversion either way stops at a total
number of items the caller sets, because neither fuel nor staticjs's
limits bound it.

The package needs seven small additions to staticjs's C API. Each one also
serves C embedders. Calling a function value (R1) needed a staticjs bug
fixed first, and now is: a closure read the globals of whatever run called
it, not of the run that made it (F19).

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
  can free what they hold, compile errors that keep their subject, and
  frames that say which function they are in.
- **Then: a future browser host of tinyactors with the staticjs data
  model.** Its state would live in staticjs values that never cross into
  JavaScript (`Value` handles, below).
- **Who pays:**
  - per evaluation: the C API's own work, plus 2 to 4 wasm calls per
    element converted. Each function in the globals adds one native value
    per run: 40 to 80 ns to make and free it, measured with the natives'
    table as an array (`gn2.mjs`), and 170 to 285 ns with a `Map`
    (`gn.mjs`);
  - per context: one evaluator, 101,616 B (99.2 KiB) with the default
    limits or 40,176 B (39.2 KiB) with `maxStack: 256` (spike
    `vmsize2.mjs`, against the module built after the inspection commit),
    plus the C layer's tables of rows;
  - per package: about 1 MB of wasm (970,907 B; 318,221 B with
    `gzip -9`, before the C layer) and about 1,000 lines of JavaScript and
    C (an estimate);
  - API surface: `load`; the interfaces `StaticJS`, `Context`, `Program`,
    `Run`, `Value`, `Frame` and `FunctionSummary`; the class
    `StaticJSError`;
  - staticjs's library: seven additions (fields in three structs, a
    vtable entry, and about seven functions; "Requests to staticjs"), with
    their tests and documentation. The closure fix R1 waits on is
    staticjs's own bug and is costed there;
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
| F1 | `staticjs.wasm` imports nothing and exports the C API and its memory, nothing else: no function table. The header declares 69 `SJS_API` functions, the inspection calls included | spike `sz.mjs` on the module built from the current amalgamation with `CRelease.build_wasm` into the scratchpad: 69 functions and `memory`, 0 imports; `grep -c '^SJS_API' staticjs/include/staticjs.h` gives 69. The module in `staticjs/build/wasm/` (65 functions) and the spikes' `spike.wasm` predate commit `cb90b10` |
| F2 | The wasm32 C ABI passes a struct by value as a pointer to a copy, returns one through a pointer given first, and passes a 64-bit integer as a BigInt | `staticjs/docs/guide/wasm.md`; `staticjs/tests/wasm/release_test.mjs` |
| F3 | A context, and everything made with it, is used by one thread at a time. A JavaScript realm is one thread, and a wasm instance belongs to one realm | `staticjs.h:187` |
| F4 | Values are reference counted, and immutable once shared. Copying the struct does not retain it. `sjs_release` does not recurse. A run cannot change a global the embedder also holds | `staticjs.h:185`, `:225`, `:245`; spike `mutate.mjs`: `p.push(2)` on such a global faults ("staticjs changes a value only while one variable holds it"), the global stays `[1]` |
| F5 | A native function is a C vtable called synchronously from inside `sjs_vm_run`, which takes its result there. The module's C can call JavaScript only through an import (F1). Node 22 has no `WebAssembly.Function` to make a wasm function from a JavaScript one | `staticjs.h:313-333`; `src/vm.c:847-863`; spike: `typeof WebAssembly.Function` is `undefined` |
| F6 | A modules adapter (`sjs_modules_vtable.find`) is called during compilation and answers with exports the compiler borrows while it runs | `staticjs.h`, "Compiler and programs" |
| F7 | The value API reads arrays, objects, sets, maps, strings, numbers, booleans and BigInts (as text). It has nothing that reads or makes a Temporal value, a Date or a regular expression, and nothing that reads an iterator's values: `sjs_length` gives 0 for an iterator. It cannot call a staticjs function from C. Only the writer reads those kinds, and it writes them as JavaScript source | `staticjs.h`, "Context and values" and "Writer"; `src/value.c:631-643`; `src/writer.c:106-138` |
| F8 | This repository keeps C structs out of the languages: "Only C touches host structs". The rule is written for tinyactors' host structs; applying it to staticjs's structs is an analogy with the same reason (layouts copied into another language must match the module byte for byte) | `tinyactors-core/docs/bindings.md:265`; `tinyactors-core/docs/boundary.md:270-272` |
| F9 | A binding's direct layer is C, mirroring the C API, with a language layer above it. Scratch shared between calls is not reentrant, so a call takes a mark and releases to it | `tinyactors-napi/AGENTS.md`, "Layers and the resolver" and "The binding's memory"; napi commit `8d600f4c`, which is not in this clone and is cited by `tinyactors-core/docs/boundary.md:532` and `.agents/skills/boundary-design/SKILL.md:78` |
| F10 | A call from JavaScript into wasm costs about 3 ns, and one from wasm into JavaScript 3.5 to 5.8 ns | ADR 0010:59 (2.9 ns); spikes below |
| F11 | A context has one evaluator per `sjs_vm_init`, and `sjs_vm_start` resets it: starting a program abandons the evaluation it held. `sjs_vm_reset` frees the frames, so a reset from inside `sjs_vm_run` (from a native) frees the frames being executed | `src/vm.c:1600-1628` |
| F12 | `sjs_native` has no release hook. The library stores the vtable and `self` in the function value and calls nothing when it frees them. `self` is a `void *`, 32 bits on wasm32, which the library never dereferences | `staticjs.h:326-333`; `src/value.c:687-694`, and `drain`'s default case (`src/value.c:77-115`) |
| F13 | `sjs_compile` destroys the parser and compiler its error's `subject` points into before it returns. The example works around this by reporting a compile error without its subject | `src/compiler.c:3512-3532`; `staticjs.h:99-103`; `examples/eval.c:56-63` |
| F14 | An error's subject is cut to 64 bytes: the stage keeps it in a fixed slot | `src/internal.h:1320-1324`; `src/vm.c:34`, `:851` |
| F15 | staticjs can inspect a paused or faulted evaluation (its frames: kind, position, built-in, element progress) and a program (its functions: position, parameters, shape), on demand. A frame carries a position but not which program or function it is in | `staticjs.h:761-820`, "Inspection"; `src/inspect.c:47` (a faulted run's frames count) |
| F16 | staticjs's library can be changed: the maintainer invited debugging and observability additions to it, and every caller of it is in this repository | the maintainer, 2026-10-14; root `AGENTS.md`, "The libraries are private" |
| F17 | A `FinalizationRegistry` callback may never run, and when it runs, it runs as a job between synchronous calls: never inside a call (a native included), but possibly between two steps of a paused run | ECMA-262, `HostEnqueueFinalizationRegistryCleanupJob`; MDN, [FinalizationRegistry](https://developer.mozilla.org/en-US/docs/Web/JavaScript/Reference/Global_Objects/FinalizationRegistry), "Avoid where possible" (a conforming implementation is not required to call cleanup callbacks) |
| F18 | A wasm module's C cannot make JavaScript objects; something in JavaScript, or an emulated Node-API, must | `ROADMAP.md:840-845` |
| F19 | A closure reads a global through `SJS_OP_GLOBAL`, which indexes the globals of the run executing now, unchecked. A closure made in one run and called in another reads the other run's globals, or past their end. This was a staticjs bug, now fixed: a closure holds the globals of the run that made it (as an array, made by the run's first closure) and reads those; `tests/api_test.c` `test_closure_globals` covers both spikes | `src/compiler.c:1321-1323`; `src/vm.c:1180-1185`; spike `p.mjs`: `(x) => x + limit` made with `limit` 100 and called in a later run with `limit` 5 gives 6, where JavaScript gives 101; spike `g.mjs`: called from a program with one global, it reads index 1 past the array and faults by chance ("staticjs adds two numbers, or a string and a primitive") |
| F20 | Fuel bounds the instructions of a run, not the size of its result. Values share parts by reference, so a run can return a DAG whose tree is exponentially larger, or a value nested to any depth | spike `s.mjs`: `Array.from({ length: 16 }).reduce((a) => [a, a], 0)` uses 140 fuel, and the writer turns it into 262,141 bytes; spike `d.mjs`: a run returns an array nested 200,000 deep, and a recursive conversion of that depth throws "RangeError: Maximum call stack size exceeded" |

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
- **The C API itself** lacks what the package needs. Without the
  additions, the binding would have to work around each gap. The first
  draft did this, and the first review showed what it cost:
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
  createContext(options: ContextOptions): Context;
}

export interface ContextOptions {
  limits?: Partial<Limits>;                       // over SJS_LIMITS_DEFAULT; names as in sjs_limits, camelCase
  conversion: { maxItems: number };               // required: items one conversion may make, in or out (Values)
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

export interface Program {
  readonly globals: readonly string[];
  functions(): FunctionSummary[];                 // sjs_program_function_count, sjs_program_function
  start(globals?: Record<string, unknown>): Run;  // abandons a paused run of this context (decision 3)
  dispose(): void;                                // also [Symbol.dispose]
}

export interface Run {
  step(fuel: number | bigint): boolean;           // true when done; false when the fuel is spent; throws StaticJSError on a fault
  readonly fuelUsed: bigint;                      // sjs_vm_fuel_used
  frames(): Frame[];                              // where a paused run is: sjs_vm_frame_count, sjs_vm_frame
  result(): unknown;                              // once done: the value, as JavaScript
  resultValue(): Value;                           // once done: the value, kept in staticjs
  cancel(): void;                                 // sjs_vm_reset; nothing once ended; also dispose(), [Symbol.dispose]
}

export interface Frame {                          // sjs_frame
  readonly kind: 'function' | 'loop';
  readonly line: number; readonly column: number; readonly offset: number;
  readonly object: string; readonly name: string; // a loop's built-in: "" and "map", "Array" and "from"
  readonly index: number; readonly length: number;// a loop's progress; 0 and 0 otherwise
  readonly program?: Program;                     // R7: the program the frame's code is in, while not disposed
  readonly function?: number;                     // R7: its index in that program's functions()
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
  start(args: readonly unknown[]): Run;           // a function: call it (R1), stepped like a program's run
  dispose(): void;                                // also [Symbol.dispose]
}

export class StaticJSError extends Error {
  readonly kind: 'syntax' | 'unsupported' | 'name' | 'limit' | 'memory' | 'fault';   // sjs_error_code, nothing added
  readonly line: number;                          // from 1
  readonly column: number;                        // from 1, in UTF-8 bytes, as sjs_position
  readonly offset: number;
  readonly subject: string;                       // at most 64 bytes (F14)
  readonly reason: string;                        // the library's message
  readonly frames: readonly Frame[];              // a run's fault: where it faulted (F15); [] for a compile error
  // `cause`: the error a native function threw, whole, when one did
}
```

`evaluate` from the first draft is dropped: it was `compile`, `run` and
`dispose` in one call. `Program.run` and `runValue` from the first
revision are dropped for the same reason: each was `start`, `step` and
`result` (or `cancel`) in one call. When the fuel ran out they reset the
evaluator, which threw away the frames that `frames()` exists to show, and
threw a `RangeError` with a message of the package's own, against
`tinyactors-napi/AGENTS.md:370-373` ("No defaults, limits, methods, events
or messages of our own"). A paused `Run` keeps its frames until it is
stepped, cancelled or abandoned.

Choices, each against what JavaScript users expect:

- **Globals by name.** `compile` takes the names, as `sjs_compiler_config`
  does; `start` takes an object keyed by them. A missing name is a
  `TypeError` before the run; an extra key is ignored.
- **`fuel` is given to each step**, as `sjs_vm_run` takes it. There is no
  default, per context or per function. `step(fuel)` returns `false` when
  the fuel is spent, as `SJS_RUN_PAUSED` says, and leaves the run paused.
  A number crosses as an f64 to an export that converts it to `uint64_t`
  (exact to 2^53), so no BigInt is made per call; a bigint crosses to an
  `i64` export. The spike measured no difference between the two
  (`gn.mjs`, `gn2.mjs`: 615 to 937 ns per guard either way).
- **A fault** throws a `StaticJSError` whose `frames` say where the run
  faulted: staticjs keeps a faulted run's frames until the next start or
  reset (`staticjs.h:796-798`; `src/inspect.c:47`). The run has then
  ended.
- **The clock and the seed are the host's** (`sjs_vm_set_clock`,
  `sjs_vm_set_seed`). Nothing sets them for you, as the C API does
  nothing for you, so a program that reads an unset one faults.
  `setClock` takes anything with `epochMilliseconds` or
  `epochNanoseconds`, so a `Temporal.Instant` works directly.
- **The conversion budget is the caller's,** like fuel. It bounds the
  package's own work, which fuel does not (F20). It is given once per
  context, not per call, because a native's arguments and result are
  converted inside a run, where no call of the caller's could carry it.
  This is a limit the package adds; the alternative is no bound at all.
- **Disposal is explicit,** with `dispose()`, and `Symbol.dispose` for
  `using` where the runtime has it. A `FinalizationRegistry` is a fallback
  only. Its callbacks may never run (F17). Nothing a live run reads
  depends on one: a `Run` holds its `Program` and the `Value`s it
  borrows, both as JavaScript references and as references in C
  (`sjs_vm_start` retains the program, `src/vm.c:1628`; the C layer
  retains each borrowed global). A callback releases the C row of an
  object that has become unreachable, after checking the row's context
  and generation (Lifetimes). For a `Run` that still holds the evaluator,
  that release resets the evaluator (decision 3).
- **Inspection** comes from staticjs (F15). `Run.frames()` says where a
  paused run is, down to the element a `map` has reached, so an operator
  can say where a program ran out of fuel; `StaticJSError.frames` says
  where it faulted. `Program.functions()` lists the functions and the
  shapes the compiler found. All read on demand, so an evaluation pays
  nothing when they are not called.
- **Names** come from familiar APIs, with this library's meaning:
  `compile` as in `node:vm`'s `Script`; `start` and `step` as the C API
  names them; `dispose` as in quickjs-emscripten's handles and TC39's
  explicit resource management; `epochMilliseconds` and
  `epochNanoseconds` as in Temporal.

## Values

| JavaScript → staticjs | | staticjs → JavaScript | |
|---|---|---|---|
| number, string, boolean, `null`, `undefined`, bigint | the same | number (`-0` and `NaN` kept), string, boolean, `null`, `undefined`, BigInt | the same |
| `Array` without holes | array | array | `Array` |
| a plain object (prototype `Object.prototype` or `null`): own enumerable string keys, read with `[[Get]]` | object, keys in JavaScript's order | object | a plain object |
| `Set` of primitives, `Map` with primitive keys | set, map | set, map | `Set`, `Map` |
| a function | a native function (below) | a function | a `Value` of kind `'function'`, which the caller disposes; `start(args)` calls it (R1) |
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
- a `Value` of another context, or one already disposed;
- while R1 is not in staticjs: a function out. Its `Value` can be kept and
  passed back in, but `start` throws, as there is no call to make it with.

**Every conversion is bounded and iterative.** Both directions walk the
value with an explicit stack in JavaScript, not by recursion, so depth
costs memory, not the call stack (F20: a recursive walk overflows at a
depth a program reaches for free). Each conversion counts the items it
makes, every element, entry, key and scalar, and stops at the context's
`conversion.maxItems` with a `RangeError` that names the budget, releasing
what it made. In, the count also stops a cycle and input whose shared
parts unfold to a large tree: staticjs has no identity (`staticjs.h:290`),
so an object that appears twice is converted twice. Out, it stops a DAG
(F20, `s.mjs`: 140 fuel, 262,141 bytes of text). `Value.toString()` writes
in chunks (`sjs_writer_write` returns `SJS_NEED_SPACE` when its buffer is
full, `staticjs.h:852-855`) and stops at the context's `maxStringBytes`,
the limit staticjs puts on one string, since the result is one string.
The first revision refused input nested deeper than `maxNesting`; that
limit counts brackets and blocks in source (`staticjs.h:131`), not the
depth of a value, so the budget replaces it.

**User code runs during conversion in.** A getter runs during `[[Get]]`. A
`Proxy` passes the plain-object and `Array.isArray` checks and runs its
traps (spike: `Object.getPrototypeOf(new Proxy({}, {}))` is
`Object.prototype`, and `Array.isArray(new Proxy([], {}))` is `true`). A
patched `Map.prototype` or `Set.prototype` would run too, so the package
calls the intrinsics it captured at `load` instead of looking methods up
on the value. Conversion in is guarded like a run:

- the context's running flag is set for the whole conversion, so a getter
  that calls the same context gets a `TypeError` (Reentrancy);
- the builder keeps no state between calls: each container is a handle,
  and each append names its container (How it crosses);
- everything a conversion makes sits above the call's mark, so a getter
  that uses another context in the same instance allocates above it and
  gives back only its own.

Conversion out runs no user code: it calls the intrinsics captured at
`load` (`Map`, `Set`, `Date`, `RegExp`, the `Temporal` constructors).

**A function out** stays in staticjs, as a `Value` of kind `'function'`,
wherever it appears in a result: `toJS()` of `{ net: f }` gives
`{ net: <Value> }`. The caller disposes it, or the context's `dispose`
does. `value.start(args)` converts the arguments in and starts the call on
the context's evaluator (R1). It returns a `Run`, stepped with explicit
fuel like a program's. Nothing is inherited from the run that made the
function: not its fuel, and not its pause. The first revision gave each
call the fuel of the run that returned the function, which was a default
of the package's own. A function out keeps its program alive after
`Program.dispose` (spike `callf.mjs`: a closure called after its defining
program was released gives 43; it reads a captured variable, no global).
Until R1 lands, `start` on a function throws a `TypeError`.

**Native functions.** For each JavaScript function in the globals of
`start`, or in `context.value({ f })`, the C layer makes one `sjs_native`
value. Its `self` is the 32-bit number `slot | generation << 16`, never
dereferenced (F12), and the package keeps the function in an array at
that slot. No record is allocated in C. The function is called
synchronously during the run, through the import `native(self, base,
count)`: `base` is the handle of the first argument on the C layer's
stack, so JavaScript reads the arguments with the same accessors as any
conversion out, never a C pointer. Its return value is converted in.

- **The import catches every exception,** the function's and the
  conversions' (a refused argument, a spent budget). An exception that
  unwound through `sjs_vm_run`'s wasm frames would leave the evaluator in
  the middle of an instruction. A caught throw becomes a fault: the
  message, cut to 64 bytes, is the fault's subject (F14), and the whole
  thrown error is the `StaticJSError`'s `cause`.
- A Date or Temporal value returned is made without running anything on
  the evaluator (request R2).
- As in C, a native must not change anything the program can see; the
  package cannot check that.

**A native's scope.** Without R5, a native lives as long as what made it:
a run's natives until the run ends (done, a fault, `cancel`, abandoned by
a later `start`), a `Value`'s until it is disposed. At the end the slot
is cleared and its generation bumped, so a native that escaped (the
program returned it, or stored it in a result) faults when called
("a native function of an ended run"). A slot whose 16-bit generation
would wrap is retired, never reused, so a stale `self` never names a
later function: one slot of 8 bytes is lost per 65,536 uses of it.
Nothing else grows with the number of runs. With R5, the slot is cleared
when staticjs frees the function value, so a native that escaped keeps
working while something holds it.

**Reentrancy.** The C layer keeps a running flag per context, set during a
step, a conversion in, and a native's call. While it is set, every call
that touches the context's evaluator, makes or frees its objects, or
changes what a run reads is refused with a `TypeError` ("the context is
running"):

- `compile`, `value`, `Program.start`, `Value.start`, every `dispose`;
- `Run.step`, `cancel`, `result`, `resultValue`, `frames`: `sjs_vm_reset`
  from inside `sjs_vm_run` would free the frames being executed (F11);
- `setClock`, `advanceClock`, `setSeed`: staticjs promises that two reads
  of the clock with no call between them agree, "as a run calls nothing
  here" (`staticjs.h:703-705`).

Reading another `Value` (`toJS`, `toString`, `kind`), `liveBytes` and
`fuelUsed` are allowed. Another context in the same instance may be used:
every call of the C layer takes a mark on its handle stack and scratch at
entry and releases to it at exit, so a nested call never overwrites an
outer one's (F9). A paused `Run` keeps nothing on that stack (Lifetimes).

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

**Bytes, checked by lint.** The C layer is linted with napi's rules over
all of its C, exports included: `at-most-three-parameters` and
`buffers-are-ta-buffer` (`tinyactors-napi/tools/lint/rules/`), adapted to
staticjs's view types `sjs_str` and `sjs_buffer`. That rule matches a
pointer and a length passed together, or a returned pointer with a length
out-parameter (`tinyactors-core/tools/lint/rules/buffers-are-ta-buffer.yml`).
The exports as sketched match neither, so they need no exemption:

- **In:** no pointer is passed. JavaScript asks for room (`_scratch(n)`
  returns an offset into scratch the C layer owns, above the call's
  mark), writes UTF-8 there with `encodeInto`, and passes the length
  only. `_string(len)`, `_key(len)` and `_compile_global(c, len)` read
  their own scratch.
- **Out:** an offset from one call and a length from the next, as a view
  whose owner is stated (the value, borrowed while the conversion's root
  is retained).

If an export ever has to match the pattern, it is excluded by its export
attribute in a rule file beside the others, with its reason there, as
`tinyactors-napi/tools/lint/utils/fixed-signature.yml` lists the
signatures a typedef fixes. The first revision argued an exemption by
analogy with a typedef; nothing fixes an export's signature that way.

Sketch:

| Group | Functions |
|---|---|
| contexts | `sjs_js_context_new(limits)`, `_context_free(ctx)`, `_set_clock(ctx, seconds, nanoseconds)` (seconds as an f64, exact: Temporal's limit is 8.64 × 10^12 s, under 2^53), `_advance_clock(ctx, high, low)` (nanoseconds as two u32 halves, so no BigInt is made), `_set_seed(ctx, seed)` |
| compile | `_compile_begin(ctx)`, `_compile_global(c, name_len)`, `_compile_module(c, name_len, value)`, `_compile_end(c, goal, source_len)` → a program, or 0 and the error |
| run | `_run_begin(program)` or `_call_begin(ctx, function)` (R1), `_run_global(r, value)` or `_call_argument(r, value)`, `_step(r, fuel)` (an f64) and `_step_big(r, fuel)` (an i64) → status, `_run_take(r)` → value, `_frame_count(r)`, `_frame(r, i)` with field reads (also after a fault) |
| reading a value | `_kind(v)`, `_number(v)`, `_boolean(v)`, `_length(v)`, `_at(v, i)`, `_entry(v, i)` then `_key_ptr()` and `_key_len()`, `_string_ptr(v)` then `_string_len()`, `_bigint_text(v)` then `_text_ptr()` and `_text_len()`, `_iterator_count(v)` and `_iterator_at(v, i)` (R3), `_time_read(v)` then `_time_field(i)` (R2), `_regexp_source(v)` and `_regexp_flags(v)` (R4), `_write(v, cap)` (the writer, for `Value.toString`) |
| building a value | `_array_new(n)`, `_array_append(a, v)`, `_object_new(n)`, `_object_put(o, key_len, v)`, `_set_new(n)`, `_set_add(s, v)`, `_map_new(n)`, `_map_set(m, k, v)`, `_number(d)`, `_string(len)`, `_bigint(len)`, `_boolean(b)`, `_null()`, `_undefined()`, `_time_begin(type)`, `_time_put(i, d)`, `_time_end()` (R2), `_native(self)` |
| handles | `_keep(v)` → a persistent row and its generation; `_drop(ctx, row, generation)`; `_mark()`, `_release_to(mark)` |
| errors | `_error_code()`, `_error_line()`, `_error_column()`, `_error_offset()`, `_error_message_ptr/len()`, `_error_subject_ptr/len()` |
| imports | `staticjs_js.native(self, base, count)` → the result's handle, or a fault; `staticjs_js.release(self)` (R5), which only clears a slot |

- **Values are handles:** indices into a stack of `sjs_value` slots in C.
  A slot is borrowed from its container and released to a mark. A
  `Value` is a row in a separate table, with a generation, and is
  retained until disposed. This is the boundary skill's "values are
  handles, metadata are views".
- **Generations are unique per instance.** One counter per instance
  numbers every row of every context, and crosses as an f64 (exact to
  2^53, so it does not wrap in practice). A context is itself a row with
  a generation. `_drop` names the context's row and generation as well as
  its own, so a finalizer that runs after its context was disposed, and
  after a new context was made at the same address, finds the generation
  changed and does nothing.
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
| `sjs_context`, its `sjs_vm`, the context's tables (Values, Programs, Runs) | `Context` | `createContext` | its calls | `dispose`: it resets a paused run, releases every row of every table, clears its natives' slots, then calls `sjs_vm_destroy` and `sjs_context_destroy`, which needs everything gone first (`staticjs.h:198`). The context's row gets a new generation, so each wrapper's next call throws a `TypeError`. Refused while the context is running |
| `sjs_program` | `Program`, a row of its context | `compile` | `start`; retained by a run (`src/vm.c:1628`) and by functions it made (`callf.mjs`) | `dispose`, or its context's `dispose` |
| a `Run` and its hold on the evaluator | the `Run`, a row of its context | `Program.start`, `Value.start` | each `step`, `frames`, `result` | done (after `result` or `resultValue`), a fault, `cancel`, a later `start` on the context (decision 3), its finalizer after a generation check, or the context's `dispose` |
| globals or arguments converted for `start` | the `Run`: an array it owns, allocated from the context's allocator | `start` | each `step` (borrowed, `staticjs.h:672-678`) | when the `Run` ends. A closure the run makes holds what it needs of these past the run: they are retained in an array the closure holds, so the `Run` may free its own array when it ends |
| a `Value` given as a global | its `Value`; the run also retains it in C and references it in JavaScript | `value()`, `resultValue()`, a function out | during the run | `Value.dispose` drops the `Value`'s reference; the run's lasts until the run ends |
| a native, for a function in the globals or in `value()` | staticjs (the function value); the function in the package's slot array; nothing in C beyond the value | the conversion in | by the program, inside `sjs_vm_run` (`src/vm.c:847-863`) | its slot: when staticjs frees the value (R5); without R5, when its `Run` ends or its `Value` is disposed. The function value itself: when staticjs frees it |
| slots of a conversion out, and of a native's arguments | the C layer's stack | `_at`, `_entry`, `_iterator_at`; the native's call | during that conversion or call | `_release_to(mark)` at its end |
| the walk's stack and item count | that conversion, in JavaScript | its start | for each item | its end, or the budget's refusal |
| the key cache of a conversion out | that conversion | its start | for each key | its end, never later (`keyreuse.mjs`) |
| scratch for text in | the C layer, above the call's mark | `_scratch(n)` | the next `_string`, `_key`, `_bigint` or `_compile_global` (copied by staticjs) | the call's mark |
| a native's arguments | the evaluator | inside `sjs_vm_run` | the JavaScript function, converted on entry | when it returns (borrowed, `staticjs.h:318-320`) |
| a native's result | the C layer, then the program | converted on return, with no evaluator (R2) | the program | the program's (it takes the reference) |
| a finalizer's work | the `FinalizationRegistry` | an object becomes unreachable | the C layer's tables; for a `Run` that holds the evaluator, `sjs_vm_reset` | releases one row if its context's and its own generations still match; nothing otherwise. Runs as a job, never inside a call (F17) |
| an error's text and frames | the C layer (a compile's subject: R6's buffer) | at the failing call | copied into the `StaticJSError` at once | the next call |

A paused `Run`'s globals are not on the shared mark stack. Two contexts'
runs can interleave their steps; with globals on a shared stack, one run's
release would free the other's. A run that is abandoned without `cancel`
would also strand the slots above it. So the `Run` owns its globals, and
whatever ends the run frees them.

## Requests to staticjs

Seven additions, each the smallest that removes a binding workaround or
answers a question the package cannot answer otherwise. Names and exact
shapes are the library's to choose; the sketches show what the package
needs. Every request also serves C embedders, who today have the same
gaps (F7, F12, F13, F15). None of them costs an evaluation anything when
it is not used.

| # | Addition (sketch) | The binding's scenario | A C embedder's scenario | Removes from the binding | Costs staticjs |
|---|---|---|---|---|---|
| R1 | **Call a function value.** Needs F19's fix first. `sjs_run_params` gains `sjs_value function`, `const sjs_value *arguments` and `uint32_t argument_count`, apart from `globals`. When `program` is NULL, `sjs_vm_start` calls `function` with the arguments, on fuel, pausable like a program. `sjs_vm_take_result` stops reading `vm->program->goal` when there is no program (`src/vm.c:1669`). `sjs_vm_frame`'s comment says frame 0 is the program or the function called (`staticjs.h:800`) | a module's export called from JavaScript: `exports.net(rows)` | a host that compiles a module of handlers once and calls `onEvent(e)` for each event; without F19's fix, a handler that reads a global reads the caller's | the helper program `f(...a)` compiled per context, and the arguments array built for each call | three fields; at start, a path that pushes the callee and its arguments and builds the frame as `enter_closure` does (`src/vm.c:743-771`; `RETURN` at frame count 0 already sets the result, `:1476-1484`); the `take_result` check; the comment; tests. Code size unmeasured. The fix of F19 is staticjs's bug and is not counted here |
| R2 | **Make and read Dates and Temporal values.** A public struct with the type (PlainDate, PlainTime, PlainDateTime, Instant, Duration, Date) and its fields: the ISO date and time, an Instant's `sjs_epoch`, a Duration's ten numbers, a Date's time value (NaN for an invalid one). `sjs_time_new(context, fields, out)` checks the fields as the constructors do. `sjs_time_read(value, out)` reads them | a `Date` or `Temporal.Instant` in an event; a Date a program returns, or a native returns from inside `sjs_vm_run` | a host that passes an event's timestamp as a Date or an Instant, or reads a Date a program returns, without running `new Date(t)` on its evaluator or parsing the writer's text | the helper programs `new Date(t)` and `Temporal.X.from(s)`: one threw a paused run away (F11), and one started inside `sjs_vm_run` ran on the evaluator that was running (F5). Also the writer round trip and its reparse for these kinds out | two functions and a struct; the fields exist already (`src/internal.h:635-690`) |
| R3 | **Read an iterator's values.** `sjs_iterator_count(iterator)` gives what it has left to yield; `sjs_iterator_at(iterator, index, out)` gives the value at `index`, borrowed; for an entries iterator, the key and value without making the pair. `sjs_length` is unchanged | a program that returns `map.keys()` or `xs.values().filter(f)` | a host that reads such a result | the only route out today, the writer (F7); without R3, iterators out are refused | two functions over what exists: an internal `sjs_iterator_count` (`src/internal.h:493`, used at `src/writer.c:249`) and the writer's `iterated` (`src/writer.c:130-136`). Iterator helpers hold their items already (`src/internal.h:474-479`). `sjs_length` keeps giving 0, which its internal callers rely on (`src/builtins.c:736`, `:4561`; `src/vm.c:600`, `:2768`) |
| R4 | **Read a regular expression's source and flags.** `sjs_regexp_source(value)` → `sjs_str`, and its flags | a program that returns a pattern | a host that stores results in a format of its own (CBOR, as a tinyactors data model would) | parsing the writer's `/…/flags` in JavaScript | one or two functions. An internal `sjs_regex_source` exists (`src/regex.h:62`); where the flags are kept is **unverified** |
| R5 | **A release hook for natives.** `sjs_native_vtable` gains an optional `release(self)`, called when the native's function value is freed. It may be called inside `sjs_vm_run`, whenever a value is freed there, and in the middle of `drain`'s walk (`src/value.c:77-115`), so it must not call the library | none needed: the package scopes natives without it (Native functions); with it, a native that escaped keeps working | a host whose native's `self` is allocated per session (an `In()` bound to one session's configuration) and today cannot free it until the context is destroyed | the run scope: an escaped native faults without R5 | a pointer per vtable (NULL allowed); a call in `drain` when a native's value is freed |
| R6 | **A compile error whose subject outlives `sjs_compile`.** `sjs_compile_params` gains an optional `sjs_buffer *subject`. The subject is copied into it, cut to its capacity, and `error->subject` points there | every compile error the package reports | every C caller of `sjs_compile`: `examples/eval.c:56-63` drops the subject today | driving the parser and the compiler in eight calls to read the error before they are destroyed | a field and a copy on the error path |
| R7 | **Say which function a frame is in.** `sjs_frame` gains the program (`const sjs_program *`) and the function's index in it, as `sjs_program_function` numbers them | `Frame.program` and `Frame.function`; with R1, a frame of a closure from another program has a position that names no source the caller can tell (F15) | a debugger or profiler that maps a frame to its source when a host calls closures from several programs | nothing: without it, `Frame` has a position only, and the package says so | two fields, filled from what the frame already points at (unverified: how a frame reaches its function in `src/inspect.c`) |

Not requested:

- **A call outside an evaluator.** A staticjs function needs fuel and
  frames, so R1 runs it on one.
- **Making a regular expression from C.** No scenario asks for it, and it
  would put the regular expression compiler behind a new call.
- **More observability.** Fuel used (`sjs_vm_fuel_used`), the inspection
  calls (F15) and R7 answer the operator's questions: how much a run
  used, and where a paused or faulted one is.

**If a request is declined,** the package falls back as follows, and says
so in its guide:

- R1: after F19's fix, the helper `f(...a)`, which abandons a paused run
  like any `start` (decision 3) and is refused while the context is
  running. Before the fix, functions out cannot be called.
- R2: the helpers, under the same rules. A Date or Temporal value
  returned by a native faults the run, since the evaluator is running.
- R3: iterators out are refused.
- R4: the writer's text is parsed from its last `/`.
- R5: natives are scoped as in "Native functions".
- R6: the C layer drives the stages itself.
- R7: `Frame` has no `program` or `function`.

## Measurements

Node 22.22, a 4-core x86_64 VM, the release flags (`zig cc -O2`);
10,000 rows of `{ id, name, total }` unless stated; medians of 7 runs.
The spikes are in this session's scratchpad: `crossing.mjs`, `tape.c`,
`load.mjs`, the first review's `review.mjs`, `fair.mjs`, `keyreuse.mjs`,
`vmsize.mjs`, `mutate.mjs`, `callf.mjs` and `holes.mjs`, and the second
review's `g.mjs`, `p.mjs`, `s.mjs`, `d.mjs` and `u.mjs`. This revision
adds `gn.c` with `gn.mjs` and `gn2.mjs` (a guard with a native, through a
C layer of handles; built from the current amalgamation), `sz.mjs` (module
sizes) and `vmsize2.mjs`. `spike.wasm`, which the earlier spikes use, and
the measured `staticjs.wasm` predate the inspection functions; inspection
reads on demand and adds nothing to a run (`staticjs.h:765-766`), so their
timings stand, but their sizes are replaced. Each range spans the runs
recorded: the first draft's two, the first review's, the first revision's,
and the second review's on an idle machine. Some ran while a test suite
used the same machine, so treat differences under about 20% as noise.

| What | Result | Decides |
|---|---:|---|
| a call from JavaScript into wasm | 2.7 to 4.7 ns | a call per element is affordable (F10) |
| a call from wasm into a JavaScript function (an import) | 3.5 to 5.8 ns | natives cost their conversions, not the call |
| `staticjs.wasm` built from the current amalgamation (69 functions) | 970,907 B; 318,221 B with `gzip -9` (before inspection: 969,106 B; 317,387 B) | the package's size, before its C layer |
| compiling `staticjs.wasm` | 2.5 to 7.2 ms | `load` once per page or process; share the `WebAssembly.Module` with workers |
| instantiating it | 0.07 to 0.77 ms | one instance per tenant is cheap |
| an evaluator (`sjs_vm_init`) | 101,616 B (99.2 KiB) by default; 40,176 B (39.2 KiB) with `maxStack: 256`; the same after inspection | the cost of decision 3's options |
| a guard, raw exports, globals built beforehand: start, run, take | 90 to 106 ns | the floor |
| a guard, a one-object `event` built by handles, `limit` written by layout, a constant BigInt for fuel, no native | 200 to 215 ns | the first revision's headline; it left out the native and the layout write is ruled out by F8 |
| the Summary's guard by handles only: a two-level `event`, `limit`, fuel as an f64; no native in the source | 382 to 394 ns | the cost without natives |
| the same, with `In` made as a native but not called | 431 to 466 ns (slots in an array); 564 to 668 ns (a `Map`) | making and freeing a native per run; the slot array is the design |
| the same, with `In("heating")` called | 620 to 689 ns (array), 874 to 915 ns (`Map`); fuel as a BigInt: 615 to 674 and 896 to 939 | the per-evaluation cost to keep; f64 and BigInt fuel even |
| **out**, writer text + `JSON.parse` | 593 to 1,102 ns/row | lossy, and slowest |
| **out**, raw accessors reading layouts (a key cache) | 351 to 362 ns/row; 334 to 378 with the ASCII loop | ruled out by F8, not by speed |
| **out**, a tape (C encodes, keys as bytes; JavaScript decodes, ASCII loop) | 358 to 740 ns/row; the C encode alone 68 to 99 | — |
| **out**, handles, no key cache, `TextDecoder` for every string | 589 to 839 ns/row | the tricks, not the layout, made the difference |
| **out**, handles, key cache per conversion, ASCII loop | 187 to 303 ns/row | the design |
| **out**, a tape with key references, the same cache and ASCII loop | 218 to 336 ns/row (handles 187 to 303 in the same runs) | handles are no slower, and have no format |
| **out**, rows of `{ id: BigInt, tags: Set(2), m: Map(2) }`: handles / the tape with key references | 832 to 900 / 863 to 1,019 ns/row | even; BigInt, Set and Map cost about four times a small row |
| **in**, the C API's constructors reading layouts | 741 to 1,547 ns/row | ruled out by F8 |
| **in**, `JSON.stringify` + staticjs's `JSON.parse` | 497 to 542 ns/row | lossy (BigInt, Map, Set, `undefined`, `-0`) |
| **in**, a tape (JavaScript encodes, C decodes) | 481 to 638 ns/row | — |
| **in**, the scalar builder | 577 to 755 ns/row | the design; no consistent gap to the tape (one run each way) |

What these show, and what they do not:

- **The out side was won by two tricks, not by handles.** They are a key
  cache and an ASCII loop. Without them, handles took 589 to 839 ns per
  row and were the slowest structured path. With both tricks on both
  sides (`fair.mjs`), handles took 187 to 303 ns per row and a tape that
  emits key references 218 to 336, in the same runs. On an idle machine
  handles took 189 to 206 twice and 297 to 303 once (the tape 333 to 336
  in that run). On rows with a BigInt, a Set and a Map the two were even.
- **The guard's cost is mostly the package's JavaScript, not staticjs.**
  The raw floor is 90 to 106 ns. The spike's handles path adds about
  280 ns for a two-level `event` without a native. Making the native
  adds 40 to 80 ns, and calling it 190 to 230 ns more: the argument read
  through a fresh `Uint8Array` view, the function, and the call. The
  spike is not tuned, and how much of each part tuning removes is
  unverified. The difference from the first revision's 200 to 215 ns is
  probably the second object, the key text written per key, and
  `Object.keys` (unverified: not broken down).
- **The first draft's key cache was keyed by pointer across
  conversions.** That was wrong (`keyreuse.mjs`). Its numbers stand only
  because the spike's value stayed alive throughout. The cache is now per
  conversion, and `fair.mjs` measured it that way.
- **The first conversion after load** took 308 to 946 ns per row in
  `fair.mjs`, against about 190 once warm. That is JIT warm-up. A
  one-shot conversion pays it; how much in a browser is unverified.
- **Not measured:** conversion in of BigInt, Set, Map, Date and Temporal
  values; strings past ASCII; the iterative walk and the item count
  against the recursive walk the spikes use; a browser; and the code size
  of the C layer and of the requests.

## Costs

| Who | Pays |
|---|---|
| per evaluation | the C API's work, plus about 2 to 4 wasm calls per element crossed and an item count; per function in the globals, a native made and freed (40 to 80 ns in `gn2.mjs`) and, when called, its call and conversions (190 to 230 ns in that spike) |
| per context | one `sjs_vm` (101,616 B with the default limits; 40,176 B with `maxStack: 256`), the C layer's tables, one handle stack shared by the instance |
| per instance | the natives' slot array: one slot per native alive at once, plus one retired slot per 65,536 uses of a slot |
| per package | the module (970,907 B, 318,221 B with `gzip -9`, before the C layer), the C layer (~400 lines, estimated), the JavaScript (~600 lines, estimated) |
| API surface | `load`, the interfaces `StaticJS`, `Context`, `Program`, `Run`, `Value`, `Frame`, `FunctionSummary`, the class `StaticJSError` |
| the release | a second wasm artifact, `staticjs-js.wasm`, or the npm package alone (decision 1) |
| staticjs's library | R1 to R7: fields in three structs, a vtable entry, about seven functions, their tests and their documentation in `staticjs.h` and the guide; R1 after the fix of F19 |

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
  it was no faster out (218 to 336 against 187 to 303 ns per row; even on
  BigInt, Set and Map rows). In, it showed no consistent gap (481 to 638
  against 577 to 755 ns per row). It is a format kept in two languages,
  the second codec that `.agents/skills/boundary-design/SKILL.md:75`
  rejects. This is **plausible**, not settled: the out difference is
  within the noise. An input workload with BigInt, Map and Set would
  settle the in side.
- **Not `run` and `runValue` beside `start`** (the first revision's).
  Each was a second way to do what `start`, `step` and `result` do, and
  on a spent budget it reset the evaluator, losing the frames, and threw
  an error of its own. A caller who wants one call writes it in two
  lines, and decides what a spent budget means to it.
- **Not a JavaScript function for a function out.** A call needs fuel,
  which a plain call has no place for, and can pause, which a plain call
  cannot return. Giving it the fuel of the run that made it was a default
  of the package's own. `Value.start(args)` keeps both explicit.
- **Not a record in C per native.** `self` holds the slot and generation
  itself (F12), so nothing is allocated per native and nothing grows per
  run.
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
  npm tarballs, and confirmed by the second review). The C it adds to the
  module is not measured. Each object it makes is still a call from wasm
  into JavaScript (`napi_create_*`), as many crossings as handles, so a
  speed gain is unverified and not expected. Decision 5.
- **Not lazy `Proxy` values by default,** because a property read through
  a proxy costs a trap and calls per read, every time, while an eager copy
  costs 187 to 303 ns per row once. `Value` covers the case where data
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
| `createContext` | `sjs_context_init`, `sjs_vm_init`; `conversion.maxItems` is the package's |
| `Context.value` | conversion in (below), `sjs_retain` |
| `Context.dispose` | `sjs_vm_reset`, `sjs_release` and `sjs_program_release` for every row, `sjs_vm_destroy`, `sjs_context_destroy` |
| `Context.liveBytes` | `sjs_context_live_bytes` |
| `Context.setClock`, `advanceClock` | `sjs_vm_set_clock`, `sjs_vm_advance_clock` |
| `Context.setSeed` | `sjs_vm_set_seed` |
| `Context.compile` | `sjs_compile` with R6; until then `sjs_parser_init`, `sjs_parser_push`, `sjs_parser_finish`, `sjs_parser_error`, `sjs_compiler_init`, `sjs_compiler_sink`, `sjs_compiler_finish`, `sjs_compiler_error`, the destroy calls; `modules`: an `sjs_modules_vtable` in C |
| `Program.start` | `sjs_vm_start`; `sjs_retain` for the run's globals |
| `Value.start` | `sjs_vm_start` with `sjs_run_params.function` and `.arguments` (R1, after F19's fix) |
| `Run.step` | `sjs_vm_run`; on a fault `sjs_vm_error`, `sjs_vm_frame_count`, `sjs_vm_frame` |
| `Run.fuelUsed` | `sjs_vm_fuel_used` |
| `Run.cancel`, a later `start`, a `Run`'s finalizer | `sjs_vm_reset`, `sjs_release` for the run's globals |
| `Run.result`, `resultValue` | `sjs_vm_take_result`, conversion out or `sjs_retain` |
| `Run.frames` | `sjs_vm_frame_count`, `sjs_vm_frame`; `Frame.program`, `.function` (R7) |
| `Program.functions` | `sjs_program_function_count`, `sjs_program_function` |
| `Program.dispose` | `sjs_program_release` |
| `StaticJSError` | `sjs_vm_error` and the fault's frames, `sjs_compiler_error`, `sjs_parser_error` (or `sjs_compile_params.error` with R6); the native's thrown error as `cause` |
| a function in the globals or in `value()` (the native import) | `sjs_function_native` with `self` = slot and generation, `sjs_native_vtable.call`; `.release` (R5) |
| conversion in | `sjs_string_new`, `sjs_bigint_new`, `sjs_array_new`, `sjs_array_append`, `sjs_object_new`, `sjs_object_put`, `sjs_set_new`, `sjs_set_add`, `sjs_map_new`, `sjs_map_set`, `sjs_number`, `sjs_boolean`, `sjs_null`, `sjs_undefined`, `sjs_function_native`, `sjs_time_new` (R2) |
| conversion out | `sjs_length`, `sjs_array_at`, `sjs_object_at`, `sjs_set_at`, `sjs_map_size`, `sjs_map_at`, `sjs_string_text`, `sjs_bigint_text_bound`, `sjs_bigint_text`, `sjs_iterator_count` and `sjs_iterator_at` (R3), `sjs_time_read` (R2), `sjs_regexp_source` (R4), `sjs_retain` for a function kept as a `Value` |
| `Value`, its `dispose` | `sjs_retain`, `sjs_release` |
| `Value.toString` | `sjs_writer_init`, `sjs_writer_write` (in chunks, up to `maxStringBytes`), `sjs_writer_destroy` |

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
   - the first draft's ISO string, which the first review counted as a
     conversion into something else.
3. **One evaluator per context.** Recommended: C's semantics. A `start`
   (of a program or a function) while a `Run` is paused abandons that
   run, as `sjs_vm_start` does (F11): the package frees its globals and
   clears its natives at once, and the abandoned `Run`'s next call throws
   the `TypeError` any ended object throws. A `Run` that becomes
   unreachable while it holds the evaluator is reset by its finalizer,
   after the generation check; that is safe because a cleanup job never
   runs inside a call, and an unreachable `Run` cannot be stepped again
   (F17). So an abandoned `Run` blocks nothing. The cost: a second
   `start` silently ends the first run, as in C, and the caller learns of
   it at the first run's next call. The alternatives:
   - the first revision's lock: while a `Run` holds the evaluator,
     `start` throws. Nothing ends silently, but it is a rule C does not
     have, and an abandoned `Run` blocks its context until it is
     collected or the context is disposed;
   - `context.evaluator()` for several runs at once, at 101,616 B each
     with the default limits.
4. **The requests R1 to R7,** together or one by one. Each declined
   request has the fallback "Requests to staticjs" names. R1 waits on the
   fix of F19 in any case.
5. **emnapi.** Not now (recommended): staticjs has no Node-API addon to
   reuse, and emnapi would add 31,716 B gzipped of runtime and C of
   unmeasured size for no measured gain. Revisit with tinyactors' own
   browser decision (`ROADMAP.md:840-845`): if that package uses emnapi
   anyway, a shared runtime changes the sum.
6. **The conversion budget** (`conversion.maxItems`). Recommended: one
   required number per context, counting items both ways, refused with a
   `RangeError`. It is a limit of the package's own, which a binding
   avoids, but staticjs has nothing that bounds a conversion (F20), and
   a native's conversions have no call to carry a per-call value. The
   alternative is no bound: a 140-fuel program can then make a result the
   package cannot finish converting.

## The review, answered

The first review:

| Finding | Answer |
|---|---|
| 1. Helpers on the single evaluator | R1 and R2 remove them; the fallbacks abandon a paused run like any `start` and are refused while running (Requests; decision 3) |
| 2. Iterators have no route out | F7 lists them; R3; refused without it |
| 3. The key cache keyed by pointer | per conversion, while the root is retained (How it crosses; `keyreuse.mjs`) |
| 4. Handles faster than the tape | rerun fairly (`fair.mjs`); restated as even, chosen for having no format; marked plausible (Measurements; Alternatives) |
| 5. Headline numbers | restated again after the second review (its finding 12) |
| 6. Requests to staticjs | the section of that name; the status line no longer says the library is unchanged |
| 7. Natives leak | run-scoped natives with `self` as slot and generation, no growth; R5 for C embedders (Native functions) |
| 8. `dispose` with live objects | releases every row, changes the context's generation, refuses while running; finalizers check the context's and the row's generations (Lifetimes) |
| 9. Paused runs' globals on a shared stack | the `Run` owns them (Lifetimes) |
| 10. User code during conversion in | running flag, intrinsics, a builder without state (Values) |
| 11. `FinalizationRegistry` | F17; a fallback only; a `Run` holds what it reads |
| 12. Repository rules | the export exemption recast as lint (second review, finding 9); text in through owned scratch; `_compile_module` takes a name length; no fuel default and no `fuel` kind; no ISO fallback (decision 2); holes refused; the rejected "language layer" idea and F18 addressed; emnapi measured in part, decision 5 |
| 13. Temporal input | the five types in the ISO calendar; the rest refused |
| 14. Invalid Dates out | `new Date(NaN)` (R2's time value) |
| 15. Function calls through `f(...a)` work when the evaluator is free | confirmed only for closures that read no global: `callf.mjs` reads a captured variable. The second review showed a closure reads the calling run's globals (F19), so R1 and its fallback wait on that fix |
| 16. A borrowed `Value` is never changed in place by a run | confirmed; F4 cites `mutate.mjs` |
| 17. RegExp in, holes | both refused (Values) |
| 18. F8 and F9 evidence | cited as given, F8 marked an analogy; F9's commit is not in this clone |
| 19. KiB and timing ranges | bytes and KiB corrected; compile 2.5 to 7.2 ms, instantiate 0.07 to 0.77 ms |
| 20. Details | class count, the lifetimes reference, the API map rows, `_set_clock` packing, `using`, the 64-byte subject, shared parts growing: each fixed where it appears |

The second review:

| Finding | Answer |
|---|---|
| 1. R1 as "a field" is refuted: closures read the current run's globals | F19 (`p.mjs`: 6, not 101; `g.mjs`: a read past the array). R1 waits on staticjs's fix, has `function` and `arguments` fields apart from `globals`, fixes `sjs_vm_take_result` (`src/vm.c:1669`) and frame 0's comment (`staticjs.h:800`); its cost restated without the fix. Until the fix, `Value.start` on a function throws |
| 2. Fuel does not bound conversion out | F20 (`s.mjs`, `d.mjs`); iterative walks both ways, a total item budget per conversion, refused with a `RangeError` (Values; decision 6); `toString` capped at `maxStringBytes`; the misuse of `maxNesting` for value depth removed |
| 3. `run` and `runValue` lose frames and invent a message | dropped; `step` leaves a run paused with its frames; `StaticJSError.frames` for a fault (The API; Alternatives) |
| 4. The running flag misses `step`, `cancel`, `result`, the clock and the seed | every evaluator call and the clock and seed refused while running, with the reasons (Reentrancy) |
| 5. Decision 3's lock invents semantics | recommended C's semantics, with a finalizer that resets a collected `Run` after a generation check; the lock kept as the alternative (decision 3) |
| 6. Run-scoped natives without a record; R5 by its C scenario; natives in `value()`; the hook's rule | `self` is slot and generation, slots retire at wrap; R5's binding scenario is "none needed"; natives in a `Value` live until it is disposed; the hook may fire inside `sjs_vm_run` and in `drain` and must not call the library (Native functions; R5) |
| 7. Generations per instance; `_drop` with a context | one counter per instance, as an f64; a context is a row; `_drop(ctx, row, generation)` (How it crosses) |
| 8. A function out reuses its maker's fuel | a function out is a `Value`; `start(args)` returns a `Run` stepped with explicit fuel (Values; Alternatives) |
| 9. The `buffers-are-ta-buffer` exemption argued wrongly | the exports match neither of the rule's patterns; the C layer is linted with napi's rules, an exclusion would go by export attribute in a rule file; the typedef analogy dropped (How it crosses) |
| 10. R3 changes `sjs_length` | a separate `sjs_iterator_count`, from the internal one; `sjs_length` unchanged, with its callers cited (R3) |
| 11. R2, R6 confirmed; R4 unchecked | R4's internal `sjs_regex_source` cited, its flags marked unverified |
| 12. The guard's headline had no native and wrote by layout | measured with a native and fuel as a number (`gn.mjs`, `gn2.mjs`: 615 to 689 ns with a slot array; 382 to 394 without the native); f64 and BigInt fuel even; the old row kept and labelled (Summary; Measurements) |
| 13. Ranges too narrow | handles 187 to 303, tape 218 to 336, writer and `JSON.parse` 593 to 1,102 ns per row; 2.0 to 5.9 times |
| 14. F1 and sizes predate inspection | F1: 69 functions; the module rebuilt from the current amalgamation (`sz.mjs`): 970,907 B, 318,221 B with `gzip -9`; the evaluator's size unchanged (`vmsize2.mjs`) |
| 15. emnapi figures | confirmed; noted (Alternatives) |
| 16. F15: a frame has no program or function | F15 says so; R7 asks for both, needed once R1 runs closures from other programs |
| 17. Other frame facts confirmed | F9's commit noted as missing from this clone |
| 18. "`liveBytes` back to zero" cannot hold | "back to its value after `createContext`" (If adopted) |
| 19. The native import's `args` and exceptions | `base` is a handle on the C layer's stack; the import catches every exception and turns it into a fault (Native functions) |
| 20. Findings 15 and 16 of the first review unaccounted | both in the first table |

The claims new in this revision have not been refuted by a fresh agent
(reasoned-analysis, step 8). Until they are, they count as plausible.
Not settled:

- how F19's fix keeps the globals a closure reads, and so whether a
  `Run`'s globals must outlive it (Lifetimes);
- R4's flags and R7's fields in `src/inspect.c` (unverified);
- the cost of the iterative walk and the item count against the spikes'
  recursive walk (unmeasured);
- how much of the guard's 615 to 689 ns a tuned C layer removes;
- the tape against handles on input with BigInt, Map and Set.

## If adopted

1. The fix of F19 in staticjs, then the requests R1 to R7, each with its
   tests and its documentation in `staticjs.h` and the embedding guide,
   before the package depends on them.
2. `staticjs-js/` with `AGENTS.md`, `docs/api-map.md` (checked as napi's
   is) and the C layer, built with
   `Toolchain::Target.release('wasm32-freestanding')` from staticjs's
   amalgamation, and linted with napi's `at-most-three-parameters` and
   `buffers-are-ta-buffer` over all of its C.
3. The value conversions, with tests that:
   - every row of the values table round-trips;
   - every refusal throws;
   - a getter or Proxy that reenters the context is refused, and so is
     each call the Reentrancy list names, from inside a native;
   - a DAG result and a value nested 200,000 deep are refused at the
     budget, without a `RangeError` from the call stack;
   - an escaped native faults after its run ends, and a stale `self`
     never calls a later function;
   - `dispose` with live objects releases everything: `liveBytes` comes
     back to its value right after `createContext`, since the evaluator
     is allocated from the context.

   Run the spec cases that expect a value through the package, so the
   package agrees with `diff-node`.
4. `load` for Node (`fs`) and browsers (`instantiateStreaming`), and a
   test in Chromium with the Playwright the environment has.
5. Packaging (the package's module, decision 1) and its changelog.
6. Replace `staticjs/examples/wasm/` with an example of the package, and
   `staticjs/docs/guide/wasm.md` with its guide.
