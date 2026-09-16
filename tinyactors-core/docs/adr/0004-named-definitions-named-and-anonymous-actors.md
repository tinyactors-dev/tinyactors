# ADR 0004: Named definitions; named and anonymous actors

**Status:** Accepted  
**Date:** 2026-09-15

## Context

Many actors correspond to durable business entities such as a Stripe subscription, while others represent transient resources such as a WebSocket connection.

## Decision

Definitions have durable families. Every actor has a numeric runtime identity.
Actors may additionally have a durable logical address scoped by definition
family. Named business actors are addressable as `(definition family, name)`.
Anonymous actors use only their runtime identity. The core defines the address
protocol while the host provides its directory implementation (ADR 0037).

## Consequences

Business identity is not confused with memory residency. Names can survive unloading. Anonymous actors avoid registry overhead when symbolic identity has no value.
