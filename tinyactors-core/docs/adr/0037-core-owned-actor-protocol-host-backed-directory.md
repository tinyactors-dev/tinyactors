# ADR 0037: Core-owned actor protocol; host-backed directory

- Status: Accepted
- Date: 2026-09-15

## Context

Named business actors must be routable while unloaded and must survive an
application replacing one actor session with another. At the same time, the
directory may be an in-memory map, a database, or a distributed router, and
therefore cannot be a fixed `ta_system` data structure.

An actor may also be ephemeral. Runtime `ta_actor_id` is the correct compact
reply address for messages exchanged solely by live actors, but it is not a
durable named route.

## Decision

Tinyactors defines two actor-reference forms:

- `ta_actor_id` is a runtime-local reference for ephemeral/live actors.
- `ta_actor_address` is a durable logical `(definition family, name)` route
  for named actors.

The core owns the named-actor protocol: claim, resolve, replace, retire,
actor-to-actor origin construction, and SCXML communication-error handling.
The host implements `ta_actor_directory` operations and chooses the backing
data structure, synchronization, loading, and physical routing.

The host persists session checkpoints and decides when to evict or load an
actor. The core owns checkpoint encoding/decoding because it owns interpreter
state; it does not own a store, transaction batch, or permanent directory.

Checkpoints identify a definition family and session schema. An implementation
build may restore any schema it declares compatible. An incompatible schema is
not implicitly transformed by the core.

Applications migrate incompatible business logic by replacement: they fence
the old named route, use host-owned data-model objects to construct migration
input, create a new actor under the target definition, bootstrap it with
ordinary SCXML behavior, persist it, and atomically replace the directory
entry. Bootstrap is application-authored and side-effect-free until activation.

## Consequences

`ta_event.origin` and `ta_send.source` carry a tagged actor reference rather
than assuming every actor origin is a runtime ID. A late reply to a retired
named address is handled by host routing policy, while a stale runtime ID is
rejected by its runtime generation.

There is no core migration language, replay mode, event-log requirement, or
universal effect/outbox subsystem. I/O processors and invokers remain the
host boundary for normal SCXML execution.
