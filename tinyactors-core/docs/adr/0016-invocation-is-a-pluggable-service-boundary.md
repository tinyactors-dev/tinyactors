# ADR 0016: Invocation is a pluggable service boundary

**Status:** Accepted  
**Date:** 2026-09-15

## Context

SCXML invocation represents a logically separate service that can emit events, be cancelled on state exit, receive forwarded events, and eventually complete with `done.invoke.<id>`.

## Decision

Invokers are `{self, vtable}` interfaces for start/cancel/forward. A built-in statechart invoker may implement invocation by spawning a child actor and establishing parent/invoke metadata. External services may be arbitrary host integrations. Invocation bookkeeping is part of actor session state.

## Consequences

Production services and simulated verification services use the same statechart boundary. A simulator can model an invocation by generating any response events allowed by its model.
