# ADR 0006: Host-defined values own lifetime and serialization

**Status:** Accepted for lifetime/ownership; serialization requirement superseded by ADR 0038  
**Date:** 2026-09-15

## Context

Event data, targets, data-model values, and errors may be native host objects. Their ownership rules vary radically by host runtime.

## Decision

`ta_value` is a fat pointer whose vtable owns retain/release and clone behavior. (ADR 0050 made retain the only way core shares a value, and the `clone` member has since been removed.) A binding may wrap `PyObject *`, Ruby `VALUE`, N-API handles/references, native C objects, or immediate/arena-backed embedded values.

Tinyactors core does not require a universal serialization hook. Portable durability modules may define host-specific value codecs or stable-reference schemes outside the core ABI.

## Consequences

The common case stays small while host bindings remain correct. A live/session image can retain host references without copying the underlying object graph. Whether those references can cross a process boundary is a property of an optional durability layer, not `ta_value` itself.
