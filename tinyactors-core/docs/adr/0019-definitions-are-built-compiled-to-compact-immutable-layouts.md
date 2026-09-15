# ADR 0019: Definitions are built/compiled to compact immutable layouts

**Status:** Accepted  
**Date:** 2026-09-15

## Context

Embedders need a programmatic definition API and later importers need a common target. Runtime execution should not walk a mutable object graph or XML DOM.

## Decision

A builder creates states, transitions, targets, executable actions, history, and metadata. Compilation validates the statechart and produces immutable tables plus specialized configuration/history layouts. Runtime references use compact IDs/offsets rather than strings where practical.

## Consequences

The same compiled representation can be produced by C code, generated code, future SCXML importers, or language bindings. Definitions can eventually be serialized or emitted as constant data.
