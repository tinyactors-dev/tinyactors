# ADR 0034: Named parameter aggregation is a data-model operation

- Status: Accepted
- Date: 2026-09-15

## Context

SCXML uses named values from `<param>` and `namelist` to construct data for send, invoke and done events. The exact representation of this data is data-model-dependent.

The core cannot assume that the result is JSON, a C map, a Python dictionary, a Ruby hash, or a JavaScript object.

## Decision

The core evaluates each parameter's value or location and collects an ordered sequence of `(name, value)` pairs. It then asks the data model to materialize those pairs into one legal `ta_value` payload.

The semantic operation preserves duplicate names. The plugin decides how duplicates are represented when its value model has no direct equivalent.

A content payload and a named-parameter payload are different IR source forms. The statechart layer enforces the mutual-exclusion rules required by whatever frontend/definition semantics are in use.

## Consequences

I/O processors receive an already materialized `ta_value` payload. They do not need to know about expression evaluation or data-model locations.

Delayed sends naturally persist the already evaluated payload, preserving the rule that send arguments are evaluated when the send action executes rather than when the timer fires.
