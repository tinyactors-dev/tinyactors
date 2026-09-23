# Tinyactors Node-API implementation roadmap

The addon is being rewritten from scratch against the ADRs in
[`docs/adr/`](docs/adr/README.md). The first addon (`src/addon.c` and its
tests) is not extended. It is deleted in step 1, and features come back one
step at a time on the new foundation.

Where this roadmap and an ADR disagree, the ADR wins, and this file should be
corrected.

## Shape of the result

- **Core interprets, the addon hosts, and JavaScript scripts** (ADR 0001).
- **Generated code:** everything JavaScript calls on core. The generator
  reads `tinyactors.api.json`, and its output is checked in (ADR 0013).
- **Hand-written plumbing:**
  - memory: a chunk source, size-class pools and arenas (ADR 0014);
  - scheduling: the slot-indexed scheduling table, pooled mailboxes, ready
    and runnable queues, and the pump (ADRs 0005, 0007);
  - the router, the actor directory and the address table (ADR 0006);
  - the clock;
  - adapters for core's callback interfaces;
  - the JavaScript data model, the handle table and value boxes (ADR 0011).
- **Actors cross the boundary as `(slot, generation)` pairs of small
  integers** (ADR 0002), passed to native code as two positional numbers.
  Nothing is allocated per actor on the JavaScript side.
- **Singular calls** (ADR 0003). The only batch path is paged enumeration,
  which continues from a cursor.
- **No `malloc` on the hot path** (ADR 0014). Memory is pooled by lifetime,
  and a steady workload requests no new chunks after warm-up.

## Target file tree

```text
tinyactors-napi/
├── README.md
├── ROADMAP.md
├── package.json
├── mise.toml
├── bindings.rb                  policy: bound, hooked, plumbing-owned, overridden, skipped (ADR 0013)
├── index.js                     public entry: createSystem, statechart, literal, TinyactorsError, version
├── index.d.ts                   public types
├── lib/                         JavaScript layer
│   ├── system.js                System facade over the native System and the generated core module
│   ├── actor.js                 ActorId/Actor view: { system, slot, generation } (ADR 0002)
│   ├── definition.js            Definition facade; identity read from core
│   ├── statechart.js            fluent builder; emits the generated spec shape (ADR 0013)
│   ├── events.js                event-name cache: text <-> ta_event_name_id (ADR 0004)
│   └── errors.js                TinyactorsError; codes from the generated result table (ADR 0008)
├── src/
│   ├── gen/
│   │   ├── ta_napi.c            GENERATED, checked in; never edited by hand
│   │   └── ta_napi.h            GENERATED, checked in
│   ├── addon.c                  module init: registers classes and the generated module; nothing else
│   ├── runtime.{c,h}            support for generated code: primitive and (slot, generation) codecs,
│   │                            result -> TinyactorsError, reentrancy guard, handle wrapping (ADR 0013)
│   ├── system.{c,h}             native System: owns ta_system and all plumbing, close, stats (ADR 0012)
│   ├── containers.{c,h}         vec, ring, map, slice over the pools (ADR 0009)
│   ├── allocator.{c,h}          chunk source, counting size-class pools shared with core, turn and
│   │                            call arenas over core's ta_arena (ADRs 0009, 0014)
│   ├── scheduler.{c,h}          slot-indexed scheduling table, pooled envelope mailboxes, ready and
│   │                            runnable queues, deliver/resume, one bounded turn, per-actor faults
│   │                            and budgeted dead letters (ADRs 0005, 0008)
│   ├── pump.{c,h}               setImmediate turns, deadline timer via global setTimeout, monotonic
│   │                            clock read every turn, flush/advance promises, hook calls (ADR 0007)
│   ├── hooks.{c,h}              post-call hooks for spawn/load/reconstruct/prepare/destroy/unload (ADR 0013)
│   ├── router.{c,h}             external-event router into mailboxes (ADR 0006)
│   ├── directory.{c,h}          in-memory actor directory and the interned address table (ADR 0006)
│   ├── events.{c,h}             dotted event-name interning and formatting (ADR 0004)
│   ├── values.{c,h}             handle table and reference-counted value boxes (ADR 0011)
│   ├── napi_datamodel.{c,h}     the JavaScript data model (ADR 0011)
│   ├── enumerate.{c,h}          cursor-paged actor enumeration (ADR 0003)
│   └── adapters/
│       ├── io.{c,h}             JavaScript I/O processors
│       ├── invoker.{c,h}        JavaScript invokers and the in-system SCXML invoker
│       └── log.{c,h}            buffered log sink
├── mise/
│   ├── tasks/
│   │   ├── bindings             regenerate src/gen/ from tinyactors.api.json and bindings.rb
│   │   ├── build                compile core amalgamation + src/**/*.c into build/tinyactors_napi.node
│   │   └── test                 staleness check, native unit tests, then the JS suites on Node and Bun
│   └── lib/
│       └── napi/                the generator (owned by this package; not shared)
│           ├── api.rb           read and validate the API description
│           ├── plan.rb          Node-API binding decisions: positional args, codecs, outputs, handles
│           ├── policy.rb        load bindings.rb and enforce complete, non-stale classification
│           └── emit.rb          format the plan as C
├── test/
│   ├── native/                  C unit tests (pools, arenas, containers, scheduling table), built by
│   │                            mise run test
│   ├── *.js                     one suite per area (system, definition, actor, post, router, time, ...)
│   └── scale.js                 population ratio benchmark and target-population measurements
│                                (steps 10, 11, 13)
└── docs/
    ├── design.md                earlier design notes; superseded where they differ from the ADRs
    └── adr/                     binding ADRs
```

`build/` holds only the compiled addon and is not checked in.

## Core prerequisites

The rewrite depends on these core ADRs. Each step below names the ones it
needs, and a step waits for its prerequisites instead of working around them
(ADR 0001).

| Core ADR | Needed by | State |
|---|---|---|
| 0054: delayed sends live with their owner | step 8 | implemented |
| 0055: deliver is the admission test (`TA_ERR_DONE`) | step 6 | implemented |
| 0056: a finished actor reports its final state | step 5 | implemented |
| 0057: core ships the null data model | step 4 | implemented |
| 0058: compile errors say what and where | step 4 (error quality only) | implemented |
| 0059: an actor ID is a public slot and generation | step 3 | **proposed, not implemented** |
| 0061: a macrostep can pause inside executable content | step 5 | implemented |
| 0062: the actor walk is paged by position | step 9 | implemented |

Step 3 and everything after it wait for core ADR 0059:
`ta_actor_id_slot()`, `ta_actor_id_generation()` and `ta_actor_id_make()`
are not in `tinyactors.h` yet.

The libraries in this repository are private (root `AGENTS.md`). When the
addon needs a core change, core's API changes in place and every caller
moves with it, the addon included. There are no second functions and no
compatibility shims.

Some core changes would help but are not prerequisites, because the pools
of ADR 0014 make their cost small. Each needs its own core ADR:
- grow the actor table in fixed pages instead of doubling it;
- keep an actor's sparse state and internal-event array between
  macrosteps;
- an interface that lets the router keep named addresses without copying
  them.

## Steps

Each step leaves `main` building and passing `mise run test`, and is
committed separately, in several commits where that helps. Every test runs on
both Node and Bun.

### 1. Reset the package

- Delete `src/addon.c`, `src/binding.h`, `src/null_datamodel.{c,h}` and the
  tests that exercise them. Keep `index.d.ts` as the target API.
- Create the tree above with empty modules. `addon.c` exports only
  `version`.
- Add `allocator.{c,h}` (ADR 0014):
  - the chunk source, the only code that calls `malloc` or `mmap`;
  - size-class pools with free lists, and dedicated allocations for large
    or over-aligned requests;
  - turn and call arenas over core's `ta_arena`;
  - counters per size class, including chunk requests.
- Add `containers.{c,h}` over the pools.
- Native unit tests under `test/native/` cover:
  - growing, wrapping, and map deletion with tombstones;
  - pool reuse: freeing and allocating again requests no chunk;
  - arenas that are reset request no chunk;
  - accounting that balances to zero.
- `mise run build` compiles every `src/**/*.c` together with the core
  amalgamation.

Exit criterion: `version` loads on Node and Bun, and the native allocator
and container tests pass.

### 2. Own a system

- `system.{c,h}`: `createSystem()` builds a `ta_system` with the pool
  allocator as core's allocator. `close()` is idempotent and authoritative,
  and returns every chunk. The environment cleanup hook closes any system
  still open. `stats()` reports the counters per lifetime and size class.
- `lib/errors.js` and `lib/system.js`.

Exit criterion: many systems can be created, closed and finalized in any
order without leaks (the bytes in use return to zero before `close()`
returns the chunks).

### 3. Generate the control plane

Needs core ADR 0059.

- `mise/lib/napi/{api,plan,policy,emit}.rb`, `mise/tasks/bindings` and
  `bindings.rb`. Every function and type in the API description is
  classified, and the generator fails otherwise.
- `runtime.{c,h}`:
  - primitive codecs, and the `(slot, generation)` codec for
    `ta_actor_id`, which takes two positional numbers and never reads
    properties of a view object;
  - enum symbols for result codes;
  - the reentrancy guard;
  - handle wrapping, with finalizers that call the release function the
    description names.
- Commit the first `src/gen/ta_napi.{c,h}`.
- `mise run test` regenerates into a temporary directory and fails on any
  difference.

Exit criterion:
- a bound function (for example `ta_system_time`) is callable through the
  internal module;
- a plumbing-owned function (for example `ta_actor_deliver`) is absent;
- editing `bindings.rb` without regenerating fails `mise run test`.

### 4. Define charts

Needs core ADR 0057; core ADR 0058 improves error messages.

- `lib/statechart.js` emits the generated spec shape.
- `System.define()` calls the generated `ta_definition_compile_spec` and
  `ta_system_register_definition` with `ta_null_datamodel()`.
- A refused chart throws a `TinyactorsError` that carries core's
  `ta_compile_error` (core ADR 0058): its code, path, subject and message.
  The call arena is passed as `error_arena`, and the error is copied into
  JavaScript before the arena is reset.
  - The `error` member of `ta_definition_compile_params` has no
    `TA_VALID_ON` annotation, so a generated binding writes nothing for it
    when the compile fails. The test host has the same gap (core ADR 0057,
    implementation notes). Fix the annotation in core, in place, before
    this step. Do not override the binding in the addon.
- `lib/definition.js` reads `identity`, `inspect()` and `retired` through
  generated calls. `retire()` is bound.

Exit criterion:
- the charts from the old `test/definition.js` compile;
- malformed specs fail with the generated codec's error path;
- under the null data model, a chart with any expression other than
  `In(state)` is refused when it is defined, with
  `TA_COMPILE_ERROR_EXPRESSION_REJECTED` and the expression's path;
  `In()` used as a value compiles, and faults as `error.execution` when it
  runs (core ADR 0057);
- nothing about a definition is stored in the addon.

### 5. Spawn, run, inspect, destroy

Needs core ADRs 0056 and 0061.

- `lib/actor.js`: the `ActorId` view.
- Spawn, run and destroy:
  - the scheduling table (ADR 0005), in pages from the pools;
  - the spawn hook fills the actor's entry and puts it on the runnable
    queue;
  - `scheduler.{c,h}` runs the runnable part of one turn, with a handle
    scope around each resume;
  - each resume passes two budgets: `budget` counts microsteps, and
    `action_budget` counts units of executable content (core ADR 0061). The
    system options expose them as `resumeBudget` and `actionBudget`. The
    action budget is what bounds one resume when a single microstep runs a
    long script or a `<foreach>` over a large array;
  - an actor paused by either budget answers `TA_RUN_PAUSED` and stays
    `runnable` (ADR 0005);
  - `pump.{c,h}` schedules coalesced turns with `setImmediate` from native
    code, and implements `flush()` with native promises;
  - hook records go into the turn arena;
  - the destroy hook clears the actor's entry.
- `inspect()` goes through the generated `ta_actor_inspect`. `finalState`
  comes from the inspection's `final_state` (core ADR 0056), named through
  `ta_definition_state()`. It is `null` while the actor runs, while it is
  paused inside a microstep, and for a cancelled invoked child. The addon
  keeps no copy of a chart's final states.
- The `done` hook reads the final state and completion the same way,
  before a `finished: "destroy"` actor is destroyed.
- The `done` hook and the `finished: "destroy" | "retain"` option
  (ADR 0012).

Exit criterion:
- spawned actors reach their initial state after `await flush()`, and
  nothing runs synchronously inside `spawn()` (checked on both runtimes;
  ADR 0007);
- while a large population bootstraps over many turns, an I/O callback and
  a timer callback run between two consecutive turns (on both runtimes);
- a chart with two top-level finals reports the one that was actually
  reached;
- with a small `actionBudget`, one entry handler with a long `<foreach>`
  runs over several turns, and other actors run between them;
- with `finished: "destroy"`, finished actors are gone after `done`.

### 6. Post events through mailboxes

Needs core ADR 0055.

- `events.{c,h}` and `lib/events.js` (ADR 0004).
- `post()`, mailboxes of pooled envelopes and the ready queue (ADR 0005):
  - mail for a stale generation is a dead letter at enqueue;
  - delivery acts on `ta_actor_deliver`'s result alone. Core checks in
    this order: `TA_ERR_INVALID` for a malformed event, `TA_ERR_NOT_FOUND`,
    `TA_ERR_DONE`, then `TA_ERR_BUSY` (core ADR 0055). An actor paused by
    an action budget is inside its macrostep and answers `TA_ERR_BUSY`,
    which the scheduling table already rules out because that actor is
    `runnable`;
  - `TA_ERR_DONE` and `TA_ERR_NOT_FOUND` go to `deadLetter`, at most
    `messageBudget` records per turn;
  - other failures quarantine the actor in its entry and call `fault`
    (ADR 0008).
- `maxQueuedMessages` backpressure.

Exit criterion:
- per-target FIFO holds even when a turn stops at its message budget;
- one faulting actor does not stop the others;
- a quarantined actor with an empty mailbox stays out of scheduling when
  new mail arrives, and `release()` brings it back;
- mail posted to an actor between `spawn` and its bootstrap is delivered
  after the bootstrap, with no `TA_ERR_BUSY`;
- a stale ID's mail never reaches the slot's next actor;
- posting a name that was seen before does no string work;
- after warm-up, posting and delivering messages without data requests no
  chunk (ADR 0014).

### 7. Route actor-to-actor events

- `router.{c,h}` and `directory.{c,h}`, installed at system creation
  (ADR 0006), with the interned address table.
- Named spawn and `findActor()`.
- Literal `<send>` between actors.
- The router's `submit` only enqueues. It never destroys or unloads an
  actor. Core relies on this: a due send that the router refuses goes back
  to its owner's list without allocating only while the owner is still
  there (core ADR 0054, implementation notes).

Exit criterion:
- a ping-pong pair exchanges N messages with no JavaScript involvement
  between `post` and `flush`;
- sends to a named address follow the directory;
- an external send to self is delivered in a later macrostep, with no
  `TA_ERR_BUSY`;
- after warm-up, a named ping-pong pair requests no chunk and copies no
  address.

### 8. Time

- Manual clock: `advanceTo()` and `advanceBy()`.
- Realtime clock in `pump.{c,h}`:
  - time comes from a native monotonic clock and is set at the start of
    every turn;
  - one timer is armed from native code with the global `setTimeout`,
    based on `ta_system_next_deadline`, and unreferenced so it does not
    keep the process alive;
  - the timer is re-armed only for an earlier deadline.
- Due sends are dispatched in the pump turn.
- Inspection shows `delayedSends` with `submitted` and `destination`
  (core ADR 0054). For a custom I/O processor, core leaves `destination`
  as a zeroed runtime ref (ID 0). JavaScript sees `null` there, and the
  send's `target`.

Exit criterion:
- delayed sends fire in deadline order under both clocks;
- cancelled sends never fire;
- under the realtime clock, a send armed by the first message after an
  idle period fires no earlier than its delay;
- an idle realtime system does not keep the process alive.

### 9. Enumerate

Needs core ADR 0062.

- `enumerate.{c,h}`: `system.actors()` and `system.actorIds()` as pages
  built in the call arena (ADR 0003). Each page is one call of
  `ta_system_visit_actors` with a `ta_actor_visit_params`:
  - `from` is the position the previous page returned in `next`, or 0 for
    the first page. `limit` is the page size;
  - the iteration ends when `next` is `TA_ACTOR_VISIT_END`. A last page
    that is exactly full takes one more call, which visits nothing;
  - the iterator keeps the position as an opaque number and only passes
    back a `next` that core gave it;
  - if a visit fails (for example, the page cannot copy for want of
    memory), `next` is the actor that failed, so the page can be retried
    without skipping it.
- Pages follow core ADR 0062's rules for changes between pages: an actor
  resident for the whole iteration is visited exactly once.

Exit criterion:
- iterating over a large population keeps peak JavaScript heap bounded by
  the page size;
- the last page of a large population costs about the same as the first,
  so a whole iteration is linear;
- an actor spawned or destroyed between pages follows core ADR 0062 §4.

### 10. Prove the scale properties

This step sets a baseline with charts that have no data. Steps 11 and 13
run it again with data and I/O, which is the load of one actor per
websocket connection.

- `test/scale.js`: a population benchmark in the style of tinyactors-test's
  `scale_test.c`. That test now measures 50,000, 200,000, 1,000,000 and
  4,000,000 actors, and core measures about 560 bytes per data-less actor.
  `scale.js` uses the same populations and the same
  `TAT_SCALE_MAX_POPULATION` limit for machines with less memory. It checks
  that each of the following stays flat per actor from one population to
  the next: spawn, post plus delivery, inspect, one enumeration page, and
  heap bytes. It measures CPU time, not wall time.
- A steady-state check: after warm-up, a round of posts and deliveries
  requests no chunk (ADR 0014).
- The end-to-end many-actor test from the old roadmap (spawn N, post `work`
  to all, flush once, all done), with N from the same populations.
- At the largest population measured, the test reports:
  - the longest pump turn and the largest event-loop delay;
  - RSS, V8 heap and native bytes per actor;
  - total GC pause time.

Exit criterion: every ratio stays within its bound on Node and Bun, and
steady state requests no chunk. No test asserts an absolute duration. The
target-population numbers are recorded for comparison, not asserted.

### 11. The JavaScript data model

- `values.{c,h}` (the handle table and value boxes from the pools) and
  `napi_datamodel.{c,h}`:
  - data declarations;
  - conditions, values, locations, assignments and scripts as
    JavaScript functions;
  - event data;
  - completion data.
- The reentrancy guard is exercised by expressions that try to call core.

Exit criterion:
- the ping-pong player chart in `docs/design.md` runs;
- the number of value boxes returns to zero after `close()`;
- a system holds one `napi_ref` for values, whatever the number of live
  values;
- `test/scale.js` runs again with a chart that has data: its ratios stay
  within their bounds, steady state with data requests no chunk, and the
  target-population numbers are recorded.

### 12. `send()` and backpressure

- `send()` promises with acceptance and completion modes, abort signals and
  timeouts.
- A retained-bytes limit.

Exit criterion: pending promises settle on completion, on abort, and on
`close()`.

### 13. Services

- `adapters/io`, `adapters/invoker` (JavaScript invokers and the in-system
  SCXML invoker) and `adapters/log`.
- JavaScript-facing `registerIo`, `registerInvoker` and `setLogSink`.

Exit criterion:
- an asynchronous I/O processor completes by enqueuing an event;
- an invoked child chart reports `done.invoke` to its parent;
- `test/scale.js` runs a websocket-shaped load: one actor per simulated
  connection, with data, an inbound post and an outbound I/O send per
  message, and an idle timeout that is re-armed on each message. Its
  ratios stay within their bounds, and the target-population numbers are
  recorded.

### 14. Persistence and replacement

- Capture, unload, load and reconstruct, through generated calls with
  hooks.
- `prepareReplacement()` and `activate()`.

Exit criterion: an actor with pending delayed sends round-trips through
capture and load, and a named actor is replaced in place with no lost
messages.

### Later

- Port the tinyactors-test scenario suite to run against the addon.
- Consider publishing a filtered raw core module (it would need an ADR, see
  ADR 0013).
- Add bulk mutation only when a measurement justifies it (ADR 0003).
