# ADR 0018: Optimize memory per actor; share everything definition-level

**Status:** Accepted  
**Date:** 2026-09-15

## Context

The design target includes roughly four million actors in one process and tiny microcontroller builds. Per-actor pointers, vtables, mailboxes, locks, or generic containers multiply into large costs.

## Decision

Move immutable topology, transition/action tables, event patterns, configuration layout, history layout, and prepared data-model program state into `ta_definition`. Keep actor hot state definition-specialized and compact. Avoid per-actor external mailboxes and per-actor allocator/runtime pointers.

## Consequences

Simple sequential actors can be only a handful of words beyond application data. Richer actors pay only for features used by their definition. Sparse structures may hold active invocations, timers, or uncommon continuations.
