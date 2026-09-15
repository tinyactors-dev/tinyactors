# ADR 0013: Snapshot the complete running session

**Status:** Accepted  
**Date:** 2026-09-15

## Context

Unloading may occur while an actor is idle or paused inside a macrostep. Persisting only active states and data is insufficient to resume at the identical semantic point.

## Decision

A complete snapshot includes actor/definition identity, name, active configuration, history, data-model state, active invocations, delayed sends, and—when paused—continuation, current external event, and internal event queue. All host values involved must serialize through their value/data-model contracts.

## Consequences

Unload/reload can be observationally invisible to the statechart. Snapshot format must be versioned, and compact runtime handles require resolution on restore.
