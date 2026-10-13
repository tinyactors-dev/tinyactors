# Roadmap: tinyactors-napi

**Being rewritten from scratch.** The first addon (M1 of the earlier
plan, 2026-10-12) was removed (`70434bee`: `src/`, the adapter and the
binding's docs; the tooling stays). **Next for this binding: step 8.5 of
the root [ROADMAP.md](../ROADMAP.md), later.** It starts after the Python
binding's step 8.4 and its API review, so that the API shape is settled
once, in Python, before the second binding is written. Until then this
session does nothing here. The binding's rules, true at every step, are
in [AGENTS.md](AGENTS.md); the plan of both bindings is the root
roadmap's step 8, which decides.

## Step 8.5: Node, to the same "done means" as Python's 8.4

From the root ROADMAP.md, step 8, "8.5"; read it there, and 8.4 with it,
before changing this copy.

### Done means

- **Scope:** the package's `Host`; `ta_host_create` from JavaScript
  options (the addon reads `host.config` itself); images, boot, spawn,
  send, terminate; the loop, one drain at a time, on the event loop with
  one `setTimeout`, and `SimulatedClock.increment(ms)`; I/O processors as
  the application's output channel, asynchronous and synchronous;
  `subscribe` with `actor.changed`; typed methods over post returning
  promises settled at a later drain; `LOG` to a logger; `DONE` and `ERROR`
  internal; rows in `ta_table`; `encode` and `decode`; the native
  `"ecmascript"` datamodel for `<data>`, `cond`, `assign`, `<script>`,
  `_event`, `In()`, `<send>` with params, `<foreach>`, with quickjs
  beside it; the suite's adapter (`conform.mjs`), framing the driver's
  calls with `encode` and `decode` and importing nothing but
  `@tinyactors/node` and `node:` built-ins.
- **Gates**, each in `mise run test`:
  - the suite's M1 set (`--milestone M1`) under node and under bun, on the
    native datamodel and on quickjs;
  - the value table's own tests, against `values.txt`'s `ecmascript`
    lines;
  - the mapping table (`docs/api-map.md`);
  - the layer lints, the prefix, no mutable globals;
  - each rule of AGENTS.md's "Rules the suite cannot see" with a test of
    its own.
- **Then the API review**, side by side with the Python binding. The rest
  of 8.5 (W3C per datamodel, every effect, lifecycle and peers, examples
  and release) follows for both bindings, as the root roadmap lists it.

### Plan and progress

The session writes its plan here as a list of steps, each with the commit
that finished it, when 8.5 starts. `GATE` in `mise/napi.rb` names what of
the set `mise run test` runs so far, and grows with the work.

## How the work runs

The root ROADMAP.md, step 8, "Rules for the work", sets these for every
session; here they are as they apply to this one.

- **The Node session owns `tinyactors-napi/` and nothing else.** A Python
  session owns `tinyactors-cpython/`, in the same checkout; a
  coordinating session owns `tinyactors-core`, `tinyactors-test` and the
  root documents.
- **This file is the session's.** The session keeps it current and
  commits it with its own paths. It never edits the root `ROADMAP.md`:
  the coordinator reads this file, moves each request below into the
  root's "Requests to core", and marks the step done there after its
  audit and review.
- **Core and the suite are frozen.** A gap in either (AGENTS.md, "Gaps in
  core") is written under "Requests to core" below and committed alone
  (`docs(napi): request to core: ...`). The coordinator makes the change,
  with C tests and suite cases, and says when to go on.
- **One checkout, on `main`, commits of this project's paths only:**
  `git commit -- tinyactors-napi/` (`git add` a new file by its path
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
- **An audit after the step**, by the coordinator: both bindings side by
  side, against the guide's "never" list, each `AGENTS.md`'s "What not to
  build", and their mapping tables. What it finds is fixed here.

## Requests to core

Gaps in core or in the suite this session finds. Each says what is
missing, why, and which suite case or test shows it; the coordinator moves
it into the root ROADMAP.md's "Requests to core" and answers there.

None open. The first addon's two requests were answered on 2026-10-12
(the runner skips envelope-level vectors for a binding, `34c09cfa`; the
tick rate through `host.config`, `9bd33f03`); their text is in this file's
history before `70434bee`.
