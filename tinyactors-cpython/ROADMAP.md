# Roadmap: tinyactors-cpython

**Being rewritten from scratch.** The first binding (M1 of the earlier
plan, 2026-10-12) was removed (`6426816a`: `src/`, the adapter and the
binding's docs; the tooling stays). **Next: step 8.4 of the root
[ROADMAP.md](../ROADMAP.md): implement the decided design, then measure
it.** The design (2026-10-13) is in [AGENTS.md](AGENTS.md) ("The API",
"The loop", "Lifetimes and thin views", "The binding's memory", "The
"python" datamodel"); there is no design document to write first. The
step starts when the root roadmap marks steps 8.2 (core implements
[ADR 0012](../docs/adr/0012-lifetimes-follow-the-loop.md) with its second
amendment: `ta_table`, `ta_status_name`, `ta_growable_arena`, one batch
at a time, `detach_event` checked) and 8.3 (the full loop end to end in
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
3. **Inputs and images:** `add_image`, `spawn`, `send`, `terminate`.
4. **Effects as thin views:** I/O processors with `answer` and `fail`, a
   record for `async def` processors, the log handler; the ack count and
   the stale check.
5. **Typed methods** with response handlers and the pending table.
6. **Values:** `tinyactors.encode` and `tinyactors.decode`, the value
   table's tests against `values.txt`'s `python` lines.
7. **The `"python"` datamodel:** the class per chart, the rewrite, the
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

### Open, for the API review or the maintainer

- **`dict` and `list` subclasses** in the value table: refused (a
  conversion runs no user code) or converted.
- **The rewrite's allowed constructs and names** (which builtins, which
  AST nodes), listed in `src/datamodel/expressions.c` and the README.
- **`add_image(charts)`:** the form of `charts` and what it returns.
- **The log entry's `chart`:** a `TA_EFFECT_LOG` carries `SEGMENT`,
  `ACTOR` and `SESSION`, not the chart (`host.h:262-289`); reading it
  through the actor (`ta_host_locate`, `ta_host_system`,
  `ta_actor_info_get`, `ta_image_chart_name`) answers `TA_ERROR_STALE`
  for an actor that ended in the same run (actor handles carry a 7-bit
  generation, `tinyactors-core/src/base/pool.h:75-81`). Read it from the session row (the `"python"`
  datamodel's sessions only), or ask core for the chart on `LOG`
  effects (a request below, if a test shows the need).
- **Export of a `"python"` actor:** `serialize` cannot tell export from
  hibernation: both reach the same call, `serialize` with no reason
  (`tinyactors-core/src/system/hibernate.c:329`), then the same `unload`
  and `destroy` (`hibernate.c:502-503`, from `:525` and `:563`), so an
  exported actor's row stays hibernated until `ta_host_free`; and a bare
  handle can be live in another host's table by coincidence (two hosts
  in one process both start at slot 0, generation 1). The blob needs the
  writing datamodel's identity beside the handle (a random 16 bytes per
  `Host`), and export a refusal; how is open.
- **An `async def` processor under the synchronous driver**, which runs
  no event loop: refused, or run to completion by the driver.
- **Where an `async def` processor's exception goes:** the task's, or
  raised by the driver after the turn as a plain handler's.

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
