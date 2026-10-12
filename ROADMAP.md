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

The repository has no binding now. Step 8 builds them, and this roadmap is
its plan.

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
   (`tinyactors-js` is dropped) and no JavaScript above the addon beyond
   loading it.
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
  session owns `tinyactors-napi/`. They run in parallel after M0.
- **Core is frozen for the binding sessions.** A gap in core is written
  under "Requests to core" below (what, why, which test shows it) and the
  binding waits or works around nothing. The coordinator makes the change,
  with C tests and suite cases, and tells both sessions to rebase. A
  binding never patches core.
- **One checkout, on `main`, commits scoped to paths**
  (`git commit -- tinyactors-cpython/`). Each binding builds core from the
  amalgamation into its own `build/`; nobody but the coordinator runs
  `tinyactors-core`'s build or its `mise run test`.
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
| **Size tripwire**: a line budget per file, set at M1 | each binding | slow growth; exceeding it needs a sentence in the commit |

What a black box cannot show stays as each project's own tests: memory
failures, refusal during a run, threads, garbage collection, liveness.

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
   `--kind binding`.

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

Follow-ups for M0 (not done):

- **(a) The runner fails a run that proves nothing:** zero passes in a
  suite, or a skipped method the implementation's kind must offer. A
  required-method list per kind: `ta-conform` offers all of them; a
  binding all but the C-level ones (`value.encode`, `envelope.read`, and
  `host.request`'s capacity).
- **(b) Outcome canaries:** scenarios that must end in `fail`, `timeout`
  and `compile`, with the expected outcome stated in their `steps.txt`, so
  a runner or adapter that reports `pass` for everything fails.
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
  as core does (`test_router.c:1279-1301`).
- **(d) Vectors:** lone surrogates refused (in a value and in a key); an
  encoded surrogate (`63 eda080`) malformed; Python lines for the encode
  and invalid cases where they apply; round trips outside ECMAScript for
  tag 32 and `undefined`.
- **(e) An adapter lint in each binding skeleton:** the adapter imports
  only the binding's public module.
- **(f) The runner picks the chart language from the datamodel name**
  `host.create` reports, so a run that asks for a native datamodel cannot
  be served by quickjs.

#### M1: walking skeletons with native datamodels (both sessions)

Each binding: its module and `Host` type; `ta_host_create` from the
language's options; images, boot, spawn, send, terminate; the loop with
every run status; `DONE`, `ERROR`, `LOG`; `request` with the language's
error type; the CBOR conversion with the value table (`Address`,
`UNDEFINED`, big integers, bytes); the native datamodel (`"python"`,
`"ecmascript"` in Node's engine) for `<data>`, `cond`, `assign`,
`<script>`, `_event`, `In()`, `<send>` with params, `<foreach>`; quickjs
wired in beside it; and the suite's adapter (`conform`).

Both bindings: **a simulated clock** in the public API, its shape
borrowed from xstate (`SimulatedClock`, `increment(ms)`) and its meaning
ours. A host created with it arms no timer; `increment(ms)` calls
`ta_host_advance_time` (the wall clock is the simulated time) and runs the
loop until idle; inputs run the turn at the current time. It is the
binding's form of "a caller's own clock" (`tinyactors-core/docs/bindings.md`,
section 3). It serves application developers testing their charts first;
the adapter uses it for the scenarios' `advance` steps. Each
`docs/api-map.md` lists it against `ta_host_advance_time` and the turn.

Gates: protocol vectors; the basics scenario on both datamodels;
`values.txt` round trips on the native datamodel; mapping table; layer
lints. **Then the API review**, both bindings side by side: how a Python
chart reads, how a Node program reads.

Pitfalls each binding handles in M1, with a test of its own for each (the
suite cannot see them; spikes and their review: the reasoned-analysis
pass of 2026-10-12):

- **Both: dropping a payload's last reference can run the language's
  code.** Core calls `release` outside a run as well (the start of
  `ta_host_run`, ack, `ta_host_free`, requests, deliver), where the host
  does not refuse reentry. Defer the last drop to a list emptied after the
  outer call into core returns.
- **Both: two slot tables** (`handles.c`), for payloads and for sessions,
  each with a generation (guide section 8).
- **Both: text is valid UTF-8.** A lone surrogate is refused
  (`TA_ERROR_INVALID`); Node-API replaces it with U+FFFD silently, so the
  addon checks (`String.prototype.isWellFormed`).
- **Python:**
  - one globals dict per session and no locals: with separate locals,
    generator expressions and lambdas do not see session variables;
  - system variables (`_event`, `_sessionid`, `_name`, `_ioprocessors`,
    `_x`) stay as they were: after each evaluation, a changed one is
    restored and the evaluation is `error.execution`; `assign` checks that
    its location is declared; `variable` and `serialize` skip
    `__builtins__`;
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
    realm, and Node-API cannot make another.
- **Both READMEs:** nesting is limited to 64, which stops cycles but not
  a value that shares subtrees (`a = [a, a]` sixty times): charts are
  trusted code.

#### M2: W3C in each language

`confPython.xsl` beside the W3C stylesheets (ours, checked in: M0 item
5), the suite generated per datamodel: on python every test but the 20 in
`skip-python.txt` (ECMAScript data model tests, and 567), 191 of 191 on
Node's native ECMAScript datamodel and on quickjs. Decide here whether Python needs
compile-time checks of its expressions (the deleted
`ta_datamodel_compiler`; this time the compiler would call it).

#### M3: every effect

I/O processors (`SEND`, sync sends), `INVOKE`/`UNINVOKE`, `REQUEST` and
`ta_host_resume`, `CHANGED` with `actor.subscribe`, `TRACES`, `LOGS` and
`ta_host_metrics`, inputs during a run (an expression calling
`host.send`), both drivers in Python (`run_until_idle`, `attach`), and
Node's liveness. Graceful shutdown: a signal is an event on the loop, so
`stop()` runs between turns, drains until a deadline the application
gives, ends the open spans (`trace.end`), takes the traces and frees the
host; a server with several hosts stops each on its own loop. Gates: the
invoke scenario on both datamodels, and a shutdown test in each binding
(spans marked `scxml.incomplete`, nothing after the stop): a chart cannot
see spans, so it is not a scenario.

#### M4: lifecycle and peers

Hibernation with a store (and a wake before the put settled), export and
adopt, migration jobs, snapshots, envelopes between two hosts in one
process and across processes, and a Python host exchanging envelopes with
an ECMAScript host. Gates: the hibernate, migrate and snapshot scenarios;
the envelope vectors.

#### M5: examples and release

The examples (chess, ui-elements, access-reconciler) against
`tinyactors-napi`; release tasks and changelogs for both packages; core's
`mise run test --full` (owed since step 5b changed scheduling) and every
full pass.

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
│                            the binding's public module; rules/, tests/
├── lib/                     the runner (Ruby): driver.rb (process, frames),
│                            runner.rb (suites, compare, wildcards, report),
│                            vectors.rb, scenarios.rb, cbor.rb, w3c.rb; no scheduler
├── mise/tasks/
│   ├── run                  run -- <command…> [--suite protocol|scenarios|w3c] [--datamodel …]
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
├── README.md                for users: install, Host, handlers, liveness, limits
├── CHANGELOG.md
├── mise.toml                pins node, bun, pnpm, zig
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
│   ├── request.c            JSON-RPC request and reply; TinyactorsError(code, reason, path)
│   ├── values.c             CBOR tokens ↔ JavaScript values: BigInt, Uint8Array, Address
│   └── datamodel/           the native "ecmascript" datamodel: vtable entries only
│       ├── datamodel.c      the vtable; scopes, _event, In()
│       ├── handles.c        slot tables for payloads and sessions: generation, reference counts
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
├── README.md                for users: Host, charts in Python, both drivers, limits
│                            (trusted code; values outside the host's allocator)
├── CHANGELOG.md
├── mise.toml                pins python (3.11+, and a free-threaded build for tests), uv, zig
├── pyproject.toml           one abi3 wheel (3.11+) per platform, built by the mise task
├── docs/
│   ├── api-map.md           each API item → ta_* function, request method or effect (checked)
│   └── values.md            the Python side of the value table
├── src/                     one extension module, tinyactors; no .py files in the wheel
│   ├── extension.h          and the few macros the free-threaded test build needs
│   ├── module.c             PyInit_tinyactors, per-interpreter state (PEP 489), the types
│   ├── host.c               Host: one function per method → ta_* or a request
│   ├── loop.c               the turn; run_until_idle and attach(loop), one TimerHandle
│   ├── effects.c            one Effect record per effect, built with the accessors; dispatch
│   ├── request.c            JSON-RPC request and reply; TinyactorsError(code, reason, path)
│   ├── values.c             CBOR tokens ↔ Python objects; Address, UNDEFINED; deep copy
│   └── datamodel/           the "python" datamodel: vtable entries only
│       ├── datamodel.c      the vtable; per-session globals, _event, In()
│       ├── handles.c        slot tables for payloads and sessions: generation, reference counts
│       └── expressions.c    code objects compiled once per chart, by kind
├── conform/                 the suite's adapter (python -m conform), not in the wheel
├── tests/                   what the suite cannot see: threads, GC, asyncio, refusals
├── build/                   core's amalgamation and the extension (gitignored)
├── mise/tasks/              build, test, conform, wheel, lint
└── tools/lint/              ast-grep: layer rules, mapping check
```

The two bindings have the same files on purpose: the audit compares them
file by file.

**The free-threaded build is for tests only.** The limited API cannot be
compiled for a free-threaded interpreter before 3.15 (`Python.h` refuses
it), so the shipped wheel is abi3 from 3.11, and the test task builds the
same sources a second time without `Py_LIMITED_API` for the free-threaded
interpreter. The functions that differ (`PyDict_GetItemRef`,
`PyList_GetItemRef`, `Py_mod_gil`, limited only from 3.13) sit behind
macros in `extension.h`; module state comes from `PyType_GetModule` and
`METH_METHOD`, limited since 3.9. A free-threaded wheel waits for the
stable ABI of free-threaded builds (abi3t, 3.15).

### Decided later

- **The browser.** A wasm module's C cannot make JavaScript objects, so a
  browser package needs either JavaScript doing what `host.c` and
  `effects.c` do (a twin, which ADR 0008's web binding was) or the
  Node-API addon compiled to wasm over an emulated Node-API (emnapi, which
  ADR 0008 did not choose when the logic sat above the addon). Decide with
  measurements (download size, call cost) after M3; either way it passes
  the suite.
- **PostgreSQL**, and other languages, after M5.

## Requests to core

Gaps a binding session found, for the coordinator. Each names what is
missing, why, and the suite case or test that shows it.

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
  it comes with the change (it fails on quickjs today).

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
