# ADR 0014: Unload is residency change; destroy is termination

**Status:** Accepted; reconstruction mechanics refined by ADR 0038; unload also refuses an image captured in another system (ADR 0085)  
**Date:** 2026-09-15

## Context

Named actors may consume no execution-state RAM while idle, yet their logical identity must continue to exist. Treating eviction as termination would cancel semantic state.

## Decision

The host may capture a core session image and evict resident interpreter state without changing the actor's logical existence. If the actor must survive process loss, an optional durability layer first makes an equivalent checkpoint durable. `ta_actor_destroy()` terminates/removes the resident actor. For named actors, the core drives removal through the host-backed directory protocol.

The public protocol is `capture -> unload` and `load`. Unload checks that the
image still matches the actor's session epoch, compare-and-swaps its directory
entry from resident to unloaded, and invalidates the runtime actor ID. Load
reconstructs off-route and compare-and-swaps the entry back to resident with a
new runtime ID. Directory replacement/removal always includes the expected old
entry.

## Consequences

Residency is a host cache concern. Reconstruction requires the exact definition revision plus the host data-model binding; portable persistence is not implied merely by capturing an in-memory session image.
