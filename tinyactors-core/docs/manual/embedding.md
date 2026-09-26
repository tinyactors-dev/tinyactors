# Embedding Tinyactors

Tinyactors follows a SQLite-like ownership model: calls run in the caller's thread; the library does not create workers, sockets, files, databases, or event loops for you.

Core ships the standard SCXML Event I/O Processor. The embedder installs an
external-event router that schedules its normalized inter-session deliveries.
On a single thread this may be a small work queue; on a server it may select a
shard and load an unloaded actor. Custom I/O processors remain for non-SCXML
destinations such as HTTP, client streams, and application storage.

A typical host loop is conceptually:

```c
/* host chooses an event */
ta_actor_deliver(system, actor, &event);

for (;;) {
    ta_run_status status;
    ta_result r = ta_actor_resume(system, &(ta_actor_resume_params){
        .actor = actor,
        .budget = budget,
        .status = &status,
    });
    if (r != TA_OK)
        break;
    if (status != TA_RUN_PAUSED)
        break;
}
```

A named actor may instead resolve as `TA_ERR_UNLOADED`. The host retrieves its
checkpoint and data-model binding, calls `ta_actor_load()`, and retries delivery
with the returned runtime actor ID. `virtual-actors.md` shows this path.

Statechart-level failures are normally represented as internal `error.*` events, not as `ta_result` failures.

## Data-model binding

Actor creation supplies a host data-model binding. In CPython that can be a wrapper around an already-existing `PyObject *`; in Ruby/Node the analogous host object/reference. Tinyactors does not copy that graph into per-actor storage. The adapter decides how expressions, assignment, and inspection operate on it.

## Concurrency

Tinyactors does not serialize calls for you. Treat a `ta_system` as externally synchronized unless an API explicitly states otherwise. A simple embedding uses one thread. A server can shard actors across multiple independent systems or place an external lock/ownership discipline around each system.

An actor processes one external event at a time. If an actor is paused inside a macrostep, the host must not deliver another external event to it.

## Delivering without inspecting

`ta_actor_deliver()` is the admission test (ADR 0055). A host decides what to
do with a queued message from its result alone, and does not need to call
`ta_actor_inspect()` before delivering:

| Result | Meaning | What the host does |
|---|---|---|
| `TA_OK` | The actor accepted the event | Resume the actor |
| `TA_ERR_BUSY` | The actor is inside a macrostep, possibly paused, or already holds an event | Keep the message queued for this actor |
| `TA_ERR_DONE` | The actor reached a top-level final state | Drop the message, or apply a dead-letter policy |
| `TA_ERR_NOT_FOUND` | No resident actor has this ID | Drop the message, or load the actor and retry |
| anything else | A host bug (`TA_ERR_INVALID`: a null or malformed event) or a resource failure | Report it |

A finished actor answers `TA_ERR_DONE`, never `TA_ERR_BUSY`, so a host never
keeps a message waiting for an actor that will not take it.

## Unwedging an actor

An actor stuck in a state, waiting for an event that will never come, can be
moved by an operator without the chart having been written for it
(ADR 0082). Deliver an event, named as you like, with an operator
transition:

```c
ta_state_id target = /* ta_definition_find_state(...) */;
ta_actor_deliver(system, &(ta_actor_deliver_params){
    .actor = actor,
    .event = &operator_event,          /* e.g. "operator.goto", with data */
    .transition = &(ta_operator_transition){
        .source = TA_STATE_NONE,       /* the root; or an active state, e.g. a region */
        .targets = &target,
        .target_count = 1,
    },
});
```

Core takes the event with that transition instead of the chart's: an
internal transition with no guard and no content, run by the normal engine.
Exit handlers, history, invocations, entry handlers, default states and
`done.state` events all happen as for any transition, and `_event` in the
handlers is your event, so a chart can tell a jump from a normal entry.
Targets may be history states (to put a region back where it was) and a
top-level `<final>` (to finish the session). Deliver refuses a malformed
transition, or a source that is not active, with `TA_ERR_INVALID`.

A macrostep that never becomes stable, such as eventless transitions that
cycle forever, keeps the actor busy, so a jump is refused too.
`ta_actor_abandon(system, actor)` ends it: the internal queue and the
macrostep's event are discarded, and the actor is idle with the
configuration after its last completed microstep. What the macrostep
already did stays done. It is allowed only between microsteps: an actor
paused inside one answers `TA_ERR_BUSY`, and resuming it with a budget of 1
and no action budget brings it there. Then deliver the jump.

The trace shows both: the jump's `MICROSTEP` record has no transitions and
carries the targets in `states`, and abandoning writes `ABANDONED` with the
number of internal events dropped, then `MACROSTEP_END`.

## Node, CPython, CRuby, WASM, ESP32

The same synchronous C ABI works in all of these environments. Host callbacks may call directly into Node/Python/Ruby when execution is already on the host's permitted interpreter thread. If the embedder chooses native worker threads, reacquiring the host runtime's required thread/GIL/GVL/event-loop context is the embedder's responsibility.

In browser WASM, a host callback can initiate asynchronous JS work and return immediately; JavaScript later re-enters Tinyactors to deliver the resulting event.
