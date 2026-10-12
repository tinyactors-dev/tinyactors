# Roadmap: tinyactors-napi

The Node binding's plan and progress, milestone by milestone, how the
work on it runs now, and its requests to core. The plan of both bindings
is the root [ROADMAP.md](../ROADMAP.md), step 8: its milestones, gates and
rules decide; this file says how this binding gets there and how far it
is. The binding's own rules, true whatever the milestone, are in
[AGENTS.md](AGENTS.md).

**The milestone now: M1** (`MILESTONE` in `mise/napi.rb`; `mise run test`
runs the suite's set for it, `--milestone M1`).

## How the work runs now

The root ROADMAP.md, step 8, "Rules for the work", sets these for every
session; here they are as they apply to this one.

- **The Node session owns `tinyactors-napi/` and nothing else.** A Python
  session owns `tinyactors-cpython/` at the same time, in the same
  checkout; a coordinating session owns `tinyactors-core` and
  `tinyactors-test`.
- **This file is the session's.** The Node session keeps it up to date as
  it works and commits it with its own paths. It never edits the root
  `ROADMAP.md`: the coordinator reads this file, moves each request below
  into the root's "Requests to core" when it takes it up, and marks a
  milestone done there after its audit and review. Then the session moves
  `MILESTONE` in `mise/napi.rb` on to the next milestone.
- **Core and the suite are frozen.** A gap in either (AGENTS.md, "Gaps in
  core", says what a report holds) is written under "Requests to core"
  below and committed alone (`docs(napi): request to core: ...`). The
  coordinator makes the change, with C tests and suite cases, and says
  when to go on (rebase onto it).
- **One checkout, on `main`, commits of this project's paths only:**
  `git commit -- tinyactors-napi/` (`git add` a new file by its path
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

- **Scope:** the package and its `Host` class; `ta_host_create` from
  JavaScript options; images, boot, spawn, send, terminate; the loop on
  the runtime's event loop with every run status; `DONE`, `ERROR`, `LOG`;
  `request` with `TinyactorsError` (code, reason, path); the CBOR
  conversion with the value table (big integers as `BigInt`, bytes as
  `Uint8Array`, an actor reference as its address string), exported as
  `encode(value) -> Uint8Array` and `decode(bytes) -> value`; the native
  `"ecmascript"` datamodel in the application's engine for `<data>`,
  `cond`, `assign`, `<script>`, `_event`, `In()`, `<send>` with params,
  `<foreach>`; quickjs wired in beside it; `SimulatedClock` with
  `increment(ms)`; `index.mjs` and `index.d.ts`; the suite's adapter
  (`conform.mjs`), which frames the driver's calls with `encode` and
  `decode` and imports nothing but `@tinyactors/node` and `node:`
  built-ins; and each rule of AGENTS.md's "Rules the suite cannot see"
  with a test of its own.
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
    `outcome-fail`, `outcome-timeout`), under node and under bun, each on
    the native datamodel and on quickjs;
  - `values.txt` round trips on the native datamodel (in the set above);
  - the mapping table (`docs/api-map.md`);
  - the layer lints, the prefix, no mutable globals;
  - the rules of AGENTS.md's "Rules the suite cannot see", each with a
    test of its own in `test/`;
  - the value table's own tests, against `values.txt`'s `ecmascript`
    lines: the vectors go through this binding's codec both ways, so they
    cannot catch a value mapped wrong the same way in both directions
    (the root ROADMAP.md's "Known limit").
- **Then the API review**, by the coordinator, both bindings side by
  side: how a Python chart reads, how a Node program reads. M2 starts
  after it, when the coordinator marks M1 done in the root `ROADMAP.md`
  and this session moves `MILESTONE` on.

### Plan and progress

The session writes its plan here as a list of steps, each with the commit
that finished it, and keeps it current.

Each step is one or more commits that build and pass `mise run test`
here. The order gets the suite's gate live early: an adapter answering
the protocol vectors on quickjs under node and bun (step 5) comes before
the native datamodel (step 6). Until step 6, the conform task runs the
datamodels the addon has (`DATAMODELS` in `mise/napi.rb`, quickjs only);
step 6 adds `native` there, and from then on both must pass. Every API
item gets its `docs/api-map.md` row in the commit that adds it.

1. **The plan** (this list). Decided before it, by a spike
   (2026-10-12, node 22.23.2 and bun 1.4.1, an addon built with the
   pinned clang): `napi_set_instance_data`, `napi_get_instance_data` and
   `napi_type_tag_object` work in both runtimes, and each worker thread
   gets instance data of its own, so per-environment state needs no
   global. Recorded with its evidence in `docs/decisions.md` in step 2.
2. **The addon builds and loads.** `mise run build` compiles core's
   amalgamation (`tinyactors.c`, `tinyactors_runtime.c` with its hosted
   file, `tinyactors_datamodel_ecmascript.c`) and `src/` into
   `build/tinyactors.node` with `Toolchain::Target.host` and `#plugin`;
   `module.c` keeps its state in the environment's instance data;
   `index.mjs` loads the addon and re-exports it. A test loads it under
   node and bun, in the main thread and a worker.
3. **`encode` and `decode`** (`src/values.c`): the value table of
   `docs/values.md`, both ways, with the UTF-8, integer and depth checks;
   the decode error type decided (reasoned analysis) and written in
   `docs/values.md`. Tests: every row of `docs/values.md`, and
   `values.txt`'s `ecmascript` lines through `encode` (the known limit:
   the vectors alone cannot catch a row mapped wrong both ways).
4. **`Host` on quickjs**, in small commits:
   - (a) `new Host(options)`: the config from a JavaScript value
     (`ta_host_create`), refused with `TinyactorsError` (code, reason,
     path); `request(method, params)` (`ta_host_request`);
     `close()` (`ta_host_free`); type tags on the wrapped object;
   - (b) charts: `compile` (`ta_compile_charts`, `ta_compile_image`),
     `addImage`, `boot`, `spawn`, `send`, `sendMany`, `terminate`; the
     loop on the runtime's event loop (one `setTimeout`, ref'd while a
     deadline is pending) with every run status; `LOG`, `DONE`, `ERROR`
     to the application's handlers; `SimulatedClock` with
     `increment(ms)`;
   - (c) peers: `addPeer` (`ta_host_peer_add`), `deliver`
     (`ta_host_envelope_read`, `ta_host_deliver`), and `PEER` and
     `PEER_ERROR` effects as envelope bytes (`ta_host_envelope`).
   The API's open shapes (options, handlers, errors, ids as `BigInt`)
   are decided by reasoned analysis and recorded in `docs/decisions.md`.
5. **The adapter** (`conform.mjs`): every method a binding must offer,
   over the public API, framed with `encode` and `decode`; the M1 set on
   quickjs under node and bun in `mise run test`. A vector the public
   API cannot express goes to "Requests to core" below, committed alone.
6. **The native `"ecmascript"` datamodel** (`src/datamodel/`): the two
   slot tables with generations (`handles.c`), expressions compiled once
   per chart as strict functions inside `with (scope)` (`expressions.c`),
   and the vtable (`datamodel.c`): `<data>`, `cond`, `assign` with
   quickjs's location check, `<script>`, `_event`, `In()`, `<send>` with
   params (captures copied through the value table), `<foreach>`,
   `to_wire`/`from_wire`, `variable`; one handle scope per entry; the
   last drop of a payload deferred until the outer call into core
   returns. `native` joins `DATAMODELS`: the M1 set passes on both.
7. **The rules the suite cannot see** (AGENTS.md), each with a test of
   its own in `test/`, where steps 2 to 6 did not already add it.
8. **M1 done**: README (trusted chart code, nesting 64, liveness on
   each runtime), `index.d.ts` complete, `docs/api-map.md`'s core
   functions no item uses, `docs/values.md`'s TODO answered; "M1 done
   means" checked item by item, then the report.

Progress:

- Step 1: `75e99ae2`.
- Step 2: `7a0c46d9`.
- Step 3: this commit. The decode error is `TinyactorsError` with core's
  status (`docs/decisions.md`, "Errors").

## Later milestones

From the root ROADMAP.md, step 8: the whole W3C suite (M2), every effect,
liveness and shutdown (M3), hibernation, migration and peers (M4), and
the release: one prebuilt addon per platform as optional dependencies
(M5).

## Requests to core

Gaps in core or in the suite this session found. Each says what is
missing, why, and which suite case or test shows it; the coordinator
moves it into the root ROADMAP.md's "Requests to core" and answers there.

- **The runner skips, for a binding, the request and config vectors its
  API cannot send** (2026-10-12). The same gap as the Python session's
  first request (`tinyactors-cpython/ROADMAP.md`, "Requests to core",
  1), found here independently by a fresh agent's refutation of
  `docs/decisions.md` ("The Host's API"); one answer serves both
  bindings.
  - *What is missing:* `tinyactors-test/lib/runner.rb` sends every case of
    `requests.txt` and `config.txt` to a binding's adapter. The cases
    that send bytes no `request(method, params)` can make (bindings.md,
    section 10: "build the map from the caller's arguments") and no
    config value can encode: config.txt "not CBOR" (`ff`), and
    requests.txt's "not CBOR", "not a map", "a batch", "no method, no
    id", "no method", "a member a request does not have", "id is not an
    integer, text or null", "id is not a float", the notifications ("no
    reply", "no error either", "applied", "a request that is not one is
    answered") and "more items than a request holds" (no `id`). The
    adapter may use only the public API (`tinyactors-test/lint`), so it
    could pass them only by writing core's replies itself, a twin of
    core's request parsing (bindings.md, section 2).
  - *What we ask:* as the Python session asks: for `--kind binding`, the
    runner skips, with a reason, a request case unless each send is a map
    of `method`, an `id` (integer, text or null) and optionally `params`,
    and a config case whose bytes are not one CBOR item; and
    `docs/driver.md` says a binding's adapter writes each reply from what
    its API returned or raised, with the call's `id`.
  - *Which test shows it:* `mise run conform -- --suite protocol` here
    once the adapter offers the vector methods (ROADMAP.md, M1, step 5):
    these cases fail, and the protocol suite stays out of `mise run
    test`'s gate (`GATE` in `mise/napi.rb`) until the runner skips them.
- **A binding cannot read a host's tick rate** (2026-10-12).
  - *What is missing:* a way to read the `ticksPerSecond` a host runs on
    after `ta_host_create` filled the config's defaults: a member of a
    reply (`host.stats`, or a request that answers the effective config)
    or a field the create call writes.
  - *Why:* the binding drives the host's monotonic clock from the
    runtime's (`performance.now()`, milliseconds) and from
    `SimulatedClock.increment(ms)` (ROADMAP.md, M1), so it converts
    milliseconds to ticks before every `ta_host_advance_time`. Without
    `ticksPerSecond` in the config, the system's default applies:
    `1000000000` (`tinyactors-core/src/system/system.c:43`), which no
    header or doc states (`include/tinyactors/system.h:82` has the field
    without a default; `docs/host-config.md:79-83` says "0 or absent: the
    system's default, `ta_system_config` in `tinyactors/system.h`"), and
    a binding may not copy it ("No defaults ... of the binding's own",
    `docs/bindings.md` section 13). `bindings.md` sections 3 and 7 say
    "the config's `ticksPerSecond`, 1000 for milliseconds", which a
    config without the key does not have.
  - *Which test shows it:* a host created with `{"systemID": "h"}` (the
    first case of `tinyactors-test/suites/protocol/config.txt`) and a
    `<send delay="50ms">`: the binding cannot compute the `monotonic` of
    the turn 50 ms later. The suite's configs all give
    `ticksPerSecond` 1000, so the suite does not fail; the binding's own
    test of a real-clock delayed send on a config without it will.
  - *Until then:* the binding converts with the `ticksPerSecond` of the
    config the application gave (its own value, not a default), and a
    host whose config has none refuses the clock's calls with
    `TA_ERROR_UNSUPPORTED`; nothing else waits on this.
