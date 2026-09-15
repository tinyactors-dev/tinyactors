# Core concepts

## System

A `ta_system` owns the runtime tables shared by a group of definitions and actors: atoms, structured event names, registered definitions, I/O processors, invokers, logical time, actor residency metadata, and the scheduler index for delayed sends. It does **not** own an OS thread or event loop.

## Definition

A definition is an immutable compiled statechart. It describes hierarchy, parallel regions, transitions, executable actions, history, and the data-model interface. Definitions are named and may be shared by any number of actors.

## Actor

An actor is a running instance of a definition. Its semantic state includes its active state configuration, history, data model state, active invocations, delayed sends, and—when execution is paused in the middle of a macrostep—its continuation, current external event, and internal event queue.

Actors have a numeric runtime identity. A durable business entity may additionally have a name scoped by definition. Transient actors such as WebSocket connection actors may remain anonymous.

## Event

An event name is a structured sequence of dot-separated segments, not an opaque string internally. A transition listening for `error` matches `error`, `error.execution`, and `error.communication.send`, because SCXML matching is token-prefix matching.

## Macrostep and pause

SCXML run-to-completion is a semantic rule: an actor does not begin another external event until the current macrostep reaches a stable configuration. It does **not** require a C call to run without interruption. TA may pause and resume execution at internal interpreter boundaries.

## Parallel states

A statechart configuration is a set of active states. Under a parallel state, multiple atomic descendants are simultaneously active. TA therefore never models the generic actor configuration as a single current-state integer.
