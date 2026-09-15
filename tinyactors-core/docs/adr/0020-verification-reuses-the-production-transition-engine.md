# ADR 0020: Verification reuses the production transition engine

**Status:** Accepted  
**Date:** 2026-09-15

## Context

A separate formal-semantics implementation would risk diverging from production behavior and reduce confidence in counterexamples.

## Decision

Simulation, replay, fuzzing, and future model checking call the same interpreter used in production. The verifier substitutes its own scheduler, logical clock, I/O processors, invokers, and environment models around the core.

## Consequences

A failing trace can be replayed directly against production semantics. Verification features should not require a special actor execution model.
