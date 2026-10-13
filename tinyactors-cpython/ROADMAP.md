# Roadmap: tinyactors-cpython

**Being rewritten from scratch.** The first binding (M1 of the earlier
plan, 2026-10-12) was removed (`6426816a`: `src/`, the adapter and the
binding's docs; the tooling stays). **Next: step 8.4 of the root
[ROADMAP.md](../ROADMAP.md), the design task of step 8: a Python API with
the native `"python"` datamodel that keeps tinyactors' memory
efficiency, written and measured before any code.** It starts when the root roadmap marks steps 8.2 (core
implements [ADR 0012](../docs/adr/0012-lifetimes-follow-the-loop.md)) and
8.3 (the full loop end to end in C) done. The binding's rules, true at
every step, are in [AGENTS.md](AGENTS.md); the plan of both bindings is the
root roadmap's step 8, which decides.

## Step 8.4: a minimal Python binding

From the root ROADMAP.md, step 8, "8.4"; read it there before changing
this copy.

### First: the design, before any code

The maintainer: the memory efficiency is "the whole point and where we
failed before". Native datamodel first stays. The step starts with a
proposal in `docs/proposals/` or an ADR, decided by the maintainer before the binding's code, that
states:

- **the Python API shape**: `Host`, its drivers, spawn, send,
  `subscribe` (`actor.changed` per macrostep, `state(session)` for the
  starting point), the typed methods, and how a chart's Python code
  reads; each item a core function, a method or an effect;
- **the `"python"` datamodel's memory model**: what one session costs:
  its globals dict (one per session, no locals; [AGENTS.md](AGENTS.md),
  "Rules the suite cannot see"), `_event`, its `ta_table` rows, and the
  compiled code, per chart and shared by its sessions, never per
  session;
- **a per-actor budget against quickjs's shared realm**: a session scope
  ~0.5 KB in one shared context, the `fit` actor 2.9 KB instead of
  60.7 KB (`tinyactors-core/docs/architecture.md:844`), and how a Python
  actor stays within its budget;
- **spikes that measure it**: bytes per actor at 10,000 actors of one
  chart, resident and idle, on both datamodels side by side on one
  machine (root `AGENTS.md`, "Benchmarks"); every number from a spike or
  marked unverified.

### Done means

- **Scope:** the `tinyactors` module and its `Host`; `ta_host_create` from
  Python options (the extension reads `host.config` itself); images, boot,
  spawn, send, terminate; the loop, one drain at a time, with the
  synchronous driver and `SimulatedClock.increment(ms)`; I/O processors as
  the application's output channel, asynchronous and synchronous;
  `subscribe` with `actor.changed`; typed methods over post, settled at a
  later drain; `LOG` to `logging`; `DONE` and `ERROR` internal; rows in
  `ta_table`; `tinyactors.encode` and `tinyactors.decode`; the `"python"`
  datamodel for `<data>`, `cond`, `assign`, `<script>`, `_event`, `In()`,
  `<send>` with params, `<foreach>`, with quickjs beside it; the suite's
  adapter (`conform/`, `python -m conform`), framing the driver's calls
  with `tinyactors.encode` and `tinyactors.decode` and importing nothing
  but `tinyactors` and the standard library.
- **The design** above, decided by the maintainer before the code, and
  **the measured bytes per actor at 10,000 actors** on the `"python"`
  datamodel, within its budget, beside quickjs's on the same machine, by
  a task of this project that reruns the measurement.
- **Gates**, each in `mise run test`:
  - the suite's M1 set (`--milestone M1`: every protocol vector file, the
    `basics` scenario and the outcome canaries) on the native datamodel
    and on quickjs;
  - the value table's own tests, against `values.txt`'s `python` lines;
  - the mapping table (`docs/api-map.md`): every item a core function, a
    method of `requests.md` or an effect;
  - the layer lints, the prefix, no mutable globals;
  - both interpreters: the abi3 build on 3.13 and the free-threaded test
    build;
  - each rule of AGENTS.md's "Rules the suite cannot see" with a test of
    its own.
- **Then the API review** by the maintainer: how a Python program and a
  Python chart read. After it, the Node session starts (8.5) and this
  binding goes on with 8.5's work for both.

### Plan and progress

The session writes its plan here as a list of steps, each with the commit
that finished it, when 8.4 starts; the design and its spikes are the
first steps. `GATE` in `mise/cpython.rb` names what
of the M1 set `mise run test` runs so far, and grows with the work; 8.4 is
done only when it is the whole set on both datamodels.

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

None open. The first binding's two requests were answered on 2026-10-12
(the runner skips envelope-level vectors for a binding, `34c09cfa`; the
tick rate through `host.config`, `9bd33f03`); their text is in this file's
history before `6426816a`.
