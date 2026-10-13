# Roadmap: tinyactors-cpython

**Being rewritten from scratch.** The first binding (M1 of the earlier
plan, 2026-10-12) was removed (`6426816a`: `src/`, the adapter and the
binding's docs; the tooling stays). **Next: step 8.4 of the root
[ROADMAP.md](../ROADMAP.md): implement the decided design, then measure
it.** The design (2026-10-13) is in [AGENTS.md](AGENTS.md) ("The API",
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

In this order, each step a commit (or a few) that passes this project's
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
  (`tinyactors-core/src/system/route.c:439-497`). A runtime resolver
  configured on the `Host` will answer it once run-time fetching is
  built (root `ROADMAP.md`, "Decided later"); not now, and it is not the
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
  scratch arena [8.2].
- **`traceparent`:** in, W3C text passed as the `str`'s UTF-8 for the
  call (`send(..., *, traceparent=None)`, `send_many` items, `spawn(...,
  traceparent=None)`), parsed by core; out, `send.traceparent`, formatted
  from the effect's `ta_trace_context` on first read (`None` when
  untraced); `send.answer(..., traceparent=send.traceparent)` keeps the
  trace by default, `None` starts a new one; `send.fail()` carries it.

### Open, for the API review or the maintainer

- **What a resolver's other exceptions become.** A `ResolveError`, a
  `SyntaxError` and the rewrite's refusal are diagnostics at their line;
  whether any other exception (an `OSError` from reading a file, a bug in
  the resolver) is a diagnostic too, or is raised from `compile_charts`
  after the result is freed, is not decided.

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
