# Roadmap: tinyactors-cpython

**Step 8.4 is implemented and measured (2026-10-14); next: the API
review by the maintainer, then step 8.5.** The first binding (M1 of the
earlier plan, 2026-10-12) was removed (`6426816a`); the rewrite follows
the design of [AGENTS.md](AGENTS.md), with the changes below ("Decided in
8.4, for the API review"). The design (2026-10-13) is in [AGENTS.md](AGENTS.md) ("The API",
"The loop", "Lifetimes and thin views", "The binding's memory", "The
"python" datamodel"); there is no design document to write first. The
step starts when the root roadmap marks steps 8.2 (core implements
[ADR 0012](../docs/adr/0012-lifetimes-follow-the-loop.md): `ta_table`,
`ta_status_name`, `ta_growable_arena`, one batch at a time, the
resolver's errors and scratch arena; and `detach_event` checked) and 8.3 (the full loop end to end in
C) done. The plan of both bindings is the root roadmap's step 8, which
decides.

## Step 8.4: a minimal Python binding

From the root ROADMAP.md, step 8, "8.4"; read it there before changing
this copy.

### Plan

**Bootstrap (authorized 2026-10-13):** write a new extension and adapter
from scratch before the actor binding. The codec and driver framing
start items 6 and 8 below early; steps 8.2 and 8.3 still precede the host
and actor implementation. The first slice supports driver-frame values
(docs/values.md), and the adapter reports unimplemented methods with
-32601. M1 must fail explicitly until the real host methods exist.
The existing conformance gate stays enabled. The maintainer explicitly
requested committing this deliberately red bootstrap (2026-10-13), as
an exception to the passing-before-commit rule. Record the expected M1
failure in the commit; no suite or gate is weakened to land it.

Bootstrap verification (2026-10-13): lint passes; both the abi3 build on
Python 3.13.16 and the free-threaded build on Python 3.14.8 pass nine
codec and adapter tests. `mise run test` then reaches M1 and fails for
missing `host.create`: protocol has 170 skips and no passes, and the
scenario language probe fails. A separate
`TA_PYTHON_DATAMODELS=quickjs mise run conform -- --milestone M1` gives
the same diagnostic. Neither selection crashes or produces malformed
driver frames.

**Codec completion — done (2026-10-13):** finish the existing value codec with
`UNDEFINED` and `Address` (CBOR tag 32), as AGENTS.md's value table already
requires. Add tests driven by the shared
`values.txt`: its `python` expression lines and applicable roundtrip and
malformed cases, including the non-ECMAScript cases that preserve
references and undefined. Done means those codec cases pass on both
interpreters and the public documentation and mapping table cover the
added types. This serves the binding author and maintainer; it adds only
the already decided codec surface and work during conversion. Host and
actor execution, their per-event work, and core changes are outside this
increment. M1 still reports missing `host.create`; the overall roadmap's
next steps remain core 8.2 and the C loop 8.3 before Host implementation.

Implemented: immutable `Address(text)` with read-only `.text`, text-based
equality/hash among the same exact type and tag 32 encoding; a module-local
`UNDEFINED` singleton, preserved at every value position; and the shared
Python-expression, applicable roundtrip and malformed vector cases. Module
state owns the heap types and singleton, with GC traversal/clear and
refusal of calls before module execution. Public help, docs/values.md and
the mapping table describe the API, errors and lifetimes. Twenty tests
pass on both Python builds. The expected M1 failure is still missing
`host.create`; the gate stays enabled for this authorized bootstrap commit.

**Steps 1 to 9 — done (2026-10-14).** `mise run test` passes: lint,
both builds, 68 tests on each interpreter (`tests/`: every rule of
AGENTS.md's "Rules the suite cannot see"), and the suite's M1 set on the
native datamodel and on quickjs (94 protocol vectors and 4 scenarios
each; the 68 others skipped as the reference's or C-level). The
measurement (`mise run measure`, x86_64 Linux, 4 CPUs, CPython 3.13.16):
**423 B resident per actor on `"python"` against 1093 B on quickjs** at
10,000 actors, Python's heap 193 B and core's region 204 B of it
(AGENTS.md, "Measured cost per session"). One change outside this
project, for the maintainer to confirm: three method-level vectors of
`requests.txt` ("method is not text", "no such method", "params is not a
map") are marked `only reference`, since typed methods cannot send them
(below).

The plan, each step a commit (or a few) that passes this project's
`mise run test`:

1. **The `Host` and its memory:** `Host(config, io_processors, log,
   clock)`, the per-host `ta_growable_arena` with a mark per call, the
   tables (charts, sessions, payloads, pending), `with` and `async with`,
   `ta_host_create` and the `host.config` read; the lint rule
   `allocation-in-tables-and-host` moved to the new files.
2. **The loop:** `ta_python_drain` and `ta_python_turn` as AGENTS.md
   writes them, the synchronous driver (`run_until_idle`, `run`,
   `SimulatedClock.increment`) and asyncio (`await host.idle()`), every
   status of the table.
3. **The compiler, images and inputs** (the direct layer, AGENTS.md
   "Layers"): `compile_charts`, `Compiled` (`image`, `diagnostics`,
   `close`, `with`, `BufferError` while `image` is exported),
   `CompileError`; the resolver protocol on `compile_charts` only, with
   `ResolveError` (and a `SyntaxError` converted), and `.py` sources
   through `src` with the rewrite at resolve time; `add_image(image) ->
   int`, `chart(name)`, `spawn(ref, ...)`, `send`, `send_many` (these
   three with their `traceparent`), `terminate`; `TA_EFFECT_REQUEST`
   answered in C with `TA_ERROR_INVALID` until run-time fetching is
   built. No convenience layer (no
   `DirectoryResolver`).
4. **Effects as thin views:** I/O processors with `answer` and `fail`
   (both keeping `send.traceparent`), `send.traceparent` formatted on
   first read, a record for `async def` processors (refused under
   `with`), a failed send for a processor that raises, the log handler;
   the ack count and the stale check.
5. **Typed methods** with response handlers and the pending table.
6. **Values:** `tinyactors.encode` and `tinyactors.decode`, the value
   table's tests against `values.txt`'s `python` lines.
7. **The `"python"` datamodel:** the class per chart, the rewrite (its
   allow-list as AGENTS.md writes it), the
   instance per session, `_event` and `detach_event`, capture,
   hibernation by handle, quickjs beside it.
8. **The adapter** (`conform/`) and the suite's M1 set on both
   datamodels.
9. **The measurement:** a task that spawns 10,000 actors of one chart,
   resident and idle, on the `"python"` datamodel and on quickjs, and
   reports bytes per actor from the process, `tracemalloc` and
   `host.stats`' `regionBytes`, with the machine's signature (root
   `AGENTS.md`, "Benchmarks"). Its numbers replace the estimates in
   AGENTS.md ("The "python" datamodel").

### Done means

- **Scope:** the design of AGENTS.md, implemented as written; a change
  to it is the maintainer's, written into AGENTS.md first.
- **The measured bytes per actor at 10,000 actors** on the `"python"`
  datamodel beside quickjs's on the same machine, by this project's task
  (plan step 9), against quickjs's reference: a session scope ~0.5 KB in
  one shared context, the `fit` actor 2.9 KB
  (`tinyactors-core/docs/architecture.md:844`).
- **Gates**, each in `mise run test`:
  - the suite's M1 set (`--milestone M1`: every protocol vector file, the
    `basics` scenario and the outcome canaries) on the native datamodel
    and on quickjs;
  - the value table's own tests, against `values.txt`'s `python` lines;
  - the mapping table (`docs/api-map.md`): every item a core function, a
    method of `requests.md` or an effect, its *planned* marks gone;
  - the layer lints, the prefix, no mutable globals;
  - both interpreters: the abi3 build on 3.13 and the free-threaded test
    build;
  - each rule of AGENTS.md's "Rules the suite cannot see" with a test of
    its own.
- **Then the API review** by the maintainer: how a Python program and a
  Python chart read. After it, the Node session starts (8.5) and this
  binding goes on with 8.5's work for both.

### Decided (2026-10-13), no longer open

- **`dict` and `list` subclasses** are refused by the value table (exact
  types only; a conversion runs no user code).
- **The rewrite's allow-list** is AGENTS.md's ("The "python"
  datamodel"); the README says it is not a sandbox.
- **`add_image(charts)`** left the direct layer: `compile_charts`,
  `Compiled`, `add_image(image) -> int`, `chart(name)` and `spawn(ref,
  ...)` replace it; a mapping form belongs to the convenience layer,
  later.
- **The log entry** is `(session, label, text)`, no chart.
- **Export of a `"python"` actor:** nothing is done; unsupported, with
  the known limitation that an exported actor's row stays hibernated
  until `ta_host_free` (AGENTS.md).
- **An `async def` processor under the synchronous driver** raises
  `TypeError` naming it when the `Host` is entered with `with`.
- **A processor that raises before answering** (sync or async) has its
  send failed (`send.fail()`, `error.communication` for the sender), and
  the exception is raised from the driver after the turn.
- **The resolver lives only at compile time** (ADR 0012, decision 7): the `Host` has no resolver. A
  `TA_EFFECT_REQUEST` (a fetch at run time, which stays: ADR 0012,
  decision 8) is answered in C with `ta_host_resume` and
  `TA_ERROR_INVALID` (`error.execution` in the parent): core has already
  normalized the name and looked in the requesting segment's image
  (`tinyactors-core/src/system/route.c:439-497`). How it will be
  answered is decided with the build-out of run-time fetching (root
  `ROADMAP.md`, "Decided later"); not now, and it is not the
  control plane.
- **`Compiled.close()` while a `memoryview` of `image` is held** raises
  `BufferError`, as `bytearray` does.
- **`DirectoryResolver`** is the convenience layer's, pure Python,
  reading files under its root; the direct layer has the resolver
  protocol only (`ta_runtime_resolve` does not confine reads to a root).
- **A Python resolver refuses** by raising
  `tinyactors.ResolveError(message, line=, column=)`; a `SyntaxError` or
  the rewrite's refusal is converted with its `lineno` and `offset`. The
  extension fills `ta_resolve_error`, the message in the compilation's
  scratch arena.
- **`traceparent`:** in, W3C text passed as the `str`'s UTF-8 for the
  call (`send(..., *, traceparent=None)`, `send_many` items, `spawn(...,
  traceparent=None)`), parsed by core; out, `send.traceparent`, formatted
  from the effect's `ta_trace_context` on first read (`None` when
  untraced); `send.answer(..., traceparent=send.traceparent)` keeps the
  trace by default, `None` starts a new one; `send.fail()` carries it.

### Decided in 8.4, for the API review

Made while implementing, each the least the gates or the platform
needed; the maintainer confirms or changes them at the API review.

- **Typed methods take their params as keywords:** `method(handler,
  on_error=None, /, **params)`, passed to core as given. The method-level
  vectors (a param a method does not take, of the wrong type, missing)
  then reach core through the API; `actor_state(session, handler)` could
  not leave `session` out or pass `"x"`.
- **Three request vectors are the reference's:** "method is not text",
  "no such method" and "params is not a map" assumed the request facade
  ADR 0012 removed; no typed method can send them. They carry `only
  reference` in `tinyactors-test/suites/protocol/requests.txt` (a change
  to the suite, made in this step, `e738095`); ta-conform still runs them.
- **Peers:** `Host(..., peer=handler)`, `peer_add(id, *, trusted=False)`
  and `deliver(peer, envelope)`, the least that runs M1's envelope and
  value vectors (which deliver to the echo setup and read the envelope it
  sends) through the API. AGENTS.md had `PEER` and `PEER_ERROR` deferred.
- **A resolver's other exceptions** (an `OSError`, a bug) are raised from
  `compile_charts` after the result is freed; the compiler's diagnostic
  for that `src` is not reported.
- **The rewrite is Python** (`src/datamodel/support.py`, embedded by the
  build into the extension, run once per interpreter): the AST is not in
  the limited C API. A `.py` file's constructs are checked as the
  resolver reads it; its names when its chart is attached (the resolver
  does not know the chart's `<data>`).
- **`_event` holds its data's value**, not a reference on the payload row.
- **Any script's top-level `def`s** are the chart's methods.
- **The system variables** are made at `create` only for a chart whose
  code reads them (the rewrite records which).
- **`run()`** returns when the host is idle with no deadline left.
- **An input during a turn** (an answer, a typed method, a resume) makes
  the turn yield and turn again, rather than report idle; a turn is
  bounded at 64 runs and a drain at 64 batches, so a handler that answers
  every send (a ping-pong) never holds the event loop.

### The memory and throughput review (2026-10-14)

A fresh agent reviewed the binding against the memory and throughput
rules; what it found is fixed:

- a turn had no bound across runs (an answer per send kept asyncio from
  running other tasks): bounded, as above, with a test;
- every Host was in a cycle with itself through In() (freed only by the
  collector), and a host with a SimulatedClock through the clock: In()
  reaches the datamodel through a capsule cleared at free, the clock
  holds its hosts weakly; a test frees a dropped host with the collector
  off;
- deserialize's migration held a session row pointer across Python code:
  it reads the row by its handle again before writing;
- one large send_many kept an arena chunk of its size for the host's
  life: it converts in slices of 256;
- each event made a name `str` per session and a key per bind: names are
  the chart's shared `str`, the key is interned; the measurement reports
  the cost after one event too;
- a thin view whose fields were read could still answer after its ack:
  answer and fail check the batch first;
- a driver could start inside another call (a finalizer): refused.

Left for core (capacity planning, not the binding): a segment's region
grows by doubling under a burst of events and is kept (12.4 KB per actor
on quickjs after one event each at 10,000 actors).
- **Not in the API yet** (8.5): invoked services of the application
  (`INVOKE`, `UNINVOKE` are dropped), hibernation with a store (`WAKE`),
  migration's progress, snapshots and export of `"python"` actors, a
  `<foreach>` item that is not a declared `<data>`.

### Telemetry (8.5's first item, 2026-10-14) — done

`Host(..., telemetry=handler(host, signal, data))` gets each OTLP request
the encoder writes (`TRACES`, `LOGS`) as `bytes`, copied once;
`host.metrics()` pulls `ta_host_metrics`; the stop posts `trace.end` and
hands over the last take. Whom it serves: the operator and the
observability backend, through the application's exporter; the binding
does no I/O (bindings.md section 2: retries are the application's). Cost:
nothing without `telemetry` in the config (core's null encoder); with
it, one `bytes` per request (not per event) and one Python call. Tests in
`tests/test_telemetry.py` (spans decoded back with `trace_spans_decode`,
the last take at close, metrics in and outside a drain, a handler that
raises, an `async def` handler); checked by hand against otel-tui 0.7.5's
OTLP/HTTP receiver (spans, logs and metrics accepted and shown, with the
resource and the scope's chart attributes).

## How the work runs

The root ROADMAP.md, step 8, "Rules for the work", sets these for every
session; here they are as they apply to this one.

- **The Python session owns `tinyactors-cpython/` and nothing else.** A
  Node session owns `tinyactors-napi/` (from step 8.5), in the same
  checkout; a coordinating session owns `tinyactors-core`,
  `tinyactors-test` and the root documents.
- **This file is the session's.** The session keeps it current and
  commits it with its own paths. It never edits the root `ROADMAP.md`:
  the coordinator reads this file, moves each request below into the
  root's "Requests to core", and marks the step done there after its
  audit and review.
- **Core and the suite are frozen.** A gap in either (AGENTS.md, "Gaps in
  core") is written under "Requests to core" below and committed alone
  (`docs(cpython): request to core: ...`). The coordinator makes the
  change, with C tests and suite cases, and says when to go on.
- **One checkout, on `main`, commits of this project's paths only:**
  `git commit -- tinyactors-cpython/` (`git add` a new file by its path
  first). Never `git add -A`, `git add .` at the root, `git commit -a`,
  `git stash`, `git checkout -- .` or `git reset --hard`: they take or
  undo the other sessions' work. History stays linear: `git pull
  --rebase` when `main` moved.
- **`.git/index.lock` belongs to another session's git command.** When
  git says the index is locked, wait a few seconds and run the same
  command again. Never delete the lock.
- **Tests: this project's only.** Run this project's `mise run test`
  before every commit, and the suite against the adapter (`mise run
  conform`). Never the root `mise run test` and never `tinyactors-core`'s
  build or tests: the coordinator runs those. `mise run build` here runs
  core's `amalgamate` task, the only core task this session runs.
- **An audit after the step**, by the coordinator: the binding against
  the guide's "never" list, its `AGENTS.md`'s "What not to build", and its
  mapping table. What it finds is fixed here.

## Requests to core

Gaps in core or in the suite this session finds. Each says what is
missing, why, and which suite case or test shows it; the coordinator moves
it into the root ROADMAP.md's "Requests to core" and answers there.

None open. What the design needs from core is step 8.2 of the root
roadmap. The first binding's two requests were answered on 2026-10-12
(the runner skips envelope-level vectors for a binding, `34c09cfa`; the
tick rate through `host.config`, `9bd33f03`); their text is in this
file's history before `6426816a`.
