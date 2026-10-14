# Roadmap

Work across packages, in the order it is meant to happen. Ideas that are not
decided yet are in [`docs/proposals/`](docs/proposals/README.md); how the
layers divide the work is in the [README](README.md#layers-and-responsibilities).
The plan below carries out [ADR 0009](docs/adr/0009-one-host-two-queues.md)
(one host, two queues). Earlier roadmaps are in git: this file before
2026-10-12, and the one of the ring protocol at `943b1f31^`.

## Where we are

ADR 0009's steps 1 to 7 are done (2026-10-11):

| Step | What | Commits |
|---|---|---|
| 1 | ADR 0009; at most three parameters (`AGENTS.md`) | `6b79712d` |
| 2 | The bindings, examples, link protocol, JSON reader, framing and batcher deleted | `997706b8` |
| 3 | CBOR as token lists (`cbor.h`) | `2a2c6820`, `ad45e3a2` |
| 4 | Neutral values are CBOR | `b114102d` |
| 5 | The host: structs and `ta_buffer` (5a), inputs during a run (5b), the OTLP encoder and a null encoder (5c), `ta_host_request` with the JSON-RPC 2.0 shape (5d), effect accessors, `actor.context`, `actor.subscribe`, CBOR config (5e), envelopes across processes, decoded at the target (5f) | `3509db80` … `93a9c047` |
| 6 | Protocol vectors (`tinyactors-core/tests/protocol`), `protocol:encode` | `cc0f193c`, `b539dae9`, `4881cbd1` |
| 7 | The binding guide, [`tinyactors-core/docs/bindings.md`](tinyactors-core/docs/bindings.md); its cleanup (`area.verb` methods, the vtable on the buffer convention) | `082f95a1` … `72823569` |

Step 8 builds the bindings, and this roadmap is its plan. A first pass
(M1 of the earlier plan, 2026-10-12) built both bindings far enough to
show where the boundary was hard to use: four states of an effect, five
lifetime classes, a synchronous request facade. Both bindings were gutted
for a rewrite from scratch (`6426816a` cpython, `70434bee` napi: their
`src/`, adapters and binding docs deleted, their tooling kept), and the
maintainer set a new order on 2026-10-13 (below, "Order of work"): the
documents first, then core, then the whole loop in C, then a minimal
Python binding, then Node.

## Step 8: the bindings

### What changed from ADR 0009's sketches

The sketches in `docs/proposals/one-host-two-queues/` are a record of
findings, not a specification: they predate steps 3 to 7. Build from the
guide and the headers. Three decisions replace theirs:

1. **The suite is shared, not code.** A conformance suite,
   `tinyactors-test`, holds every test that is data (protocol vectors,
   self-checking scenarios, W3C) and runs it against one command per
   implementation. A binding is done when the suite passes against it.
2. **Each binding is its extension's C.** The Node-API addon defines the
   JavaScript classes, runs the loop, dispatches effects, converts values
   and implements the native ECMAScript datamodel in C, as the CPython
   extension does for Python. There is no shared JavaScript package
   (`tinyactors-js` is dropped). The extension is the binding's **direct
   layer**, mirroring the C API; a **convenience layer** in the language
   (chart builders, spawning by name) may sit above it later, built only
   on the direct layer's public API, never reaching core another way
   (amended 2026-10-13).
3. **Native datamodels come first.** Python programmers write chart logic
   in Python; a binding without its language's datamodel is not the
   product. The datamodel is in the first milestone of each binding,
   held to the same suite as quickjs: both must pass it.

### Rules for the work

- **Whom it serves first:** the application developer in Python and in
  Node; then the binding author (the guide, held to account by the suite)
  and the maintainers (one suite for every implementation).
- **Sessions.** One coordinating session owns `tinyactors-core` and
  `tinyactors-test`. A Python session owns `tinyactors-cpython/`; a Node
  session owns `tinyactors-napi/`. The Python session starts at step 8.4,
  after steps 8.2 and 8.3; the Node session starts at step 8.5, after
  the Python binding's API review.
- **Each binding keeps its own roadmap**, `tinyactors-cpython/ROADMAP.md`
  and `tinyactors-napi/ROADMAP.md`: the session's plan and progress for
  its step (the step's scope and gates copied from here, with a link
  back, and its "done means"), how the work runs for that session (these rules
  as they apply to it), and its "Requests to core". The binding's
  `AGENTS.md` holds only the binding's own rules, true whatever the
  milestone. A binding session never edits this file; the coordinator
  reads theirs, moves requests into "Requests to core" below, and marks
  milestones done here.
- **Core is frozen for the binding sessions.** A gap in core is written
  under "Requests to core" in the binding's own `ROADMAP.md` (what, why,
  which test shows it) and the binding waits or works around nothing. The
  coordinator moves it here, makes the change, with C tests and suite
  cases, and tells both sessions to rebase. A binding never patches core
  or the suite.
- **One checkout, on `main`, each session commits only its project's
  paths** (`git commit -- tinyactors-cpython/`). Git's `.git/index.lock`
  belongs to whichever session's command holds it: wait and retry, never
  delete it. Each binding builds core from the amalgamation into its own
  `build/`; nobody but the coordinator runs `tinyactors-core`'s build or
  its `mise run test`.
- **A binding is held to its milestone's set of the suite.** Each
  binding's `mise run test` runs `conform -- --milestone <M>` (the
  milestone in its `mise/<binding>.rb`, the one place to move it), and
  `tinyactors-test/suites/milestones.txt` lists what each milestone runs,
  cumulative: M1 every protocol vector file, the basics scenario and the
  outcome canaries; M2 the W3C suite; M3 invoke; M4 hibernate, migrate
  and snapshot; M5 nothing more (the suite's names for its sets; step
  8.4's gate is M1, and the sets grow with step 8.5). Without `--milestone` the whole suite
  runs, as `ta-conform`'s does.
- **Each session runs its own project's `mise run test`** and the suite
  against its command. The root `mise run test` and core's `--full` run in
  the coordinating session.
- **Rewrites with ast-grep**, rules and checks as ast-grep rules with
  tests. The repository's rules hold in every binding: at most three
  parameters, `ta_bytes`/`ta_buffer`, no serialization on the hot path,
  acronyms. At most three parameters applies to every C function a
  binding defines, static helpers included, and its lint checks all of
  its C; a function a platform's typedef fixes (CPython's `PyCMethod`
  with five parameters, a `METH_FASTCALL | METH_KEYWORDS` method with four)
  is exempt by that typedef, not by name.
- **Our names carry the binding's prefix**: `ta_python_*` in
  `tinyactors-cpython`, `ta_napi_*` in `tinyactors-napi`. A platform's names
  appear only where the platform requires them (the CPython slot
  `Py_tp_traverse` filled with `ta_python_host_traverse`).
- **Findings are checked before they change the plan**
  (`.agents/skills/reasoned-analysis`): a scenario in this design, evidence
  or a spike, and a refutation by a fresh agent.
- **An audit after each milestone**, by the coordinator: both bindings
  against the guide's "never" list, against each other file by file, and
  against their mapping tables. It writes no code; what it finds is fixed
  in the owning session.

### Gates

Every gate is mechanical and runs in the owning project's `mise run test`.

| Gate | Where | Catches |
|---|---|---|
| **Protocol vectors**: byte-exact requests, config, envelopes and values, through the binding's own CBOR conversion | `tinyactors-test/suites/protocol` | drift in what crosses as CBOR |
| **Scenarios**: charts in the W3C notation that check what happened themselves and end in `pass` or `fail`, with the host operations between runs (`steps.txt`), run by the implementation's real loop and effect handlers; no effect is compared | `tinyactors-test/suites/scenarios` | the loop, effect handling, ordering, time, hibernation, migration, snapshots |
| **Differential runs**: the same scenarios and W3C suite on the native datamodel and on quickjs; both must pass, and no outcome is compared beyond that | the runner | a native datamodel, or quickjs, that fails the suite |
| **W3C IRP**: every automatic test per datamodel, except its language's skip list (`skip-<language>.txt`, each with a reason) | `tinyactors-test/suites/w3c` | SCXML semantics end to end |
| **Mapping table**: `docs/api-map.md`, each API item next to the `ta_*` function, request method or effect behind it, checked against `tinyactors.api.json` and `requests.md` | each binding | API items with nothing in core behind them |
| **Layer lints** (ast-grep): the extension calls only `ta_*` and its runtime's API; `src/datamodel/` calls only `ta_datamodel_environment_*` and the language's API; one timer, in `loop.c` | each binding | reinvented layers |
| **Size tripwire** (designed, not built; below): a line budget per file of `src/` and the adapter, in the binding's `size-budget.txt`, set at the M1 audit; a file over its budget fails the binding's lint until the budget file raises it with a reason | each binding | slow growth |

What a black box cannot show stays as each project's own tests: memory
failures, refusal during a run, threads, garbage collection, liveness.

**The size tripwire, designed (2026-10-12; built at step 8.4's audit):**

- **Where it lives.** `tinyactors-<binding>/size-budget.txt`, one line per
  file it budgets: `<path> <baseline> [<raised> <reason…>]`. It covers
  every file under `src/` and the adapter (`conform/`, `conform.mjs`),
  and `index.mjs`/`index.d.ts`. Lines are counted as `wc -l` counts them,
  so the number is never in dispute. One checker for both bindings,
  `mise/lib/size_budget.rb` at the root (with its test, run by the root's
  `test` like `api-map-test`), called by each binding's `mise run lint`.
- **How the budget is set.** At step 8.4's audit (8.5's for Node) the coordinator runs
  the checker's `--baseline` mode in each binding: every file's count at
  that commit, rounded up to the next multiple of 50 (room for fixes, not
  for features), written as its baseline, and committed with the audit.
  Before that the lint only prints the counts. A file added later gets
  its baseline the same way in the commit that adds it, so a new file is
  a line in the diff.
- **How an excess is acknowledged, mechanically.** A file over its
  budget (`<raised>` if the line has one, else `<baseline>`) fails the
  lint. The commit that grows it past the budget edits its line: a
  `<raised>` number at least its new count and a reason of at least a few
  words; the checker refuses a raised number without a reason, a raised
  number not above the baseline, and a file with no line. The baseline
  never moves after it is set, so `git log -p size-budget.txt` lists every
  raise with its reason, and the audit after each milestone reads that
  list.

### Milestones

#### M0: the suite and its reference (coordinator)

1. **`tinyactors-test`**: the project, the runner
   (`mise run //tinyactors-test:run -- <command…>`), the protocol it speaks
   to an implementation (`docs/driver.md`), and the data moved out of core:
   `tinyactors-core/tests/protocol` and `protocol:encode`, and the W3C
   suite's fetch and conversion (`tinyactors-core/mise/core.rb` `w3c_suite`,
   `tools/w3c/convert.py`).
2. **`ta-conform`**, the C reference implementation of that protocol, in
   `tinyactors-core/tools/conform/`. Core's `mise run test` runs the suite
   against it, in place of `run_protocol.c` and `run_w3c.c`; the sources
   table in `AGENTS.md` gets a `tinyactors-test` row. Done: every method
   of `docs/driver.md` (`1526c3da`, `7d860a0d`); `mise run conform` runs
   every suite on quickjs, scenarios and W3C in isolated realms, and W3C on
   the tests' datamodel with its skip list; `run_protocol.c`, `run_w3c.c`,
   `run_c.c` and `tests/conformance` are deleted, their checks moved into
   unit tests (`47afc430`).
3. **Scenarios**, ported from `tinyactors-core/tests/conformance`
   (basics, invoke, hibernate, migrate, snapshot) into
   `tinyactors-test/suites/scenarios` as self-checking charts; each
   `steps.txt` names the expectations a chart cannot check and the C test
   that covers them. Nothing is recorded: `ta-conform` runs them
   (`scenario.run`) and they must pass. Done: ported with item 1, run
   through `ta-conform` with item 2 (`7d860a0d`), and each expectation a
   chart cannot check is in a unit test (`47afc430`).
4. **The project skeletons** `tinyactors-napi/` and `tinyactors-cpython/`
   (`mise.toml`, tasks, lint configuration with the typedef exemption,
   empty mapping tables), and the changelog components in
   `mise/lib/changelog/components.rb` (the `napi` component points at the
   deleted `bindings/node` today). Each skeleton's `AGENTS.md` carries the
   prefix rule, the host's allocator (`PyMem_RawMalloc`; `malloc` with
   `napi_adjust_external_memory`), and the pitfalls M1 lists for its
   binding. Each skeleton's lint task wires in the adapters' lint
   (`mise run //tinyactors-test:lint -- conform.mjs`, `-- conform`), so
   its adapter imports only the public module (`@tinyactors/node`,
   `tinyactors`), and its conform task runs the suite with
   `--kind binding`. Done (`798a0191` … `82db7e4d`): each builds core's
   amalgamation into its `build/core` (core's `amalgamate --out`), lints
   all of its C with core's parameter and buffer patterns (utility rules
   in `tinyactors-core/tools/lint/utils`, the typedef exemption by
   parameter types) and with the layer, timer and prefix rules, and checks
   its `docs/api-map.md` (`mise/lib/api_map.rb`); its `mise run test`
   passes with no code and skips the suite until the adapter exists; the
   root's `mise run test` runs both. zig stays pinned once, in the root
   `mise.toml`, which every release build uses.

5. **The W3C suite in git, and `confPython.xsl`.** The IRP suite is
   vendored in `tinyactors-test/suites/w3c/irp`, unmodified, with its
   `SOURCE` and `LICENSE` (W3C's 3-clause BSD license), and the fetch is
   gone. A language is a stylesheet in `suites/w3c` named by the datamodel
   it writes, with a skip list and a compile check
   (`tinyactors-test/README.md`, "Languages"): `confPython.xsl` translates
   the `conf:` notation for the python datamodel, `skip-python.txt` lists
   the 19 tests of the ECMAScript data model and 567, and
   `mise run check-languages` (in tinyactors-test's `mise run test`)
   compiles every expression, location and script of the rest with
   Python's `compile()`. Done (`c3f3328f`, `56b08120`, `08ff787b`,
   `b3c6a047`).

Done when: the whole suite passes against `ta-conform`, and core's tests
run through it.

Follow-ups for M0, done (2026-10-12):

- **(a) The runner fails a run that proves nothing:** zero passes in a
  suite, or a skipped method the implementation's kind must offer. A
  required-method list per kind: `ta-conform` offers all of them; a
  binding all but the C-level ones (`value.encode`, `envelope.read`, and
  `host.request`'s capacity). Done (`0bd04902`): the run task's `--kind
  binding|reference` (default `binding`), stated by the project that runs
  the suite rather than by the command under test, so an adapter cannot
  lower its own bar and the protocol needs no new field
  (`tinyactors-test/docs/driver.md`, "Kinds"); core's `mise run conform`
  passes `--kind reference`.
- **(b) Outcome canaries:** scenarios that must end in `fail`, `timeout`
  and `compile`, with the expected outcome stated in their `steps.txt`, so
  a runner or adapter that reports `pass` for everything fails. Done
  (`e16237ea`): `outcome-fail`, `outcome-timeout`, `outcome-compile`, each
  with an `expect` line.
- **(c) `value.roundtrip` replaced by the public path:** `envelope.deliver`
  to an echo chart that sends `_event.data` back to `_event.origin`,
  comparing the data of the outgoing `PEER` envelope. A spike gave
  identical bytes for all 11 roundtrip cases of `values.txt` in both
  quickjs realms: one `from_wire` at the target
  (`tinyactors-core/src/host/router.c:546-580`) and one `to_wire` in
  `ta_host_envelope` (`router.c:1229-1267`), as
  `tinyactors-core/tests/unit/test_router.c:1402-1407` counts them. And
  `envelope.deliver` calls `ta_host_deliver` after a failed read, so every
  malformed case answers route `REFUSED` and a `malformed` error envelope,
  as core does (`test_router.c:1279-1301`). Done (`cc5f64fd`): the echo
  setup's chart is written once in the `conf:` notation
  (`suites/protocol/setups/echo.txml`) and converted per language;
  `value.roundtrip` is gone from the driver and `ta-conform`.
- **(d) Vectors:** lone surrogates refused (in a value and in a key); an
  encoded surrogate (`63 eda080`) malformed; Python lines for the encode
  and invalid cases where they apply; round trips outside ECMAScript for
  tag 32 and `undefined`. Done (`9ff8b2bb`); core already refused each of
  them, in both realms.
- **(e) An adapter lint in each binding skeleton:** the adapter imports
  only the binding's public module. Done (`cfe4b62e`): ast-grep rules
  with tests in `tinyactors-test/lint`, run by
  `mise run //tinyactors-test:lint -- <adapter>`; each skeleton wires it
  in (item 4).
- **(f) The runner picks the chart language from the datamodel name**
  `host.create` reports, so a run that asks for a native datamodel cannot
  be served by quickjs. Done (`0bd04902`): `--language` is gone. Where the
  native datamodel is ECMAScript too (Node's), both names are
  `"ecmascript"`, and the adapter's `host.create` puts only the datamodel
  asked for in the host.

Preparing the skeletons for M1, done (2026-10-12), after a
reasoned-analysis review of both and the maintainer's decisions:

- **The Python floor is 3.13**, with `PyMem_RawMalloc`/`PyMem_RawFree` as
  the host's allocator (`4eda333e`; "File trees", below).
- **The runner runs a milestone's set** (`--milestone`,
  `tinyactors-test/suites/milestones.txt`; `ea3cc988`), and each
  binding's `test` runs its milestone's (`acf7be61`, `1195cccb`).
- **Each binding keeps its own `ROADMAP.md`** with its plan, progress and
  requests to core (`a5fa4e43`, `b2d54c42`; "Rules for the work").
- **The adapters use the binding's own CBOR** (M1, below), and their lint
  refuses any other package (`e64e96bb`).
- **"M1 done means"** in each binding's `ROADMAP.md` (first written in
  its `AGENTS.md`, which now holds only the binding's rules); the
  alignment core asks for (at most 8) and Bun's resolving the package by
  name, checked in place of two unverified notes (`a5fa4e43`,
  `b2d54c42`).
- **The root's checks reach the bindings:** `check-portability` scans
  `*/mise/*.rb` and `check-acronyms` `.py` files (`bf7c2f65`), and each
  binding's lint runs both on its own paths (`acf7be61`, `1195cccb`).
- **No mutable globals**, an ast-grep rule in each binding (`4eda333e`,
  `b14ff12c`).
- **Conversions are published whole** (`W3C.publish`; `13b9b0e5`), so the
  bindings' and core's runs can convert at once.
- **The size tripwire is designed** ("Gates").

#### Order of work (decided 2026-10-13)

The maintainer's order after the first pass. Each step is done when its
"done means" holds and its gates pass; the next starts after that.

##### 8.1: the documents (coordinator) — done

[ADR 0012](docs/adr/0012-lifetimes-follow-the-loop.md) (lifetimes follow
the loop), the contract of every host call in
[`tinyactors-core/docs/boundary.md`](tinyactors-core/docs/boundary.md),
[`tinyactors-core/docs/embedding.md`](tinyactors-core/docs/embedding.md)
(how to use the core, with a complete loop in C), the guide and
`requests.md` updated, the `AGENTS.md` files, and each binding's
`AGENTS.md` and `ROADMAP.md` rewritten for the rewrite.

Done means: no document contradicts ADR 0012; every host call has its
row; the example of embedding.md compiles and runs. Gates:
`mise run check-acronyms`; core's `mise run lint` (host.h's comment).

##### 8.2: core implements ADR 0012 (coordinator)

**Base APIs implemented (2026-10-14):** `ta_status_name`, `ta_table` and
`ta_growable_arena` have public headers, freestanding implementations,
unit tests and API descriptions. The table retires exhausted generations;
the arena retains regular and oversized chunks, supports nested marks,
and supplies an allocator view. The remaining 8.2 items below still need
implementation; the host loop has not changed.
Contracts and usage are in
[`tinyactors-core/docs/base-storage.md`](tinyactors-core/docs/base-storage.md).
Validation: core's full gate passes (sanitizers, conformance, wasm,
every torture scenario and all native cross-builds). The full run exposed
an existing allocation-refusal sweep defect, reproduced at the preceding
commit and repaired without changing host behavior or weakening checks.
Root `mise run test:full` still stops at the deliberately red Python M1
`host.create` gate; both Python builds pass their 20 codec tests.

In ADR 0012's order of work:

Boot unit coverage (2026-10-14): `test_host.c`'s `test_boot_manifest`
exercises `ta_host_boot` through public calls: an invalid segment leaves
no actors, a manifest's three named actors fit after growth from capacity
one, and entry actions run only when the host loop runs. Allocation refusal
and calls during a run or datamodel guard remain to be covered.

Compiler arena migration (2026-10-14): temporary compiler storage and
returned diagnostic strings now use separate `ta_growable_arena`s; the
private `ta_chunk_arena` is deleted. Compiler allocations remain zeroed
and allocation refusals remain latched. XML parser vectors still use
the backing allocator and are freed when parsing ends. This serves the
binding author: the next resolver increment can lend the shared scratch
arena to Python. It adds no work per actor or event and no Python API;
compilation keeps 32 KiB payload chunks with the shared arena's headers.
The resolver's scratch and structured errors follow this increment.
Validation: core's full gate passes (sanitizers, conformance, wasm, every
torture scenario and cross-builds). Root `mise run test:full` reaches
Python's existing red M1 gate and stops because `host.create` is absent;
both Python builds still pass their 20 codec tests.

- **Base:** `ta_status_name` (NULL for a status `types.h` does not
  define), `ta_table` (64-bit handles, 31-bit generation, rows grown by
  doubling through a `ta_allocator`, a row pointer valid until the next
  add) and **`ta_growable_arena`**, a port of tinyxml's `txml_arena`
  (`tinyxml/src/arena.c`): linked chunks from a `ta_allocator`
  (allocations never move; a request larger than the chunk size gets its
  own chunk), marks (take a mark, release to it), a release that **keeps
  its chunks** (no allocator call after warm-up; chunks freed only by its
  free call), and a `ta_allocator` view so `ta_cbor_writer` grows into
  it. The internal `ta_arena` (`src/base/arena.h`) keeps its name and
  job. Unit tests and `tinyactors.api.json` entries for all three.
- **One chunked arena in core — done** (ADR 0012, decision 5): the compiler's private `ta_chunk_arena`
  (`src/compile/compile_support.h:14-26`; the per-compilation arena,
  `compile.c:37`, and the result's, `compile.c:13`) moves onto
  `ta_growable_arena`, and `ta_chunk_arena` is deleted.

Resolver implementation (2026-10-14): requests lend the compiler arena;
callbacks fill `ta_resolve_result`, with source views or structured errors.
Both compile entry points copy a refusal's resolved filename, location and
message into result diagnostics. The file resolver preserves its attempted
path on failure. Caller migrations used ast-grep. Focused tests cover
scratch-backed data, scripts and invoked charts, non-NUL diagnostic views,
canonical filenames, status-only refusals and both compile entry points.
This serves binding authors reporting source errors to application
developers. The cost is one scratch pointer per resolver request and a
result struct per callback; no work per actor or event is added.
Validation: core's full gate passes, including conformance, wasm, all
torture scenarios and cross-builds. The focused compiler and runtime
programs pass 727 and 43 checks. Root `mise run test:full` still stops at
Python M1's missing `host.create`; both Python builds pass 20 codec tests.

- **The resolver reports its errors and gets a scratch arena — done** (ADR 0012,
  decision 7): `ta_resolve_request` gains `ta_growable_arena *scratch`
  (the compiler's per-compilation arena, from the start of
  `ta_compile_charts` to its return), the out parameter becomes
  `ta_resolve_result` (`ta_source source; ta_resolve_error error`, the
  error `{line, column, message}`), still three parameters; a refusal
  becomes a diagnostic with the resolver's file, line, column and message
  (copied into the result's arena, valid until `ta_compile_free`)
  instead of `cannot resolve src` (`scxml.c:212`). Every caller and
  implementer migrates: `fetch_source`, `ta_compile_system`
  (`compile.c:191`), `ta_runtime_resolve`, `ta-conform`'s
  `document_resolve`, the tests' `test_resolve` and `abort_resolve`, and
  the lint rule's test. A test of a refusal's diagnostic and of a text
  left in scratch.
- **One drain at a time — done (2026-10-14):** `ta_host_effects`
  freezes a nonempty batch's pointer, count and fields until the ack;
  appended effects wait for the next take. An empty take opens no batch.
  `ta_host_run` refuses while a batch is open (`TA_RUN_FAULT`, detail
  `TA_ERROR_STATE`, no work or release). The ack closes publication before
  release callbacks. This serves the C embedder and binding author with
  one extra pointer per host, no per-actor cost and no serialization.
  Focused growth, refusal and release-callback tests cover the contract;
  the C driver drains successful posts and retains peer envelopes until
  its next run. Host outputs and `TRACEPARENT` remain the next increment.
- **Outputs until the ack:** the host's outputs and `TRACEPARENT`.
  `TRACEPARENT` done (2026-10-14, `62255a80`): a slot per effect of the
  batch, written on first ask. Outputs (2026-10-14): each envelope,
  traces, logs, metrics, hibernation or export blob is a block of its own
  in an arena the ack empties (chunks kept); an output taken with no
  batch open opens a batch of outputs alone, so `ta_host_run` and
  snapshots refuse and a take returns 0 until the ack. Tests, ta-conform
  and the torture bench ack after copying an output. Serves the binding
  author and C embedder (an output survives later calls in its batch);
  costs nothing per event, and one extra block per output until the ack.
  Bindings copy an output once into language memory (boundary.md,
  section 7). Left: the blob scratch of migration (`blob`, `blob2`) is
  internal and keeps its own lifetime.
- **`actor.subscribe`, `actor.unsubscribe` and `actor.changed`
  removed — done (2026-10-14)** (ADR 0012, decision 4): the host's
  subscription table, the methods, the config key `maxSubscriptions`,
  the subscribe exchange in `requests.txt`, `host.h`'s comment, and the
  self-test that reads their heading (`mise/tasks/api-map-test:17`), in
  one commit; `requests.md` then drops the heading it keeps until then.
  `TA_HOST_EFFECT_NOTIFICATION` stays. `actor.state` does not change.
- **The findings of boundary.md, "What no test checks yet"**, each
  decided and tested: `ta_host_free` from a datamodel's code; lookups
  that answer during a run beyond `host.h`'s list; `ta_host_envelope`
  taking the effect's address instead of its index; a test calling
  `ta_system_*` from datamodel code (`test_host_inputs.c`'s `flood`
  hook); the width of ids in `types.h` (`types.h:7` says 32 bits;
  sessions, payloads and table handles are 64).
- **The contract table's missing tests** (boundary.md, every "needed").
- **`detach_event`:** verify that core calls it when SCXML can no longer
  process the event (a terminate's exit handlers after a stable point,
  `serialize` writing "the bound `_event`"), and make the call or
  `datamodel.h`'s comment ("materializes `_event`") match: the
  `"python"` datamodel drops its `_event` there (boundary.md, finding 6).

Every caller in core, its tests and `ta-conform` migrates in the same
commits; the "(0012)" and "[8.2]" marks leave `boundary.md`,
`embedding.md`, `bindings.md`, `requests.md` and the bindings' documents
as each lands.

Done means: every "needed" test of boundary.md's "What no test checks
yet" exists and passes; `host.h` states the new lifetimes; no "(0012)"
or "[8.2]" mark is left. Gates: core's `mise run test`, and
`mise run test:full` (the change touches the host's memory and the run,
`AGENTS.md`, "Tests: what to run"), with the commit saying it passed.

##### 8.3: the full loop, end to end in C (coordinator)

One C program in `tinyactors-core/tests/` (`test_loop.c`), written as
embedding.md's loop and run by core's `mise run test` under ASan and
UBSan, drives hosts through every effect with a C handler for each. It
is the reference a binding's loop is checked against, and embedding.md's
example is compiled and run with it so the page cannot rot. End to end
means each of these happens and is answered through the public calls
only:

- **I/O processors:** an asynchronous one (`TA_EFFECT_SEND`, answered by
  an event later) and a synchronous one (`TA_RUN_EFFECTS`, answered
  before the next run; a failure answered with
  `TA_EVENT_IN_ERROR_COMMUNICATION`);
- **invoked services:** `INVOKE` starts a C service keyed by (segment,
  token), a `<send>` to `#_<invokeid>` reaches it (`IO_PROCESSOR`
  `TA_NIL`), it answers with `TA_EVENT_IN_FROM_INVOKE` and ends with
  `TA_EVENT_IN_INVOKE_DONE`; `UNINVOKE` stops another; `REQUEST` answered
  with `ta_host_resume`;
- **hibernation:** `ta_host_sweep`, `ta_host_hibernate` into a store the
  program keeps, a `WAKE` answered with `ta_host_wake`; a migration job
  (`MIGRATED`, `JOB_DONE`) and a retired version (`SEGMENT_FREED`);
- **peers:** two hosts in one process exchanging envelopes as bytes
  (`ta_host_envelope`, `ta_host_envelope_read`, `ta_host_deliver`), a
  refused one coming back as `PEER_ERROR` (`ta_host_peer_error_read`,
  `ta_host_peer_error`) and becoming `error.communication`;
- **telemetry:** `TRACES` and `LOGS` taken in their handlers, metrics
  pulled, `trace.end` at stop with the last take after it;
- **the control plane:** every method of `requests.md` posted and its
  `REPLY` read, matched to its message by id;
- **lifecycle:** `DONE` and `ERROR` releasing what the program keeps for
  a session; `LOG` to its log;
- **the loop's rules:** one drain at a time, an ack before every run,
  outputs read after later calls in the same batch, every run status
  handled.

Done means: every effect kind of `host.h` and every method of
`requests.md` appears in the program at least once and is answered; it
passes under ASan and UBSan; nothing in it reaches past the public
headers. Gates: core's `mise run test` and `mise run conform`.

##### 8.4: a minimal Python binding (Python session)

**The design is decided** (the maintainer, 2026-10-13), and written in
[`tinyactors-cpython/AGENTS.md`](tinyactors-cpython/AGENTS.md) (the
rulebook), the guide (sections 3, 6, 8 and 10) and ADR 0012 (decisions 3 to
6). 8.4 implements it, then measures it. There is no separate
design document. Native datamodel first (decision 3 above) stays, and
so does the reason for the step: a Python API with the native
`"python"` datamodel that keeps the memory efficiency that is "the whole
point and where we failed before" (the maintainer).

The design, in short (the rulebook has each rule with its evidence):

- **API:** `tinyactors.Host(config, io_processors={type: handler},
  log=handler, clock=None)`, everything per host (no global
  configuration); `with` and `async with` on the same `Host`. The direct
  layer mirrors the C API: `tinyactors.compile_charts(sources,
  resolver=None) -> Compiled` (a context manager; `CompileError` with
  every diagnostic), `host.add_image(compiled.image) -> int`,
  `host.chart(name)`, `spawn(ref, name=None, params=None,
  traceparent=None) -> int`, `send(session, event, data=None, *,
  traceparent=None)`, `send_many(items)` (items may carry a
  `traceparent`), `terminate(session)`, applied at the call; a
  `traceparent` is W3C text, passed as the `str`'s UTF-8 and parsed by
  core. A resolver object (`resolve(base, source)`, refusing with
  `tinyactors.ResolveError`) is given to `compile_charts` only: the
  resolver lives only at compile time and the `Host` has none. The
  extension answers a `TA_EFFECT_REQUEST` (a fetch at run time) in C with
  `TA_ERROR_INVALID` until run-time fetching is built ("Decided later";
  ADR 0012, decision 8). A convenience
  layer in Python above it is later work (`DirectoryResolver` among
  it). An I/O processor's handler is `handler(host, send)`, with
  `send.traceparent` (formatted on first read, `None` when untraced),
  `send.answer(event, data=None, *, traceparent=send.traceparent)` and
  `send.fail()`, both keeping the send's trace; an `async def` handler
  is scheduled as a task. `log=handler(host, entry)` gets the
  host's `<log>` lines. Typed methods over post (`actor_state`,
  `actor_context`, `actor_list`, `actor_get`, `host_stats`, `trace_*`)
  take `(..., handler, on_error=None)` and return `None`; no synchronous
  request, no raw post, no reply object. `DONE` and `ERROR` internal;
  `PEER` and `PEER_ERROR` deferred.
- **Drivers:** synchronous `run_until_idle()`, `run()` and
  `SimulatedClock.increment(ms)`; asyncio `await host.idle()`; every
  run status handled as the rulebook's table says.
- **C memory:** one `ta_growable_arena` per host with a mark per call;
  tables (`ta_table`) for charts, sessions, payloads and pending typed
  methods; thin views of effects, stale after the ack; materialized
  records only where a value must outlive the ack.
- **The `"python"` datamodel:** one class per chart (`__slots__` for the
  `<data>` ids and `_event`, read-only system variables), expressions
  compiled once per chart by a restrictive rewrite into functions of the
  session, one instance per session, `_event.data` the payload row's
  value, hibernation keeping the row's handle. Estimated per session,
  unmeasured: the instance 32 B + 8 B per slot, the system variables
  ~100–250 B, a row ~16 B.

It needs step 8.2's `ta_table`, `ta_status_name`,
`ta_growable_arena`, the one-batch drain and `detach_event`'s check.

Then the code: the `tinyactors` module and `Host`; `ta_host_create`
from Python options (the binding reads `host.config` itself); images,
boot, spawn, send, terminate; the loop with both drivers and
`SimulatedClock`; I/O processors with answer and fail; the log handler;
typed methods with response handlers; `tinyactors.encode` and
`tinyactors.decode`; the `"python"` datamodel for `<data>`, `cond`,
`assign`, `<script>`, `_event`, `In()`, `<send>` with params,
`<foreach>`, with quickjs beside it; and the suite's adapter
(`conform/`).

**Then the measurement**, once the binding runs: bytes per actor at a
population of 10,000 actors of one chart, resident and idle, from the
process's memory, Python's allocator (`tracemalloc`) and the host's
(`host.stats`' `regionBytes`), on the `"python"` datamodel and on
quickjs side by side, on one machine named by its signature (root
`AGENTS.md`, "Benchmarks"), by a task of the binding that reruns it.
quickjs's reference: a session scope of about 0.5 KB in one shared
context of ~54 KB, the `fit` session actor 2.9 KB instead of 60.7 KB
(`tinyactors-core/docs/architecture.md:844`; a resident ECMAScript actor
~2.9 KB, a hibernated one ~150 B, `architecture.md:879`). The numbers
replace the estimates above in the rulebook, and a gap between them is
explained there.

Done means: the design implemented as written (a change to it is the
maintainer's, recorded in the rulebook first); the **measured bytes per
actor at a population of 10,000** on the `"python"` datamodel beside
quickjs's on the same machine, by the binding's task; the suite's M1 set
(`--milestone M1`) on both datamodels; the value table's own tests; the
mapping table (`docs/api-map.md`), every item a core function, a method
or an effect; the pitfalls below, each with a test; then the **API
review** by the maintainer: how a Python program and a Python chart
read. Gates: the binding's `mise run test` (lint, build, both
interpreters, the suite's set).

##### 8.5: Node, then the rest (both sessions)

The Node binding to the same "done means" as 8.4, on its native
ECMAScript datamodel and quickjs. Then, for both bindings, what the
earlier plan's M2 to M5 held: the W3C suite per datamodel (`skip-python.txt`;
191 of 191 on ECMAScript), with sync sends in that gate; every effect
(the 8.3 list) through the language's API, both drivers in Python and
Node's liveness, graceful shutdown (`trace.end`, the last take, free);
hibernation with a store, export and adopt, migration, snapshots,
envelopes between hosts and across processes, a Python host and an
ECMAScript host exchanging envelopes; the examples, release tasks and
changelogs. The browser (below, "Decided later") after that.

#### Pitfalls each binding handles

Found in the first pass (2026-10-12), each with a test of its own in the
binding (the suite cannot see them; spikes and their review: the
reasoned-analysis pass of 2026-10-12). They hold for the rewrite:

- **Both: dropping a payload's last reference can run the language's
  code.** Core calls `release` outside a run as well (the start of
  `ta_host_run`, ack, `ta_host_free`, posts, deliver), where the host
  holds the run's rules (ADR 0010, decision 3). Defer the last drop to a list emptied after the
  outer call into core returns.
- **Both: handle tables are core's `ta_table`** (ADR 0012, decision 5),
  for payloads and for sessions, each handle with a generation (guide
  section 6); no `handles.c` of the binding's own.
- **Both: text is valid UTF-8.** A lone surrogate is refused
  (`TA_ERROR_INVALID`); Node-API replaces it with U+FFFD silently, so the
  addon checks (`String.prototype.isWellFormed`).
- **Python:**
  - a session is an instance of its chart's class, not a globals dict
    (decided 2026-10-13): `__slots__` for the `<data>` ids and `_event`,
    read-only properties for the system variables, so assigning to an
    undeclared name or a system variable raises and is
    `error.execution`; expressions are rewritten into functions of the
    session (`tries < 3` → `s.tries < 3`), so generator expressions and
    lambdas see session variables through `s` (the first pass's reason
    for one globals dict and no locals);
  - `bool` before `int` when converting values;
  - a `BaseException` that is not an `Exception` (`KeyboardInterrupt`,
    `SystemExit`) still gives `error.execution` to the chart, and the
    binding raises the same object again after the run;
  - handler closures make cycles: fill `Py_tp_traverse` and `Py_tp_clear`.
- **Node:**
  - expressions are compiled once as a strict-mode function inside
    `with (scope)`, as the quickjs datamodel does, so an undeclared
    assignment throws; `assign` keeps quickjs's check of the location;
  - one handle scope per vtable entry, since a run is one native call;
  - reference counts live in the slot table, one `napi_ref` per slot,
    deleted at zero: a finalizer may call only basic-env functions, and
    `napi_reference_unref` is not one;
  - `napi_get_value_int64` saturates silently: ids are always `BigInt`,
    and a number in event data is checked with `Number.isSafeInteger`;
  - `napi_type_tag_object` on every wrapped object;
  - the timer is the global `setTimeout`, ref'd while a deadline is
    pending: Bun has no `uv_timer_*` (oven-sh/bun#18546);
  - the README says chart code is trusted: it runs in the application's
    realm, and Node-API cannot make another;
  - **an actor reference is a string** (decided 2026-10-12). Asked for
    whatever is best for performance and memory with good ergonomics,
    the analysis (reasoned-analysis, refuted by a fresh agent) found that
    tag 32 never reaches a Node application: core writes no tag 32
    (`ta_cbor_write_tag` has no caller in `tinyactors-core/src`), both of
    Node's datamodels are ECMAScript, which reads a reference as its
    address text (`datamodel_ecmascript.c:2600-2602`, `values.txt`
    "ECMAScript: an actor reference is its address", which the native
    datamodel passes too), and values reach the application as that
    datamodel's payloads, or through its `to_wire` in request replies. So
    a reference class would carry nothing a string does not, and it costs:
    made through Node-API (a spike, node 22.23.2 / bun 1.4.1, ns per value
    with its string): a string 155/123, a class defined in C 393/257 and
    64 more bytes, a class compiled in JavaScript 228/135 and 32 more
    bytes, a type tag about 90 ns and 32 bytes more; interning 856 ns and
    ~80 bytes per new address in plain JavaScript; a lazily formatted text
    must copy core's borrowed bytes (288 bytes, 147 ns). A string compares
    with `===`, is a `Map` key, prints and writes JSON as the address, and
    is what `send` and the charts already use. `values.c` reads tag 32
    as its text and never writes it, as quickjs does; `docs/values.md`
    says so. Python keeps `Address`: its datamodel has a reference type,
    and the "not ECMAScript" vectors of `values.txt` hold it to tag 32.
- **Both READMEs:** nesting is limited to 64, which stops cycles but not
  a value that shares subtrees (`a = [a, a]` sixty times): charts are
  trusted code.

### File trees

#### tinyactors-test: the conformance suite

```
tinyactors-test/
├── AGENTS.md                rules for adding suites, cases and driver methods
├── README.md                what the suite is, how to run it against a command
├── mise.toml                pins uv and python (SaxonC for the W3C stylesheets)
├── docs/
│   └── driver.md            the protocol a conforming command speaks: JSON-RPC 2.0
│                            without "jsonrpc", CBOR, length-prefixed on stdin/stdout;
│                            scenario.run and w3c.run (outcomes), and for the vectors
│                            host.create, host.request, image.add, actor.spawn,
│                            actor.send, clock.advance, host.run, peer.add,
│                            envelope.read, envelope.deliver, value.encode;
│                            the kinds; the scenario and W3C environments
├── suites/
│   ├── milestones.txt       what each milestone of step 8 runs (--milestone), cumulative
│   ├── protocol/            requests.txt, config.txt, envelopes.txt, values.txt;
│   │                        setups/ (the charts of the vectors' setups)
│   ├── scenarios/           <name>/<document>.txml (conf: notation) and steps.txt
│   │                        (host operations); the charts judge: pass or fail
│   └── w3c/
│       ├── irp/             the W3C IRP suite, vendored: manifest.xml, <id>/*.txml,
│       │                    confEcma.xsl, confXPath.xsl, SOURCE, LICENSE
│       ├── confPython.xsl   ours: the conf: notation in Python
│       ├── skip.txt, skip-python.txt
│       ├── check.py         check-languages: well-formed, no conf: left, compiles
│       └── convert.py       the XSLT 2.0 step (SaxonC), for W3C and --scenarios
├── lint/                    the adapters' lint (ast-grep): an adapter imports only
│                            the binding's public module and the standard library;
│                            rules/, tests/, utils/
├── lib/                     the runner (Ruby): driver.rb (process, frames),
│                            runner.rb (suites, compare, wildcards, report),
│                            vectors.rb, scenarios.rb, cbor.rb, w3c.rb; no scheduler
├── mise/tasks/
│   ├── run                  run -- <command…> [--milestone M1…M5] [--suite protocol|scenarios|w3c]
│   │                        [--datamodel …]
│   ├── encode               the vectors' hex from their diag lines (was protocol:encode)
│   ├── w3c                  convert the W3C suite for a language
│   ├── check-languages      each stylesheet's translation, checked
│   ├── lint                 the adapters' lint: its tests, then the adapter given
│   └── test                 encode --check, check-languages, lint, the runner's tests (test/)
├── test/                    the runner's tests against fake_implementation.rb
└── build/                   the converted W3C suite and scenarios (gitignored)
```

#### tinyactors-core: what M0 adds

```
tinyactors-core/
└── tools/conform/           ta-conform: the C reference of docs/driver.md, over ta_host
    ├── conform.c            and the quickjs datamodel (and the tests' one); core's
    ├── datamodel.c          `mise run conform` runs the suite with it
    ├── vectors.c
    ├── scenario.c
    └── w3c.c
```

`tests/protocol/`, `tests/w3c/` and `tests/conformance/` left core (M0
item 2); `tests/unit/` and the torture chamber stay.

#### tinyactors-napi: the Node and Bun package (`@tinyactors/node`, ADR 0001)

```
tinyactors-napi/
├── AGENTS.md
├── ROADMAP.md               the session's plan, progress and requests to core
├── README.md                for users: install, Host, handlers, liveness, limits
├── CHANGELOG.md
├── mise.toml                pins node, bun, pnpm (zig is the root's)
├── package.json             @tinyactors/node; per-platform packages as ADR 0001 says
├── index.mjs                loads the platform's .node and re-exports it; nothing else
├── index.d.ts               the types
├── docs/
│   ├── api-map.md           each API item → ta_* function, request method or effect (checked)
│   └── values.md            the JavaScript side of the value table
├── src/
│   ├── addon.h
│   ├── module.c             module init, per-environment state, the classes
│   ├── host.c               Host: one function per method → ta_* or a request
│   ├── loop.c               the turn, statuses, one setTimeout (ref'd while a deadline is pending)
│   ├── effects.c            one record per effect, built with the accessors; dispatch
│   ├── methods.c            typed methods over ta_host_post, settled at a later drain;
│   │                        TinyactorsError(code, reason, path)
│   ├── values.c             CBOR tokens ↔ JavaScript values: BigInt, Uint8Array; tag 32 as its text;
│   │                        the encode and decode exports
│   └── datamodel/           the native "ecmascript" datamodel: vtable entries only
│       ├── datamodel.c      the vtable; scopes, _event, In()
│       ├── rows.c           payload and session rows in ta_table: reference counts
│       └── expressions.c    strict functions compiled once per chart, by kind
├── conform.mjs              the suite's adapter, over the public API only
├── test/                    what the suite cannot see: liveness, workers, GC, refusals
├── build/                   core's amalgamation and the addon (gitignored)
├── mise/tasks/              build, test, conform, pack, lint
└── tools/lint/              ast-grep: layer rules, mapping check
```

#### tinyactors-cpython: the Python extension (`tinyactors`)

```
tinyactors-cpython/
├── AGENTS.md
├── ROADMAP.md               the session's plan, progress and requests to core
├── README.md                for users: Host, charts in Python, both drivers, limits
│                            (trusted code; values outside the host's allocator)
├── CHANGELOG.md
├── mise.toml                pins python (3.13, and a free-threaded build for tests), uv
│                            (zig is the root's)
├── pyproject.toml           one abi3 wheel (3.13+) per platform, built by the mise task
├── docs/
│   ├── api-map.md           each API item → ta_* function, request method or effect (checked)
│   └── values.md            the Python side of the value table
├── src/                     one extension module, tinyactors; no .py files in the wheel
│   ├── extension.h          the extension's declarations
│   ├── module.c             PyInit_tinyactors, per-interpreter state (PEP 489), the types
│   ├── host.c               Host: one function per method → ta_* or a request
│   ├── loop.c               the turn; run_until_idle, run, idle(), one TimerHandle
│   ├── effects.c            dispatch; thin views of sends and log lines, records for async
│   ├── methods.c            typed methods over ta_host_post, response handlers in the
│   │                        pending table; TinyactorsError(code, reason, path)
│   ├── values.c             CBOR tokens ↔ Python objects; Address, UNDEFINED; deep copy;
│   │                        tinyactors.encode and tinyactors.decode
│   └── datamodel/           the "python" datamodel: vtable entries only
│       ├── datamodel.c      the vtable; a class per chart, an instance per session, _event, In()
│       ├── rows.c           chart, session and payload rows in ta_table: reference counts
│       └── expressions.c    the rewrite: functions of the session, compiled once per chart
├── conform/                 the suite's adapter (python -m conform), not in the wheel
├── tests/                   what the suite cannot see: threads, GC, asyncio, refusals
├── build/                   core's amalgamation and the extension (gitignored)
├── mise/tasks/              build, test, conform, wheel, lint
└── tools/lint/              ast-grep: layer rules, mapping check
```

The two bindings have the same files on purpose: the audit compares them
file by file.

**The Python floor is 3.13, and the free-threaded build is for tests
only** (decided 2026-10-12). The shipped wheel is abi3 from 3.13
(`Py_LIMITED_API 0x030D0000`): 3.13 is the first limited API with the
host's allocator, `PyMem_RawMalloc` and `PyMem_RawFree` (CPython's
`Include/pymem.h`, 3.13: line 90). The limited API cannot be compiled for
a free-threaded interpreter before 3.15 (`Python.h` refuses
`Py_LIMITED_API` with `Py_GIL_DISABLED`), so the test task builds the
same sources a second time without `Py_LIMITED_API` for the free-threaded
3.14t. No compatibility macros are needed for it any more:
`PyDict_GetItemRef`, `PyList_GetItemRef`, `Py_mod_gil` and
`PyType_GetModuleByDef` are limited from 3.13, so the same calls compile
in both builds (a spike compiled every CPython function the cpython
`AGENTS.md` names against 3.13.16's headers with
`Py_LIMITED_API 0x030D0000`; at 3.12, those four and `PyMem_Raw*` are
undeclared). Module state comes from `PyType_GetModuleByDef` and
`METH_METHOD`. A free-threaded wheel waits for the stable ABI of
free-threaded builds (abi3t, 3.15).

### Decided later

- **The browser.** A wasm module's C cannot make JavaScript objects, so a
  browser package needs either JavaScript doing what `host.c` and
  `effects.c` do (a twin, which ADR 0008's web binding was) or the
  Node-API addon compiled to wasm over an emulated Node-API (emnapi, which
  ADR 0008 did not choose when the logic sat above the addon). Decide with
  measurements (download size, call cost) after step 8.5's every-effect work; either way it passes
  the suite.
- **PostgreSQL**, and other languages, after step 8.5.
- **Run-time fetching (`<invoke srcexpr>`) through
  `TA_EFFECT_REQUEST`** (decided 2026-10-13: the REQUEST stays; ADR 0012,
  decision 8). Today core normalizes the name and looks it up
  in the requesting segment's image (`route.c:439-489`), gives a REQUEST
  only when that fails (`:491-497`), and a binding answers it with
  `TA_ERROR_INVALID`. The build-out needs from core:
  - a resume with a chart of another segment, after the binding has added
    the fetched document's image (`ta_host_add_image`); today a resume
    names only a chart of the requesting segment's image (`:544-553`);
  - how the binding is told to fetch (one candidate: a resolver object
    for run time configured on the `Host`, not the compile call's);
    undecided.

  It is not the control plane (`ta_host_post`, `REPLY`, `NOTIFICATION`).

## Requests to core

Gaps a binding session found, for the coordinator. Each names what is
missing, why, and the suite case or test that shows it. A binding session
writes its requests in its own `ROADMAP.md` (`tinyactors-cpython/`,
`tinyactors-napi/`); the coordinator moves each one here when it takes it
up, and answers here.

- **quickjs shares mutable values between actors.** What:
  `ecmascript_capture_`
  (`tinyactors-core/src/datamodel_ecmascript/datamodel_ecmascript.c:1557-1600`)
  puts the values of `<param>`, the namelist and `<content>` into the
  payload as references (`value_of`, then `JS_SetPropertyStr`), not
  copies. So a sender that mutates an array after its `<send>`, or a
  receiver that mutates `_event.data`, is seen by the other actor. Why:
  actors share nothing (`tinyactors-core/docs/architecture.md:25`, C2;
  `bindings.md` section 8, "Actors share no mutable value"), and
  hibernation, which writes a blob, copies. Which test: a spike
  (2026-10-12), two scenarios against `ta-conform` in the auto and the
  isolated realm: a parent sends `[1,2,3]` to the child it invoked and
  pushes 4 after the `<send>`, and the child sees 4 items; a child pushes
  onto `_event.data.a`, and the parent's array grows. Both are seen in
  both realms; without the mutation, neither is. A scenario that checks
  it comes with the change (it fails on quickjs today). Decided
  (2026-10-12): it stays filed here, and is not fixed now. A binding's
  native datamodel copies what crosses between actors all the same
  (`bindings.md` section 8, "Actors share no mutable value"); on quickjs
  the sharing stays until the fix, and no scenario mutates sent data.
- **A binding cannot read a host's tick rate** (2026-10-12; both
  bindings: `tinyactors-napi/ROADMAP.md` and `tinyactors-cpython/ROADMAP.md`,
  "Requests to core", commits `e1aa3c27` and `4081125b`). What: a binding
  converts its clock to ticks for `ta_host_advance_time` and
  `next_deadline`, but a config without `ticksPerSecond` got the system's
  default, 1e9, stated nowhere, and core hard-coded it again in the host.
  Done (`9bd33f03`): the host keeps its effective config, every default
  filled in at `ta_host_create` by the system's and the encoder's own code
  (`ta_system_config_normalize`, `ta_otlp_config_normalize`), and the
  request `host.config` answers it in the shape `ta_host_create` takes
  (`tinyactors-core/docs/requests.md`; vectors in `requests.txt` and
  `config.txt`). A binding reads `result.system.ticksPerSecond` once
  after creating the host and converts with it, never with a default of
  its own (`bindings.md` sections 3 and 7). `docs/host-config.md` states
  every default.
- **A binding cannot send the envelope-level request and config vectors**
  (2026-10-12; `tinyactors-cpython/ROADMAP.md`, "Requests to core", item
  1, commit `7bcce98c`). What: cases of `requests.txt` and `config.txt`
  send bytes no binding's API makes (not CBOR, not one request map, a
  member or id `request(method, params)` never writes, notifications), so
  no binding could pass M1's protocol gate. Done: each such case carries
  the line `only reference`; `--kind binding` skips it ("envelope-level:
  reference only"), `ta-conform` runs every case. Decided from the data,
  not detected from the bytes; the method-level cases stay a binding's
  (`tinyactors-test/suites/protocol/README.md`, "Envelope-level cases";
  `tinyactors-test/docs/driver.md`, "Kinds").

## Now

In progress beside M0, in core:

- **Open spans are ended, not lost.** A host stopped with spans open ends
  them, so the binding's last take of traces includes them, and
  `ta_host_free` drops nothing a caller could still have taken. Done
  (`32d63292`: the `trace.end` request; `docs/requests.md`).
- **The torture bench's `traceBytesTotal` is removed:** it reads 0 since
  the host drains the journals (5c). Done (`1c8c4fa6`).
- **Definition logs:** a log record when a chart version is linked,
  retired and freed (0.1's `tinyactors.definition.*`). Done (`dbe8435d`;
  `tinyactors-core/docs/architecture.md`, 16.3).
- **ADR 0010: outputs are views; values stream.** Implemented in core
  (2026-10-12), in the ADR's order: the guard, calls that reach a
  datamodel outside a run follow a run's rules (`2eef3d25`); a datamodel's
  `to_wire`, `serialize` and `payload_serialize` give views it owns, each
  asked for once per value (`4c90a97c`); the host's outputs (a request's
  reply, envelopes, traces, logs, metrics, hibernation and export blobs)
  are views of memory it grows and keeps, and the driver protocol's
  `host.request` takes `refuse` instead of `capacity` (`633f79a1`); the
  streaming `ta_cbor_writer` and `ta_cbor_reader`, `ta_cbor_encode` gone
  (`4b5a4234`); `to_wire` at the token encoder's cost again (`ef1fbd24`:
  a 1 KB envelope's `ta_host_envelope` 9.3 µs against 17.6). The
  bindings were removed for their rewrite instead of migrating (8.4, 8.5).
- **ADR 0011: the control plane is posted messages; images are bytes the
  host keeps.** Implemented in core (2026-10-13), in the ADR's order:
  `ta_host_post` answers a message with a `TA_HOST_EFFECT_REPLY` effect,
  and `ta_host_request` is deleted (`42401f69`); `actor.changed`
  notifications replace the host-level CHANGED (`db7d1220`);
  `ta_host_add_image` takes bytes and the host keeps a copy, freed with
  its segment, `ta_host_snapshot_input.images` as `ta_bytes`, and the
  ECMAScript datamodel's restore reads no freed image (`71f6547c`); the
  driver's `host.post` and `host.run`'s notifications, with a subscribe
  exchange in `requests.txt` (`770d94d6`); the guide, `requests.md` and
  `boundary.md`. The bindings were removed for their rewrite instead of
  migrating (`6426816a`, `70434bee`; 8.4, 8.5).
- **ADR 0012: lifetimes follow the loop.** The documents are step 8.1
  (done); core's change is step 8.2.

## Deferred

Decided not to do now; each comes back as its own proposal or roadmap
entry.

- **Payload fields as span attributes.** `scxml.event.data` and
  `scxml.value` are text (CBOR diagnostic notation); opt-in flattening of
  top-level scalar fields into typed attributes would make them queryable.
- **Tracing:** `tracestate` for the detail class across processes; traces
  for host work and waits (ADR 0007, open questions).
- **Release packaging** of the browser package and PostgreSQL, with their
  bindings.
- **Refusing a send to the sending chart.** A full pool stops the run until
  the host grows the block; the chart never hears about it. An
  `error.execution` to the sender when a pool is full would let charts
  handle overload themselves.
