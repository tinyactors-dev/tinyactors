# ADR 0011: JavaScript values cross as reference-counted boxes

**Status:** Accepted; one handle table per system (amended with ADR 0014)
**Date:** 2026-09-23

## Context

Core treats values as host-defined `ta_value`s that it retains and releases
(tinyactors-core ADRs 0006 and 0050). Event data, data-model roots, delayed
send payloads and completions are JavaScript values. A `napi_ref` is a V8
global handle, so the per-value cost matters for the same reasons as in ADR
0002.

## Decision

1. **Values live in one handle table per system.** The table is a
   JavaScript array held by the system's only strong `napi_ref` for values.
   - A value box is a small native record from the system's pools
     (ADR 0014): an index into the table and a native reference count.
   - `retain` increments the count. `release` decrements it, and at zero it
     stores `undefined` at the index and puts the index on a native free
     list. Core shares values only by retain; `ta_value_vtable` has no
     `clone` (tinyactors-core ADR 0050).
   - Reading a value is one `napi_get_element`. Writing a new one takes an
     index from the free list, or appends, and is one `napi_set_element`.
   - Values are shared, not structured-cloned (as `docs/design.md`
     explains).
2. **Boxes exist only for values that are present.** An event without data,
   and a chart without data, create no boxes. A table entry is an ordinary
   array element, not a V8 global handle. The number of global handles is
   one per system, whatever the number of values or actors with data. This
   matters for the main use case: one actor per websocket connection almost
   always has data, and a handle per value would bring back the problem of
   ADR 0002 at four million handles.
3. **Data-model callbacks** (conditions, values, locations, scripts) are
   synchronous JavaScript functions that the definition retains. They are
   called inside a macrostep through a pre-built call context. They must
   not return promises. An exception becomes a fault that core turns into
   `error.execution`.
4. **Box records and table indices are reused** through free lists, so a
   message with data allocates nothing natively after warm-up (ADR 0014).
   The table array only grows. It holds as many elements as the most values
   that were live at once.

## Consequences

- Charts with data and events with payloads pay one array element and one
  pool record per live value, and no global handle. `system.stats()` shows
  the live boxes and the table's length (ADR 0009).
- The garbage collector still marks every live value, as it must. It does
  not visit a root per value, and in Node-API a value costs no
  `napi_ref` tracking entry.
- A data model that stored plain data natively (for example, JSON in core
  memory) could avoid handles altogether. It is left as a future option for
  charts whose data never needs to be a live JavaScript object.
