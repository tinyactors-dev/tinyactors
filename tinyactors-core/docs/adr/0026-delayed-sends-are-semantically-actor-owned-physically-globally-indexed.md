# ADR 0026: Delayed sends are semantically actor-owned, physically globally indexed

**Status:** Accepted  
**Date:** 2026-09-15

## Context

An unloaded actor may have timers due far in the future. Keeping timers only inside its serialized blob would require scanning/loading actors to discover due work.

## Decision

A delayed send belongs semantically to its originating actor/session, but the running system may index all delayed sends in a global timer heap/wheel for efficient deadline lookup. Snapshot/storage operations must preserve the ownership relationship transactionally.

## Consequences

Timers can wake unloaded actors efficiently. Destroying an actor can locate/cancel its sends. Persistence adapters may normalize timers separately as long as they preserve one logical session state.
