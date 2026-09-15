# ADR 0004: Named definitions; named and anonymous actors

**Status:** Accepted  
**Date:** 2026-09-15

## Context

Many actors correspond to durable business entities such as a Stripe subscription, while others represent transient resources such as a WebSocket connection.

## Decision

Definitions are named. Every actor has a numeric runtime identity. Actors may additionally have a durable name scoped by definition. Named business actors are addressable as `(definition, name)`. Anonymous actors use only their runtime identity.

## Consequences

Business identity is not confused with memory residency. Names can survive unloading. Anonymous actors avoid registry overhead when symbolic identity has no value.
