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
  - scheduling: mailboxes, ready and runnable queues, and the pump;
  - the router and the actor directory;
  - the clock;
  - adapters for core's callback interfaces;
  - the JavaScript data model and value boxes.
- **Actors cross the boundary as `(slot, generation)` pairs of small
  integers** (ADR 0002). Nothing is allocated per actor on the JavaScript
  side.
- **Singular calls** (ADR 0003). The only batch path is paged enumeration.

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
│   ├── pump.js                  MessageChannel wake-up, deadline timer, flush/advance waiters (ADR 0007)
│   ├── hooks.js                 fault, deadLetter and done hook dispatch (ADRs 0008, 0012)
│   └── errors.js                TinyactorsError; codes from the generated result table (ADR 0008)
├── src/
│   ├── gen/
│   │   ├── ta_napi.c            GENERATED, checked in; never edited by hand
│   │   └── ta_napi.h            GENERATED, checked in
│   ├── addon.c                  module init: registers classes and the generated module; nothing else
│   ├── runtime.{c,h}            support for generated code: primitive and (slot, generation) codecs,
│   │                            result -> TinyactorsError, reentrancy guard, handle wrapping (ADR 0013)
│   ├── system.{c,h}             native System: owns ta_system and all plumbing, close, stats (ADR 0012)
│   ├── containers.{c,h}         vec, ring, map, slice (ADR 0009)
│   ├── allocator.{c,h}          per-system counting allocator, shared with core (ADR 0009)
│   ├── scheduler.{c,h}          mailboxes, ready queue, runnable queue, deliver/resume, one pump turn,
│   │                            per-actor faults and dead letters (ADRs 0005, 0007, 0008)
│   ├── hooks.{c,h}              post-call hooks for spawn/load/reconstruct/prepare/destroy/unload (ADR 0013)
│   ├── router.{c,h}             external-event router into mailboxes (ADR 0006)
│   ├── directory.{c,h}          in-memory actor directory (ADR 0006)
│   ├── events.{c,h}             dotted event-name interning and formatting (ADR 0004)
│   ├── values.{c,h}             reference-counted JavaScript value boxes (ADR 0011)
│   ├── napi_datamodel.{c,h}     the JavaScript data model (ADR 0011)
│   ├── enumerate.{c,h}          paged actor enumeration (ADR 0003)
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
│   ├── native/                  C unit tests (containers, scheduler invariants), built by mise run test
│   ├── *.js                     one suite per area (system, definition, actor, post, router, time, ...)
│   └── scale.js                 population ratio benchmark: per-actor cost stays flat (step 10)
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
| 0055: deliver is the admission test (`TA_ERR_DONE`) | step 6 | in progress |
| 0056: a finished actor reports its final state | step 5 | in progress |
| 0057: core ships the null data model | step 4 | in progress |
| 0058: compile errors say what and where | step 4 (error quality only) | in progress |
| 0059: an actor ID is a public slot and generation | step 3 | in progress |

## Steps

Each step leaves `main` building and passing `mise run test`, and is
committed separately, in several commits where that helps. Every test runs on
both Node and Bun.

### 1. Reset the package

- Delete `src/addon.c`, `src/binding.h`, `src/null_datamodel.{c,h}` and the
  tests that exercise them. Keep `index.d.ts` as the target API.
- Create the tree above with empty modules. `addon.c` exports only
  `version`.
- Add `containers.{c,h}` and `allocator.{c,h}`, with native unit tests under
  `test/native/`. These cover growing, wrapping, map deletion with
  tombstones, and allocator accounting balancing to zero.
- `mise run build` compiles every `src/**/*.c` together with the core
  amalgamation.

Exit criterion: `version` loads on Node and Bun, and the native container
tests pass.

### 2. Own a system

- `system.{c,h}`: `createSystem()` builds a `ta_system` with the counting
  allocator. `close()` is idempotent and authoritative, the environment
  cleanup hook closes any system still open, and `stats()` reports the
  allocator's counters.
- `lib/errors.js` and `lib/system.js`.

Exit criterion: many systems can be created, closed and finalized in any
order without leaks (`stats()` returns to zero before `close()` frees
core).

### 3. Generate the control plane

Needs core ADR 0059.

- `mise/lib/napi/{api,plan,policy,emit}.rb`, `mise/tasks/bindings` and
  `bindings.rb`. Every function and type in the API description is
  classified, and the generator fails otherwise.
- `runtime.{c,h}`:
  - primitive codecs, and the `(slot, generation)` codec for
    `ta_actor_id`;
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
- `lib/definition.js` reads `identity`, `inspect()` and `retired` through
  generated calls. `retire()` is bound.

Exit criterion:
- the charts from the old `test/definition.js` compile;
- malformed specs fail with the generated codec's error path;
- nothing about a definition is stored in the addon.

### 5. Spawn, run, inspect, destroy

Needs core ADR 0056.

- `lib/actor.js`: the `ActorId` view.
- Spawn, run and destroy:
  - the spawn hook puts the actor on the runnable queue;
  - `scheduler.{c,h}` runs the runnable part of one pump turn;
  - `lib/pump.js` drives turns with a coalesced `MessageChannel` token and
    implements `flush()`;
  - the destroy hook.
- `inspect()` goes through the generated `ta_actor_inspect`, including
  `finalState` for finished actors.
- The `done` hook and the `finished: "destroy" | "retain"` option
  (ADR 0012).

Exit criterion:
- spawned actors reach their initial state after `await flush()`, and
  nothing runs synchronously inside `spawn()`;
- a chart with two top-level finals reports the one that was actually
  reached;
- with `finished: "destroy"`, finished actors are gone after `done`.

### 6. Post events through mailboxes

Needs core ADR 0055.

- `events.{c,h}` and `lib/events.js` (ADR 0004).
- `post()`, per-target mailboxes and the ready queue (ADR 0005):
  - delivery acts on `ta_actor_deliver`'s result alone;
  - `TA_ERR_DONE` and `TA_ERR_NOT_FOUND` go to `deadLetter`;
  - other failures quarantine the actor and call `fault` (ADR 0008).
- `maxQueuedMessages` backpressure.

Exit criterion:
- per-target FIFO holds even when a turn stops at its message budget;
- one faulting actor does not stop the others;
- posting a name that was seen before does no string work;
- a post allocates nothing when there is no data.

### 7. Route actor-to-actor events

- `router.{c,h}` and `directory.{c,h}`, installed at system creation
  (ADR 0006).
- Named spawn and `findActor()`.
- Literal `<send>` between actors.

Exit criterion: a ping-pong pair exchanges N messages with no JavaScript
involvement between `post` and `flush`, and sends to a named address follow
the directory.

### 8. Time

- Manual clock: `advanceTo()` and `advanceBy()`.
- Realtime clock: one JavaScript timer armed from
  `ta_system_next_deadline`, unreferenced so it does not keep the process
  alive.
- Due sends are dispatched in the pump turn.
- Inspection shows `delayedSends` with `submitted` and `destination`.

Exit criterion: delayed sends fire in deadline order under both clocks,
cancelled sends never fire, and an idle realtime system does not keep the
process alive.

### 9. Enumerate

- `enumerate.{c,h}`: `system.actors()` and `system.actorIds()` as pages
  (ADR 0003).

Exit criterion: iterating over a large population keeps peak JavaScript heap
bounded by the page size.

### 10. Prove the scale properties

- `test/scale.js`: a population ratio benchmark in the style of
  tinyactors-test's `scale_test.c`. It compares a small population with one
  four times larger and checks that each of the following stays flat per
  actor: spawn, post plus delivery, inspect, and heap bytes.
- The end-to-end many-actor test from the old roadmap (spawn N, post `work`
  to all, flush once, all done), with N taken from an environment variable
  for stress runs.

Exit criterion: every ratio stays within its bound on Node and Bun. No test
asserts an absolute duration.

### 11. The JavaScript data model

- `values.{c,h}` (value boxes) and `napi_datamodel.{c,h}`:
  - data declarations;
  - conditions, values, locations, assignments and scripts as
    JavaScript functions;
  - event data;
  - completion data.
- The reentrancy guard is exercised by expressions that try to call core.

Exit criterion: the ping-pong player chart in `docs/design.md` runs, and the
number of value boxes returns to zero after `close()`.

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

Exit criterion: an asynchronous I/O processor completes by enqueuing an
event, and an invoked child chart reports `done.invoke` to its parent.

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
