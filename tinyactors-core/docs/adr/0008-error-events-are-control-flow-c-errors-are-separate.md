# ADR 0008: `error.*` events are control flow; C errors are separate

**Status:** Accepted  
**Date:** 2026-09-15

## Context

SCXML requires execution and communication errors to be queued as internal events and processed normally. Returning them as C API failures would bypass statechart error handling.

## Decision

`ta_result` represents failures of the C/runtime/API operation itself. Plugin operations return `TA_OP_OK` or `TA_OP_ERROR` plus a `ta_fault`. The interpreter maps a fault, based on operation context, to a structured internal `error.execution...`, `error.communication...`, or `error.platform...` event. Fault details may be placed in event data.

## Consequences

Statecharts can handle errors hierarchically (`error`, `error.communication`, or a detailed suffix). Adapters do not need to know the entire SCXML error taxonomy. Interpreter code must implement the standard's operation-specific consequences after a fault.
