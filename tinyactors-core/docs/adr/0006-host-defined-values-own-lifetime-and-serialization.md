# ADR 0006: Host-defined values own lifetime and serialization

**Status:** Accepted  
**Date:** 2026-09-15

## Context

Event data, targets, data-model values, and errors may be native host objects. Their ownership and persistence rules vary radically by host runtime.

## Decision

`ta_value` is a fat pointer whose vtable owns retain/release, clone, and serialization behavior. A binding may wrap `PyObject *`, Ruby `VALUE`, N-API handles/references, native C objects, or immediate/arena-backed embedded values.

## Consequences

The common case stays small while host bindings remain correct. A non-serializable value can prevent actor eviction; this is reported rather than silently losing state.
