# ADR 0022: Actor event origin is the implicit reply address

**Status:** Accepted  
**Date:** 2026-09-15

## Context

Actor protocols become noisy if every application payload needs a `reply_to` field. SCXML events already carry origin/origin-type metadata.

## Decision

Actor-to-actor delivery records origin structurally in `ta_event.origin`. Reply helpers may target the current event's actor origin without modifying application payload data. I/O- and invocation-originated events use other origin kinds.

## Consequences

Request/reply protocols remain clean and transport metadata stays outside domain payloads. Snapshot/replay must preserve origin metadata when an event is part of a paused macrostep.
