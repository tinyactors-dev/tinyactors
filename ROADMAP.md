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
- **Each binding keeps its own roadmap**, `tinyactors-cpython/ROADMAP.md`
  and `tinyactors-napi/ROADMAP.md`: the session's plan and progress for
  its milestone (M1's scope and gates copied from here, with a link back,
  and "M1 done means"), how the work runs for that session (these rules
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
  and snapshot; M5 nothing more. Without `--milestone` the whole suite
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

**The size tripwire, designed (2026-10-12; built when M1 is done):**

- **Where it lives.** `tinyactors-<binding>/size-budget.txt`, one line per
  file it budgets: `<path> <baseline> [<raised> <reason…>]`. It covers
  every file under `src/` and the adapter (`conform/`, `conform.mjs`),
  and `index.mjs`/`index.d.ts`. Lines are counted as `wc -l` counts them,
  so the number is never in dispute. One checker for both bindings,
  `mise/lib/size_budget.rb` at the root (with its test, run by the root's
  `test` like `api-map-test`), called by each binding's `mise run lint`.
- **How the budget is set at M1.** At the M1 audit the coordinator runs
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
  never moves after M1, so `git log -p size-budget.txt` lists every
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

#### M1: walking skeletons with native datamodels (both sessions)

Each binding: its module and `Host` type; `ta_host_create` from the
language's options; images, boot, spawn, send, terminate; the loop with
every run status; `DONE`, `ERROR`, `LOG`; `request` with the language's
error type; the CBOR conversion with the value table (big integers,
bytes; Python's `Address` and `UNDEFINED`; Node's actor reference is a
string, below); the native datamodel (`"python"`,
`"ecmascript"` in Node's engine) for `<data>`, `cond`, `assign`,
`<script>`, `_event`, `In()`, `<send>` with params, `<foreach>`; quickjs
wired in beside it; and the suite's adapter (`conform`).

Both bindings: **the CBOR conversion is public** (decided 2026-10-12):
the module's own `encode` and `decode`, top-level and named after the
platform's conventions, Python's `tinyactors.encode(value) -> bytes` and
`tinyactors.decode(data) -> object` (the shape of `json` and `cbor2`),
Node's `encode` and `decode` exports. They are core's token lists
(`ta_cbor_encode`, `ta_cbor_decode`; `tinyactors/cbor.h`) and the
binding's value table (`values.c`), the same conversion the API uses, and
each has its row in `docs/api-map.md` (bindings.md, section 9). The
adapter reads and writes the driver's frames with them, and its lint
(`tinyactors-test/lint`) refuses every import but the binding's public
module and the language's standard library, so no third-party codec
stands between the suite and the binding. **Known limit:** the vectors
then go through the binding's codec both ways (the runner encodes the
expected bytes, the adapter decodes the call and encodes the answer), so
a value that maps wrong the same way in `decode` and `encode` passes them.
The binding's own value tests catch that: `docs/values.md`, row by row,
and the language's lines of `values.txt` (`python`, `ecmascript`).

Both bindings: **a simulated clock** in the public API, its shape
borrowed from xstate (`SimulatedClock`, `increment(ms)`) and its meaning
ours. A host created with it arms no timer; `increment(ms)` calls
`ta_host_advance_time` (the wall clock is the simulated time) and runs the
loop until idle; inputs run the turn at the current time. It is the
binding's form of "a caller's own clock" (`tinyactors-core/docs/bindings.md`,
section 3). It serves application developers testing their charts first;
the adapter uses it for the scenarios' `advance` steps. Each
`docs/api-map.md` lists it against `ta_host_advance_time` and the turn.

Gates: the suite's M1 set (`--milestone M1`: every protocol vector file,
the basics scenario and the outcome canaries) on both datamodels, which
holds `values.txt` round trips on the native datamodel; the value
table's own tests; mapping table; layer lints and no mutable globals.
**Then the API review**, both bindings side by side: how a Python chart
reads, how a Node program reads. Each binding's own `ROADMAP.md` has
"M1 done means", the plan and progress.

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

#### M2: W3C in each language

`confPython.xsl` beside the W3C stylesheets (ours, checked in: M0 item
5), the suite generated per datamodel: on python every test but the 20 in
`skip-python.txt` (ECMAScript data model tests, and 567), 191 of 191 on
Node's native ECMAScript datamodel and on quickjs. Decide here whether Python needs
compile-time checks of its expressions (the deleted
`ta_datamodel_compiler`; this time the compiler would call it).

**Sync sends are in M2** (decided 2026-10-12; they were in M3). The W3C
environment runs BasicHTTP as a synchronous I/O processor
(`tinyactors-test/docs/driver.md`, "The W3C environment"), and the 12
tests that use it are in this gate: 201, 509, 510, 518, 519, 520, 522,
531, 532, 534, 567, 577 (all optional; 567 is on python's skip list, so
11 on python). So each binding's loop answers `TA_RUN_EFFECTS` (the sync
sends are inputs, then run again now: `tinyactors-core/docs/bindings.md`,
section 3), its API lets an I/O processor be synchronous, and its adapter
answers the BasicHTTP `SEND` effects as the W3C environment says.

#### M3: every effect

I/O processors (`SEND`; sync sends are in M2), `INVOKE`/`UNINVOKE`, `REQUEST` and
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
the envelope vectors (in every binding's set from M1 on:
`suites/milestones.txt`).

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
│   ├── request.c            JSON-RPC request and reply; TinyactorsError(code, reason, path)
│   ├── values.c             CBOR tokens ↔ JavaScript values: BigInt, Uint8Array; tag 32 as its text;
│   │                        the encode and decode exports
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
│   ├── loop.c               the turn; run_until_idle and attach(loop), one TimerHandle
│   ├── effects.c            one Effect record per effect, built with the accessors; dispatch
│   ├── request.c            JSON-RPC request and reply; TinyactorsError(code, reason, path)
│   ├── values.c             CBOR tokens ↔ Python objects; Address, UNDEFINED; deep copy;
│   │                        tinyactors.encode and tinyactors.decode
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
  measurements (download size, call cost) after M3; either way it passes
  the suite.
- **PostgreSQL**, and other languages, after M5.

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
  a 1 KB envelope's `ta_host_envelope` 9.3 µs against 17.6). Next: both
  bindings migrate to the new calls (their call sites are listed in the
  coordinator's report), then delete their rooms, hints and retries
  (`bindings.md` sections 6 and 9).

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
