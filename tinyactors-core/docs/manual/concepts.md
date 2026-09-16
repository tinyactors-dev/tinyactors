# Core concepts

## System

A `ta_system` owns the runtime tables shared by a group of definitions and actors: atoms, structured event names, registered definitions, I/O processors, invokers, logical time, actor residency metadata, and the scheduler index for delayed sends. It does **not** own an OS thread, event loop, database, or object store.

## Definition

A definition is an immutable compiled statechart revision. It describes hierarchy, parallel regions, transitions, executable actions, history, and the data-model interface. Its persistent identity is a logical family plus an exact revision fingerprint. Multiple revisions of one family may coexist while old sessions still run.

## Actor

An actor combines a Tinyactors interpreter session with a host-owned data-model binding. Tinyactors owns the active state configuration, history, invocations, delayed sends, and—when execution is paused in the middle of a macrostep—the continuation/current event/internal queue. The application or language runtime owns the bound object graph.

Actors have a numeric runtime identity. A named actor additionally has a logical address scoped by definition family; the address survives replacement of one interpreter instance with another.

## Session image

A session image is an owned in-memory capture of Tinyactors interpreter state. It can reconstruct the same session against the exact same definition revision when the host supplies the data-model binding. It is not a portable persistence format and does not copy a Python/Ruby/JavaScript/native object graph.

## Event

An event name is a structured sequence of dot-separated segments, not an opaque string internally. A transition listening for `error` matches `error`, `error.execution`, and `error.communication.send`, because SCXML matching is token-prefix matching.

## Macrostep and pause

SCXML run-to-completion is a semantic rule: an actor does not begin another external event until the current macrostep reaches a stable configuration. It does **not** require a C call to run without interruption. Tinyactors may pause, inspect, capture, and resume execution at explicit interpreter boundaries.

## Parallel states

A statechart configuration is a set of active states. Under a parallel state, multiple atomic descendants are simultaneously active. Tinyactors therefore never models the generic actor configuration as a single current-state integer.
