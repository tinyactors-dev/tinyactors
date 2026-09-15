# ADR 0021: Verification begins as simulation/fuzzing, not proof

**Status:** Accepted  
**Date:** 2026-09-15

## Context

Unbounded actors, queues, values, arbitrary data models, and host callbacks make general reachability undecidable and exhaustive model checking impractical for many real systems.

## Decision

The first verification tool is a Monte Carlo/coverage-guided simulator and fuzzer. It randomizes external-event ordering, time advancement, and modeled I/O/invocation responses; checks user invariants; records seeds/traces; and shrinks failures. More systematic exploration can be added later.

## Consequences

Embedders are not forced into a restricted data language. Verification quality depends on how deterministic/simulatable their extensions are, but the runtime remains general-purpose.
