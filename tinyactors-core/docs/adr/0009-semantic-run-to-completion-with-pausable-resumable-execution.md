# ADR 0009: Semantic run-to-completion with pausable/resumable execution

**Status:** Accepted  
**Date:** 2026-09-15

## Context

SCXML processes an external event until the machine reaches a stable configuration, but an embedded library must allow cooperative CPU budgeting and even infinite macrosteps.

## Decision

Run-to-completion is a **semantic** boundary, not a C stack boundary. `ta_actor_resume()` may return `TA_RUN_PAUSED` while preserving an interpreter continuation. The actor remains busy with the same external event and cannot accept another external event until the macrostep becomes stable or terminates.

## Consequences

Long or non-terminating control flows do not monopolize the caller. Continuation state becomes part of persistence whenever an actor is paused mid-macrostep.
