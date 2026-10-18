# The browser package: design (p5-design)

**Status:** proposal, 2026-10-18, written before any code
(docs/plan.xml, `p5-design`). Not yet refuted: the refutation by a fresh
agent is pending (below, "Refutation"), and the plan's steps p5-build to
p5-quickjs are refined from this document only after it.

The maintainer decided (2026-10-17, plan.xml phase p5): core compiled to
WebAssembly with a JavaScript layer over it, not the Node-API addon over
emnapi; the same JavaScript API in Node.js, Bun and the browser (one
`index.d.ts`); no quickjs at first; async continuations on the same
thread's event loop; arenas over an allocator, never `malloc` and `free`
per block; user convenience balanced against performance. This document
answers the step's questions within those decisions. Every number below
was measured on 2026-10-18 on this machine (x86_64 Linux 6.18, 4 CPUs,
node 22.23.3, bun 1.4.1, Chromium 141.0.7390.37 from the pre-installed
Playwright 1.56.1); the programs are in `tools/spikes/browser/`.

## Contents

- [Summary](#summary)
- [Whom it serves, and who pays](#whom-it-serves-and-who-pays)
- [The frame](#the-frame)
- [Measurements](#measurements)
- [What is C and what is JavaScript](#what-is-c-and-what-is-javascript)
- [The loop on the event loop](#the-loop-on-the-event-loop)
- [Memory](#memory)
- [How values cross](#how-values-cross)
- [The native datamodel without node:vm](#the-native-datamodel-without-nodevm)
- [Lifetimes](#lifetimes)
- [One API](#one-api)
- [Packaging and releases](#packaging-and-releases)
- [Node.js and Bun](#nodejs-and-bun)
- [Not chosen](#not-chosen)
- [Choices for the maintainer](#choices-for-the-maintainer)
- [Refutation](#refutation)
- [The steps after this one](#the-steps-after-this-one)

## Summary

| Question | Recommendation | Evidence |
|---|---|---|
| What is C | core, the host, the compiler, txml, and a C layer of ours (`ta_web_*`) that builds core's structs, owns the arenas, runs the drain and the turn, reads CBOR and effect fields, and forwards the datamodel's vtable to imports | ADR 0009 ("only C touches host structs"); staticjs-js's `src/js.c` is the same split (ADR 0013) |
| What is JavaScript | the classes, scheduling (`setImmediate` where it exists, else `MessageChannel`; one `setTimeout` at the deadline), the handlers' dispatch, making JavaScript values from CBOR tokens and back, the native datamodel's side (with the addon's `realm.js`, shared) | a wasm module cannot make JavaScript objects (root ROADMAP.md, "Decided later") |
| Download | about 140 KB brotli for core and the compiler at `-O2` (114 KB at `-Oz`), plus txml's 25 to 32 KB and our C layer (unmeasured); quickjs later as a second module, about 300 KB brotli more | "Sizes" |
| Boundary | 5 to 11 ns a call either way, against 75 to 210 ns for a Node-API call; a short string 50 to 600 ns depending on how it is copied | "Boundary costs" |
| Memory growth | no view of linear memory outlives a call that can allocate: the layer re-derives its views when one reads `byteLength === 0` (1 to 2.4 ns); nothing the application holds is a view | "Memory" |
| Native datamodel | one realm per host (a hidden iframe on a page, `node:vm` in node and bun, the worker's own global in a Worker); a chart that declares nothing: a scope per session, as the addon (49 B); a chart whose scripts declare with `var` or `function`: a generator per session in the host's realm (245 B); with `let`, `const`, `class` or a strict script: a realm per session (an iframe: 608 KB, 3.6 ms) | "The native datamodel without node:vm" |
| CSP | the module needs `'wasm-unsafe-eval'`, the native datamodel `'unsafe-eval'`; under a policy without it, a host on the native datamodel is refused at creation (`TA_ERROR_UNSUPPORTED`) and quickjs (later) is the way | spike, "CSP" |
| Values | CBOR is read and written by core's `ta_cbor_reader` and `ta_cbor_writer` in C, one token per call, and JavaScript makes the values; payloads of the native datamodel are never encoded (a row in `ta_table`, the value at its slot in a JavaScript array, as the addon) | "How values cross" |
| One API | one `index.d.ts`; the test files unchanged, run against the wasm package by an export condition in node and bun and by an import map in Chromium; a parity check of the exports | "One API" |
| Package | a sibling package `@tinyactors/wasm`, made from `tinyactors-napi/` at the same version, changelog and tag, released to `tinyactors-dev/tinyactors-napi` | "Packaging and releases" |
| Node and Bun | the wasm package is supported there (its tests run there anyway), the way on Windows, which has no addon; no silent fallback in `@tinyactors/node`; whether one implementation can go is decided on measurements at p5-parity | "Node.js and Bun" |

## Whom it serves, and who pays

Following [docs/roles.md](../../docs/roles.md):

- **First: the application developer writing JavaScript for a browser**
  (a page's or a worker's statecharts: UI flows, offline sync, games),
  who wants the API they know from Node.js, charts compiled in the page,
  and values that are JavaScript values. Today nothing answers them: the
  addon needs Node-API, and the old web binding was deleted with the
  bindings (`997706b8`).
- **Then: the application developer on a platform with no addon**
  (Windows: `mise/pack.rb:46-49` builds macOS and Linux only), and the
  **maintainers**, who run one suite and one set of test files against
  both packages.
- **Out of scope:** quickjs in the browser (p5-quickjs, later);
  run-time fetching (`TA_EFFECT_REQUEST` stays answered in C with
  `TA_ERROR_INVALID`, as the addon does); a convenience layer of the
  browser's own (the existing one is shared); changes to core.
- **Who pays:**
  - *download:* about 175 to 190 KB brotli for the module (estimate:
    core and the compiler measured, txml measured, our C layer not), plus
    the JavaScript layer (unmeasured);
  - *per event:* core's part of an event costs 1.1 to 2 times its native
    cost in wasm (0.1 to 0.25 us more on a 5.2 us event of the addon's);
    each crossing costs 5 to 11 ns where a Node-API call costs 75 to 210
    ns, so the JavaScript side may be cheaper than the addon's
    (unverified until p5-api's benchmark);
  - *per actor:* the same as the addon for a chart that declares nothing
    (a scope object); 245 B for a chart that declares with `var` or
    `function`, against the addon's 60 to 143 KB `node:vm` context; 608 KB
    in a browser for a chart that declares with `let`, `const` or `class`;
  - *memory:* linear memory never shrinks, so a page keeps its peak
    (core's `allocator_wasm.c`; bindings.md section 11);
  - *API surface:* none: the same `index.d.ts`;
  - *work in every binding:* a second implementation of the JavaScript
    API's plumbing (a C layer of a few thousand lines, unverified, and
    the JavaScript classes and datamodel), held to the addon by shared
    tests, the suite and a parity check; every later API item is written
    twice. This is the largest cost, and "Node.js and Bun" says how it
    could shrink.

## The frame

Facts the design depends on, each checked:

- **One thread per host.** Every call comes from the thread that made the
  host (bindings.md section 5); in a browser there is no other thread
  that can reach a module instance's memory unless it is shared, and ours
  is not. Reentrancy is on that thread only: a handler calling the host,
  a getter, a datamodel expression.
- **Inputs are read during their call; effects and the host's outputs
  live until `ta_host_effects_ack`** (ADR 0012; `tinyactors-core/docs/boundary.md`).
- **Only C reads core's structs** (ADR 0009, "Only C touches host
  structs"; boundary-design skill, rejected ideas: "Struct layouts read
  from Python or JS"). The old web binding read them at wasm32 offsets
  generated from the API descriptions (`git show 997706b8^:tinyactors-core/bindings/web/README.md`),
  before ADR 0009.
- **Core's wasm builds today:** `mise run wasm:test` builds
  `tinyactors.wasm` (wasm32-freestanding, its own page allocator,
  `src/base/allocator_wasm.c`), `tinyactors_runtime.wasm` and
  `tinyactors_datamodel_ecmascript.wasm` (wasm32-wasi with wasi-libc
  compiled in, `-mexec-model=reactor`, `_initialize` exported), each
  importing nothing and exporting the API (`mise/lib/c_release/build.rb:359`,
  `CRelease.build_wasm`; `link_flags` lets a binding add its own).
  `include/tinyactors/wasm.h` gives `ta_wasm_alloc` and `ta_wasm_free`;
  the runtime module's are `aligned_alloc` and `free` over wasi-libc
  (`dist/tinyactors_runtime_wasm.c`). `tests/wasm/release_test.mjs`
  drives a module from JavaScript by struct offsets, as a test.
- **The compiler needs the C library** (`src/compile/compile_support.c:4`
  includes `stdlib.h`; `tinyactors_runtime` is `libc: true`,
  `tinyactors-core/mise/core.rb:124`), so our module is a wasm32-wasi
  reactor like the runtime module.
- **staticjs-js is the pattern** (ADR 0013): its C layer (`src/js.c`)
  calls the library's API, its exports take and return numbers, the few
  records JavaScript reads are the layer's own arrays of words, and its
  module imports `staticjs_js.native` and `staticjs_js.release`
  (staticjs-js/AGENTS.md). Its loader resolves the module with
  `new URL('./staticjs.wasm', import.meta.url)` and reads a `file:` URL
  with `node:fs` (staticjs-js/index.mjs); its Chromium test serves the
  package and imports it in a page (staticjs-js/test/browser.test.mjs).
- **The addon:** about 11,900 lines (`src/`, `lib/`, `index.d.ts`), of
  which the native datamodel is 2,800 (`src/datamodel/`, its kit
  `realm.js` 423 lines of plain JavaScript run in every `node:vm`
  context); its loop, drain, dispatch and values are C over Node-API.

## Measurements

### Sizes

Core's release modules as `mise run wasm:test` builds them (`-O2`), and
spike builds with the same zig (`tools/spikes/browser/`, commands in
"Reproducing"). "Stripped" drops the custom sections (names, DWARF):

| Module | File | Stripped | gzip -9 | brotli 11 |
|---|---|---|---|---|
| `tinyactors.wasm` (core, host, OTLP, HTTP codec; no compiler) | 418,617 | 409,450 | 146,783 | 116,288 |
| `tinyactors_runtime.wasm` (the above, the compiler, wasi-libc) | 1,403,617 | 504,137 | 181,071 | 144,949 |
| `tinyactors_datamodel_ecmascript.wasm` (core and quickjs) | 2,551,245 | 1,527,647 | 530,218 | 417,059 |
| core and compiler, only the 95 functions the addon calls exported, `-O2` | | 481,854 | 174,918 | 139,269 |
| the same at `-Oz` | | 331,958 | 135,528 | 113,560 |
| txml (`tinyxml/include/txml.c`), `-O2` / `-Oz` | | 119,685 / 77,004 | 39,505 / 29,207 | 31,502 / 24,989 |

- Exporting only what the layer calls saves little (139 KB against 145
  KB brotli): almost all of core is reachable from the host.
- `-Oz` saves 19 % (139 KB to 114 KB); its speed is unmeasured.
- The release's `tinyactors_runtime.wasm` carries 899 KB of DWARF
  (`.debug_info`, `.debug_loc`, `.debug_line`, ...) though its sources
  compile with `-g0`: they come with the C library's objects (unverified
  where exactly). Our module strips custom sections; logged for core as
  `l-p5-design-release-wasm-debug`.
- A wasm32-wasi link starts with 257 pages (16 MiB) of memory, the
  freestanding one with 17 (spike: `memory.buffer.byteLength` after
  instantiation); likely the linker's default stack size (unverified;
  p5-build sets and measures it).
- For scale, not measured here: quickjs is about 300 KB brotli of the
  ECMAScript module (417 KB less core's 116 KB).

### Boundary costs

`tools/spikes/browser/bench.mjs`, 2,000,000 calls each (str2.mjs: a
million), ns per operation:

| Operation | node | bun | Chromium |
|---|---|---|---|
| JavaScript calls a wasm export | 5.2 | 5.0 | 10.9 |
| wasm calls a JavaScript import | 6.6 | 8.2 | 8.5 |
| a Node-API call: the addon's `decode` of `null` (its arena mark included) | 75 to 88 | 210 | — |
| a 16 B string in: `TextEncoder.encodeInto` over a `subarray` | 86 to 92 | 95 | 588 |
| a 16 B string in: a loop of `charCodeAt` (ASCII; else `encodeInto`) | 51 to 109 | 61 | 76 |
| a 16 B string out: `TextDecoder.decode` over a `subarray` | 157 to 188 | 104 | 263 |
| a 16 B string out: `String.fromCharCode.apply` | 231 | 137 | 146 |
| `memory.buffer` read and compared | 27 | — | 30 |
| a view's `byteLength === 0` | 2.4 | 1.0 | unmeasured |
| `String.prototype.isWellFormed` (lone surrogates) | 6.3 | — | 26 |

So a crossing is nearly free, and text is what costs: a string in is a
copy into linear memory, a string out a decode. The layer copies ASCII by
hand and reads text lazily (thin views), and never reads `memory.buffer`
per access.

### Core's cost per event

`tools/spikes/browser/perevent.c`: one actor of a two-state chart, the
null datamodel, tracing off; `ta_system_send`, `ta_runtime_run`, a take
and an ack per event; three runs of a million, the last three of eight
rounds:

| Build | ns per event |
|---|---|
| native (zig, `-O2`) | 261, 284, 288 |
| wasm32-wasi in node (`node:wasi`) | 392, 392, 534 |
| wasm32-wasi in bun | 370, 513, 310 |

Core's part runs 1.1 to 2 times slower in wasm. The addon's whole event
on the native datamodel is 5.2 us on node (AGENTS.md, "Measured cost per
session"), so core's part is about 5 % of it and the wasm penalty on it
0.1 to 0.25 us. Chromium was not measured (the same V8 as node:
unverified).

### Realms

`tools/spikes/browser/drive.mjs`, Chromium; the JavaScript heap after two
collections (`Runtime.getHeapUsage` through the DevTools protocol) and
the renderer's resident set (`/proc`), divided by the count:

| A session's datamodel state | Made | JS heap each | Resident each | Time each |
|---|---|---|---|---|
| a scope object in the host's realm (a chart that declares nothing; the addon's shape) | 10,000 | 49 B | 46 B | — |
| a generator in the host's realm, one script of three `var`s and a `function` run in it | 10,000 | 245 B | 248 B | 3 us |
| a same-origin iframe | 200 | 350,793 B | 607,744 B | 3.6 ms |
| (the addon's `node:vm` context per session, AGENTS.md) | | 143 KB node, 60 KB bun | | |

- A generator session's evaluation is a direct `eval` in it: 230 ns for
  `count < 3`, against 82 ns for the shared chart's compiled
  `with (scope)` function (Chromium, 200,000 each).
- The generator (`realms2.mjs`) keeps its `var` and `function`
  declarations between scripts, its functions close over them, two
  sessions do not see each other's, nothing lands on the realm's global
  or the page's, a throwing script leaves it running (a `try` inside the
  generator), and a script after `throw 0;` instantiates its declarations
  without running a statement (a `function` declared so closes over the
  `var` set later). It does not keep `let`, `const` or `class`, nor the
  `var`s of a script that starts with `'use strict'` (a strict `eval` has
  its own variables); its two protocol names (`__tinyactors_in` and the
  catch's) are visible to chart code; an undeclared assignment in sloppy
  code lands on the realm's global. The same generator works in a
  `node:vm` context on node and bun (`vmgen.mjs`).
- An iframe removed from the document keeps running: its realm's
  functions, `eval` and constructors still work after `f.remove()`
  (`detach.mjs`, Chromium 141).
- `ShadowRealm` is absent from Chromium 141 and node 22; bun 1.4.1 has
  one. `WebAssembly.Memory.prototype.toResizableBuffer` is absent from
  Chromium 141 and node 22, present in bun.

### Scheduling, Workers and CSP

- A chain of 200 turns: `setTimeout(fn, 0)` 4.045 ms a turn (the nesting
  clamp), a `MessageChannel` message 0.011 ms (Chromium).
- A module Worker has no `DOMParser`, no `document` (so no iframe), no
  `setImmediate`; it has `MessageChannel`, `WebAssembly`, and
  `new Function` (no policy).
- Pages served with a Content Security Policy (`csp.mjs`, `drive3.mjs`):

  | `script-src` | `WebAssembly.instantiate` | `new Function` | an iframe's `eval`, `Function` |
  |---|---|---|---|
  | `'self'` | refused (`CompileError`) | `EvalError` | `EvalError` |
  | `'self' 'wasm-unsafe-eval'` | works | `EvalError` | `EvalError` (the policy is inherited) |
  | `'self' 'unsafe-eval' 'wasm-unsafe-eval'` | works | works | works |

  Trusted Types (`require-trusted-types-for 'script'`) also guards
  string evaluation: not spiked (unverified).

## What is C and what is JavaScript

The rule that decides: **what reads or writes core's structs, owns
memory, or must hold an invariant is C; what makes or calls JavaScript
objects, or schedules, is JavaScript.** "Nothing C does is done again in
a language" (ADR 0009, Consequences) is kept by doing each thing once,
on the side that can.

| Piece | Where | Why |
|---|---|---|
| core: base, image, trace, core, system, host, OTLP, BasicHTTP; the compiler and runtime; txml | C, from core's and tinyxml's amalgamations | the library |
| the host's creation and free, the allocator, the host's arena and a mark per call, the environment's arena for `encode`, `decode`, `compileCharts`, `Address` | C (`ta_web_*`) | allocation by lifetime, as the addon (AGENTS.md, "The binding's memory") |
| inputs (`spawn`, `send`, `sendMany`, `terminate`, `boot`, `wake`, `hibernate`, `migrate`, ...) | C builds core's structs from the layer's own input record, words written by JavaScript in the order its header states, the text JavaScript copied into the call's arena | only C touches core's structs; the record is ours, as staticjs-js's |
| the drain and the turn | C: `ta_web_turn(host)` returns what the driver does next (yield, idle with a deadline, exhausted, fault), and calls one import per effect JavaScript must see | "one drain at a time" made structural: no take or ack is exported, so no handler can nest a drain; the guide's two functions in C, as both bindings (bindings.md section 3) |
| effects JavaScript never sees: `REQUEST` (resumed with `TA_ERROR_INVALID`), `DONE` and `ERROR` (rows released), `SEGMENT_FREED`, an unknown `NOTIFICATION`, a `REPLY`'s lookup in the pending table | C | the addon's rules, with no crossing |
| an effect's fields | C accessors over `ta_host_effect_number` and `_text`; JavaScript asks per field, on first read | thin views pay nothing for a field nobody reads |
| the ack counter that makes views stale | C bumps it after the ack; JavaScript reads it | one counter, where the ack is |
| tables: payloads, sessions, charts, loops, pending typed methods, services | core's `ta_table` in C, the JavaScript object at the row's slot of one array per host | no handle table of ours, as the addon (no `napi_ref`: the array) |
| CBOR (config, typed methods' params and replies, `encode`, `decode`, hibernation, snapshots, `to_wire`) | C reads and writes with `ta_cbor_reader` and `ta_cbor_writer`, one token per call | one codec; core validates (UTF-8, nesting, tags) |
| JavaScript values from tokens and back, and the value table's refusals (safe integers, `BigInt`, lone surrogates through `isWellFormed`, plain objects only, tag 32 as `Address`) | JavaScript | C cannot make JavaScript objects |
| scheduling: a turn, the deadline's timer | JavaScript | the runtime's |
| the classes, errors, handlers, thin views, records for promise-returning handlers | JavaScript | the API |
| the native datamodel's vtable | C entries that forward to imports, each with at most three parameters, the rest in the layer's own record | core calls a vtable; imports, not the old web binding's generated module in the function table (that was because the release module imports nothing; ours is the binding's own) |
| the native datamodel's work: scopes, sessions, evaluation, copies, `_event`, XML nodes | JavaScript, with the addon's `realm.js` run in each realm | the same kit, one source, so copies and the DOM behave the same |
| XML content | txml in C parses into the datamodel's scratch arena; the layer writes the tree as its own record; `realm.js`'s `XMLNode` builds it | the addon's DOM; `DOMParser` is missing in a Worker (spike) and makes another DOM |

The module's imports are the layer's own (`ta_web.*`) and its exports
the layer's functions, `memory` and `_initialize`; a check like
staticjs-js's (`mise run build` there checks its imports exactly) holds
it. Our C follows the addon's C rules: the prefix (`ta_web_*`), at most
three parameters, `ta_bytes` in and `ta_buffer *` out, no mutable
globals (state on the host or the environment's record; a module
instance is one environment), allocation only in the tables, the arenas
and the host.

## The loop on the event loop

The addon's rules, unchanged (AGENTS.md, "The loop"), with the browser's
scheduler:

- **A turn** is `ta_web_turn` from a macrotask: `setImmediate` where it
  exists (node, bun: the addon's, so liveness is the same), else a
  `MessageChannel` message (a page, a Worker: 0.011 ms a turn, where a
  chained `setTimeout(0)` waits 4 ms). One pending at a time; an input
  from outside a turn schedules one; a turn at its bound (64 runs, 64
  batches) yields the same way.
- **The deadline:** an idle turn arms one `setTimeout` at it, replaced at
  the next idle turn. A hidden tab throttles it; a late timer only makes
  delayed sends late (bindings.md section 11), which the README says.
- **Continuations:** a handler that returns a thenable gets its view
  filled before the ack, and its continuation runs after the turn, on the
  same thread: the addon's rule, the same code path.
- **`idle()`**, errors after the turn, `close()` deferred from a handler,
  `SimulatedClock`: the same.
- **The clock:** `performance.now()` for the monotonic ticks,
  `performance.timeOrigin + performance.now()` for the wall time. A
  browser coarsens it (to 100 us without cross-origin isolation in
  Chromium: unverified), which only rounds the times core stamps.

## Memory

- **The environment's allocator** is the module's `ta_wasm_alloc` and
  `ta_wasm_free` (wasi-libc's `aligned_alloc` and `free`, as
  `tinyactors_runtime.wasm`). Every lifetime has its arena over it, as in
  the addon: the host's `ta_growable_arena` with a mark per call, the
  environment's for module-level calls, the datamodel's scratch arena for
  txml; the arena cap (256 KB a host, 64 KB the environment's,
  docs/allocation.md finding 7) is kept, so a freed chunk is reused by
  another host rather than kept by one.
- **Growth detaches every view** (spike: a `Uint8Array` over
  `memory.buffer` reads `byteLength` 0 after `memory.grow`, and
  `memory.buffer` is a new object). The layer keeps its views
  module-private and re-derives them when one reads `byteLength === 0`,
  after any export that can allocate and before reading memory in an
  import (a handler may have called the host and grown it). Nothing the
  application holds is a view of linear memory: images, telemetry,
  `metrics()`, `hibernate()` and `snapshot()` are copied once into a
  `Uint8Array` (`slice`), as the addon copies into a `Buffer`
  (`tinyactors-core/docs/boundary.md` section 7). `toResizableBuffer`
  would avoid the detaching, but node 22 and Chromium 141 lack it.
- **It never shrinks.** wasm cannot give memory back (core's
  `allocator_wasm.c`), so a page keeps its peak: hibernation makes room
  for other actors, not a smaller footprint (bindings.md section 11). A
  snapshot is packed into a buffer in linear memory (the probed size)
  before it is copied out, so a 100 MB snapshot leaves linear memory 100
  MB larger; the README says so.
- **No `napi_adjust_external_memory`:** linear memory is one
  `ArrayBuffer` the engine already counts.

## How values cross

- **Payloads of the native datamodel are never encoded**, as in the
  addon: a payload is a row of core's `ta_table` in C, its value at the
  row's slot of a JavaScript array; `capture`, `retain`, `release` and
  the input calls cross as handles. `send.data` and `invocation.data` are
  copied into the application's realm on first read (the addon's
  `exported()`, `l-p4-examples-chat-realm`), an XML node as its text.
- **CBOR crosses as tokens.** Where core takes or gives CBOR (the config,
  a typed method's params and its reply, `encode`, `decode`, the
  datamodel's `to_wire`, `from_wire`, `serialize`, snapshots), C holds a
  `ta_cbor_reader` or `ta_cbor_writer` and JavaScript pulls or pushes one
  token per call: the kind and its scalar in the layer's record, text and
  bytes through the call's arena. A reply of 200 tokens costs about 200
  crossings (1 to 2 us) and its strings: the control plane, not the hot
  path. Not chosen: a CBOR decoder in JavaScript over the bytes (a second
  codec beside core's, with its own validation; the boundary-design
  skill's rejected "a language layer ... decoding values").
- **Text in** is copied into the call's arena: ASCII by a loop, else
  `encodeInto`, after `isWellFormed` refuses a lone surrogate (the
  addon's rule). **Text out** is decoded when read: an effect's fields on
  first access, `_event`'s name from the chart's interned names by
  core's `name_id` (the addon's sharing), so an event name is decoded
  once per chart, not per event.
- **Ids** are `BigInt` (`i64` crosses as a `BigInt`), sessions accepted
  as safe integers too, as the addon.

## The native datamodel without node:vm

The addon evaluates charts in `node:vm` contexts, never the
application's realm: one shared context per host for charts that declare
nothing at the top of their scripts (a scope object per session), a
context per session for charts that declare (AGENTS.md, "The native
"ecmascript" datamodel"). A browser has no `node:vm`.

**The options for a realm** (a global of its own):

| Option | A page | A Worker | node, bun | Cost | Verdict |
|---|---|---|---|---|---|
| a same-origin iframe | yes | no `document` | no | 608 KB resident, 3.6 ms | the host's realm on a page: one per host |
| a Worker per realm | — | — | — | — | refused: the vtable is synchronous, and a page's thread cannot `Atomics.wait` |
| `ShadowRealm` | absent (Chromium 141) | absent | node absent, bun present | — | refused: not there, and only primitives and callables cross it |
| `new Function` in the application's realm | yes | yes | yes | nothing | the realm of a Worker, which has no other; not on a page, where the iframe costs one per host |
| `node:vm` | no | no | yes | the addon's | the host's realm in node and bun (`process.getBuiltinModule`) |

**The options for a declaring chart's session** (its declarations kept
between scripts):

| Option | Bytes a session | Semantics against the addon | Verdict |
|---|---|---|---|
| a realm per session (iframe; `node:vm` in node and bun) | 608 KB (iframe), 60 to 143 KB (`node:vm`) | the same: global code of its own | for charts whose top-level scripts declare with `let`, `const` or `class`, or start with `'use strict'` |
| a generator per session in the host's realm | 245 B | `var` and `function` the same, `throw 0;` instantiation the same, hibernation by name (`eval(name)`, as the addon's `(0, eval)(name)`); differs: two reserved names, and an undeclared sloppy assignment lands on the host's realm, shared by its sessions and not hibernated (the addon's shared charts already behave so) | for charts whose scripts declare with `var` or `function` only |
| refuse declaring charts | — | the addon's tests of declaring charts fail | no |

**Recommended** (`for-maintainer`, choice M2 and M3 below): the host's
realm is an iframe on a page, `node:vm` in node and bun, and the worker's
own global in a Worker; a chart that declares nothing gets a scope per
session, exactly the addon's code path; a chart that declares with `var`
or `function` only gets a generator session in the host's realm; a chart
that declares with `let`, `const` or `class`, or whose declaring script
is strict, gets a realm per session. Which kind a chart is, is asked of
the engine at `chart_attach` as the addon asks it (the scripts after
`throw 0;` in a scratch realm: the global's new names, and a
redeclaration error for lexical ones). The suite has one declaring chart,
a `var` (`grep` of `tinyactors-test/suites`), and the addon's tests have
`let` and `const` ones (`test/datamodel.test.mjs:148`,
`test/hibernation.test.mjs:274`), so both paths are exercised.

**CSP.** The native datamodel evaluates chart text, so it needs
`'unsafe-eval'`; the module itself needs `'wasm-unsafe-eval'` (spike,
"Scheduling, Workers and CSP"). A host on the native datamodel tries the
realm's `Function` at creation and is refused with `TA_ERROR_UNSUPPORTED`
when the policy forbids it, the message naming the policy;
`compileCharts`'s syntax check of `"ecmascript"` charts
(`l-p4-examples-iot-syntax-unchecked`) parses with the same `Function`
constructor (nothing runs) and is skipped under such a policy, which the
README says. quickjs (p5-quickjs) is the datamodel for a strict policy,
in a second module that a page without it does not download (bindings.md
section 11).

**Observation, out of scope:** the generator works in `node:vm` too, so
the addon could give a `var`-declaring session 245 B instead of a
context of 60 to 143 KB. It is not proposed here.

## Lifetimes

| Object | Owner | Created | Read by, when | Ends | Thread |
|---|---|---|---|---|---|
| a view of linear memory (`Uint8Array`, `DataView`) | the layer, module-private | at first need, again when it reads `byteLength` 0 | the layer, during a call | the next growth (detached) | the module's |
| an input's text and record | the call's arena | the method writes it above its mark | core, during the input call | the release to the mark at the method's exit, every path | the host's |
| an effect's text field | core | the take, in C | a thin view, decoded on first read | the ack; the view then throws `TA_ERROR_STALE` | the host's |
| a thin view (`send`, a log entry, `Migrated`) | the handler | the dispatch import | the handler | the ack, by the counter | the host's |
| the record of a promise-returning handler | the continuation | filled before the ack | after the ack | the collector | the host's |
| a host output (traces, logs, metrics, blob) | core | a take | copied once into a `Uint8Array` | the ack; the copy is the application's | the host's |
| a snapshot | a buffer the layer lends in linear memory | `snapshot()`, of the probed size | copied out | freed after the copy | the host's |
| a compiled image | the compile result in linear memory | `compileCharts` | `image` copies it once, on first read | `close()` (`ta_compile_free`) | the creating one |
| a payload's value | a `ta_table` row in C, the value at its slot in JavaScript | capture, `from_wire`, an input | the datamodel's imports; `send.data` copies it on first read | reference count 0, the drop deferred past the outer call | the host's |
| the datamodel's last output (`eval_string`, `log`, `to_wire`, `serialize`) | a buffer the C layer keeps | JavaScript writes it during the import | core | the datamodel's next call | the host's |
| a reply's CBOR | core | the `REPLY`'s dispatch | the layer's reader, token by token, into values | the ack | the host's |
| a pending typed method's handlers | the slot of its `ta_table` row | the method's call | the `REPLY`'s dispatch | its dispatch, or the host's free (dropped, never called) | the host's |
| the host's realm (iframe, context) | the host | at creation | its sessions | `close()` removes the iframe | the host's |

## One API

- **One `index.d.ts`** for both packages. What it says of bytes the host
  hands out becomes "a `Uint8Array`" (the addon's are `Buffer`s, a
  subclass), and `HostOptions.datamodel`'s note names the realm per
  runtime; nothing else in it is Node's (`grep Buffer index.d.ts`: lines
  9, 176, 643, 1118, 1127, 1150).
- **The same exports,** `directoryResolver` included: in the wasm package
  it uses `node:fs` and `node:path` through `process.getBuiltinModule`
  where they exist (node, bun) and throws `TinyactorsError` with
  `TA_ERROR_UNSUPPORTED` at the call where they do not
  (`l-p3-shapes-browser`).
- **The convenience layer is one file.** `lib/convenience.mjs` imports
  `#direct`, a subpath import each package's `package.json` maps to its
  direct layer (the addon's `lib/direct.mjs`, the wasm package's own),
  in place of `./direct.mjs`; the lint rule follows.
- **The same test files.** They import `@tinyactors/node` by name and
  their own harness (`test/harness.mjs`), never `node:test`. In node and
  bun the wasm package answers that name under an export condition the
  test task passes (`--conditions=tinyactors-wasm`, both runtimes take
  it), which the staged package drops. In Chromium a page with an import
  map runs each file, the harness reporting without `process`. A file or
  case that needs `node:fs`, `node:child_process`, `node:worker_threads`,
  quickjs or the collector's hooks is excluded with its reason.
- **The suite** runs through an adapter speaking the driver protocol as
  `conform.mjs` does, in node and in Chromium (p5-datamodel).
- **A parity check** (p5-parity): the two packages' exports compared
  property by property, and `index.d.ts` type-checked against both.

## Packaging and releases

**Recommended** (`for-maintainer`, choice M1): **a sibling package,
`@tinyactors/wasm`**, made from `tinyactors-napi/` as the platform
packages already are (`mise/pack.rb` stages `@tinyactors/node-<platform>-<arch>`
from this directory): the same version, the same `CHANGELOG.md` and tag
(`tinyactors-napi/v<version>`), released together and mirrored to
`tinyactors-dev/tinyactors-napi` (ADR 0016 names a release repository
after its directory). Its files: `index.mjs` (the entry, loading the
module with top-level `await`), the layer's JavaScript, the module,
`index.d.ts`, and its `package.json` with `"type": "module"` and plain
`exports`, `"./tinyactors.wasm"` among them so a bundler or a CDN can
place it. Its sources live in `tinyactors-napi/wasm/` (`src/*.c`,
`lib/*.mjs`).

- Not one package with conditional exports (`"browser"` to the wasm
  build): a browser project would install `@tinyactors/node` and,
  through its optional dependencies, the developer machine's addon, and
  the name says Node.
- Not a directory and component of its own: one API means one version
  and one changelog, and ADR 0016 would give it a release repository of
  its own for no reader.

**Loading** (`for-maintainer`, choice M4): the entry awaits the module at
the top level (`new URL('./tinyactors.wasm', import.meta.url)`,
`instantiateStreaming` in a page, `node:fs` for a `file:` URL), so
`import { Host } from '@tinyactors/wasm'` is the same line as in Node,
with no `load()` of our own. The old web binding did the same (its
README: "top-level `await`, `instantiateStreaming`"); staticjs-js has an
explicit `load()` instead, which this API would have to add to the
addon's too.

## Node.js and Bun

The wasm package must run in node and bun anyway: the shared tests run
there (p5-api). So it serves them at no extra cost:

- **Supported there,** with its realm from `node:vm`, and the way on a
  platform without an addon (Windows). `@tinyactors/node`'s error for a
  missing addon (`lib/direct.mjs`) names `@tinyactors/wasm`.
- **No silent fallback** inside `@tinyactors/node`: an application would
  change its memory profile and lose quickjs (until p5-quickjs) without
  asking.
- **Whether one implementation can go** (`for-maintainer`, choice M5):
  after p5-parity, `mise run measure` and the per-event benchmark of
  AGENTS.md run against both packages on node and bun side by side. If
  the wasm package is within 1.5 times the addon per event and 1.25 times
  its resident bytes per actor, propose retiring the addon (the work of
  two implementations is the largest cost here); else keep both and
  write it down.

## Not chosen

- **emnapi** (the addon over an emulated Node-API): the maintainer's
  decision (2026-10-17).
- **JavaScript reading core's structs at wasm32 offsets** (the old web
  binding): ADR 0009 keeps structs in C.
- **The drain in JavaScript over exported take and ack:** a handler could
  then nest a drain or skip the ack; with only `ta_web_turn` exported it
  cannot. The crossings cost the same either way (5 to 11 ns).
- **The release's `tinyactors_runtime.wasm` as it is:** it imports
  nothing, so the vtable could reach JavaScript only through a generated
  module in its function table, and it exports core's API, which
  JavaScript must not call by struct. The binding builds its own module
  from the release's sources (bindings.md section 11 says to link the
  release's libraries; it does).
- **A JavaScript CBOR codec:** a second codec ("How values cross").
- **`DOMParser`** for XML content: absent in Workers, another DOM.
- **A shared `ArrayBuffer` or `toResizableBuffer`** to avoid detaching:
  not in node 22 or Chromium 141, and a shared memory needs cross-origin
  isolation.

## Choices for the maintainer

Each is logged `for-maintainer` in docs/log.xml with this recommendation.

- **M1, the package** (`l-p5-design-package`): a sibling
  `@tinyactors/wasm` from `tinyactors-napi/`, same version, changelog,
  tag and release repository. Alternatives: conditional exports of
  `@tinyactors/node`; a component of its own.
- **M2, declaring charts** (`l-p5-design-declaring`): a generator session
  for `var` and `function` (245 B), a realm per session for `let`,
  `const`, `class` or a strict script (608 KB in a browser).
  Alternatives: a realm per session for every declaring chart (exact,
  608 KB each); refusing lexical ones.
- **M3, the host's realm** (`l-p5-design-realm`): an iframe on a page,
  `node:vm` in node and bun, the worker's own global in a Worker.
  Alternative: refuse the native datamodel in a Worker until quickjs.
- **M4, loading** (`l-p5-design-loading`): top-level `await` in the
  entry. Alternative: an explicit `load()`, in both packages.
- **M5, two implementations** (`l-p5-design-retire`): decide at
  p5-parity on the measured ratios above.
- **M6, size against speed** (`l-p5-design-optimize`): `-O2`, the
  release's flags, now; `-Oz` (19 % smaller) measured for speed in
  p5-pack before choosing.

## Refutation

Pending. This step had no way to spawn a fresh agent (no Agent tool in
its session), so the coordinator dispatches the refuter. Its verdicts,
each recommendation marked confirmed, plausible or dropped, go here, and
only then the plan's next steps are refined.

## The steps after this one

Proposed refinement of plan.xml, applied after the refutation:

- **p5-build:** `wasm/src/` (the C layer: the host and its arenas, inputs,
  `ta_web_turn` with the drain, effect accessors, the CBOR token calls,
  the vtable's forwarding, txml's tree record), compiled with core's and
  txml's amalgamations by `Toolchain::Target` (zig, wasm32-wasi reactor),
  its imports and exports checked; its lint rules (the prefix, three
  parameters, buffers, no mutable globals); the loader for a page and for
  node and bun; the initial memory set and measured; the size reported.
  Done when a host is made, runs a null-datamodel chart and is freed in
  Chromium and node.
- **p5-api:** the direct layer in JavaScript over it, every item of
  docs/api-map.md with the addon's names, shapes, errors and lifetimes,
  `datamodel: "quickjs"` refused with `TA_ERROR_UNSUPPORTED`; `#direct`
  for the shared convenience layer; the test files run against the wasm
  package in node, bun and Chromium, exclusions with reasons; the
  per-event benchmark beside the addon's.
- **p5-datamodel:** the realms and sessions as M2 and M3 decide, with
  `realm.js` shared; XML content through txml; the CSP refusal; the
  compile check; hibernation, migration and snapshots; the suite through
  an adapter in node and Chromium.
- **p5-parity, p5-pack, p5-quickjs:** as planned, with M1, M4, M5 and M6
  applied; p5-quickjs as a second module.

## Reproducing

From `tinyactors-napi/tools/spikes/browser/`, with the root's tools
(`mise which zig`, `mise which node`, `mise which bun`):

```sh
zig cc -target wasm32-freestanding -O2 -nostdlib -Wl,--no-entry calls.c -o calls.wasm
node drive.mjs; node drive2.mjs; node drive3.mjs     # Chromium (Playwright from npm root -g)
node run.mjs; bun run.mjs; node vmgen.mjs; bun vmgen.mjs
D=../../../../tinyactors-core/dist
zig cc -O2 -std=c11 -w -I$D perevent.c $D/tinyactors.c $D/tinyactors_runtime.c $D/tinyactors_runtime_hosted.c -o perevent
zig cc -target wasm32-wasi -O2 -std=c11 -w -I$D perevent.c $D/tinyactors.c $D/tinyactors_runtime.c $D/tinyactors_runtime_wasm.c -o perevent.wasm
./perevent; node --no-warnings runwasi.mjs perevent.wasm; bun runwasi.mjs perevent.wasm
node sections.mjs <module.wasm>; node strip.mjs <in.wasm> <out.wasm>
```
