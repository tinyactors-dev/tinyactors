# ADR 0039: Lost invocations become reconstruction communication errors

**Status:** Accepted  
**Date:** 2026-09-17

## Context

An active invocation contains logical state owned by Tinyactors and an opaque
service handle owned by the current invoker runtime. Reconstructing that handle
would require invoker-specific retention, serialization, or rebinding. Restarting
the invocation could duplicate an external effect.

SCXML already represents failure to communicate with an external entity as an
internal `error.communication` event. The chart, rather than the core API, is the
right place to choose whether to retry, compensate, wait, or terminate.

## Decision

Session images capture the logical record of each active invocation but do not
capture its runtime service handle. Reconstruction succeeds without calling an
invoker to restart or rebind the service.

For every invocation active in the image, reconstruction:

1. omits the invocation from the reconstructed actor's active runtime records;
2. appends `error.communication.invoke.<id>.unavailable` to the internal queue
   after events already captured in that queue;
3. gives the event `TA_EVENT_PLATFORM` type, `TA_ORIGIN_NONE` origin, and
   `TA_INVOKE_ID_NONE` as its envelope invocation ID;
4. orders multiple errors by captured invocation-record order; and
5. ignores later events attributed to the lost invocation.

The lost invocation does not generate `done.invoke.<id>` and is not cancelled
when its former owning state exits. An active invocation does not cause
`ta_actor_capture()` or `ta_actor_reconstruct()` to return
`TA_ERR_NOT_RECONSTRUCTIBLE`.

## Consequences

The invoker vtable needs no capture, serialization, or rebind operations.
Reconstruction has a deterministic, chart-visible loss policy. Charts may catch
the detailed event, `error.communication`, or the broader `error` prefix.
The fault mapper accepts invocation identity and defines
`TA_FAULT_CONTEXT_INVOKE_UNAVAILABLE` for generating the event consistently with
other invoker failures.

The event name describes the service condition, not why the interpreter learned
about it. A chart cannot distinguish reconstruction from any other future path
that determines the invoked service is unavailable.

Invokers report asynchronous events, normal completion, and terminal service
loss through `ta_actor_report_invoke()`. `TA_INVOKE_REPORT_UNAVAILABLE` performs
the same retirement and error-generation operation used during reconstruction.
The API is not called reentrantly from `start()`.

Applications that need an external service to survive process loss must model a
stable external protocol and reconcile or retry it from the chart or an optional
durability layer. The core does not assume that repeating `start` is safe.
