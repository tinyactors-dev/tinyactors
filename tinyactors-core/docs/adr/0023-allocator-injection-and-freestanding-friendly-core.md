# ADR 0023: Allocator injection and freestanding-friendly core

**Status:** Accepted  
**Date:** 2026-09-15

## Context

ESP32, freestanding WASM, custom arenas, and server environments need different memory management. Requiring libc allocation or filesystem facilities would limit portability.

## Decision

Allocation enters through a small allocator fat pointer. Internal transient work may use arenas. The core must not depend on POSIX, threads, filesystem APIs, sockets, or a system clock.

## Consequences

Embedders can use malloc, region allocators, fixed pools, WASM linear-memory allocators, or RTOS-specific allocation. Optional adapters can add richer platform services.
