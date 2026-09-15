# ADR 0012: Logical time and engine-owned delayed-send semantics

**Status:** Accepted  
**Date:** 2026-09-15

## Context

SCXML delayed sends must evaluate their arguments when the send executes, support cancellation by send ID, and fire later. Reading wall-clock time or delegating timing semantics to arbitrary I/O adapters complicates persistence and simulation.

## Decision

TA stores monotonic logical time supplied by the host. A delayed send is fully materialized at execution time and stored with an absolute logical due time. TA decides when it becomes due; the I/O processor is called only for dispatch. Cancellation is scoped to the actor/session that created the delayed send.

## Consequences

Timers are deterministic under a supplied clock, persistable, and simulatable. The host still decides when to advance time and when to resume work.
