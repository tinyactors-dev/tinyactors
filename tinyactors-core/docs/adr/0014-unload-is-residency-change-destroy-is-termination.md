# ADR 0014: Unload is residency change; destroy is termination

**Status:** Accepted  
**Date:** 2026-09-15

## Context

Durable actors should consume no execution-state RAM while idle, yet timers and identity must continue to exist. Treating eviction as termination would cancel semantic state.

## Decision

`ta_actor_unload()` saves and evicts resident session state without changing the actor's logical existence. `ta_actor_load()` restores it. `ta_actor_destroy()` terminates/removes the actor, removes its name, cancels its delayed sends, and cancels active invocations as required by statechart semantics.

## Consequences

Residency becomes an implementation/cache concern. An unloaded actor can still be found by name and can become runnable when a timer or externally queued event targets it.
