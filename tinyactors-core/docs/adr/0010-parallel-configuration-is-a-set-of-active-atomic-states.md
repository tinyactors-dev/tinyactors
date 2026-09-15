# ADR 0010: Parallel configuration is a set of active atomic states

**Status:** Accepted  
**Date:** 2026-09-15

## Context

SCXML parallel states activate all child regions and a single event may enable multiple non-conflicting transitions. A generic `current_state` field is therefore incorrect.

## Decision

The semantic configuration is represented by its active atomic leaves; ancestor activity is derived from the immutable definition. The compiler chooses a compact representation: one leaf for definitions proven sequential, a 32/64-bit bitmap when possible, or a fixed word set for larger parallel machines.

## Consequences

Parallelism is represented without threads. `In(state)` and transition selection operate correctly across regions. Snapshot and history logic must preserve all simultaneously active regions.
