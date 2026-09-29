# ADR 0037: Core-owned actor protocol; host-backed directory

- Status: Accepted; reconstruction details refined by ADR 0038; amended by ADR 0089 (a prepared actor needs no incumbent, and replays a host's logged inputs off-route; core still keeps no log); amended by ADR 0090 (core defines the input log's records, their byte layout and their replay, and still stores nothing) and ADR 0091 (checkpoints in that layout)
- Date: 2026-09-15

## Context

Named business actors must be routable independently of residency and must survive an application replacing one actor session with another. At the same time, the directory may be an in-memory map, a database, or a distributed router, and therefore cannot be a fixed `ta_system` data structure.

An actor may also be ephemeral. Runtime `ta_actor_id` is the correct compact reply address for messages exchanged solely by live actors, but it is not a durable named route.

## Decision

Tinyactors defines two actor-reference forms:

- `ta_actor_id` is a runtime-local reference for ephemeral/live actors.
- `ta_actor_address` is a logical `(definition family, name)` route for named actors.

The core owns the named-actor protocol: claim, resolve, replace, retire, actor-to-actor origin construction, and SCXML communication-error handling. The host implements `ta_actor_directory` operations and chooses the backing data structure, synchronization, loading, and physical routing.

The core also owns capture/reconstruction of interpreter session images (ADR 0038). Storage, portable checkpoint encoding, eviction policy, command sourcing, and replay remain outside the core.

Applications upgrade incompatible business logic by replacement: fence the old route, create a new actor under the target definition bound to the existing host-owned data-model object (or an application-selected replacement binding), bootstrap it with ordinary SCXML behavior, atomically replace the directory entry, then retire the old actor. Bootstrap must not produce externally visible effects before activation.

## Consequences

`ta_event.origin` and `ta_send.source` carry a tagged actor reference rather than assuming every actor origin is a runtime ID. A late reply to a retired named address is handled by host routing policy, while a stale runtime ID is rejected by its runtime generation.

There is no core migration language, replay mode, event-log requirement, or universal effect/outbox subsystem. I/O processors and invokers remain the host boundary for normal SCXML execution. A separate durability module may add stronger protocols without changing core actor identity/address semantics.
