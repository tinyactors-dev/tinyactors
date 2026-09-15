# ADR 0007: Structured interned event names and prefix matching

**Status:** Accepted  
**Date:** 2026-09-15

## Context

SCXML event descriptors are token sequences separated by dots and transitions use token-prefix matching. Treating names as ordinary strings would repeatedly split/compare strings and obscures structure in generated names such as `done.invoke.X`.

## Decision

Intern every event name as a path of segments in a shared trie/table. Hot events carry a compact `ta_event_name_id`. Matching operates on structural ancestry/depth. String parsing/formatting happens only at host or serialization boundaries. Generated names are assembled by appending segments.

## Consequences

Matching becomes cheap and naturally implements SCXML semantics. Runtime-generated `done.*` and `error.*` names do not require format-then-parse cycles. Persisted data must not assume process-local event-name IDs remain stable.
