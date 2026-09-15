# ADR 0029: No per-actor external mailbox is required by the core

**Status:** Accepted  
**Date:** 2026-09-15

## Context

A mailbox object per actor is expensive at multi-million scale and duplicates scheduling policy that the embedder already owns.

## Decision

The core accepts an external event only when the actor is ready to begin a new macrostep. The host may maintain any queueing topology it wants: global queue, shard queue, durable log, per-business-key queue, or none. Internal SCXML events remain actor session state while a macrostep runs.

## Consequences

Idle actors have zero mailbox overhead. Bindings must document how they queue events that arrive while an actor is busy.
