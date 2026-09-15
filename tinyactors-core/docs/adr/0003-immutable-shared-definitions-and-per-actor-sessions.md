# ADR 0003: Immutable shared definitions and per-actor sessions

**Status:** Accepted  
**Date:** 2026-09-15

## Context

Millions of actors may execute the same statechart. Repeating transition tables, strings, vtable pointers, and topology per actor would dominate memory.

## Decision

A `ta_definition` is immutable after compilation and shared by all instances. An actor stores only mutable session state: identity, configuration, history, data-model instance, continuation when necessary, internal events, invocations, and references to its delayed sends.

## Consequences

Definition memory amortizes across actors. Definitions can be stored in compact tables and eventually ROM/mmap-friendly blobs. Actor representation can be specialized from definition metadata.
