# ADR 0011: The host owns external scheduling; actor owns internal events

**Status:** Accepted  
**Date:** 2026-09-15

## Context

The embedder wants control of concurrency and queueing. SCXML nevertheless distinguishes internal events raised during a macrostep from external events delivered between macrosteps.

## Decision

Tinyactors owns the current actor's internal event queue while a macrostep is in progress. The host owns any external event queue and chooses which actor receives which external event next. Delivery to an actor already inside a macrostep is rejected/busy rather than becoming an implicit per-actor external mailbox.

## Consequences

Idle actors need no mailbox allocation. The host can integrate with libuv, Python asyncio, Ruby schedulers, RTOS queues, or custom server queues without the core dictating policy.
