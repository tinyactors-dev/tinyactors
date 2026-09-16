# ADR 0014: Unload is residency change; destroy is termination

**Status:** Accepted  
**Date:** 2026-09-15

## Context

Durable actors should consume no execution-state RAM while idle, yet timers and identity must continue to exist. Treating eviction as termination would cancel semantic state.

## Decision

The host saves a core checkpoint and evicts resident session state without
changing the actor's logical existence; it later materializes the checkpoint.
`ta_actor_destroy()` terminates/removes the resident actor. For named actors,
the core drives removal through the host-backed directory protocol.

## Consequences

Residency becomes a host cache concern. An evicted named actor can still be
resolved by address and become runnable when host-scheduled work targets it.
