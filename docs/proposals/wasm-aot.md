# Compiled charts: a chart's step as WASM code, frontloaded to compile time

**Status:** Sketch (2026-11-25). An outline for discussion; nothing here
is decided or scheduled.
**Touches:** tinyactors-core (`src/core`: a per-chart step entry and an
internal contract for generated code; a new freestanding layer that
lowers a chart to WASM functions; the module writer of
[A system as a WASM module](wasm-modules.md); `tinyactors compile`),
tinyactors-test (a differential harness and the module adapter).
**Depends on:** [wasm-modules.md](wasm-modules.md), steps 1 to 3 (a
template, a writer, `--emit wasm`).
**Not a plan:** ADRs and roadmaps win.

The maintainer's request: "I want to explore a highly optimized mode
where the charts themselves are pure WASM code, i.e. we frontload
everything to compile time." `wasm-modules.md` section 4.3 turned
lowering down until a staticjs datamodel exists. A later review found
that this rests on a measurement from the Node addon, where the
ECMAScript datamodel takes most of the time. On the null datamodel, core
is nearly the whole cost of an event, so the question is still open.
This proposal assumes the mode is wanted and designs it.

**The short answer.** Compile the statechart, not the datamodel, and
only for the null datamodel. The release then gets about **1.9 times
cheaper events** in WASM (about 520 ns to about 270 ns, projected from
measured parts). It does not get a second implementation of SCXML. The
generated code is a fast path over the interpreter's own state:

- it reads and writes the same instance;
- it calls core's own handlers for every action;
- it burns fuel and writes trace records at the same points;
- wherever it would have to stop mid-microstep, it writes the
  interpreter's continuation and lets the interpreter go on.

Snapshots, blobs, migration and OTLP stay as they are. The code lives
inside the module and adds no imports. The cost falls on the
maintainers: the lowering must change in the same commit as core's step
algorithm, and every change to the step gains a differential test.
Charts on ECMAScript or Python, which are nearly all of the repository's
charts, gain nothing worth having, and stay interpreted.

## 1. Whom it serves, what exists, who pays, what is out

**First, the C embedder whose host is a WASM runtime** (wasm-modules.md
section 1), running charts that are pure control flow: a protocol, a
device's or a game's states, a workflow's routing whose data lives
elsewhere. They ask what an event costs and how many fit in a frame or a
request. **Then the capacity planner**, who sizes systems by cost per
event. The torture chamber's charts are null-datamodel charts
(`tools/torture/scenarios/{capacity,chaos,crowd}/charts/*.scxml`), so
the planner's own benchmark is the mode's first workload.

**What already answers them:** the interpreter in a module
(wasm-modules.md 4.1). It costs 505 to 530 ns per event for the order
chart below, of which 62 % is `ta_core_step` (section 6).

**Who pays:**

| Cost | How much | Evidence |
|---|---|---|
| per event | for a compiled chart, about half: about 270 ns where the interpreter takes 520 ns (WASM, null datamodel, no tracing; projected from measured parts); for a chart that stays interpreted, nothing | section 6 |
| per actor | nothing: the same instance, the same region | section 4 |
| code size | about 50 B of WASM per transition, per chart (689 B for an 11-state order chart, whose image is 2,816 B); the image stays as well | section 6.3 |
| core | a per-chart step entry in `ta_core` (internal, beside `serial_of` and `done_event`, `src/core/core_internal.h:132-137`), and an internal header of about 20 functions that generated code calls | section 3 |
| a new layer | a lowering of about 2,500 to 3,500 lines of C (unverified estimate; the compiler is 3,267 lines) | section 5 |
| API surface | one CLI option; the template's function directory, an ABI between writer and template | section 5 |
| work in every binding | none | — |
| maintainers | each change to the step algorithm (subphases, workspace, fuel and trace points) changes the lowering in the same commit, held by a differential test | section 3.3 |

**Out of scope:** compiling ECMAScript, Python or quickjs; native code
(an `--emit c` path for C embedders is an open question, section 10);
a datamodel that does not exist yet (section 2.4); imports of any kind.
Release modules import nothing (ADR 0001 decision 19,
`docs/adr/0001-packaging-and-releases.md:255`), and nothing here needs
one. The application developer on ECMAScript or Python is not served.

## 2. What "frontload everything" can mean

### 2.1 The frame

Each of these facts is checked:

- **Core is the step and nothing else.** `ta_core_step` is called from
  one place, `run_actor` (`src/system/schedule.c:134`). It returns
  sends, cancels, invokes and the rest to the system as `TA_STEP_*`. It
  never touches mailboxes, timers or other actors
  (`docs/architecture.md` section 7.2). The layers run base, image,
  core, system, and never the reverse (`tinyactors-core/AGENTS.md`).
- **The state is the instance, and the workspace inside a microstep.**
  The instance's configuration, history and invoke words have a layout
  derived from the chart record (`ta_instance_layout_of`,
  `src/core/core_internal.h:212`). A workspace holds the continuation
  (subphase, cursors, instruction, sets; `ta_workspace`, `:236-250`).
- **The step can stop mid-microstep.** It does so on a send that leaves
  the system: the actor waits, `schedule.c:139-151`. It also stops for
  invoke, uninvoke, an exhausted pool (`TA_STEP_EXHAUSTED`) and a
  missing workspace. When the fuel runs out inside a microstep, the step
  finishes it on debt, and yields mid-microstep only once that debt
  reaches `overrun_max` (`exhausted`, `src/core/core.c:85`;
  `core_internal.h:127-131`). Generated code needs the same yield
  point, as a fallback. A snapshot can be taken while an actor waits
  mid-microstep.
- **Fuel is observable.** OTLP span times are interpolated from fuel
  stamps (`otlp_wall_of`, `src/otlp/otlp.c:317`; `ta_core_trace_fuel`,
  `src/core/trace_step.c:95`), and chart stats count fuel.
- **The null datamodel has no data.** Its conditions are `In('id')`, and
  it passes a quoted `<log>` literal through. It carries no event data
  (`src/core/datamodel_null.c`). A step on it is decided by the
  configuration, the event's name id, the internal queue and the
  history words alone.
- **Release WASM modules import nothing** (ADR 0001 decision 19).

### 2.2 The spectrum

| Level | What is frozen at compile time | What it needs | Verdict |
|---|---|---|---|
| **0. Facts in the image** | what the interpreter recomputes per event: `In()` resolved to a state index, static entry lists (`architecture.md` section 15: "a future optimization") | compiler and image changes, no code generation | small: resolving `In()` once saves 3 % of an event (measured); entry sets are about 11 % of the step in a profile without inlining (measured, an upper bound). Worth doing for its own sake, for every datamodel |
| **(a) The interpreter specialised to one image** | the image's records as constants in `core.c`, dead opcodes removed | a C compiler per image on the user's machine, or a partial evaluator; neither exists | not recommended: the cost is the phase machine, the workspace and the bitset walks over a configuration known only at run time (section 6.2), which constant records do not remove. The gain is unverified |
| **(b) The step lowered** | selection, conflicts, exit and entry sets, and executable content as WASM functions per chart; queues, timers, mailboxes, hibernation and routing stay in the runtime | a code generator in core (section 5); the template exposes core's handlers to generated code | **recommended for the null datamodel**: the lowered step runs in 14 ns where the interpreter's takes about 320 ns (measured on one chart) |
| **(c) Tables per (configuration, event)** | the whole step for each pair: next configuration and an action list | an exhaustive search; feasible for the null datamodel, because its steps are a finite-state machine | small for real charts, at most 8.6 KB (section 2.3), but it buys nothing over (b) at run time (14 ns against the runtime's 200 ns). Its use is as an **oracle**: every pair can be checked exhaustively |
| **(d) The datamodel compiled** | data as fields of linear memory, conditions as compares | a datamodel whose expressions are total and whose values have fixed shapes | null: done by (b). Others: section 2.4 |

### 2.3 Measured: reachable configurations per chart (level c)

`count.c` in the scratchpad (see Measurements) runs the analysis layer's
breadth-first search (`ta_analysis_search_run`,
`src/analysis/search.c`) over every distinct `.scxml` in the repository
that compiles (195 charts), and over the W3C suite as converted for
ECMAScript (225 tests). The search ignores events and conditions and
takes every transition that can be taken, so a count is an upper bound
of the configurations that can be reached. It enters history by its
default only, so history can add configurations it does not count
(`src/analysis/analysis.h:15-38`). "Classes" counts the elementary
intervals that the chart's descriptors cut the event trie into, plus one
for eventless transitions. A table entry is 8 B: the next configuration
and an action list.

| Chart | States | Transitions | Conditions | Configurations | Classes | Table |
|---|---|---|---|---|---|---|
| `tinyactors-napi/examples/elevator-control/charts/car.scxml` | 27 | 47 | 22 | 27 | 40 | 8,640 B |
| `tinyactors-core/tests/diagram/document-approval.scxml` | 24 | 32 | 3 | 18 | 35 | 5,040 B |
| `tinyactors-core/tests/diagram/order-processing.scxml` | 15 | 18 | 0 | 36 | 17 | 4,896 B |
| `tinyactors-core/tests/diagram/checkout.scxml` | 17 | 17 | 0 | 32 | 14 | 3,584 B |
| `copywriter/charts/annotation.scxml` | 14 | 46 | 15 | 11 | 31 | 2,728 B |
| `tinyactors-core/tests/diagram/shopping-cart.scxml` | 11 | 43 | 20 | 9 | 27 | 1,944 B |
| `tinyactors-core/tests/diagram/order-fulfillment.scxml` | 15 | 25 | 4 | 11 | 20 | 1,760 B |
| the spike's order chart (section 6.1) | 11 | 12 | 1 | 12 | 14 | 1,344 B |

| Population | Charts | Configurations: median, p90, max | Table: median, p90, max |
|---|---|---|---|
| the repository's charts | 195 | 1, 5, 36 | 64 B, 640 B, 8,640 B |
| the W3C tests | 225 | 3, 5, 9 | 96 B, 240 B, 504 B |
| synthetic: an order's fulfilment as *k* parallel regions of 4 states | *k* = 4, 6, 8, 10 | 256; 4,096; 65,536; 1,048,576 (1.5 s to search) | 20 KB; 459 KB; 9.4 MB; 184 MB |

107 of the 195 charts have a single state below the root. In such a
chart all the work is executable content, and lowering the statechart
saves nothing. The configuration space grows as the product of the
parallel regions, so tables (c) cannot be the general mode. Lowered code
(b) grows with the chart's size, not its state space.

### 2.4 Which datamodels can be compiled (level d)

| Datamodel | Exists | Compiled how | Verdict |
|---|---|---|---|
| null | `src/core/datamodel_null.c` | `In('id')` becomes a bit test of the configuration word, `<log>`'s literal a string id; there is no data | **in scope.** Today `In()` is parsed on every evaluation: `null_condition` → `parse_in` → `ta_image_state_by_name`, 403 instructions each (measured) |
| ECMAScript (quickjs, Node's native, Python's) | yes | not possible: the values are the engine's | out: a lowered step still calls the vtable, and core is about 5 % of a 5.2 us event on the addon (`tinyactors-napi/docs/browser.md:200-216`) |
| staticjs | the language in `staticjs/`, as a VM; there is no staticjs datamodel | a WASM backend for staticjs itself (its values are 8-byte boxes with shared layouts, `docs/proposals/staticjs-compiler.md` section 5), so most operations still call its runtime | plausible later, as a staticjs proposal; the gain is unverified |
| JSON (`docs/proposals/json-datamodel.md`, a sketch) | no | paths over a CBOR document could become fixed accessors only with a schema; registered functions are the application's, and would be imports (an amendment to ADR 0001 decision 19) | partial at best |
| a typed record (`<data id="attempts" type="u32">`, a small total expression language) | no | a fixed record per chart in the instance; conditions become `i32` compares | where frontloading pays most, but a new, non-standard datamodel with a proposal of its own |

The repository's charts show who this serves today. 171 name
`ecmascript`, 75 `python`, 8 `null`, and 18 name none, which is null
(`src/compile/scxml.c:1235`), mostly diagram fixtures. The website's
order chart (`website/examples/order/order.scxml`) counts its payment
attempts in data (`attempts < 3`). On the null datamodel that becomes
states (`retrying-1`, `retrying-2`).

## 3. How a compiled chart plugs into core

### 3.1 Not a second implementation: a fast path over the interpreter's state

These are the alternatives, and why each fails or works:

- **Not a separate small runtime.** It would be a second SCXML. Queues,
  timers, invoke, routing, hibernation and tracing would be duplicated
  or wrapped, and the conformance suite would hold one of the two.
- **Not "a new chart kind behind a vtable" with its own state.** A
  layout per compiled chart would change what snapshots and blobs hold.
  Every version would then need migration code, and the layout id would
  no longer say what a region holds.
- **The design: an entry per chart in `ta_core`**, checked first by
  `ta_core_step`. The generated function has `ta_core_step`'s contract
  exactly (`docs/architecture.md` section 7.2): the same arguments, the
  same `TA_STEP_*`, the same instance and workspace. It handles the
  common path, a whole microstep from a stable or eventless phase. For
  anything else it **falls back**: it writes the interpreter's
  continuation into the workspace (subphase, `cursor2`, instruction,
  the exit and entry sets) and returns. The next call, or the call it
  falls through to, interprets from there. "Anything else" means
  `PHASE_INIT`, `PHASE_EXIT`, a cancel flag, an `<invoke>` to start, or
  a resume into a workspace it did not write.

An actor can therefore move between compiled and interpreted at any
microstep boundary, and at any point where the step returns, because the
two share one state. The fallback costs only speed.

### 3.2 What the generated code calls

Every action goes through core's own handler, so its semantics live in
one place. The generated code calls the same functions the interpreter's
`vm` and `micro` call (`src/core/core.c`):

| Step part | The interpreter | Generated code |
|---|---|---|
| bind the event, begin the macrostep | `bind_event`, `begin_macrostep` (`:567`, `:430`) | the same functions |
| select transitions | walks active atomic states and ancestors, tests intervals, asks the datamodel (`select_transitions`, `:735`) | unrolled per state; intervals as constant compares; `In()` as a bit test; `trace_condition` at the same point |
| remove conflicts | pairwise over domains (`remove_conflicts`, `:696`) | emitted only for the pairs whose domains nest; none when no two can (the generator proves this) |
| exit set, onexit, uninvoke | range masks over the configuration (`micro`, `:1703-1770`) | constant masks per transition; onexit in reverse document order |
| entry set | computed at run time with a stack in the workspace (`compute_entry`, `:886`) | constant masks per transition, from **one** function shared with the interpreter (section 3.4); history-flagged transitions call core's completion |
| executable content | the bytecode loop (`vm`, `:1374`) | the handler per opcode (`opcode_send` `:1090`, cancel, raise, log) with constant operands; no dispatch |
| fuel | `burn` at each candidate, state and instruction | the same units at the same points, so the fuel clock and OTLP are byte for byte the same |
| a send, an invoke, a cancel | returns `TA_STEP_SEND` etc. to the system | the same return, after writing the continuation (section 3.1) |
| trace records | `trace_microstep`, `trace_sent`, `trace_macrostep_end` (`src/core/trace_step.c`) | the same functions |

That list, about 20 functions, becomes an internal header
(`src/core/compiled.h`). It is no public ABI: core already holds internal
function pointers of this kind (`serial_of`, `name_of`, `done_event`,
`core_internal.h:132-137`), and "no callbacks except the datamodel
vtable" is about `include/tinyactors/`. Today some of these functions
are static helpers that read the workspace (the trace helpers read
`workspace->trace`, `trace_event`, `trace_fuel`). Making them callable
with explicit arguments is a refactor of core, done first, and it
changes nothing observable.

### 3.3 Held to the conformance suite

The suite says little about the null datamodel. The W3C tests are
written for `conf:datamodel`, and only `test436` is null
(`tinyactors-test/suites/w3c/irp/436/test436.txml`). The scenarios use
the tests' datamodel or quickjs. Three checks hold the compiled mode
instead:

1. **The module adapter** (wasm-modules.md step 5) runs every suite
   against a module whose null charts are compiled. Whatever is null in
   it runs compiled.
2. **An exhaustive differential test.** For each null chart in the
   repository, the analysis layer's configurations (at most 36, section
   2.3) are crossed with every event class (at most 40), at most 1,080
   pairs. Each pair is stepped once interpreted and once compiled from
   the same instance bytes, and the test compares the instance, the
   workspace, `core->out`, the fuel and the trace journal bytes. The
   null datamodel's step depends on nothing else, so this is a proof
   over the reachable space (history and internal queues add state, and
   the test seeds them). It is fast: thousands of steps.
3. **A fallback sweep**, as the torture chamber sweeps pauses. The test
   forces a fallback at each point where the generated code can return,
   and checks the same results. It belongs in `--full` when it is large
   (root `AGENTS.md`, "Where a new test goes").

The spike ran the second check in a small form: 5,002 random events,
with no configuration that differed from the interpreter's (section
6.1).

### 3.4 Generated from the same algorithm?

The interpreter cannot be partially evaluated by machine, since no
partial evaluator exists (level a). What can be shared is **every
computation that depends only on the image**. Today the entry set is
computed at run time (`architecture.md` section 15), and the analysis
layer computes it again for lint (`ta_analysis_enter`,
`src/analysis/machine.h`). Both are reimplementations of SCXML's
`computeEntrySet`. The order of work makes one function in core compute
a transition's static entry set and domain mask. The compiler writes the
results into the image (level 0), and then the interpreter, the analysis
and the lowering all read the same words. My spike computed them a third
time in JavaScript (`lower.mjs`, `enter`). That is what the real layer
must not do.

## 4. Snapshots, hibernation, migration, versions

| Object | Interpreted | Compiled | Why it holds |
|---|---|---|---|
| a hibernation blob | header (image hash, chart), instance words, invoke ids, datamodel bytes (`src/system/hibernate.c:1-40`) | the same bytes | the same instance layout; the null datamodel serializes nothing |
| a snapshot | the region, layout id checked at restore (`src/system/region_layout.c:1-10`) | the same | compiled code holds no region state; the dispatch table is module data, not region |
| a run that waits mid-microstep | the continuation in the workspace | the same continuation, written by the generated code before it returns | section 3.1; a restore in either mode resumes in the interpreter, or in compiled code that recognizes its own resume point |
| a version | an image per segment, oldest first (wasm-modules.md 4.1) | each image carries its compiled code, keyed by the image hash | code is a cache of the image; a hash with no code runs interpreted |
| a migration | `ta_blob_migrate` maps an instance between images by state id (`architecture.md` section 16.2) | unchanged; the new version's code runs on the migrated instance | migration reads instances, never code |
| interpreted ↔ compiled | — | a snapshot of one restores in the other, when both come from the same template build | same layout id and same image hash; the template is the same with an empty or a filled directory |

The layout id still differs between wasm32 and x86_64 (wasm-modules.md
section 2.3). Compiled code changes nothing there.

### Lifetimes

| Object | Owner | Created | Read by, when | Ends | Thread |
|---|---|---|---|---|---|
| generated functions, directory | the module | the writer, at `tinyactors compile` | `ta_core_step`'s dispatch, every step | with the instance of the module | the instance's |
| the image (strings, records) | the module, then the host's copy | instantiation, `ta_host_add_image` | the generated code for strings and sends, inspection, snapshots, migration | never: linear memory does not shrink | the instance's |
| the instance | the segment's slab | spawn | the interpreter or the generated code, during a step | the actor's end | the instance's |
| the workspace and its continuation | the segment's pool | the start of a microstep (`acquire_workspace`) | whichever mode resumes | the end of the microstep | the instance's |
| `core->out` (a send, an invoke) | core | a step returning `TA_STEP_*` | the system, before the next step | the next step (`release_temporaries`) | the instance's |
| effects and outputs | the host | the drain | the embedder | the ack (ADR 0012; `tinyactors-core/docs/boundary.md` section 7) | the instance's |

None of these rows differs between the two modes but the first.

## 5. Code generation without LLVM

| Option | On the user's machine | Size | Verdict |
|---|---|---|---|
| **core writes WASM binary directly** | nothing: the `tinyactors` binary | an encoder of about 400 lines (sections, LEB128, a function body builder); the spike's is 30 lines of JavaScript | **recommended** |
| emit C, compile with their compiler | clang or zig with a wasm32 target | the lowering plus a C writer | out: it brings back the toolchain that wasm-modules.md removed; possible later for native embedders (section 10) |
| emit WAT, assemble it | a WAT assembler (wabt, binaryen) | a text writer, plus a dependency | out: WAT is a debugging view; the binary is as easy to write (the spike wrote it) |

**Where it lives.** A new freestanding layer, `src/lower`, in
`libtinyactors_runtime` beside `src/compile` and wasm-modules.md's
`src/wasm_module`. It reads an image view (and the analysis layer), and
writes function bodies through `ta_output`. It uses no recursion, and
takes memory from the arena it is given. Its input and output follow the
C rules: `ta_bytes` in, `ta_buffer *` out, at most three parameters.

```c
/* tinyactors/lower.h (sketch): which charts of an image can be compiled,
 * and the code of those that can, for a template's function directory. */
typedef struct ta_lower_input {
    uint32_t size;
    uint32_t pad;
    ta_bytes image;     /* read during the call */
    ta_bytes directory; /* the template's custom section: core's functions by index */
} ta_lower_input;
/* Writes the functions and a per-chart table (compiled, or the reason it is
 * not: datamodel, unsupported content) into `out`. TA_ERROR_NO_MEMORY:
 * `length` is what it needs. TA_ERROR_INVALID: an image that does not open,
 * a directory of another ABI. */
TA_API ta_status ta_lower_charts(const ta_lower_input *input, ta_buffer *out);
```

**The module.** wasm-modules.md's writer already copies a template and
adds a data segment. To that it adds three things:

- the generated functions, appended to the function and code sections,
  so no existing index moves;
- an element segment that puts them in the template's table;
- a directory in the descriptor's segment: image hash, chart and table
  index.

The template lists the indices of core's functions from `compiled.h` in
a custom section, the directory the lowering reads. It is versioned with
the template's ABI (wasm-modules.md section 9). The module imports
nothing.

**Size.** The lowering for the whole null datamodel has to handle
history, eventless loops, `<raise>` and the internal queue, conflicts,
done events, fallback points and resume points. That comes to about
2,500 to 3,500 lines with the encoder and the writer's additions, which
is unverified. The compiler (`src/compile`) is 3,267 lines.

**The CLI.**

```sh
tinyactors compile order.scxml --emit wasm --mode compiled -o order.wasm
tinyactors inspect order.wasm   # its template, images, and per chart: compiled or interpreted, and why
```

`--mode interpreted` is the default. `--mode compiled` compiles every
chart it can and reports each one's mode in the answer, with a reason
for those it leaves interpreted (`datamodel ecmascript`, `history` while
the lowering lacks it). It never refuses a module for them.
`disassemble`, `graph`, `states` and `diff` read the image, which the
module still holds, so they are unchanged.

## 6. Measurements

Machine: Intel Xeon @ 2.10 GHz, 4 vCPUs, Linux 6.18, Node 22.23.5, zig
0.16.0 at `-O2`, the 0.4.0 amalgamation (`tinyactors-core/dist/`),
2026-11-25, run one after another with nothing else running. The files
are in
`/tmp/claude-0/-home-user/48aea8a4-edf2-5799-91e9-403c27e0dc94/scratchpad/wasm-aot/`
(`bench/`, `lowered/`, `configs/`).

### 6.1 The chart and the spike

`bench/order.scxml`, on the null datamodel, has two parallel regions:

- **the order:** `cart`, then `paying` on `checkout`, guarded by
  `In('cleared')`. `paying` has a 30 s `payment.timeout`, a delayed send
  that is cancelled on exit. From there `paid`, then `shipped`. A
  declined payment or the timeout leads back to `cart`.
- **the risk check:** `scoring`, `cleared` and `held`.

`item.added` is a targetless transition. A benchmark cycle is
`item.added`, `checkout` (the guard, an entry, a delayed send) and
`payment.declined` (an exit, a cancel).

- **The interpreter:** `bench/interp.c`, as
  `tinyactors-napi/tools/spikes/browser/perevent.c`. One actor per
  event: `ta_system_send`, `ta_runtime_run`, a take and an ack, with
  tracing on or off. It was built natively, and for wasm32-wasi to run
  in `node:wasi`.
- **The lowering:** `lowered/lower.mjs` reads the `.tai` and writes a
  WASM module in binary. It refuses what it does not handle: more than
  32 states, history, eventless transitions, `<raise>`, dynamic sends, a
  datamodel other than null, and conflicts. `lowered/drive.mjs` replays
  the interpreter's recorded sequence (5,002 events from a seeded
  xorshift, visiting 8 configurations, 558 sends and cancels). **No
  configuration differs from the interpreter's.** Then it times
  `bench(…)`, a loop inside WASM.

### 6.2 Per event

| Build | ns per event, the last rounds | Instructions per event (callgrind, native) |
|---|---|---|
| interpreter, native, no tracing | 343, 344, 343, 344 | 4,121 |
| interpreter, native, tracing | 505, 503, 527, 656 | 6,000 |
| interpreter, WASM in Node, no tracing | 522, 529, 526, 505 | — |
| interpreter, WASM in Node, tracing | 778, 836, 800, 862 | — |
| **the lowered step alone**, WASM in Node | 14.1, 14.8, 13.8, 13.5 | — |

Where the interpreter's time goes, with no tracing:

| Part | Share | Evidence |
|---|---|---|
| `ta_core_step`, inclusive | 64 % natively (2,636 instructions, 3.33 calls per event); **62 % in WASM** | callgrind; V8's sampling profiler over the WASM build (`bench/prof.mjs`) |
| of which the phase machine, `init_k`, workspace and cursors | `micro` 610, `select_transitions` 443, `ta_core_step` itself 414, `init_k` 414, `run_code` 200 instructions | callgrind, exclusive |
| of which `In()` parsed on each evaluation | 134 per event (403 per evaluation) | callgrind: `null_condition`, `parse_in`, `ta_image_state_by_name` |
| `ta_system_send` | 14 %, of which the event's name looked up in the trie, 279 instructions (7 %) | `ta_event_set_name` |
| the rest of the system's run: routing, timers, the queues | about 20 % | — |

**Projection** (WASM, no tracing). The interpreter takes 520 ns, and the
62 % in the step is about 320 ns. Without the step, about 200 ns
remain, and those stay in every mode. The lowered step costs 14 ns, but
it leaves out bookkeeping that a real compiled step keeps: binding and
releasing the event, acquiring and releasing a workspace, the macrostep
hooks, fuel, the fallback writes. Natively that bookkeeping is about 450
instructions, or about 57 ns at the same rate. That gives **about 270
ns, 1.9 times cheaper**, and 212 ns (2.45 times) if the bookkeeping
cost nothing. With tracing on, trace records cost the same in both
modes, so about 820 ns becomes about 570 ns (1.45 times). The
projections are unverified until step 4 of section 9 measures a real
compiled step in the torture chamber.

**Level 0, measured.** `bench/level0.c`, a copy of the amalgamation
whose `null_condition` resolves each `In()` once, saves 120 instructions
per event (3 %). It costs nothing and helps the interpreter, but it is
not the mode.

### 6.3 Size

| Chart | Image | Generated module | `step` function |
|---|---|---|---|
| the order (11 states, 12 transitions) | 2,816 B | 831 B | 689 B |
| `tests/diagram/insurance-claim.scxml` (11, 13) | 2,600 B | 842 B | 700 B |
| synthetic, 4 regions × 4 states (22, 21) | 3,680 B | 1,252 B | 1,110 B |
| synthetic, 6 regions × 4 states (32, 31) | 5,032 B | 1,848 B | 1,706 B |

The spike refused `order-processing`, `warehouse-order` and `checkout`
(transitions in different regions can conflict), `support-ticket`
(history) and `subscription` (ECMAScript). The real layer must handle
the first three, and does so by falling back where needed. The
interpreter's step functions are about 10 KB of the WASM build's 242 KB
of code (`bench/sizes.mjs`). The interpreter stays as the fallback, so
compiling adds code and removes none.

### 6.4 Not measured

- a real compiled step: the bookkeeping cost above, the fallback, and a
  trace journal identical to the interpreter's;
- wasmtime or any other runtime (only Node);
- charts with history, `<raise>` or conflicts lowered;
- static entry lists in the image (level 0); 11 % of the step is an
  upper bound from a profile without inlining;
- the lowering's size in C;
- the effect of name ids on inputs (section 10).

## 7. What remains runtime no matter what

| Part | Where it lives | Why it cannot be frontloaded |
|---|---|---|
| mailboxes, the internal queue, the event pool | system, core's event pool | contents arrive at run time; `<raise>` pushes onto the queue in order |
| timers (delayed sends) | system (`ta_system_route_send`, `ta_system_cancel`) | the clock enters through `ta_host_advance_time` |
| routing, segments, the transport, I/O processors | system and host | targets resolve against live sessions |
| `<invoke>`, children, `done.invoke` | system | a child is another actor, with its own turns |
| hibernation, migration, snapshots | system and host | they read and write instances, which do not change |
| tracing, OTLP, metrics | core's trace helpers, system, `src/otlp` | the same records at the same points (section 3.2) |
| the datamodel's values (other than null) | the datamodel | the engine's |
| the scheduler, fuel, the quantum | system | fairness across actors |

## 8. Recommendation

**Level (b), for the null datamodel only, inside the module.** It
should be a fast path that shares the interpreter's state and handlers,
generated as WASM binary by a freestanding layer in core, with no
compiler and no imports. Level 0 (facts in the image: `In()` resolved,
static entry sets and domain masks) comes first. It helps every
datamodel, and it is the one place where interpreter, analysis and
lowering compute the same thing. Level (c) is used as the test oracle,
not at run time. Level (a) is not built. Level (d) waits for a datamodel
worth compiling (section 2.4).

**What is given up:**

- a second code path: every change to core's step changes the lowering
  in the same commit, and the continuation encoding and the fuel and
  trace points become a contract between them;
- some freedom in the interpreter: an optimization that moves fuel or
  trace points must move them in the lowering too;
- code size per chart, about 50 B per transition, with the image kept;
- nothing for ECMAScript, Python or quickjs charts, which are nearly all
  of today's charts;
- nothing for native embedders, until an `--emit c` path exists;
- the gain is bounded: the system's share, about 200 ns of 520 ns, is
  untouched. Lowering cannot make an event cheaper than routing,
  queueing and the loop around it.

## 9. Order of work

Each step is a commit that passes `mise run test` in core:

1. **Level 0.** The compiler resolves `In('id')` into an expression of
   its own kind with the state index (`TA_EXPRESSION_*` and
   `datamodel_null.c`), and writes static entry sets and domain masks
   into the image, which `compute_entry` reads (`architecture.md`
   section 15). The image version changes. Test: `test_core`, `conform`,
   and `mise run layout`, unchanged. Measured with the torture chamber.
2. **The step entry and `compiled.h`.** A per-chart pointer in
   `ta_core`, NULL by default. The trace helpers take their facts as
   arguments. `tests/unit/test_compiled_step.c` writes a "compiled" step
   for the spike's order chart by hand **in C**, plugs it in natively,
   and runs the exhaustive differential test (section 3.3) and the
   fallback sweep. This holds the contract before any WASM exists.
3. **`src/lower` and its encoder**, writing WASM for the null charts it
   can handle and leaving the rest interpreted. Test: `wasm:test` runs
   the order chart and the torture charts compiled in Node, and repeats
   the differential test inside WASM.
4. **The writer merges code** (wasm-modules.md's writer, with a template
   directory) and **`--mode compiled`**, with `inspect`. Measured: the
   torture chamber's null scenarios, interpreted against compiled,
   throughput and memory side by side.
5. **Coverage:** history, eventless loops, `<raise>`, conflicts and done
   events, each with its differential cases. Then compiled resume
   points, so that a step that returns on a send goes on in compiled
   code.
6. **The module adapter's suites** against compiled modules.

## 10. Open questions for the maintainer

- **Is the null datamodel enough?** The mode's users are those who
  write pure control flow. Should a typed-record datamodel (section 2.4)
  be proposed so that applications with counters and flags can compile
  too?
- **Native too?** `--emit c` of the same lowering, for C embedders with
  a compiler, would need a public way to bind a step to a chart, and so
  an ADR.
- **Level 0 alone:** should resolved `In()` and static entry sets land
  whatever the decision on compiled charts? They are measured at 3 %,
  plus up to about 7 % (unverified).
- **Name ids on inputs:** a send pays 279 instructions (7 %) to look its
  name up in the trie. A module could export event-name ids, which an
  embedder passes instead of text. That is a change to `ta_event_in`,
  for every datamodel and outside this mode.
- **The template's function directory:** a custom section of indices,
  or reserved exports? Either way it is versioned with the template's
  ABI.

## 11. Checking the recommendation

The reasoned-analysis skill asks for a fresh agent to refute the
recommendation against the code. **That check has not run.** This
proposal was written by a delegated agent that has no tool to start
another agent. The session that asked for it should run the check before
the proposal reaches a decision, and until then the verdicts below are
the author's own. The author's pass:

| Claim | Check | Verdict |
|---|---|---|
| The null datamodel's event is mostly core | 62 % in `ta_core_step` in WASM, 64 % natively | confirmed for one chart; other charts unmeasured |
| A lowered step agrees with the interpreter | 5,002 events, 0 configurations differ | plausible: configurations only, not effects or trace bytes; no history, raise or conflicts |
| About 1.9 times cheaper per event | measured parts plus an instruction-count estimate of the retained bookkeeping | plausible: settled by step 4 |
| Snapshots and blobs are unchanged | compiled code keeps the instance layout and writes the interpreter's continuation | plausible: rests on the fallback writing every workspace field, which step 2's sweep tests |
| OTLP is unchanged | fuel and trace calls at the same points (`otlp_wall_of` reads fuel) | plausible: settled by comparing journals in step 2 |
| Tables (c) are small for real charts | 195 + 225 charts measured; product growth shown | confirmed as upper bounds (the search over-approximates), except for configurations reached through history |
| No imports are needed | generated code calls functions inside the module | confirmed by construction; the template's directory is new |
| The fallback at a mid-microstep return is correct | core resumes from the workspace (`ta_core_step`, `PHASE_MICROSTEP`) | plausible: the interpreter must accept every continuation the generated code writes, which needs the sweep |

Changed during the pass:

- Tables (c) moved from "a mode" to "the oracle" once the lowered step
  measured 14 ns.
- Level 0 went from "the cheap win" to "a prerequisite", once resolving
  `In()` measured 3 %.
- "A new chart kind with its own layout" was dropped for the shared
  state, because of snapshots and migration.

## Glossary for this proposal

When the proposal is adopted, these terms go to `GLOSSARY.md`.

- **Compiled chart:** a chart whose step has WASM code in the module,
  keyed by its image's hash; its actors run that code where it applies,
  and the interpreter elsewhere.
- **Lowering:** turning a chart's records and bytecode into that code
  (`src/lower`).
- **Fallback:** the compiled code writing the interpreter's continuation
  and leaving the rest of the step to the interpreter.
- **Function directory:** the template's list of the core functions
  that generated code may call, by index.
