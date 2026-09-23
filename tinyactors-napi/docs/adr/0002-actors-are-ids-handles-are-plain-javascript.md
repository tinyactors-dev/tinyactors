# ADR 0002: Actors are IDs; handles are plain JavaScript

**Status:** Accepted
**Date:** 2026-09-23

## Context

The first addon made every spawned actor a native-wrapped JavaScript
object. That meant one `napi_new_instance` (a constructor call), a
`napi_wrap` with a finalizer, and two strong `napi_ref`s (to the system and
the definition). That is about three V8 global handles per actor, or
about 12 million at 4 million actors. V8 visits global handles on every
garbage collection, so GC time grows with the population and dominates long
before core's per-actor cost matters. It also kept state (`destroyed`) in
the wrapper that core already knows.

## Decision

1. **The identity of an actor is its `ta_actor_id`.** Core already packs a
   slot and a generation into it, and a stale ID fails with
   `TA_ERR_NOT_FOUND`. JavaScript sees it as `ActorId`, a branded `bigint`.
   In bulk it is a `BigUint64Array` (ADR 0003).
2. **The addon keeps no per-actor native record and no per-actor Node-API
   reference.** Whether an actor exists, is done, or is destroyed is asked of
   core each time.
3. **`Actor` is a plain JavaScript class**, defined in `index.js`. It has
   two fields: the system and the ID. Its methods call system methods with
   the ID. It holds no native resource, needs no finalizer, and costs what
   any small JavaScript object costs. Creating one is optional. Every API
   that takes an `Actor` also takes an `ActorId`.
4. **APIs that create actors return IDs.** `spawn()` returns an `ActorId`.
   `system.actor(id)` returns an `Actor` view when a caller wants one.
   Nothing creates a handle object that the caller did not ask for.
5. **Native wrapping is kept for objects that are few**: `System` (one per
   system) and `Definition` (one per registered definition). Their
   lifetimes follow ADR 0012.

## Consequences

- Spawning four million actors creates no JavaScript objects beyond the
  returned IDs, or none at all with the bulk API.
- Two `Actor` views of one ID are two different JavaScript objects. Code
  compares actors by `id`, not by `===`.
- A view of a destroyed actor is harmless: its calls fail with a
  `not-found` error from core.
