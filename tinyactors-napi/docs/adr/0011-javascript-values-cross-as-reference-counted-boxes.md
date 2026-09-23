# ADR 0011: JavaScript values cross as reference-counted boxes

**Status:** Accepted
**Date:** 2026-09-23

## Context

Core treats values as host-defined `ta_value`s that it retains and releases
(tinyactors-core ADRs 0006 and 0050). Event data, data-model roots, delayed
send payloads and completions are JavaScript values. A `napi_ref` is a V8
global handle, so the per-value cost matters for the same reasons as in ADR
0002.

## Decision

1. **A value box holds one strong `napi_ref` and a native reference
   count.** `retain` increments the count, `release` deletes the reference
   when the count reaches zero, and `clone` shares the box. Values are
   shared, not structured-cloned (as `docs/design.md` explains).
2. **Boxes exist only for values that are present.** An event without data,
   and a chart without data, create no boxes. The number of global handles
   grows with the number of values in flight and actors with data, never
   with the total number of actors.
3. **Data-model callbacks** (conditions, values, locations, scripts) are
   synchronous JavaScript functions that the definition retains. They are
   called inside a macrostep through a pre-built call context. They must
   not return promises. An exception becomes a fault that core turns into
   `error.execution`.
4. **Box freelists** keep allocation off the per-message path.

## Consequences

- Charts with data and events with payloads pay one global handle per live
  value. That cost is accepted and shown in `system.stats()` (ADR 0009).
- A data model that stored plain data natively (for example, JSON in core
  memory) could avoid handles altogether. It is left as a future option for
  charts whose data never needs to be a live JavaScript object.
