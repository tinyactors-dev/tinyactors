# ADR 0016: Invocation is a pluggable service boundary

**Status:** Accepted  
**Date:** 2026-09-15

## Context

SCXML invocation represents a logically separate service that can emit events, be cancelled on state exit, receive forwarded events, and eventually complete with `done.invoke.<id>`.

## Decision

Invokers are `{self, vtable}` interfaces for start/cancel/forward. A built-in statechart invoker may implement invocation by spawning a child actor and establishing parent/invoke metadata. External services may be arbitrary host integrations. Invocation bookkeeping is part of actor session state.

Invoker service handles are runtime-only. Session reconstruction does not
restart or rebind an invoked service. It retires each invocation that was active
in the image and reports the loss to the chart as an internal
`error.communication.invoke.<id>.unavailable` platform event.

After `start()` returns, an invoker reports ordinary events, normal completion,
or terminal unavailability through `ta_actor_report_invoke()`. Network loss and
loss discovered during reconstruction use the same unavailable path.

## Consequences

Production services and simulated verification services use the same statechart boundary. A simulator can model an invocation by generating any response events allowed by its model.
