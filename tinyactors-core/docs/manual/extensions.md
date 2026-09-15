# Values, data models, I/O, and invocation

Tinyactors deliberately permits rich host integration.

## Values

`ta_value` is a fat pointer: host object pointer plus vtable. A binding can wrap `PyObject *`, Ruby `VALUE`, N-API references, native C objects, or compact embedded values. The wrapper owns the host-specific lifetime, reference counting, cloning, and serialization rules.

## Data models

A `ta_datamodel` interprets conditions, value expressions, locations, assignments, and scripts. The core does not impose an expression language. Data models may perform arbitrary host work; disciplined embedders should route externally visible effects through I/O processors when they want replayable and testable systems.

## I/O processors

An I/O processor callback is synchronous as a C call, but the operation it initiates may be asynchronous. For example, a Node callback may start `fetch()` and return; the promise completion later delivers another event to the actor.

Delayed-send timing belongs to Tinyactors statechart semantics. The I/O processor receives a fully materialized send only when it is due.

## Invoked services

Invokers start, cancel, and forward events to invoked services. A native SCXML/statechart invoker may map invocation directly to spawning a child actor. Other invokers may wrap host services. Completion is represented by structured `done.invoke.<id>` events.
