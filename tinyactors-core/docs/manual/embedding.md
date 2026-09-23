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

## Node, CPython, CRuby, WASM, ESP32

The same synchronous C ABI works in all of these environments. Host callbacks may call directly into Node/Python/Ruby when execution is already on the host's permitted interpreter thread. If the embedder chooses native worker threads, reacquiring the host runtime's required thread/GIL/GVL/event-loop context is the embedder's responsibility.

In browser WASM, a host callback can initiate asynchronous JS work and return immediately; JavaScript later re-enters Tinyactors to deliver the resulting event.
