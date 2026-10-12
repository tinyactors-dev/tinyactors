# Roadmap: tinyactors-cpython

The Python binding's plan and progress, milestone by milestone, how the
work on it runs now, and its requests to core. The plan of both bindings
is the root [ROADMAP.md](../ROADMAP.md), step 8: its milestones, gates and
rules decide; this file says how this binding gets there and how far it
is. The binding's own rules, true whatever the milestone, are in
[AGENTS.md](AGENTS.md).

**The milestone now: M1** (`MILESTONE` in `mise/cpython.rb`; `mise run
test` runs the suite's set for it, `--milestone M1`).

## How the work runs now

The root ROADMAP.md, step 8, "Rules for the work", sets these for every
session; here they are as they apply to this one.

- **The Python session owns `tinyactors-cpython/` and nothing else.** A
  Node session owns `tinyactors-napi/` at the same time, in the same
  checkout; a coordinating session owns `tinyactors-core` and
  `tinyactors-test`.
- **This file is the session's.** The Python session keeps it up to date
  as it works and commits it with its own paths. It never edits the root
  `ROADMAP.md`: the coordinator reads this file, moves each request below
  into the root's "Requests to core" when it takes it up, and marks a
  milestone done there after its audit and review. Then the session moves
  `MILESTONE` in `mise/cpython.rb` on to the next milestone.
- **Core and the suite are frozen.** A gap in either (AGENTS.md, "Gaps in
  core", says what a report holds) is written under "Requests to core"
  below and committed alone (`docs(cpython): request to core: ...`). The
  coordinator makes the change, with C tests and suite cases, and says
  when to go on (rebase onto it).
- **One checkout, on `main`, commits of this project's paths only:**
  `git commit -- tinyactors-cpython/` (`git add` a new file by its path
  first). Never `git add -A`, `git add .` at the root, `git commit -a`,
  `git stash`, `git checkout -- .` or `git reset --hard`: they take or
  undo the other sessions' work. History stays linear: `git pull
  --rebase` when `main` moved.
- **`.git/index.lock` belongs to another session's git command.** When
  git says the index is locked, wait a few seconds and run the same
  command again, as often as it takes. Never delete the lock: the other
  command is still writing the index, and removing it can lose that
  session's staged work.
- **Tests: this project's only.** Run this project's `mise run test`
  before every commit, and the suite against the adapter (`mise run
  conform`). Never the root `mise run test` (it runs every project for 20
  minutes) and never `tinyactors-core`'s build or tests: the coordinator
  runs those. `mise run build` here runs core's `amalgamate` task, and
  that is the only core task this session runs.
- **An audit after each milestone**, by the coordinator: both bindings
  against the guide's "never" list, against each other file by file, and
  against their mapping tables. What it finds is fixed here.

## M1: walking skeleton with the native datamodel

From the root [ROADMAP.md](../ROADMAP.md), step 8, "M1: walking skeletons
with native datamodels"; read it there, with the gates table and the
pitfalls, before changing this copy.

### M1 done means

- **Scope:** the `tinyactors` module and its `Host` type;
  `ta_host_create` from Python options; images, boot, spawn, send,
  terminate; the loop with every run status; `DONE`, `ERROR`, `LOG`;
  `request` with `TinyactorsError` (code, reason, path); the CBOR
  conversion with the value table (big integers, bytes, `Address`,
  `UNDEFINED`), exposed as `tinyactors.encode(value) -> bytes` and
  `tinyactors.decode(data) -> object`; the `"python"` datamodel for
  `<data>`, `cond`, `assign`, `<script>`, `_event`, `In()`, `<send>` with
  params, `<foreach>`; quickjs wired in beside it; `SimulatedClock` with
  `increment(ms)`; the suite's adapter (`conform/`, `python -m conform`),
  which frames the driver's calls with `tinyactors.encode` and
  `tinyactors.decode` and imports nothing but `tinyactors` and the
  standard library; and each rule of AGENTS.md's "Rules the suite cannot
  see" with a test of its own.
- **`SimulatedClock`**, `increment(ms)` (xstate's shape, our meaning): a
  host created with it arms no timer; `increment(ms)` calls
  `ta_host_advance_time` and runs the loop until idle; inputs run the
  turn at the current time. It has its row in `docs/api-map.md`
  (`ta_host_advance_time`, `ta_host_run`), and the adapter uses it for
  the scenarios' `advance` steps.
- **Gates**, each in `mise run test`:
  - the suite's M1 set (`tinyactors-test/suites/milestones.txt`): every
    protocol vector file (`requests`, `config`, `envelopes`, `values`),
    the `basics` scenario and the outcome canaries (`outcome-compile`,
    `outcome-fail`, `outcome-timeout`), on the native datamodel and on
    quickjs;
  - `values.txt` round trips on the native datamodel (in the set above);
  - the mapping table (`docs/api-map.md`);
  - the layer lints, the prefix, no mutable globals;
  - both interpreters: the abi3 build on 3.13 and the free-threaded test
    build;
  - the rules of AGENTS.md's "Rules the suite cannot see", each with a
    test of its own in `tests/`;
  - the value table's own tests, against `values.txt`'s `python` lines:
    the vectors go through this binding's codec both ways, so they cannot
    catch a value mapped wrong the same way in both directions (the root
    ROADMAP.md's "Known limit").
- **Then the API review**, by the coordinator, both bindings side by
  side: how a Python chart reads, how a Node program reads. M2 starts
  after it, when the coordinator marks M1 done in the root `ROADMAP.md`
  and this session moves `MILESTONE` on.

### Plan and progress

The session writes its plan here as a list of steps, each with the commit
that finished it, and keeps it current.

**The gate grows with the work.** `mise run test` runs the suite only
where the adapter can already pass it: `GATE` in `mise/cpython.rb` names
the suites and datamodels it runs now (`--milestone M1` narrowed by
`--suite`), and each step below that makes more of the M1 set pass widens
it in the same commit. M1 is done only when `GATE` is the whole M1 set on
both datamodels, which is what the step 8 gates ask.

1. **The plan** (this list).
2. **The extension builds and imports** on both interpreters: `mise run
   build` compiles `src/` against core's amalgamation with
   `Toolchain::Target.host`, abi3 (`Py_LIMITED_API 0x030D0000`) for 3.13
   and again without it for 3.14t; `tests/` runs on both from `mise run
   test`.
3. **Values:** `src/values.c`, `tinyactors.encode` and `tinyactors.decode`
   (`ta_cbor_encode`, `ta_cbor_decode`), `UNDEFINED`, `Address`, the
   error type; the open rows of `docs/values.md` decided
   (reasoned-analysis); the value table's own tests against
   `values.txt`'s `python` lines and each row.
4. **`Host`:** `ta_host_create` from a Python config, the host's
   allocator (`PyMem_RawMalloc`), quickjs registered, `request(method,
   params)` with `TinyactorsError`, `close()` and the context manager,
   the creating thread checked.
5. **Charts and actors:** compiling charts into an image, adding and
   booting it, spawn, send, terminate; the loop with every run status;
   `DONE`, `ERROR`, `LOG` to the application; `SimulatedClock` and
   `increment(ms)`; a handler's exception raised after the turn.
6. **The adapter**, `python -m conform`: frames with `tinyactors.encode`
   and `decode`, the methods the scenarios need (`host.create`,
   `host.free`, `scenario.run`). Gate: the M1 scenarios on quickjs.
7. **Peers:** adding a peer, delivering envelope bytes
   (`ta_host_envelope_read`, `ta_host_deliver`), `PEER` and `PEER_ERROR`
   as bytes (`ta_host_envelope`); the adapter's vector methods. Gate: the
   M1 protocol vectors on quickjs (after the request to core below).
8. **The `"python"` datamodel, expressions:** the two slot tables
   (`src/datamodel/handles.c`), expressions compiled once at
   `chart_attach`, one globals dict per session, `<data>`, `cond`,
   `assign`, `<script>`, `<log>`, the `*expr` strings, `_event`, `In()`,
   the system variables kept as they were, `BaseException` raised again
   after the run, the last drop of a payload deferred.
9. **The `"python"` datamodel, values:** `<send>` with params, namelist
   and content, `<donedata>`, `<foreach>`, `to_wire` and `from_wire`,
   `variable`. Gate: the whole M1 set on both datamodels.
10. **What the suite cannot see:** a test of its own for each rule of
    AGENTS.md's "Rules the suite cannot see" not yet covered, the cycle
    collector's slots (`Py_tp_traverse`, `Py_tp_clear`), the README's
    limits, the mapping table complete with "Core functions no item
    uses".

Progress:

- Step 1: this commit.

## Later milestones

From the root ROADMAP.md, step 8: the W3C suite in Python (M2), every
effect and both drivers (M3), hibernation, migration and peers (M4), and
the release: one abi3 wheel per platform for Python 3.13 and later (M5).

## Requests to core

Gaps in core or in the suite this session found. Each says what is
missing, why, and which suite case or test shows it; the coordinator
moves it into the root ROADMAP.md's "Requests to core" and answers there.

None yet.
