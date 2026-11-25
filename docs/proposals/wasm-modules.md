# A system as a WASM module

**Status:** Sketch (2026-11-25). An outline for discussion; nothing here
is decided or scheduled.
**Touches:** tinyactors-core (a new freestanding layer that writes
modules, a WASM platform file beside `src/base/allocator_wasm.c`, the
release's WASM builds, `tinyactors compile`), tinyactors-test (an adapter
for the module).
**Not a plan:** ADRs and roadmaps win.

The question, in the maintainer's words: can one system be compiled to a
WASM reactor, can single actors be compiled to modules of their own, how
do the modes differ, and can the runtime be "compiled out", so that what
comes out is a plain WASM module whose external dependencies are
functions it imports. The entry point should be in the core library, and
the `tinyactors` command is where to start.

The short answer: **yes, and without a C compiler on the user's
machine.** The release builds a runtime template, a `.wasm` file. A small
freestanding writer in core copies it and adds the images and the host
config as a data segment. A spike did this (section 2.3). The unit is one
system (one host and its segments) per module instance. A module per
chart is a way to deploy, not a mode of its own. Lowering charts to WASM
code ahead of time would buy little until the datamodel is the cost
(section 5).

## 1. Whom it serves, what exists, who pays, what is out

**First, the C embedder whose host is a WASM runtime.** That means
wasmtime or wasmer in a server, a plugin system, an edge runtime, a
database with WASM functions, or a game engine with WASM scripting. They
ask: what do I load, what must I provide, what does it cost me per event
and per instance? They decide which runtime loads it and what it may
reach. They need one file, a short list of imports with core-wasm
signatures, and no C toolchain. **Then the application developer:** they
want to ship "the order system" as one artifact that is tested, versioned
and hashed, and to run it anywhere a WASM runtime runs.

**What already answers them** (section 2): `tinyactors.wasm` (core,
imports nothing), `@tinyactors/wasm` (a whole binding as a reactor with
five imports), and `tinyactors compile --emit c` (an image as a C array
for ROM, the precedent for `--emit wasm`). What none of them gives: a
module that already holds its charts and config, with a loop that needs
no binding.

**Who pays:**

| Cost | How much | Evidence |
|---|---|---|
| per event | nothing in core: the same code, in WASM; core's part costs 1.1 to 2 times native in WASM; in the driver profile, one import call per effect (5 to 11 ns per crossing in `@tinyactors/wasm`) | `tinyactors-napi/docs/browser.md:200-216`, `:80-83` |
| per actor | nothing: the same region | — |
| per module instance | about 1.06 MiB of linear memory at start (17 pages, of which 1 MiB is the stack the linker reserves), and the image twice (its data segment, and the host's copy at `ta_host_add_image`) | spike below; `include/tinyactors/host.h:142-149` |
| code size | a writer of a few hundred lines in a new layer (unverified), and a WASM platform file; the `tinyactors` binary grows by the templates it embeds (316 KB stripped for the null datamodel; several times that with quickjs) | spike below |
| API surface | one function and one struct in a new header; one CLI option; **the template's exports and imports, which become an ABI** to keep versioned like `TA_ABI_VERSION` | — |
| work in every binding | none: no binding changes; the module is another embedder of `ta_host` | — |
| the release | two to four more WASM assets, built as `tinyactors.wasm` is built now (`mise/lib/c_release/build.rb:364-386`) | — |

**Out of scope:** lowering ECMAScript to WASM; the component model as
the main interface (it is an optional layer later, section 6); WASI as a
requirement; threads and shared memory (a host has one owning thread,
`tinyactors-core/docs/embedding.md` section 9); moving actors between
modules (an actor does not move between hosts, `GLOSSARY.md`, "Export,
adopt"); run-time fetching (`TA_EFFECT_REQUEST` stays answered with
`TA_ERROR_INVALID`, `embedding.md` section 4).

## 2. What exists today

### 2.1 The WASM builds

| Artifact | Built by | Target | Imports | Exports | Size (measured 2026-11-25, `mise run wasm:test`) |
|---|---|---|---|---|---|
| `tinyactors.wasm` (base to host, OTLP, BasicHTTP) | `CRelease.build_wasm` (`mise/lib/c_release/build.rb:364-386`), `tinyactors-core/mise/tasks/wasm/test` | wasm32-freestanding, `-O2 -g0` (`mise/lib/toolchain/target.rb:216`) | none (checked: `build.rb:460-469`) | the 196 API functions, `memory` | 450,156 B; gzip -9 160,378; brotli 126,554 |
| `tinyactors_runtime.wasm` (with the compiler and runtime) | same | zig's wasm32-wasi, `-mexec-model=reactor`, wasi-libc linked in | none | 218 functions, `memory`, `_initialize` | 1,450,682 B |
| `tinyactors_datamodel_ecmascript.wasm` (with quickjs) | same | same; the WASI calls of wasi-libc are defined inside the module (`src/datamodel_ecmascript/browser_wasm.c`) | none | 211 functions, `memory`, `_initialize` | 2,586,233 B |
| `@tinyactors/wasm`'s `tinyactors.wasm` (core, compiler, quickjs, txml, the binding's C layer) | `tinyactors-napi/mise/wasm.rb` | wasm32-wasi reactor, `-Oz`, stripped, 384 KiB stack | `ta_web.clock`, `.datamodel`, `.effect`, `.output`, `.resolve` (`wasm.rb:62`, `wasm/src/web.h:230-280`) | the layer's `ta_web_*`, `memory`, `_initialize` | 1150 KiB, 378 KiB brotli (`tinyactors-napi/wasm/README.md`, 2026-10-18) |
| core's test programs | `mise run check:wasm` | wasm32-wasi commands under wasmtime | WASI | `_start` | — |

What they share:

- **The allocator is inside the module.** `ta_wasm_alloc` and
  `ta_wasm_free` (`include/tinyactors/wasm.h`) are a bump pointer with
  free lists above `__heap_base` (`src/base/allocator_wasm.c`), or
  wasi-libc's `malloc` in the modules that link a C library
  (`src/runtime/allocator_wasm.c`, `src/datamodel_ecmascript/allocator_wasm.c`).
  ADR 0001, decision 6, and decision 19's "the WASM modules are for
  browsers, and import nothing" (`docs/adr/0001-packaging-and-releases.md:36`, `:255`).
- **A datamodel is a vtable of C function pointers**
  (`include/tinyactors/datamodel.h`, `ta_datamodel_vtable`), bound in
  `ta_host_environment.datamodels` (`host.h:115-132`). In WASM a function
  pointer is an index into the module's table. So a vtable is either C
  inside the module (`ta_datamodel_null()`, quickjs's) or C entries that
  call an import. `@tinyactors/wasm` does the second for its native
  datamodel, `ta_web.datamodel(host, entry, slot)` (`web.h:276-279`).
- **The core has no clock, no randomness and no I/O.** Time enters
  through `ta_host_advance_time`. Randomness enters through
  `ta_host_environment.nonce` and the config's `system.traceSeed`. quickjs
  reads the host's clock with `TA_DATAMODEL_ECMASCRIPT_CLOCK_HOST` and
  draws `Math.random` from `random_seed` (`embedding.md` section 1). I/O
  is effects.

### 2.2 The CLI's precedent

`tinyactors compile --emit c --symbol NAME` writes an image as an
8-aligned C array (`tools/tinyactors/compile_command.c:28-48`,
`tinyactors-core/docs/cli.md:140`). The file is written whole or not at
all, and the answer reports size, hash, charts and boot actors. `--emit
wasm` fits beside it: `TOOL_EMIT_*` and the `emits` choice
(`tools/tinyactors/program.c:46-55`).

### 2.3 Spike: a module from a template and an image, with no compiler at emit time

In `/tmp/claude-0/-home-user-tinyactors/48aea8a4-edf2-5799-91e9-403c27e0dc94/scratchpad/wasm/`:

1. `compile.mjs` compiled an order chart (null datamodel: `placed`, then
   `paid` on `payment.captured` with a `<log>`, then final `shipped` on
   `shipment.sent`) with the release's `tinyactors_runtime.wasm` in Node.
   The result is a 1,384-byte image.
2. `reactor.c` is a template. It has a 16-byte descriptor
   `ta_wasm_embedded` `{magic, offset, length}`, exported (wasm-ld exports
   a data symbol as an immutable `i32` global holding its address). It
   has an allocator that starts past `max(__heap_base, offset + length)`,
   and an import `ta.effect(kind, session)` called per effect while it
   drains. Its exports are `order_start` (`ta_host_create`,
   `ta_host_add_image` of the embedded bytes, `ta_host_chart`,
   `ta_host_spawn`, run, drain) and `order_send(which)`. It was linked once
   with the release's `tinyactors.o`: **316,350 B stripped, 92,765 B
   brotli**.
3. `patch.mjs` is the writer. It is about 50 lines and rewrites only the
   memory and data sections. It appends the image as an active data
   segment at the end of initial memory, raises the memory's minimum by
   the pages the image needs, and fills the descriptor's bytes in place.
4. `run.mjs` instantiated the result (317,743 B) in Node with the one
   import:

```text
order.wasm: image of 1384 bytes at 1114112, memory 17 -> 18 pages
start 0
payment.captured -> run status 0 LOG(1)
shipment.sent -> run status 0 DONE(1)
```

Two more measurements from the same spike:

- **Exports decide the size.** The same objects linked with only the 22
  loop functions exported (create, add_image, chart, spawn, send,
  send_many, terminate, resume, advance_time, run, the effect accessors,
  ack, resolve, boot, actor_by_name, the allocator) come to 322,369 B
  stripped and 94,921 B brotli, against 450,156 B for all 196. The linker
  drops what no export reaches.
- **The layout id differs between wasm32 and x86_64.**
  `ta_system_layout_description` answers `a6074407f1e18bab` in
  `tinyactors.wasm` and `fb87ca7efbf61d87` in a native build of the same
  amalgamation (22,050 B against 22,070 B: the region's "pointer" members,
  `src/system/region_layout.c:15-16`). So a module's snapshot restores
  only in a wasm32 build of the same layout, and a native host's snapshot
  never restores in a module.

Not tried in the spike: wasmtime (only Node ran it), quickjs in a
template, and a writer in C.

## 3. The frame

What the design rests on. Each fact is checked:

- **One host, one owning thread** (`embedding.md` section 9). A module
  instance is one host; its embedder calls it from one thread.
- **The loop is take, handle, ack, run.** Everything handed out lives
  until the ack. A run refuses while a batch is open (`host.h:70-89`,
  ADR 0012).
- **Inputs are C structs, read during the call.** No serialization on the
  hot path (root `AGENTS.md`). CBOR is for the config, `ta_host_post` and
  bytes that leave the process.
- **Images are copied by `ta_host_add_image`** and read only during the
  call (`host.h:142-149`).
- **Values for another system are the application's to encode**
  (ADR 0014). A payload is a handle the datamodel owns.
- **A WASM module's memory is its own.** An export or import passes
  numbers (`i32`, `i64`, `f32`, `f64`). Bytes cross only as offsets into
  the module's linear memory, which the embedder reads or writes.

## 4. The mappings

### 4.1 One system, one reactor module (recommended)

**What it is.** One module holds one host: the runtime, the host
config (CBOR) and one or more images, one per segment, oldest first, so a
module can carry several versions of a chart. It has no `_start`. It
initializes with `_initialize` where a C library is linked, and is driven
through exports. The region, the host's tables and the datamodel's heap
live in linear memory. The module embeds the region's capacities in its
config, not as a fixed block: the host grows segments through its
allocator (`host.h:53-61`), and memory grows a page at a time.

**Two profiles of the same template family:**

- **Loop profile.** It imports nothing, and its exports mirror the host
  loop. The embedder writes the drain itself, as
  `tests/wasm/release_test.mjs` does with `tinyactors.wasm`. This is
  `tinyactors.wasm` with its charts inside, plus one export that makes
  the host from the embedded config and images, and the dead code
  stripped. It is the first step, because every existing test style can
  drive it.
- **Driver profile** (what the maintainer asked for). Core's drain runs
  inside the module, and each effect becomes a call of an import: the
  "external dependencies are functions it imports" shape. The embedder
  provides functions and calls a `turn`. It is the same idea as
  `@tinyactors/wasm`'s `ta_web.effect`, but language-free and in core.

**What it costs.** About 316 KB stripped (about 93 KB brotli) for the
null datamodel. Per instance: 17 pages at start, of which 1 MiB is the
default stack, which a template can lower with `-z stack-size`, as
`tinyactors-napi/mise/wasm.rb:52` does (384 KiB, measured for quickjs).
How low core's own stack can go is unverified; `wasm:stack` in napi
measures it. Add the image twice. Startup is instantiation plus
`ta_host_create` plus `ta_image_open`, which checks every record once
(`docs/architecture.md` section 3.2). That time is unmeasured.

**What it enables.** Every core feature in one instance: routing between
segments without a round trip, versions and migration among the
module's images, hibernation and wake, snapshots, tracing and OTLP, the
control plane.

**Limits.**

- A snapshot restores only in a module of the same template build (the
  layout id, section 2.3). That is already true of every build.
- A new version needs the module to export `add_image`, or a new module
  restored from the old one's snapshot with the new image added, then
  migrated. That second path works only when both modules come from the
  same template build.
- Linear memory never shrinks (`src/base/allocator_wasm.c` header).

### 4.2 One chart, one module

**What it means.** Compile each chart alone with `--emit wasm`, each with
its own `systemID`. Nothing new is needed in core: it is 4.1 applied per
chart.

**What is shared.** Nothing at run time. Each module has its own copy of
the runtime (about 316 KB). WASM has no shared code between instances
without a dynamic-linking convention, which this repository does not use
(ADR 0001, decision 17's reasoning: "a module cannot call into another
module's linear memory", `docs/adr/0001-packaging-and-releases.md:197-200`).
An embedder can instantiate one compiled module many times, which shares
the compiled code in the engine but not linear memory.

**Actors per instance.** As many as the region holds: a module per chart
is still a system with many actors of that chart. **A module instance
per actor is the wrong grain.** An instance starts at about 1 MiB. A
resident actor costs about 120 B in the region in core's sizing example
(100,000 actors of a 64-state chart in about 12 MB,
`docs/architecture.md` section 13). That is four orders of magnitude.
Hibernation already gives "one actor, one blob".

**How modules talk.** Through the transport. A send to another system's
address is `TA_EFFECT_SEND` with `io_processor` 0. The embedder resolves
the target in the other instance (`ta_host_resolve`), and the other
instance's `send` takes the data as CBOR: the sender's datamodel's
`to_wire`, then the receiver's `from_wire` (`datamodel.h`, `to_wire` and
`from_wire`; ADR 0014). This is what two processes do today
(`tinyactors-napi/examples/inventory-sync/`). Within one module, sends
between segments cost no round trip.

**What it gives up.** In-host routing, migration between those charts,
and one snapshot of the whole application. It suits charts that are
separate services anyway: a payment provider's adapter and a checkout
are different teams' deployments.

### 4.3 Compilation modes: interpret the image, or lower it to WASM code

**Interpreter plus embedded image** (4.1 as written): core's
continuation machine reads the image's records and bytecode
(`docs/architecture.md` sections 3 and 7-8).

**Ahead of time, the statechart lowered:** for each chart, the writer
would generate WASM functions for transition selection, exit and entry
sets, and each executable-content block, in place of reading records.
That is a WASM code generator inside core. None exists in this
repository: the writer of 4.1 only copies sections.

**Ahead of time, the datamodel lowered too:** expressions become WASM
functions.

- For the **null datamodel** this is small. Its only expression is
  `In('id')` (`src/core/datamodel_null.c:1-40`), so a condition becomes a
  bit test on the configuration, and its `<log>` passes a quoted literal
  through.
- For **ECMAScript** it is a JavaScript compiler; out of scope.
- **staticjs** (`docs/proposals/staticjs.md`) is the subset where it
  would be plausible: pure, total, no assignment. But staticjs today is a
  VM, with a register evaluator proposed (`staticjs-compiler.md`). There
  is no staticjs datamodel yet. A staticjs-to-WASM backend would be a
  proposal of its own.

| | Interpreter plus image | Statechart lowered | Statechart and datamodel lowered |
|---|---|---|---|
| code size | the template (316 KB null; MBs with quickjs) plus the image (KBs; 1.4 KB in the spike) | the template less the parts of core.c it replaces, plus generated code per chart (unmeasured; likely larger than the image per chart) | as before, without quickjs for a lowered language |
| startup | instantiate, create the host, `ta_image_open` checks the image | same; the image must still exist for its strings, records the host reads (`ta_host_chart`, inspection), and snapshots, which take images | same |
| per event | core's part: about 260-290 ns native, 310-530 ns in WASM, for a send, a run, a take and an ack on the null datamodel (`tinyactors-napi/docs/browser.md:200-216`) | saves part of core's part only: selection already uses precomputed domains and bitsets (`architecture.md` sections 3.5-3.6); the gain is unmeasured and bounded by core's share | saves the datamodel's cost, which dominates with ECMAScript (5.2 us for the native datamodel's whole event, of which core is about 5 %, same source) |
| snapshot, hibernation, migration | as any build of the same layout id | the region must not change, or the layout id changes; pcs must keep their numbering: a snapshot needs no migration and no open batch (`src/host/host.c:2760-2765`) but does not refuse a run that yielded mid-macrostep, and a continuation is an instruction plus `<foreach>` frames (`architecture.md` section 8), so a lowered build must keep those instruction numbers to restore one (plausible; settled by a test that snapshots a yielded run) | as before, and the datamodel's serialized state must stay the same format |
| debuggability | `tinyactors inspect`, `disassemble`, `graph` read the same image; one implementation of SCXML | a second implementation of SCXML's algorithm, to be held to the conformance suite separately | as before, plus generated code for expressions |
| tracing | unchanged: trace records come from the step (`src/core/trace_step.c`) | must emit the same records at the same points, or OTLP output differs | same |

**Verdict.** Keep one mode, the interpreter. Lowering earns its cost only
when the datamodel's work is lowered too. That waits for a staticjs
datamodel, a measurement of where its time goes, and a backend; none of
the three exists.

## 5. The import surface

Each external dependency the runtime has, and whether it becomes an
import, an export (an input) or stays an effect. Signatures use core-wasm
types. In the driver profile, each import declared in C takes at most
three parameters (root `AGENTS.md`). Where more is needed, the fields are
read back through exports, as thin views read them.

| Dependency | Today | In the module | Why |
|---|---|---|---|
| memory allocator | `ta_host_environment.allocator` | **none**: inside the module (`ta_wasm_alloc`), exported for the embedder's own bytes | ADR 0001 decision 6; a bounded allocator is backpressure (`embedding.md` section 8): the template can cap pages |
| monotonic and wall clocks | `ta_host_advance_time(now)` | **export** `advance_time(monotonic: i64, wall_ns: i64)`, before each turn | the host has no clock. An import `now()` would make every turn call out, and lose the simulated clock that `tinyactors run` and the tests rely on (`cli.md`, Run) |
| randomness: the host's nonce, the trace seed, quickjs's seed | `environment.nonce`, `system.traceSeed`, `random_seed` | **export** `start(nonce: i64)`; the seeds in the embedded config, or set at start | three numbers once; an import adds nothing |
| quickjs's `Date` and its C library's clock | `TA_DATAMODEL_ECMASCRIPT_CLOCK_HOST`, and `browser_wasm.c`'s WASI functions defined in the module | **none** | already inside the module |
| sends to I/O processors | `TA_EFFECT_SEND`, `io_processor` i + 1 | driver: **import** `tinyactors.send(index: i32) -> i32`; loop: the effect | an effect read in the drain; a non-zero answer fails the send before the ack (`TA_EVENT_IN_ERROR_COMMUNICATION`), as `@tinyactors/node` does when a handler throws |
| the transport (other systems) | `TA_EFFECT_SEND`, `io_processor` 0 | the same import; the handler reads `ORIGIN` and `TARGET`, and the data through an export that writes the payload's CBOR (`to_wire`) | the value is the application's to encode (ADR 0014); this is the process boundary, so CBOR here is allowed |
| services (external invocations) | `TA_EFFECT_INVOKE`, `UNINVOKE`, sends with `io_processor` `TA_NIL` | **imports** `invoke(index: i32) -> i32`, `uninvoke(index: i32)`; the service answers through the `send` export with `TA_EVENT_IN_FROM_INVOKE` | keyed by (segment, token), as every binding does (`host.h:346-350`) |
| `<log>` | `TA_EFFECT_LOG` | **import** `log(index: i32)` | — |
| an actor's end | `TA_EFFECT_DONE`, `TA_EFFECT_ERROR` | **imports** `done(index: i32)`, `error(index: i32)` | — |
| the hibernation store | `TA_EFFECT_WAKE`, `ta_host_wake`, `ta_host_hibernate` | **import** `wake(index: i32) -> i32`; the embedder answers with the `wake` export and the blob in linear memory | the blob is the application's to store; a synchronous answer inside the import is a call into the host while handling, which is allowed (`embedding.md` section 4) |
| telemetry | `TRACES`, `LOGS`, `ta_host_traces`, `_logs`, `ta_host_metrics` | **import** `output(kind: i32, pointer: i32, length: i32)`; the module takes the request on the effect, as `ta_web.output` does; metrics through an export | OTLP is bytes that leave the process |
| the control plane | `ta_host_post`, `REPLY`, `NOTIFICATION` | **export** `post(pointer: i32, length: i32)`, **import** `reply(pointer: i32, length: i32)` | rarely called; CBOR as today; no synchronous request |
| migration progress | `MIGRATED`, `MIGRATION_DONE`, `SEGMENT_FREED` | **import** `notice(kind: i32, index: i32)` | rare; one import for all three |
| a datamodel in the embedder's language | a vtable of C functions | only in the "imported datamodel" template: **import** `datamodel(entry: i32, slot: i32, record: i32) -> i32`, as `ta_web.datamodel` (`web.h:276-279`) | a datamodel's values belong to its language; the null and quickjs templates import nothing for it |
| compile-time `src` resolution | `ta_compile_options.resolve` | **none**: resolved by `tinyactors compile` before the module is written | a host has no resolver (`embedding.md` section 1) |
| run-time fetch | `TA_EFFECT_REQUEST` | **none**: the module answers `ta_host_resume` with `TA_ERROR_INVALID`, as the bindings do | decided later (root `ROADMAP.md`) |

All imports are in the module `tinyactors`. Each `index` is the effect's
place in the open batch. The handler reads it through exported accessors
over `ta_host_effect_number(host, index, field) -> i64`,
`ta_host_effect_text` and `ta_host_effect_bytes`, which already exist so
that a reader needs no struct layout (`host.h:335-378`). The module has
one host, so the exports omit it. A text comes back as two words in the
module's call record (below).

**What every import gets is valid until it returns.** The module drains
the batch, calls the import for each effect, and acks after the last.
Views live until the ack (ADR 0012), so "until the import returns" is the
conservative contract to promise. An embedder that keeps a field copies
it, as `@tinyactors/node`'s records do.

**Inputs with text** (send, spawn, fail, wake, post) take more than three
numbers. Like `@tinyactors/wasm`, they cross through a **call record**:
an array of words at an exported address, written by the embedder right
before the call and read by the export at its entry (`web.h:5-25`). The
words hold numbers and `(offset, length)` pairs into linear memory, never
a struct of core's. Inside the module they become `ta_event_in`,
`ta_spawn` and the rest, so the hot path stays structs and takes no CBOR
but a payload's data.

**Reentrancy.** An import may call the input exports (send, fail, post,
hibernate), as a handler may call the host. An import that calls `turn`
is refused: one drain at a time, no turn inside another call, as
`@tinyactors/node` refuses (`tinyactors-napi/AGENTS.md`, "The loop").

**Exports of the driver profile:** `_initialize` (templates with a C
library), `start(nonce: i64) -> i32`, `boot() -> i32`,
`advance_time(monotonic: i64, wall_ns: i64)`,
`turn(budget: i32) -> i32` (drain, run, drain, until idle or yielded;
returns the last `ta_run` status), `next_deadline() -> i64`, the inputs
through the call record (`spawn`, `send`, `fail`, `terminate`, `resume`,
`wake`, `hibernate`, `sweep`, `migrate`, `post`, `snapshot`,
`add_image`), the effect accessors, the payload's CBOR export, `alloc`
and `free`, and `memory`.

**Not imports, and why:**

- **No import per I/O processor type.** The template's imports are fixed
  when it is linked. A writer could rename import strings, but the code
  calling them cannot multiply. The handler switches on `IO_PROCESSOR`.
- **No effect record copied into the import's arguments.** The embedder
  reads the fields it needs through the accessors, with nothing
  converted eagerly, as with thin views (`GLOSSARY.md`).

## 6. Where it lives, and how it is built without a compiler

**In core, as a new freestanding layer** `src/wasm_module`, linked into
`libtinyactors_runtime` beside the compiler. It is not in
`libtinyactors`: the host does not need it, and core does not handle
bytes it has no use for. Like `src/cli`, it is freestanding, allocates
only through what it is given, and uses no recursion.

```c
/* tinyactors/wasm_module.h (sketch) */
typedef struct ta_wasm_module_input {
    uint32_t size;
    uint32_t image_count;
    ta_bytes runtime;          /* a template from the release */
    ta_bytes config;           /* the host config, CBOR (host-config.md) */
    const ta_bytes *images;    /* one per segment, oldest first */
} ta_wasm_module_input;
/* The module: the template with the config and images in a data segment
 * of its own, the descriptor filled, memory's minimum raised. Read during
 * the call; written into `out` (TA_ERROR_NO_MEMORY: `length` is what it
 * needs). TA_ERROR_INVALID: not a template of this library's ABI, an image
 * that does not open, a config the host would refuse. */
TA_API ta_status ta_wasm_module_write(const ta_wasm_module_input *input, ta_buffer *out);
```

The writer copies every section but three. It rewrites memory's
minimum, adds one active data segment (the config and the images,
8-aligned, after a header), and fills the descriptor that the template
exports as a global holding its address, in the data segment that holds
it. If the template has a data-count section, the writer raises it too.
It checks before it writes: each image opens (`ta_image_open`), and the
config is parsed as `ta_host_create` would parse it (how to reuse the
host's check without making a host is open). A malformed template is
refused, never written.

**What changes in existing code:** the WASM allocator starts at the end
of the embedded segment when there is one (`src/base/allocator_wasm.c`,
`carve`). For the templates that link wasi-libc, its `malloc` has to skip
the segment too; how is unverified (wasi-libc's heap starts at
`__heap_base`). Placing the segment at the top of initial memory and
moving the heap's start is the spike's way. A passive segment copied by
`memory.init` and dropped would avoid both the allocator change and the
second copy of the image. It needs a segment index fixed when the
template is linked, which the toolchain does not offer directly
(unverified).

**The templates.** The release builds them as it builds
`tinyactors.wasm` (`CRelease.build_wasm`), from the amalgamations and one
platform file per profile (`src/wasm_module/runtime_wasm.c`), with
exactly the profile's exports, stripped, and with their interface checked
by `verify_wasm!` against the profile's import list:

| Template | Datamodels | Imports |
|---|---|---|
| `tinyactors_module_loop.wasm` | null | none |
| `tinyactors_module_driver.wasm` | null | the `tinyactors.*` imports above |
| `tinyactors_module_driver_ecmascript.wasm` | null, quickjs | the same (quickjs's WASI defined inside, as `browser_wasm.c` does) |
| later: `…_driver_imported.wasm` | null, one imported datamodel | the same, plus `datamodel` |

Each template carries its ABI version in a custom section that the writer
checks.

**The CLI.**

```sh
tinyactors compile order.scxml payment.scxml --emit wasm \
    --config orders.json -o orders.wasm      # the driver profile, null or quickjs by the charts' datamodel
tinyactors compile system.xml --emit wasm --profile loop -o orders.wasm
tinyactors compile ... --emit wasm --runtime my-template.wasm -o orders.wasm
tinyactors inspect orders.wasm               # its template, ABI, config and images, as for a snapshot
```

`--config` takes the JSON that `tinyactors run --config` takes
(`cli.md`, Startup), with the same `systemID` default rule. The command
embeds the templates of its own release, so no C compiler and no download
is needed. `--runtime` takes another template of the same ABI. The answer
adds the module's size, its template and its imports. The null templates
add about 0.3 MB each to the binary; the quickjs one adds more
(unmeasured at `-Oz` alone; napi's module with quickjs, txml and its own
layer is 1150 KiB). Whether the CLI embeds the quickjs template or reads
it from the release is open.

**WASI: not needed.** The null templates are wasm32-freestanding, and
the quickjs template defines wasi-libc's few WASI calls inside itself, as
the release's modules do now. A WASI *command* (`_start` reading
JSON-RPC on standard input) would be `tinyactors run` built for
wasm32-wasi. That is a separate artifact, and whether `tools/tinyactors`
builds for WASI is unverified: `check:wasm` leaves `test_tool` out
(`mise/tasks/check/wasm:13-15`).

**The component model and WIT: a later, optional layer.** The driver
profile's imports and exports map onto a WIT world (`tinyactors:host`
with an `effects` interface imported and a `host` interface exported).
Building a component from the core module is then a tool's job outside
core. Which tool, and how its adapters handle the call record, is
unverified. Nothing in core should depend on it.

**Multi-memory and shared memory: not relevant.** One host has one
linear memory and one thread. Shared memory would need another thread to
call the host, which the frame excludes.

## 7. Lifetimes

| Object | Owner | Created | Read by, when | Ends | Thread |
|---|---|---|---|---|---|
| template bytes, images, config (writer input) | the caller | before `ta_wasm_module_write` | the writer, during the call | when the call returns | the caller's |
| the written module | the caller's `ta_buffer` | the write call | the caller, any time | the caller frees it | the caller's |
| the embedded segment (config, images) | the module instance | instantiation | `start` (config, images to `ta_host_create` and `ta_host_add_image`, which copy them); `snapshot` and restore (images) | never: linear memory does not shrink | the instance's |
| the host and its regions | the module, through `ta_wasm_alloc` | `start` | every export | never freed while the instance lives (no `ta_host_free` export is needed: the instance is dropped whole) | the instance's |
| an effect's fields, its text, a payload's CBOR, an OTLP request | the host | the drain inside `turn` | the import, during its call | at the ack after the batch's last import (promised: until the import returns) | the instance's |
| the call record's words and the bytes they point to | the embedder writes them in linear memory (`alloc`) | right before an input export | the export, at its entry (core copies the views, `embedding.md` section 3) | the export's return; the embedder frees what it allocated | the instance's |
| a hibernation blob, a snapshot | the host, then the embedder | `hibernate`, `snapshot` | the embedder copies it out of linear memory | the ack, as today | the instance's |

## 8. Recommendation and order of work

**Recommended:** mapping 4.1, one host per module instance, with the
interpreter. Build it as a template from the release plus a
freestanding writer in core, so `tinyactors compile --emit wasm` needs
no compiler. Offer the loop profile first, because every test can drive
it, then the driver profile, the import-shaped module the maintainer
described. A module per chart (4.2) comes free and needs no work. Lowering
(4.3) is not recommended until a staticjs datamodel exists and is
measured.

Each step is a commit that passes `mise run test` in core:

1. **The loop template and the allocator change.**
   `src/wasm_module/runtime_wasm.c` (descriptor, `ta_wasm_module_start`
   from the embedded config and images) and `allocator_wasm.c` starting
   past the segment. Built in `wasm:test` with the loop exports only.
   Test: `tests/wasm/release_test.mjs` grows a case that patches an image
   in (the spike's JavaScript writer, as a test fixture) and runs a chart
   to its final state.
2. **The writer in C**, `ta_wasm_module_write`, with
   `tests/unit/test_wasm_module.c`. It writes a module from a small
   template fixture, reads the sections back, refuses a non-template and
   an image that does not open, and fills a buffer too small with its
   length. It runs under `check:freestanding` and `check:wasm`. The
   fixture test of step 1 switches to the C writer.
3. **`tinyactors compile --emit wasm`** with the embedded loop template,
   and `inspect` of a module. Tested in `tests/unit/test_tool.c`, and in
   `wasm:test` by instantiating the command's output.
4. **The driver template** (null), its import list checked by
   `verify_wasm!`. Test: a Node driver in `tests/wasm/` that answers
   sends, fails one, wakes a hibernated actor, takes OTLP through
   `output`, and posts `host.stats`. Under wasmtime too: a small C or Rust
   embedder is a question for this step (unverified which).
5. **The conformance suite against a module.** An adapter in Node
   (`tests/conform/module_adapter.mjs`) that speaks `docs/driver.md`'s
   protocol over the driver exports, run by `mise run
   //tinyactors-test:run -- … --kind binding` as the bindings' adapters
   are. First the null datamodel's scenarios and the protocol vectors,
   then W3C with the quickjs template. Both must pass in full, as
   `mise run conform` requires of `ta-conform`. Independently, building
   `ta-conform` for wasm32-wasi and running it under wasmtime would hold
   core-in-WASM to the whole suite; it uses only the C library
   (`tools/conform/*.c`), but that it builds is unverified.
6. **The quickjs driver template**, measured: size at `-Oz`, per-event
   cost against the native build, as `tinyactors-napi/docs/browser.md`
   measured.
7. Later, each a proposal of its own: the imported-datamodel template; a
   WIT world; a staticjs datamodel and, only if measured worth it, its
   lowering.

## 9. Open questions

- **The template ABI's version** and its compatibility promise before
  1.0. Probably tied to `TA_ABI_VERSION`, with a module refused by a
  writer of another ABI.
- **Should the module export `add_image`** so it can take a new version
  live, or is a new module restored from a snapshot the only deploy?
- **Stack size per template**, measured: core's alone, and with quickjs.
- **wasi-libc's heap and the embedded segment** (section 6), or the
  passive-segment alternative.
- **The CLI's size budget:** embed every template, or only the null ones.
- **Two modules on one page or one wasmtime store:** is routing between
  instances the embedder's transport only (yes, by ADR 0014), or does a
  helper belong in the CLI's examples?

## 10. Checking the recommendation

The reasoned-analysis skill asks for a fresh agent to refute each
recommendation. This proposal was written by a delegated agent with no
way to start another, so **that check has not run**. The session that
asked for the proposal owns it, and until then the findings below are
unconfirmed. The author's own pass, against the repository and the
spikes:

| Claim | Check | Verdict |
|---|---|---|
| A module can be written from a template with no compiler at emit time | spike 2.3 ran end to end in Node | plausible: shown for the null datamodel and Node only; wasmtime, quickjs and a C writer untried |
| The writer must touch only memory, data and the descriptor | spike: one global (the stack pointer), one data segment, `__heap_base` folded into code as a constant; hence the descriptor read by the allocator | confirmed for `tinyactors.o` at `-O2`; a template with a C library is unverified |
| A per-actor module is the wrong grain | 17 pages at start (spike) against about 120 B per actor (`architecture.md` section 13) | confirmed as orders of magnitude; the exact per-actor figure is that section's example, not `mise run layout` |
| Snapshots do not cross between native and WASM | the layout ids differ (spike) | confirmed |
| Lowering the statechart buys little | core's part is about 5 % of a native-datamodel event (`browser.md:200-216`) | plausible: measured for the null chart and the addon, not for quickjs |
| A lowered build must keep pc numbering for snapshots | `host.c:2760-2765` does not refuse a yielded run; a continuation holds an instruction (`architecture.md` section 8) | plausible: settled by snapshotting a yielded run |
| The driver profile keeps no serialization on the hot path | inputs become structs in the module; CBOR only for a payload's data entering or leaving linear memory, which is the process boundary | plausible: a refuter should check this against ADR 0009's cost table |

Changed during this pass:

- A module instance per actor went from "a mapping" to "the wrong grain"
  once the instance's memory was measured.
- The import list dropped a clock import, in favour of the existing
  `advance_time` input.
- The template's exports were cut to the profile's, after the
  dead-stripping measurement.

## Glossary for this proposal

- **Runtime template.** A WASM module from the release with the runtime
  and no charts, and a descriptor the writer fills.
- **Module writer.** `ta_wasm_module_write`: a template plus a config
  plus images makes a module, by copying and adding sections, with no
  compiler.
- **Embedded segment, descriptor.** The data segment the writer adds (the
  config and the images), and the template's 16-byte record of where it
  is, exported as a global holding its address.
- **Loop profile, driver profile.** A template whose exports mirror the
  host loop and that imports nothing; one whose drain runs inside and
  calls an import per effect.
- **Reactor.** A WASM module with no `_start`, driven through exports
  after `_initialize` (WASI's term, used here without WASI).
- **Call record.** Words at an exported address through which an export
  takes more than three numbers, as in `@tinyactors/wasm`.
