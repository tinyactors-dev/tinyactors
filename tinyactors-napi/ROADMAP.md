# Tinyactors N-API implementation roadmap

The first useful milestone is a narrow vertical slice:

```text
createSystem -> define a minimal chart -> spawn many actors
             -> post events -> flush -> inspect -> close
```

The milestone deliberately excludes JavaScript data, callbacks, realtime
timers, persistence, replacement, I/O processors, invokers, and the fluent
builder. Those features depend on the ownership and scheduling foundation
proved by the first end-to-end test.

Each step below must leave `main` working and should be committed separately.

## 1. Link core and establish binding infrastructure

- Compile and link `tinyactors-core/include/tinyactors.c` into the addon.
- Preserve the existing `version` export and Node/Bun loading tests.
- Add shared helpers for N-API status checks, Tinyactors result translation,
  UTF-8 strings, and `uint64_t`/JavaScript `bigint` conversion.
- Add the allocator used by `ta_system_create`.

Exit criterion: both runtimes load an addon linked with Tinyactors core, and
the existing version test passes.

## 2. Implement System ownership and teardown

- Export `createSystem({ clock: "manual", initialTime? })`.
- Wrap one `ta_system` in a native `System` object.
- Implement `closed`, `clock`, `time`, and idempotent `close()`.
- Add an environment cleanup hook.
- Make system teardown authoritative: subordinate native wrappers must detect
  that their system is closed instead of accessing freed core state.
- Translate binding misuse and `ta_result` failures to `TinyactorsError`.

Exit criterion: Node and Bun can create, inspect, close, and safely finalize
multiple independent systems.

## 3. Compile a minimal normalized DefinitionSpec

- Add a private no-data data-model adapter suitable for charts without data,
  expressions, scripts, guards, or value-bearing actions.
- Implement `System.define(spec)` for this initial subset:
  - `family` and `revision`;
  - top-level `initial`;
  - atomic and final states;
  - event transitions and transition targets.
- Build a temporary `ta_scxml_spec`, compile it, and register the resulting
  definition with the system.
- Return a native Definition wrapper exposing `system`, `id`, `identity`, and
  `retired`; implement `retire()` if it is cheap to include here.
- Do not implement the fluent builder in C. A later JavaScript builder should
  produce the normalized object accepted by this boundary.

Exit criterion: valid minimal charts compile in Node and Bun, while malformed
specifications fail deterministically without leaks.

## 4. Spawn, resume, inspect, and destroy actors

- Implement `System.spawn(definition)` for anonymous actors.
- Implement the initial Actor surface: `system`, `id`, `definition`,
  `destroyed`, `destroy()`, and the inspection fields required by the first
  test.
- Maintain a runnable-actor queue.
- Bootstrap every new actor by resuming it with a finite budget until it is
  stable, done, or waiting for a later pump turn.
- Preserve paused execution; never assume one `ta_actor_resume` call completes
  a macrostep.

Exit criterion: many actors can be spawned from one shared definition, reach
their initial stable state, be inspected, and be destroyed or closed with the
system.

## 5. Queue external events and implement flush

- Add a sparse external-envelope queue separate from the runnable queue.
- Implement `System.post()` and `Actor.post()` for Actor and ActorId targets,
  event names, and omitted/`undefined` data.
- Copy/own queued event information before returning from `post()`.
- Deliver only to stable actors; paused actors remain on the runnable queue.
- Implement `flush()` as the quiescence boundary. The first implementation may
  drain synchronously and return a resolved Promise, but its internal pump must
  honor finite resume and message budgets.
- Enforce `maxQueuedMessages` and reject work after close.

Exit criterion: an event can move an arbitrary number of actors through a
complete macrostep, and `await system.flush()` observes stable or done actors.

## 6. Add the many-actor end-to-end test

Use a manual-clock chart with two states:

```js
const definition = system.define({
  family: "worker",
  revision: "v1",
  initial: { target: "idle" },
  states: [
    {
      id: "idle",
      transitions: [{ event: "work", target: "finished" }],
    },
    { id: "finished", kind: "final" },
  ],
});
```

The test must:

1. create a manual-clock system;
2. define the chart once;
3. spawn a population of actors;
4. flush bootstrap work;
5. post `work` to every actor without awaiting between posts;
6. flush once;
7. verify every actor is done and IDs are unique;
8. close the system and verify closed-handle behavior.

Run a modest population in the normal Node and Bun suites. Allow a larger
population through an environment variable for stress runs. Do not assert an
absolute duration; performance invariants belong in a separate population
ratio benchmark.

## After the first end-to-end milestone

Continue in this dependency order:

1. named actor directory, addresses, and `findActor()`;
2. external routing and literal send actions;
3. reference-counted JavaScript value boxes backed by `napi_ref`;
4. JavaScript data-model instances, bindings, expressions, locations, guards,
   assignments, and scripts;
5. bounded asynchronous event-loop pumping and realtime timers;
6. `send()` promises, abort signals, timeouts, and backpressure;
7. delayed sends and manual-clock advancement;
8. logging, I/O processors, and invokers;
9. capture/load/reconstruct and definition replacement;
10. the remaining inspection APIs and JavaScript fluent authoring helpers.

The JSON-RPC binding generator in `tinyactors-test` is useful as a reference
for C API ownership and codecs, but it should not be ported into the addon.
The N-API boundary needs direct JavaScript object decoding and explicit
JavaScript callback lifetime management.
