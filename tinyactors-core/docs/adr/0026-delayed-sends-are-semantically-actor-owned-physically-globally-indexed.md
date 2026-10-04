# ADR 0026: Delayed sends are semantically actor-owned, physically globally indexed

**Status:** Accepted; durability details refined by ADR 0038. Refined by ADR 0101 (2026-10-04, implementation pending): a delayed send is an envelope in the pool, in a deadline index and on its owner's list.  
**Date:** 2026-09-15

## Context

An actor may have delayed sends due far in the future. Keeping timers only in actor-local structures would require scanning actors to discover due work.

## Decision

A delayed send belongs semantically to its originating actor/session, but the running system may index all delayed sends in a global timer heap/wheel for efficient deadline lookup. A core session image preserves the actor-owned delayed-send semantics needed to reconstruct that interpreter session.

Portable deadline storage for non-resident/process-independent recovery belongs to an optional durability layer. That layer may normalize deadlines into a separate index as long as ownership and stable effect/send identity are preserved.

## Consequences

Timers can be located/cancelled by owning session. Destroying an actor cancels its sends. Capturing/reconstructing a session preserves them without requiring Tinyactors core to define a database transaction or durable timer store.
