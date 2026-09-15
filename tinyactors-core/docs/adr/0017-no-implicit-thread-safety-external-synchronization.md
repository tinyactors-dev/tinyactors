# ADR 0017: No implicit thread safety; external synchronization

**Status:** Accepted  
**Date:** 2026-09-15

## Context

Locking every internal operation would cost memory and performance, complicate embedded builds, and contradict caller-owned concurrency.

## Decision

The core does not create locks or make a `ta_system` concurrently mutable by default. The caller serializes access to a system, shards actors across independent systems, or wraps calls with its own synchronization.

## Consequences

Single-threaded embeddings pay no synchronization tax. Server embeddings can choose the scheduling topology appropriate to their workload.
