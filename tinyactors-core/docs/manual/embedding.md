# Embedding Tinyactors

Tinyactors follows a SQLite-like ownership model: calls run in the caller's thread; the library does not create workers, sockets, files, databases, or event loops for you.

A typical host loop is conceptually:

```c
/* host chooses an event */
ta_actor_deliver(system, actor, &event);

for (;;) {
    ta_run_status status;
    ta_result r = ta_actor_resume(system, actor, budget, &status);
    if (r != TA_OK)
        break;
    if (status != TA_RUN_PAUSED)
        break;
}
```

Statechart-level failures are normally represented as internal `error.*` events, not as `ta_result` failures.

## Data-model binding

Actor creation supplies a host data-model binding. In CPython that can be a wrapper around an already-existing `PyObject *`; in Ruby/Node the analogous host object/reference. Tinyactors does not copy that graph into per-actor storage. The adapter decides how expressions, assignment, and inspection operate on it.

## Concurrency

Tinyactors does not serialize calls for you. Treat a `ta_system` as externally synchronized unless an API explicitly states otherwise. A simple embedding uses one thread. A server can shard actors across multiple independent systems or place an external lock/ownership discipline around each system.

An actor processes one external event at a time. If an actor is paused inside a macrostep, the host must not deliver another external event to it.

## Node, CPython, CRuby, WASM, ESP32

The same synchronous C ABI works in all of these environments. Host callbacks may call directly into Node/Python/Ruby when execution is already on the host's permitted interpreter thread. If the embedder chooses native worker threads, reacquiring the host runtime's required thread/GIL/GVL/event-loop context is the embedder's responsibility.

In browser WASM, a host callback can initiate asynchronous JS work and return immediately; JavaScript later re-enters Tinyactors to deliver the resulting event.
