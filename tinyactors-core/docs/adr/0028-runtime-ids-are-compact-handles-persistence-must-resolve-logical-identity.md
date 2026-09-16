# ADR 0028: Runtime IDs are compact handles; persistence must resolve logical identity

**Status:** Accepted
**Date:** 2026-09-15

## Context

Compact integer IDs are ideal in RAM but process-local tables may be rebuilt after restart. Persisting raw table indices as if they were universal identifiers risks restoring against the wrong definition/event-name table.

## Decision

Use compact numeric IDs as runtime handles. Persistent formats must carry or resolve stable logical identity for definitions, names, and structured event names rather than assuming numeric handles are stable across processes. Actor IDs may be stable if the chosen store explicitly guarantees their allocation scheme.

## Consequences

The runtime stays dense. Checkpoint formats need a resolution layer and
versioning. ADR 0037 defines definition family/session-schema recovery and the
host-backed named actor directory.
