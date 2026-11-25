# Compiled charts: a chart's step as WASM code, frontloaded to compile time

**Status:** Sketch (2026-11-25, revised the same day after the
maintainer's direction and a refutation). An outline for discussion;
nothing here is decided or scheduled.
**Touches:**

- tinyactors-core: a step result that hands the system an ordered list
  of actions; a small runtime of out-of-line functions for generated
  code; a new freestanding layer that lowers a chart to WASM functions;
  the module writer of [A system as a WASM module](wasm-modules.md);
  `tinyactors compile`; the datamodels' compile-time preparation;
- tinyactors-test: a differential harness, and the module adapter in
  compiled mode.

**Depends on:** [wasm-modules.md](wasm-modules.md), steps 1 to 3 (a
template, a writer, `--emit wasm`).
**Not a plan:** ADRs and roadmaps win.

The maintainer's request: "I want to explore a highly optimized mode
where the charts themselves are pure WASM code, i.e. we frontload
everything to compile time." The maintainer's direction on the first
draft (2026-11-25): "Null datamodel is not enough. Typed records is
interesting, but WASM always needs to work with value handles anyways,
matching what our actors compiler does already." So a compiled chart
owns no data. A value is a handle the datamodel owns, as core already
treats payloads (`ta_datamodel_vtable`: retain, release, `to_wire`,
`from_wire`, expressions by index; `include/tinyactors/datamodel.h`),
and as `@tinyactors/wasm` does through its datamodel import
(`ta_web.datamodel(host, entry, slot)`,
`tinyactors-napi/wasm/src/web.h:271`).

`wasm-modules.md` section 4.3 turned lowering down until a staticjs
datamodel exists. A review of that proposal (2026-11-25, not recorded in
the repository) found that its base design stands and that its driver
profile does not, as written. It also found that the turn-down rested on
the addon's measurement, where the datamodel dominates.

**The short answer.** A chart's step can be pure WASM code for every
datamodel, with values left as handles. It is a second implementation
of SCXML's algorithm, and must be held to the full conformance suite in
compiled mode. W3C runs on quickjs, which a step that serves every
datamodel now makes possible.

The design that fits the request (section 3, design B) is **a sketch
pending the fixes of section 4.6**. A second refutation found it not
yet faithful to the interpreter in twelve places, and each place has a
proposed fix that is not shown to work yet. It works like this:

- The compiled step keeps its working sets in WASM locals and runs each
  microstep to its end.
- It hands the system **one ordered list of actions per microstep**,
  where the interpreter returns to the system on every send. The list
  holds every push onto the internal queue (raises, core's own
  `error.execution`, done events) in order with sends, cancels and logs.
  The system applies it in order, and parks the actor in the middle of
  the list at a send that leaves the system.
- It shares only the instance layout with the interpreter. Hibernation,
  migration and snapshots at microstep boundaries are unchanged.
- An actor that is mid-microstep in compiled code would block every
  snapshot of its host. Section 4.6 proposes that a `<foreach>` flush
  writes the interpreter's continuation instead, so that no such state
  exists.

What it buys, projected from measured parts and unverified until it is
built:

- **about 1.8 to 2 times cheaper events on the null datamodel** (about
  520 ns to 255–280 ns in WASM);
- **at most about 1.1 times on a quickjs chart.** There the datamodel's calls
  (quickjs and its value conversion) take 78 % of a 3.3–3.7 us event, and core's step without its datamodel
  calls takes 12 %.

For ECMAScript, frontloading on the **datamodel's** side targets larger
costs than compiling the step does:

- quickjs bytecode compiled ahead, not on first use: the first session
  of a chart costs 1.1 ms in WASM, the next ones 29–46 us;
- `_event` built lazily, on its first read: binding it is 31 % of the
  event's time on a chart that never reads it. A scan cannot decide that
  a chart never reads `_event` (section 2.4).

Both are measured costs. What their removal saves is not measured.

## 1. Whom it serves, what exists, who pays, what is out

**First, the C embedder whose host is a WASM runtime** (wasm-modules.md
section 1): what an event costs, and how many fit in a frame or a
request. **Then the capacity planner**, who sizes by cost per event and
per new session. **Then the application developer on ECMAScript**, whose
cold starts (the first session of each chart) and isolated sessions
cost most (section 7.3).

**What already answers them:** the interpreter in a module
(wasm-modules.md 4.1). In WASM it costs 505–530 ns per event on a null
order chart and 3.3–3.7 us on a quickjs order chart (section 7).

**Who pays:**

| Cost | How much | Evidence |
|---|---|---|
| per event | for a compiled chart: projected 1.8–2× less on the null datamodel, at most 1.1× less on quickjs; for an interpreted chart, nothing | section 7, projections marked |
| per actor | the same instance. Every actor parked at once (on a send that leaves the system, a full effect queue, a parked spawn) holds the rest of its list, so the pending-actions pool's worst case grows with the population: a new capacity of the system config, sized per segment | section 4.6, finding 7 |
| code size | about 50 B of WASM per transition, per chart, beside the image, which stays | section 7.4 |
| core | a step result `TA_STEP_ACTIONS`; a pending-actions pool in the region (a new record type, so the layout id changes once); the `<log>` trace record moved from the system into the step; about 20 out-of-line runtime functions | sections 4, 6 |
| a new layer | `src/lower`: about 3,000 to 4,500 lines of C for the whole algorithm and an encoder (unverified; the compiler is 3,267 lines) | section 6 |
| API surface | one CLI option; one config capacity (pending actions); **a new API between `libtinyactors_runtime` and `libtinyactors`**: the runtime functions that `src/lower` generates calls to, with their indices, signatures and meaning. ADR 0001 decision 19 lets the runtime library call only the core's public API (`docs/adr/0001-packaging-and-releases.md:235`), so these functions go in `tinyactors.api.json` under `api --check`, with an ADR | sections 4.6, 6.2 |
| work in every binding | the new capacity in each binding's config and docs: `docs/embedding.md`, `docs/bindings.md`, the protocol vectors, `mise run layout`'s `COSTS`, and the published memory numbers, all in the same change | finding 7 |
| maintainers | a second implementation of the step to keep in agreement with the first, held by the suite in compiled mode and a differential test (section 5) | — |

**Out of scope:** compiling ECMAScript, Python or quickjs code to WASM;
a value model of the compiled chart's own (records with typed fields
are at most an optimisation inside one datamodel, behind handles;
section 2.4); native code (`--emit c` is an open question); imports.
Release modules import nothing (ADR 0001 decision 19,
`docs/adr/0001-packaging-and-releases.md:255`). The compiled step needs
none. A datamodel whose vtable calls an import (`@tinyactors/wasm`'s
shape) is that template's matter, not the step's.

## 2. What "frontload everything" can mean

### 2.1 The frame

Each of these facts is checked:

- **Core is the step.** The step is called from one place, `run_actor`
  (`src/system/schedule.c:134`). It returns sends, cancels, invokes and
  the rest as `TA_STEP_*`, and the system acts on each before the next
  call (`schedule.c:138-180`). Layers run base, image, core, system,
  never the reverse (`tinyactors-core/AGENTS.md`).
- **The interpreter's continuation is rich.** `ta_workspace`
  (`src/core/core_internal.h:236-256`) holds subphase, `vm_active`,
  `foreach_top`, instruction, `block_end`, `cursor`, `cursor2`,
  `selected_count`, `stack_top`, `auxiliary`, `final_state`, `trace`,
  `done_payload`, `trace_fuel` and `trace_event`. After the header come
  the selected list, domains, the exit, entry, default-entry and
  history-default-entry sets, a stack and `<foreach>` frames
  (`ta_workspace_layout_of`, `:263`). The instance holds the phase, the
  step counters and the internal queue.
- **The step pauses in many places:**
  - every send and cancel (the step is entered 200,003 times for 60,000
    events and `micro` 100,002 times, section 7.2);
  - invoke and uninvoke, one slot at a time (`core.c:1741-1754`);
  - `TA_STEP_EXHAUSTED` retries inside `opcode_send` (`:1102-1116`,
    `:1313`), the VM (`:1395`, `:1425`), `start_invoke` (`:1617`) and
    the two-stage done events (`:1855-1871`);
  - a fuel yield once the overrun reaches `overrun_max` (`exhausted`,
    `:85`);
  - a send that leaves the system, which parks the actor mid-microstep
    until it is decided (`schedule.c:139-151`).

  Handlers also move the instruction themselves (`opcode_send`'s
  `instruction += 2`, `:1322`; `abort_block`, `:969`).
- **Fuel is observable.** OTLP span times are interpolated from fuel
  stamps (`otlp_wall_of`, `src/otlp/otlp.c:317`; `ta_core_trace_fuel`,
  `src/core/trace_step.c:95`), and chart stats count fuel.
- **Hibernation and export need a stable point**: idle, empty mailbox
  and internal queue, no workspace, no children, no pending request
  (`docs/architecture.md` section 16.1). **A snapshot** refuses only
  during a run, with effects or outputs not acknowledged, or during a
  migration. It takes actors mid-macrostep with their workspaces
  (`src/host/host.c:2760-2765`). At a microstep boundary the
  interpreter holds no workspace: it is released at the end of each
  microstep (`core.c:2144`).
- **Values are handles.** The datamodel owns them. Core calls the
  vtable with expression indices, and a payload is a `uint64_t` handle
  (`datamodel.h`). The null datamodel has no data
  (`src/core/datamodel_null.c`).
- **Release WASM modules import nothing** (ADR 0001 decision 19).

Doc drift found on the way: `architecture.md` section 7.1 names
subphases that do not exist (`SUBPHASE_SELECTED`,
`SUBPHASE_HISTORY_RECORD`, `SUBPHASE_ONEXIT`, …). The code has
`SUBPHASE_EXIT_COMPUTE`, `EXIT`, `EXIT_CODE`, `EXIT_UNINVOKE`,
`TRANSITIONS`, `ENTRY_COMPUTE`, `ENTER`, `ENTER_CODE`, `ENTER_DEFAULT`,
`ENTER_HISTORY`, `ENTER_FINAL` and `RUN_CODE` (`core.c:28-41`).

### 2.2 The spectrum

| Level | What is frozen at compile time | Verdict |
|---|---|---|
| **0. Facts in the image** | static entry sets and domain masks (`architecture.md` section 15 calls them "a future optimization"); `In()` resolved to a state index, at compile time or at chart attach through the null datamodel's unused `chart_attach` slot (`datamodel_null.c:116`) | **an independent first step.** Static sets help every datamodel's step, but only partly: entry sets are at most 11 % of the null step in a profile without inlining, about 7 % of an event. `In()` helps only the null datamodel, by 3 % (measured). That makes at most about 10 % of a null event, unmeasured as a whole. The lowering reads the same words, so the static sets are computed in one place |
| **(a) The interpreter specialised to one image** | constant records, dead opcodes removed | **no**: it needs a C compiler per image, and the cost is the phase machine and bitset walks over a configuration known only at run time (section 7.2). The gain is unverified |
| **(b) The step lowered** | selection, conflicts, exit and entry, the macrostep loop and executable content as WASM functions per chart; datamodel calls by expression index on handles | **design B, a sketch pending the fixes of section 4.6** (section 3) |
| **(c) Tables per (configuration, event)** | the whole step per pair | for the null datamodel only, where a step has no data. Small for real charts (section 2.3), but no faster than (b) at run time, and it grows as the product of parallel regions. **Use it as a test oracle**, with the limits of section 5.2 |
| **(d) The datamodel prepared ahead** | what the datamodel can do before the first session: quickjs bytecode, `_event` built lazily, `In()` targets; later a staticjs datamodel that lowers its expressions to WASM behind the same handles | **recommended for each datamodel separately** (section 2.4); for quickjs it targets larger costs than (b) |

### 2.3 Measured: reachable configurations (level c)

`configs/count.c` runs the analysis layer's search
(`ta_analysis_search_run`, `src/analysis/search.c`) over every distinct
compiling `.scxml` of the repository (195 charts) and the W3C suite as
converted for ECMAScript (225). The search takes every transition
whatever its events and conditions, so a count is an upper bound. It
enters history by its default only, so it misses configurations that
are restored through history (`src/analysis/analysis.h:15-38`).
"Classes" are the elementary intervals that the chart's descriptors cut
the event trie into, plus eventless. A table entry is 8 B.

| Chart | States | Transitions | Configurations | Classes | Pairs | Table |
|---|---|---|---|---|---|---|
| `tinyactors-napi/examples/elevator-control/charts/car.scxml` | 27 | 47 | 27 | 40 | 1,080 | 8,640 B |
| `tinyactors-core/tests/diagram/order-processing.scxml` | 15 | 18 | 36 | 17 | 612 | 4,896 B |
| `tinyactors-core/tests/diagram/document-approval.scxml` | 24 | 32 | 18 | 35 | 630 | 5,040 B |
| `tinyactors-core/tests/diagram/checkout.scxml` | 17 | 17 | 32 | 14 | 448 | 3,584 B |
| the spike's null order chart (section 7.1) | 11 | 12 | 12 | 14 | 168 | 1,344 B |

| Population | Charts | Configurations: median, p90, max | Pairs per chart, max |
|---|---|---|---|
| the repository's charts | 195 | 1, 5, 36 | 1,080 (`car`) |
| the W3C tests | 225 | 3, 5, 9 | 63 |
| an order's fulfilment as *k* parallel regions of 4 states, *k* = 4, 6, 8, 10 | 4 | 256; 4,096; 65,536; 1,048,576 (the last searched in 1.5 s; a 184 MB table) | — |

107 of the 195 charts have a single state below the root. Their step is
selection plus executable content, so most of their cost is the
datamodel's.

### 2.4 Datamodels: values stay handles, preparation moves ahead

The compiled step calls the datamodel exactly where the interpreter
does: `condition`, `assign`, `capture`, `log`, `bind_event`, the
`<data>` initialisers, `<foreach>`. It calls it with the same expression
indices, through the same binding. Nothing about values changes. What
can move to compile time is each datamodel's own preparation:

| Datamodel | What it does today | What moves ahead | Gain |
|---|---|---|---|
| null | parses `In('id')` on every evaluation: `null_condition` → `parse_in` → `ta_image_state_by_name`, 403 instructions each (measured) | `In()` resolved at compile time (level 0), or at `chart_attach`; the compiled step tests the configuration bit itself | 3 % of an event when cached (measured: `bench/level0.c`); free when compiled |
| quickjs (`src/datamodel_ecmascript`) | `chart_attach` analyses the chart's scripts by compiling them in a scratch context (`analyze_script`, `script_strict`, `datamodel_ecmascript.c:1129-1247`). Each expression is compiled **on first use**, per chart in the shared realm and **per session** in the isolated realm (`get_getter`, `:297-336`). It binds `_event` before every event (`ecmascript_bind_event`) | bytecode written at `tinyactors compile` (quickjs-ng's `JS_WriteObject` with `JS_WRITE_OBJ_BYTECODE`, `third_party/quickjs-ng/quickjs.h:1261`) and read at attach. **`_event` built lazily**: `bind_event` records the event, and the object is made on its first read. No scan is conservative here: `[].constructor.constructor('return _event')()`, identifier escapes (`\u005fevent`), a `namelist` naming `_event` as a location, a host-side read of the variable, and a migration to a version that reads `_event` all reach it. And `_event` is stored in hibernation and snapshot blobs (`datamodel_ecmascript.c:1922-1926`, `:2500-2529`), so the blob writer must make it when it is absent, to keep the stored bytes. If a scan is used at all, it is an allow-list over the compiled bytecode's atoms and opcodes, with the blob question answered first | measured costs it targets: the first session of a chart takes 1.1 ms in WASM against 29–46 us for the next; isolated sessions take 230–310 us each natively; `_event` binding is 31 % of the event in WASM. What removal saves is unverified (the split of the 1.1 ms between analysis, context and compiles is not measured) |
| Node's native, Python's | the application's engine (`tinyactors-napi/AGENTS.md`, two ECMAScript datamodels) | nothing in core | none from this proposal |
| staticjs (the language in `staticjs/`; no staticjs datamodel exists) | — | later: its own lowering of expressions to WASM functions, appended to the same module and reached through its vtable by expression index | unverified; a staticjs proposal |
| records with typed fields | — | at most an optimisation inside one datamodel: fields in its own memory behind handles, not a value model of the chart's (the maintainer) | — |

Bytecode ties the image to one quickjs build. The prepared bytes would
be keyed by the datamodel's version and refused by any other build. The
CLI links the same quickjs as the release's datamodel, so it can write
them. Whether they go in the image or beside it, and how attach receives
them (a `ta_bytes` in `ta_datamodel_chart`), is an open question.

## 3. Two designs for the compiled step

### 3.1 It is a second implementation of SCXML's algorithm

The first draft said otherwise, and that was wrong. Whatever the code
calls for actions, the lowering re-encodes these parts of `src/core`:

- the order of selection (`select_transitions`, `core.c:735`);
- preemption (`remove_conflicts`, `:696`);
- the order of exit and the recording of history (`:1703-1720`);
- default and history entry, done events and `parallel_in_final`
  (`:1813-1880`);
- the macrostep loop (`ta_core_step`, `:1906`).

`wasm-modules.md` section 4.3 said so too. The design question is how
little to re-encode and how to hold it (section 5), not whether it is a
second implementation.

### 3.2 Design A: a fast path bound to the interpreter's contract

The first draft's design:

- generated code works on the interpreter's workspace;
- it returns on every send, cancel, invoke and refusal;
- it writes the interpreter's continuation, every field of section 2.1,
  so that the interpreter can resume.

It keeps snapshots byte for byte only if it writes every scratch field
as the interpreter does (`compute_entry` leaves its stack and sets in
the workspace). It needs core's handlers out of line, though `-O2`
inlines them today: `opcode_send` has no symbol in the build. Its gain
comes only after it also has resume points of its own. Without them,
the work after each pause is the interpreter's.

### 3.3 Design B: own transient state, one action list per microstep (a sketch pending section 4.6)

The step's sets (selected, exit, entry, defaults) live in WASM locals,
never in memory. A microstep runs to its end without returning. Every
action whose outcome the system decides goes into an **ordered list**:
every push onto the internal queue (a raise, a done event, and core's
own `error.execution` from `error_exec`, `raise_failure` and
`raise_platform`), and every send, cancel, log, uninvoke and autoforward.
Raises cannot stay immediate: W3C 496 needs route.c's
`error.communication` before the raise that follows the send. Core's
errors cannot stay immediate either: W3C 402 raises `event1`, then makes
an invalid assign, and expects `event1`, the error, then `event2`. At the
end of the microstep the step returns
`TA_STEP_ACTIONS`. The system applies the list in order with the code it
already runs per `TA_STEP_*` (`schedule.c:138-180`).

The step shares with the interpreter **only the instance layout**: the
configuration, history, invoke words and the header's phase and queue.
So hibernation, export and migration, which touch only stable actors,
are unchanged. So is a snapshot taken at a microstep boundary.

| | A: bound to the interpreter | B: own state, action lists |
|---|---|---|
| returns to the system | on every send, cancel, invoke, refusal (3.33 calls per event, section 7.2) | once per microstep, and at a `<foreach>` flush (section 4.2); invokes and finalize at the macrostep's phases, as now (4.6, finding 12) |
| what is shared with the interpreter | instance, workspace, every continuation field, handler bodies | the instance layout; the action handlers' bodies (section 6.2) |
| snapshots | byte for byte, if every workspace field is written as the interpreter writes it | the same at boundaries and with a list pending; at a flush, the interpreter's continuation (4.6, finding 8) |
| an interpreter snapshot restored into compiled code | resumes in either | an actor caught mid-microstep finishes it in the interpreter, which stays in the module |
| ABI to the template | struct offsets of `ta_core`, `ta_workspace`, the instance and `kernel_context`; the shadow stack; about 20 internal functions kept out of line | function indices only (section 6.2), but those functions are a new public API between two libraries (4.6, finding 12) |
| semantics that move | none | the rest of a block after a send that leaves the system is evaluated before the send is decided, though its queue effects are applied after; no yield inside a microstep that passes `overrun_max` (4.6, finding 5) |
| projected per event, null, WASM | about 1.15× before compiled resume points (selection alone), 1.45–1.7× after (the refutation's count) | 1.8–2× (section 7.2) |
| what the spike measured | — | this design: the 14 ns step buffers its actions |

Design B is what the maintainer asked for: the chart's step is its own
WASM code with its own transient state. It also answers the first
refutation's main finding, that the spike timed a step doing B's work
while the draft recommended A. The second refutation found twelve places
where B, as first written, would not do what the interpreter does.
Section 4.6 lists them with their fixes. Until each fix is shown, B is a
sketch, not a recommendation.

## 4. Design B in detail

### 4.1 The action contract

`TA_STEP_ACTIONS` means: the microstep's executable content has run,
and `core->out` holds a list of `n` actions to apply in order. The
configuration was written **state by state as the microstep went**, as
the interpreter writes it. It clears a state's bit after that state's
onexit and sets it before its onentry (`core.c:1764`, `:1798`). `In()`
reads the instance's words in the middle of a microstep
(`ta_datamodel_environment_in`, `ta_core_in_state`, `core.c:2335-2343`;
`datamodel_ecmascript.c:735`), and W3C 409 and 411 test it. The list lives in a **pending-actions pool** in the segment's
region: an index array of fixed-size records, described in
`region_layout.c` like any other pool. Its records carry what
`core->out` carries for one action today:

- for a send: its index, an evaluated name, target, type and delay, a
  payload handle, a sendid string;
- for a cancel: a sendid;
- for a log: a label and text;
- for a raise, a done event or core's own error: a name id, and the
  error's data;
- for an autoforward: the invoke slot.

They are data the system applies without the code that made them.

| Step | The system's application | Unchanged from today |
|---|---|---|
| raise, done event, core's `error.execution` | push onto the internal queue | the event pool's push |
| send | `ta_system_route_send` (timers, routing, an effect); a refused timer raises `error.execution` (`schedule.c:141-145`). A send that leaves the system **parks the actor in the middle of the list**, as the interpreter parks it at the send (`schedule.c:147-153`). The host's `error.communication` (`host_communication_error`, `src/host/host.c:1488`) then lands before the actions that follow, and the rest of the list is applied when the send is decided. So an actor has at most one waiting send at a time, as now | the routing code |
| autoforward | the autoforward the system makes today for `TA_STEP_AUTOFORWARD` | yes |
| cancel | `ta_system_cancel` | yes |
| invoke, uninvoke | `ta_system_invoke` and its uninvoke; the child's handle written into the instance's invoke words | yes |
| log, done | the effect; the `TA_TRACE_LOG` record is written by the step at the `<log>`, not by the system (`step_log`, `schedule.c:97`), so it keeps its place and stamp in the journal | the effect, yes; the trace writer moves |

Nothing in the rest of a microstep reads the internal queue, and every
push onto it is in the list. So applying the list in order gives the
interpreter's queue order: W3C 402 and 496 are the tests. A send's own
evaluation errors (attributes, a target that `opcode_send` rejects)
still skip the rest of their block in the step, as they do in core, and
their `error.execution` goes on the list in its place.

**What moves:** the rest of a block after a send that leaves the system
is *evaluated* before the send is decided. Its queue effects are still
applied after the decision. A datamodel evaluation that reads time
(`Date`) sees the first run's time, not the time of the run that
resumes. Whether a W3C test observes this is unverified; the suite in
compiled mode settles it.

These were checked against the interpreter and need no change:

- a cancel of a send made earlier in the same block;
- generated send ids and `idlocation`;
- W3C 236;
- an uninvoke in an onexit after a send to that child;
- an assign or condition that reads `_event`, or data changed earlier
  in the block.

### 4.2 Resources, fuel, and the one pause

A compiled microstep cannot stop part-way through a block, because an
`<assign>` already done cannot be undone.

- **Reservation, before the event is taken.** The interpreter pops the
  event, binds it and evaluates conditions (side effects,
  `TA_TRACE_CONDITION` records, fuel) before the microstep
  (`internal_queue_pop`, `core.c:2005`). A refusal after that point
  would lose the event or evaluate the conditions twice: the class of
  bug commit `25ee9c8` fixed. So the step reserves **before it takes
  the event**, for the worst case of the chart or of the current
  configuration, both computed by the lowering: pending-action records,
  event slots and slab bytes. Static strings are known, and each dynamic
  string is bounded by `TA_SYSTEM_MAX_STRING`. `opcode_send` already checks the event pool,
  the timers and the slab's room before it evaluates anything
  (`core.c:1102-1116`); the compiled step does the same once per
  microstep, not once per send. When a pool refuses, the step returns
  `TA_STEP_EXHAUSTED` before anything has run, and the host grows the
  region as today. **At a pool's maximum**, where nothing can grow, the
  interpreter lets the first sends through and answers the rest with
  `error.execution` (`schedule.c:141-145`). The step must do the same:
  it runs with an admission check per action, not all-or-nothing, so
  the reservation is only the fast path. Reserving for dynamic strings
  at their bound costs slab headroom. How much, for real charts, is
  unverified.
- **`<foreach>` with actions.** Its count is known only at run time.
  This is common: 66 of the 69 repository charts with a `<foreach>`
  raise or send inside one, mostly fan-outs to members. Such a loop
  **flushes** when its reservation runs out: the step stores a small
  private continuation and returns `TA_STEP_ACTIONS`. The continuation
  is the loop's instruction, the iteration, the datamodel's array
  handle, and the masks it still needs. Section 4.6 (finding 8)
  proposes that it be written **in the interpreter's format**, so that
  a snapshot sees an ordinary interpreted actor mid-microstep. This is
  the only place where compiled code pauses mid-microstep.
- **Fuel.** The step charges the units the interpreter charges at the
  same points: one per candidate transition tested, per state exited or
  entered, per instruction, plus what the datamodel reports
  (`architecture.md` section 7.3). These are mostly constants per path.
  It yields only at microstep boundaries and `<foreach>` flushes. **So
  the fuel clock is the same only while no microstep passes
  `overrun_max`.** That is `quantum / 2 + 16`, 516 by default
  (`src/system/system.c:199`). Past it the interpreter yields in the
  middle of the microstep (`exhausted`, `core.c:85`, `:1381`, `:1700`).
  On quickjs one assign of more than about 51,600 JavaScript operations
  is enough (`FUEL_PER_CHECK` 100). From there turn boundaries, the
  interleaving of actors in the journal, and the stamps differ. The step
  gives up preemption inside a microstep; a datamodel call is already
  the unit of atomicity. A run whose evaluation reaches quickjs's
  `max_ops` ends that evaluation with `error.execution`, in both modes.
- **Cancel.** The cancel flag is checked at microstep boundaries, as
  the interpreter checks it (`core.c:1978`, `:2062`).

### 4.3 Tracing and inspection

Trace records are written through the same writers (`trace_step.c`'s
step records, `trace_condition`, `trace_sent`, `trace_cancelled`,
`trace_microstep`, `trace_macrostep_begin` and `_end`). They are called
with explicit facts, not read from a workspace, at the same points. Two
records are written by the system as it handles a step result:

- `TA_TRACE_LOG` (`step_log`, `schedule.c:97`) moves into the step,
  because under B it would follow `trace_microstep` (`core.c:2143`)
  with the microstep's end clock;
- `TA_TRACE_INVOKED` (`trace_invoked`, `route.c:405`) stays, because
  invokes stay in `PHASE_INVOKE` after the macrostep.

With these, **a run gives the same journal, and so the same OTLP,
while no microstep passes `overrun_max`** (4.2). Past it they differ.
The differential test (5.2) compares journals byte for byte under that
condition only.
`inspect`, `states`, `transitions`, `disassemble`, `graph` and `diff`
read the image, which the module keeps. `inspect` of a module adds each
chart's mode.

### 4.4 Snapshots, hibernation, migration, versions

| Situation | Design B |
|---|---|
| an actor at a microstep boundary (idle, or between microsteps) | the instance only, as the interpreter's; a snapshot is the same bytes, and restores in either mode |
| an actor whose list is pending (parked on a send that leaves the system, a full effect queue, a parked spawn, or growth) | the list is in a described pool, so it is part of the snapshot, and any build of the same layout applies it without compiled code |
| an actor at a `<foreach>` flush | proposed: the flush writes the interpreter's continuation (4.6, finding 8), so a snapshot holds an ordinary actor mid-microstep and either mode resumes it. Refusing the snapshot instead is rejected: `ta_host_snapshot` is host-wide (`host.c:2760`), the actor may wait for the embedder, and under fan-out one actor or another would always refuse |
| hibernation, export | unchanged: stable actors only (`architecture.md` section 16.1) |
| migration between versions | unchanged: `ta_blob_migrate` maps instances by state id; each version's code is keyed by its image hash, and a hash with no code runs interpreted |
| an interpreter snapshot restored into a compiled module, an actor mid-microstep | the interpreter finishes that microstep; it stays in the module for this, and for charts that are not compiled |

The pending-actions pool changes the region's types once, so the layout
id changes once, in the commit that adds the pool, as any change of
region types does (`region_layout.c`).

### 4.5 Lifetimes

| Object | Owner | Created | Read by, when | Ends | Thread |
|---|---|---|---|---|---|
| generated functions, the module's directory | the module | the writer, at `tinyactors compile` | `ta_core_step`'s dispatch | with the instance of the module | the instance's |
| the step's sets (selected, exit, entry) | the compiled step | the microstep's start, in WASM locals | the step itself | its return | the instance's |
| a pending-action record | the segment's pool | the step, during the microstep (reserved before the event is taken) | the system, after `TA_STEP_ACTIONS`, in order; the rest after a waiting send is decided, which may take the embedder's time | applied; or with the actor | the instance's |
| a send's payload handle in a record | the datamodel, by handle; the record holds one reference | `capture`, in the step | the system's routing | moved into the event or released, as `core->out.payload` today | the instance's |
| a datamodel string output (an evaluated event name, a log's text) | the datamodel, until its next call (ADR 0010) | the evaluation | copied into the slab at once (reserved) | the record's application | the instance's |
| a `<foreach>` flush continuation | a workspace, in the interpreter's format | the flush | the step's resume, or the interpreter's | the microstep's end | the instance's |
| effects, outputs | the host | the drain | the embedder | the ack (ADR 0012; `tinyactors-core/docs/boundary.md` section 7) | the instance's |

### 4.6 What design B must still fix

A second fresh refutation (2026-11-25) checked this revision against the
code. It reproduced the measurements: assign 42.0 %, `bind_event`
30.6 %, condition 5.1 %, `ta_core_step` 59.1 %, the step without its
datamodel calls 12.0 %, and the differential driver at 0 mismatches over
5,002 events at about 13–14 ns. It found design B not yet faithful.
Sections 4.1 to 4.5 now carry the fixes; this table is the checklist.
**Shown** means a test or measurement demonstrates the fix; none is
shown yet.

| # | Finding | Fix | Shown |
|---|---|---|---|
| 1 | Core's own `error.execution` (`error_exec`, `raise_failure`, `raise_platform` for assign, script, log, foreach, a cancel's `sendidexpr`, a send's own errors, donedata) pushed at once would swap with deferred raises (W3C 402); raises pushed at once would swap with route.c's `error.communication` (W3C 496) | every push onto the internal queue in a microstep goes on the list, in order (3.3, 4.1) | no: W3C 402, 496 in compiled mode |
| 2 | `In()` reads the configuration words mid-microstep (`core.c:2335-2343`, `datamodel_ecmascript.c:735`); the interpreter clears a bit after the state's onexit and sets one before its onentry (`:1764`, `:1798`); W3C 409, 411 | the step writes the instance's bits state by state, in that order (4.1) | no: W3C 409, 411 |
| 3 | The interpreter parks at the send (`schedule.c:147-153`), so the host's `error.communication` lands before the block's later pushes; applying the whole list first reorders it, and gives one actor several waiting sends | park in the middle of the list; the rest stays pending in the pool (4.1) | no: a scenario with a send that leaves the system, then a raise |
| 4 | `TA_TRACE_LOG` (`schedule.c:97`) and `TA_TRACE_INVOKED` (`route.c:405`) are written by the system per step result | the log record moves into the step; invoked stays, with invokes in `PHASE_INVOKE` (4.3) | no: the journal comparison |
| 5 | The interpreter yields mid-microstep at `overrun_max` = `quantum / 2 + 16` (`system.c:199`); B never does, so the fuel clock and the journal differ past it | the claim is restated: equal only while no microstep passes `overrun_max`; the differential test compares journals under that condition, and a test checks that an overrunning compiled microstep ends and charges its debt (4.2, 5.2). The alternative, yielding at instruction boundaries, brings back mid-microstep state | restated; the test is not written |
| 6 | Reserving after the pop, `bind_event` and conditions loses the event or evaluates twice (the `25ee9c8` class); at a pool's maximum B was all-or-nothing | reserve before the event is taken, for the chart's or the configuration's worst case; at a maximum, admission per action as the interpreter does (4.2) | no: the reservation sweep |
| 7 | The pending pool holds the rest of the list of every actor parked at once, so its worst case grows with the population; a new capacity reaches every binding and the published numbers | the cost table says so (section 1); the capacity is sized per segment like `max_events`, and refusal is growth | the cost is stated; the size is unmeasured |
| 8 | An actor mid-microstep blocks the host-wide `ta_host_snapshot` (`host.c:2760`) while it waits for the embedder; 66 of 69 charts with a `<foreach>` send or raise in it, so under fan-out a snapshot can starve | a flush writes the interpreter's continuation (its foreach frame, instruction and sets), so no compiled-format state is ever held; or the flush never yields, with the reservation sized for the loop once `FOREACH_BEGIN` knows its count. The first brings back part of design A's continuation, at one kind of point | no; how often fan-out passes the reservation is unmeasured |
| 9 | Level 0's 14 % counted selection, which it does not remove; "78 % quickjs" is the datamodel's wrapper, value conversion included; the spike's timer check is simulated from the last action's kind | at most about 10 %, unmeasured (2.2); worded as the datamodel's share (7.3); the timer check described as simulated (7.1) | stated |
| 10 | A scan for `_event` is not conservative, and `_event` is stored in blobs | `_event` built lazily, and made by the blob writer when absent; a scan only as an allow-list over bytecode atoms and opcodes (2.4) | no: lazy binding needs the pinned event's view after `bind_event` returns, which is unverified |
| 11 | `assign(core, instance, location, value)` has four parameters | runtime functions take `(context, pc)`, and C decodes the operands from the image (6.2) | stated |
| 12 | `src/lower` depends on about 20 core functions: a new API across libraries, against ADR 0001 decision 19 (`:235`); invokes start in `PHASE_INVOKE` after the macrostep (`core.c:2017`), `<finalize>` and `TA_STEP_AUTOFORWARD` run in `PHASE_EXTERNAL` (`:2073-2118`), which a per-microstep list does not cover | the runtime functions become public API in `tinyactors.api.json`, under `api --check`, with an ADR (section 1, 6.2); B keeps `PHASE_EXTERNAL` and `PHASE_INVOKE`: finalize runs compiled with its own list, autoforward is an action, invokes return one by one as today (3.3, 4.1) | no |

## 5. Holding a second implementation

### 5.1 The conformance suite in compiled mode

The module adapter (wasm-modules.md step 5) runs every suite against a
module whose charts are all compiled. That means:

- the protocol vectors;
- the scenarios;
- **the W3C IRP suite on quickjs**, with the same skip list as
  `ta-conform` (`tests/support/datamodel_test_w3c_skip.txt`).

Every case must pass, as `mise run conform` requires of `ta-conform`.
This is the main hold, and it is possible only because the step serves
every datamodel. On the null datamodel alone, W3C has one test
(`tinyactors-test/suites/w3c/irp/436/test436.txml`).

### 5.2 The differential test: what it covers and what it does not

It runs both modes from the same instance bytes and compares:

- the instance;
- the list of actions against the interpreter's sequence of `TA_STEP_*`;
- the fuel;
- the trace journal, byte for byte, **only in runs where no microstep
  passes `overrun_max`** (4.2); in other runs the configurations,
  queues and effects, but not the journal;
- the datamodel's serialized state.

**Null charts:** every reachable (configuration, event class) pair the
analysis layer finds, at most 1,080 per chart in the repository. History
words are seeded from random walks of the interpreter, because the
search has no valid source for them.

**ECMAScript charts:** seeded random event sequences, as the spike's,
and the suite's cases replayed.

What it does not cover, because a step depends on more than its
configuration and event:

| Input | Covered by instead |
|---|---|
| pool free counts and the reserve (`opcode_send`, `core.c:1102-1116`), slab room, workspace availability | targeted tests that refuse each reservation in turn (a sweep, in `--full` when large) |
| fuel and `overrun_max` | runs at quanta large enough that nothing overruns, compared in full; runs at small quanta, compared without the journal, plus a test that an overrunning compiled microstep ends and charges its debt |
| a full effect queue (`schedule.c:127`) | the system's existing tests, with compiled charts |
| tracing rules (what is written) | the journal comparison under each detail class |
| the cancel flag, an internal queue filled from outside (error.communication by the host), refused timers (`schedule.c:141-145`), a pool at its maximum | scenario cases, in both modes; W3C 402, 409, 411, 496 for the orderings of 4.6 |
| history beyond what walks reach | the scenarios' history cases (`parallel-history`) and W3C |

So the table oracle of level (c) is evidence over the configurations,
not a proof of the step.

## 6. Code generation without LLVM

### 6.1 Writing WASM

| Option | On the user's machine | Verdict |
|---|---|---|
| **core writes WASM binary directly** | nothing: the `tinyactors` binary | **recommended.** The spike's encoder is 30 lines of JavaScript; in C, about 400 lines (unverified) |
| emit C, compile it | clang or zig for wasm32 | out: the toolchain wasm-modules.md removed; possible later for native embedders |
| emit WAT | an assembler | out: a debugging view only |

The lowering is a new freestanding layer, `src/lower`, in
`libtinyactors_runtime` beside `src/compile` and `src/wasm_module`. It
reads the image view and level 0's static sets (shared with the
interpreter and the analysis, section 2.2), and writes function bodies
through `ta_output`. It uses no recursion. It takes `ta_bytes` in and
fills a `ta_buffer`, with at most three parameters per function.
`tinyactors compile --emit wasm --mode compiled` compiles every chart
and reports each one's mode. `--mode interpreted` is the default.

### 6.2 The step's runtime and its ABI

The generated code touches no struct. For everything but its control
flow it calls about 20 **runtime functions**
(`src/core/compiled.c`). Each takes at most three parameters: a context
(the core and the instance, which the step's entry receives) and a
number, such as `(context, pc)` for an `<assign>`, whose location and
value C decodes from the image's bytecode. Some of them:

- `configuration` (the address of the instance's configuration words);
- `begin` and `end` of a macrostep or microstep;
- `condition`, `assign`, `capture`, `log` and `data`, each by an
  expression index or a bytecode pc, never by several operands;
- `action` (append a pending action), `reserve`, `flush`;
- the trace writers.

Every result is a number returned. No address of a local is taken, so
generated functions need no shadow stack. Each runtime function wraps a
`static inline` body that the interpreter's `vm` inlines too, so each
action has one implementation, and the interpreter's code and speed do
not change (unverified until built: `-O2` must still inline it into
`vm`). The template lists the runtime functions' indices in a custom
section, the directory the writer reads, versioned with the template's
ABI (wasm-modules.md section 9). The writer appends the generated
functions, adds an element segment for the template's table, and fills
a per-chart directory (image hash, chart, table index), with no index
renumbered. No struct offset or layout is in the writer, so
boundary-design's rule holds: layouts are hand-written C, and only C
touches structs.

**These functions are an API between libraries.** `src/lower` lives in
`libtinyactors_runtime`, and ADR 0001 decision 19 says that library
"calls nothing of `libtinyactors`'s but its public API"
(`docs/adr/0001-packaging-and-releases.md:235`). Code the runtime library
generates, calling about 20 functions of the core library by index, with
their signatures and meaning, is such a call. So the functions are
public API of the core: declared in a header, listed in
`tinyactors.api.json`, held by `api --check`, and changed in place under
the 0.x rules, with an ADR that records the decision. Their exports in
the template are that API's WASM face.

## 7. Measurements

Machine: Intel Xeon @ 2.10 GHz, 4 vCPUs, Linux 6.18, Node 22.23.5, zig
0.16.0 at `-O2`, the 0.4.0 amalgamation (`tinyactors-core/dist/`),
2026-11-25, run one after another. The files are in
`/tmp/claude-0/-home-user/48aea8a4-edf2-5799-91e9-403c27e0dc94/scratchpad/wasm-aot/`
(`bench/`, `lowered/`, `configs/`).

### 7.1 The charts and the spike

- **Null:** `bench/order.scxml`, an order with two parallel regions.
  The order runs `cart`, then `paying` on `checkout`, guarded by
  `In('cleared')`; `paying` has a 30 s deadline, a delayed send that is
  cancelled on exit. The risk check runs `scoring`, `cleared` and
  `held`.
- **quickjs:** `bench/order-es.scxml`, an order whose `item.added`
  assigns `items` and `total`; `checkout` is guarded by `items > 0`;
  `paying` assigns `attempts` on entry with the same deadline; a
  declined payment assigns `problem` and `items`.

A benchmark cycle is `item.added`, `checkout`, `payment.declined`.
`bench/interp.c` and `bench/interp2.c` run one actor, with a send, a
run, a take and an ack per event.

**The lowering spike:** `lowered/lower.mjs` writes WASM binary from the
`.tai`. Its step buffers sends and cancels as actions, which is design
B's shape without the runtime functions. It refuses more than 32
states, history, eventless transitions, `<raise>`, dynamic sends,
conflicts and datamodels other than null. It ignores executable content
in `<initial><transition>` (`core.c:1819-1824`), which the chart has
none of. `lowered/drive.mjs` replays the interpreter's recorded 5,002
seeded events (8 configurations, 558 sends and cancels). **No
configuration and no count of pending timers differs from the
interpreter's.** The timer count on the lowered side is simulated: the
driver sets it from the kind of the last action of each step, a send
arming the deadline and a cancel disarming it. The interpreter's count
comes from `ta_system_stats.timers`. This is not a comparison of the
two sequences of actions, which the real differential test makes
(5.2).

### 7.2 The null chart

| Build | ns per event, the last rounds | Instructions per event (callgrind, native) |
|---|---|---|
| interpreter, native, no tracing | 343, 344, 343, 344 | 4,121 |
| interpreter, native, tracing | 505, 503, 527, 656 | 6,000 |
| interpreter, WASM in Node, no tracing | 522, 529, 526, 505 | — |
| interpreter, WASM in Node, tracing | 778, 836, 800, 862 | — |
| the lowered step alone (design B's work, without its runtime calls) | 14.1, 14.8, 13.8, 13.5 | — |

- **The step's share:** `ta_core_step` takes 64 % natively (2,636
  instructions per event) and 62 % in WASM (V8's sampling profiler,
  `bench/prof.mjs`).
- **Its calls:** it is entered 200,003 times for 60,000 events, `micro`
  100,002 times.
- **Its parts, exclusive, per event:** `micro` 610, `select_transitions`
  443, `ta_core_step`'s own 414, `init_k` 414 (layouts recomputed on
  each of 3.33 calls, `core.c:271`), `run_code` 200, the `In()` parse
  134, `release_temporaries` 80, `trace_sent` and `trace_cancelled` 28.
- **Outside the step:** a send's name lookup in the trie costs 279
  instructions (7 %).

**Projection for design B**, unverified. What stays outside the step is
about 38 %, or 200 ns. Kept inside it:

- the event's bind and release;
- the step-counter and macrostep hooks;
- the reservation and the appending of actions;
- the runtime calls.

That is about 300–500 instructions, 40–65 ns. With the generated 14 ns,
an event costs **about 255–280 ns: 1.8–2× cheaper**. With tracing on,
the records cost the same in both modes, so about 820 ns becomes about
570 ns, 1.45×. **For design A:** about 1.15× before compiled resume
points (selection and `In()` are about 14 % of the event), and 1.45–1.7×
after (680–1,100 instructions kept, the refutation's count).

### 7.3 The quickjs chart

| Build | ns per event, last rounds | First session: spawn and its first cycle |
|---|---|---|
| native, shared realm (the default) | 1,989, 2,014, 2,107, 2,319, 1,968, 1,961 | 116 us, then 18 us, 10 us |
| native, isolated realm | 1,371, 1,467, 1,167 | 306 us, 310 us, 234 us: every session |
| WASM in Node, shared realm | 3,532, 3,437, 3,583, 3,312, 3,306, 3,741 | 1,103 us, then 46 us, 29 us |

Where the WASM event goes (V8's profiler):

| Part | Share |
|---|---|
| `ecmascript_assign` | 42.3 % |
| `ecmascript_bind_event` | 30.8 % |
| `ecmascript_condition` | 5.1 % |
| `ta_core_step`, without its datamodel calls (59.5 − 42.3 − 5.1) | **12.1 %** |
| `ta_system_send` | 2.5 % |

Natively (callgrind) an event is 21,975 instructions. 81 % of them are
in the datamodel's calls: quickjs and the datamodel's own wrapper,
value conversion included. The other 4,141 are about the null chart's
whole event.
The chart's expressions never read `_event`.

So the compiled step can save **at most about 12 % of a quickjs event,
1.14×**, and with design B's kept work about 1.05–1.1× (a projection).
In this mode the datamodel dominates the quickjs event. Its own
frontloading targets the larger costs:

- `_event` binding, 31 %, on a chart that never reads it;
- the first session of a chart, 1.1 ms in WASM;
- the isolated realm, about 250 us per session natively.

What removing them saves is unverified. Tracing on quickjs was not
measured.

### 7.4 Size

| Chart | Image | Generated module | `step` |
|---|---|---|---|
| the null order (11 states, 12 transitions) | 2,816 B | 831 B | 689 B |
| `tests/diagram/insurance-claim.scxml` (11, 13) | 2,600 B | 842 B | 700 B |
| synthetic, 4 regions × 4 states (22, 21) | 3,680 B | 1,252 B | 1,110 B |
| synthetic, 6 regions × 4 states (32, 31) | 5,032 B | 1,848 B | 1,706 B |

The spike refused `order-processing`, `warehouse-order` and `checkout`
(conflicts), `support-ticket` (history) and `subscription`
(ECMAScript). The interpreter's step functions are about 10 KB of the
WASM build's 242 KB of code. The interpreter stays, so compiling adds
code.

### 7.5 Not measured

- a compiled step with its runtime calls, reservation and action
  application, so every projection above is unverified;
- the datamodel-side savings (bytecode ahead, `_event` skipped);
- tracing on quickjs;
- wasmtime;
- slab headroom for reservations;
- static entry sets (at most 11 % of the null step);
- the lowering's size in C;
- whether `-O2` still inlines the shared action bodies into `vm`.

## 8. What remains runtime no matter what

| Part | Where it lives | Why |
|---|---|---|
| mailboxes, the internal queue, the event pool | system, core's event pool | contents arrive at run time |
| timers | system | the clock enters through `ta_host_advance_time` |
| routing, segments, the transport, I/O processors | system and host | targets resolve against live sessions |
| `<invoke>` and children | system | a child is another actor |
| values, conditions, assignments | the datamodel, by handle and expression index | the maintainer's direction; the engine's |
| hibernation, migration, snapshots | system and host | they read instances, which do not change |
| tracing, OTLP, metrics | the trace writers, `src/otlp` | the same records at the same points |
| the scheduler, fuel, the quantum | system | fairness across actors |

## 9. Recommendation

1. **Level 0 first, on its own:** static entry sets and domain masks in
   the image, read by the interpreter, the analysis and the lowering;
   `In()` resolved. It is small (at most about 10 % of a null event,
   unmeasured as a whole) and helps the interpreter whatever is decided
   next.
2. **Prepare the datamodel at compile time, quickjs first:** measure the
   split of a first session, then remove the first-use compiles
   (bytecode at `tinyactors compile`), and build `_event` lazily with the
   blob format answered. For the ECMAScript charts the repository is
   made of, this targets the larger costs.
3. **Design B stays a sketch** until each fix of section 4.6 is shown.
   It is where the step can become the chart's own WASM code for every
   datamodel. It is worth building only where the step is the cost: the
   null datamodel (1.8–2×, projected, before the fixes' own costs) and,
   later, staticjs. On quickjs it buys at most 1.1×. The first thing to
   build for it is the native, hand-written step of step 3 below, which
   shows or refutes the fixes before any code generator exists.

**What is given up, if B is built:**

- **a second implementation of the step**, which the maintainers keep
  in agreement with the first: every change to the algorithm in
  `src/core` changes the lowering in the same commit;
- **preemption inside a microstep** with no loop, and so the same
  journal past `overrun_max`;
- **evaluation order around a send that leaves the system:** the rest
  of its block is evaluated before the decision, though its queue
  effects are applied after;
- **a new capacity** (pending actions) whose worst case grows with the
  population of parked actors, in every binding's config and in the
  published numbers;
- **a new public API** between the runtime library and the core, with
  an ADR;
- **one change of region layout,** for the pending-actions pool;
- **code size,** about 50 B per transition per chart;
- **little for quickjs,** and nothing for Node's or Python's engines;
- **nothing for native embedders** until an `--emit c` path exists.

## 10. Order of work

Each step is a commit that passes `mise run test` in core:

1. **Level 0.**
   - Static entry sets and domain masks in the image, read by
     `compute_entry` and by `ta_analysis_machine_init`.
   - `In()` resolved: an expression kind of its own, or the null
     datamodel's `chart_attach`.
   - The image version changes. Tested by `test_core`, `test_analysis`
     and `conform`.
2. **The quickjs measurements of section 7.3, split:** attach analysis,
   context creation and compiles per expression. Then bytecode ahead,
   and `_event` built lazily with the blob writer making it when absent.
   Each is measured against the interpreter, side by side.
3. **Design B's contract, shown natively, before any generator:**
   - `TA_STEP_ACTIONS`, the pending-actions pool and its capacity, with
     the binding and documentation changes of finding 7;
   - parking in the middle of the list, the log record moved into the
     step, and per-action admission at a pool's maximum;
   - a step for the null order chart written by hand in C, under the
     differential harness of 5.2 and the reservation sweep;
   - W3C 402, 409, 411 and 496, and a scenario that parks on an
     external send and then raises.

   If a fix cannot be shown here, B stops here.
4. **The public runtime API, with its ADR** (finding 12), and
   **`src/lower` and its encoder** for the null datamodel, through the
   module writer and `--mode compiled`. The differential test runs
   inside WASM in `wasm:test`. Measured: the null charts of the torture
   chamber and the scale benchmark, interpreted against compiled,
   throughput and memory side by side.
5. **Every datamodel:** datamodel calls by expression index; `<foreach>`
   flushes that write the interpreter's continuation; `PHASE_EXTERNAL`
   with finalize and autoforward; history, eventless loops, conflicts
   and done events.
6. **The suite in compiled mode:** the module adapter, with protocol,
   scenarios and W3C on quickjs. Every case must pass.

## 11. Open questions for the maintainer

- **Is a second implementation of the step worth 1.8–2× on the null
  datamodel and at most 1.1× on quickjs**, or should the work stop at
  level 0 and the datamodel's preparation until a staticjs datamodel
  makes the step the cost?
- **Evaluation order around a send that leaves the system:** is it
  acceptable that the rest of the block is evaluated before the send is
  decided (only its queue effects wait), if the suite agrees?
- **Preemption:** may a compiled microstep run past `overrun_max`
  without yielding? The fuel clock and journal then differ from the
  interpreter's for that run.
- **`<foreach>` flushes:** should they write the interpreter's
  continuation (snapshots never blocked), or should the loop never
  yield, reserving for its count when it starts?
- **The pending-actions capacity:** a new config field in every
  binding, or carved from the existing event pool?
- **Is preparing the datamodel ahead part of this mode?** Bytecode in
  the image or beside it, keyed by the datamodel's version; `_event`
  built lazily, and what the blob holds when it was never built.
- **Native too?** `--emit c` for C embedders would need a public way to
  bind a step to a chart, and so an ADR.
- **Name ids on inputs:** a name lookup is 7 % of a null event, for
  every datamodel and outside this mode.

## 12. Checking the recommendation

**Two refutations ran, both through the session that asked for this
proposal.** This revision was written by a delegated agent with no tool
to start another agent, so **no refutation of this third revision has
run**, and the session that asked should start one.

The first refutation, of the first draft, is folded in:

| Finding | What changed |
|---|---|
| 1. The 1.9× rested on a step doing other work (200,003 step calls, kept work omitted) | design B is the one the spike timed; A's figures are restated as 1.15× before and 1.45–1.7× after resume points (7.2) |
| 2. Only configurations compared; initial-transition content ignored; the 1,080-pair bound mixed two charts | pending timers are compared too, simulated (7.1); the spike's gap is stated (7.1); pairs are per chart, 1,080 being `car`'s own (2.3) |
| 3. "Not a second implementation" was false | said plainly (3.1); held by the full suite in compiled mode (5.1) |
| 4. The oracle is not a proof | what the differential test covers and does not, with what covers the rest (5.2) |
| 5. The fallback continuation is far larger than drafted; snapshots not byte-identical; architecture.md 7.1 drift | design A described with its real continuation (2.1, 3.2); B avoids it; the drift noted (2.1) |
| 6. Struct offsets and inlined handlers as a writer–template ABI | B's ABI is function indices only, wrappers over shared inline bodies (6.2) |
| 7. The null population has no application charts; the torture charts save little | the step now serves every datamodel; the torture claim is dropped; quickjs measured (7.3) |
| 8. The own-state design was dismissed | it is now the main design, with its effect contract, fuel, tracing and snapshots (3.3, 4) |
| 9. Projections stated as results; an uncited review | marked as projections throughout; the review is named with its verdict |

The second refutation, of the second revision, reproduced the
measurements, kept level 0 and the datamodel's preparation as the better
first steps, and found design B not yet faithful. Its twelve findings
and their fixes are section 4.6. As a result, B moved from "recommended"
to "a sketch pending these", and the text of sections 1 to 11 was
corrected where it was wrong:

- the cost per actor, the work in every binding, and the API;
- the fuel and journal claims;
- the level 0 figure;
- the 78 % wording;
- the timer check;
- the snapshot refusal, replaced by a flush in the interpreter's format.

The author's own pass on this revision:

| Claim | Verdict |
|---|---|
| the datamodel's calls dominate a quickjs event in WASM (78 %, wrapper included; the step's own part 12 %) | confirmed for one chart by both refutations' reruns; other charts unmeasured |
| B's queue order equals the interpreter's, with every push on the list and parking mid-list | plausible: nothing in a microstep reads the internal queue; settled by W3C 402 and 496 and the differential test |
| A reservation before the event is taken makes a microstep atomic | plausible below the pools' maxima, with admission per action at them; slab headroom unmeasured |
| Snapshots at boundaries are the same in both modes | plausible: the interpreter releases its workspace at the end of each microstep (`core.c:2144`); settled by comparing snapshot bytes in step 3 |
| The datamodel's preparation pays more than the step on quickjs | plausible: the costs it targets are measured, the savings are not |
