# ADR 0028: Runtime IDs are compact handles; persistence must resolve logical identity

**Status:** Accepted; refined by ADR 0038 and ADR 0043  
**Date:** 2026-09-15

## Context

Compact integer IDs are ideal in RAM but process-local tables may be rebuilt after restart. Treating raw table indices as universal identifiers risks reconstructing against the wrong definition/event-name table.

## Decision

Use compact numeric IDs as runtime handles. Session images and portable checkpoint encodings must carry or resolve stable logical identity for definitions, names, and structured event names rather than assuming numeric handles are stable across processes. Definitions are identified by family plus exact revision fingerprint. Named actor addresses remain logical `(family, name)` routes.

## Consequences

The runtime stays dense. Optional durability formats need a resolution layer and versioning, but core in-memory structures do not pay for portable identities on every field.
