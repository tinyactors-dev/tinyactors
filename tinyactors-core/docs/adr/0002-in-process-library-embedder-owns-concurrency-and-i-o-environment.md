# ADR 0002: In-process library; embedder owns concurrency and I/O environment

**Status:** Accepted  
**Date:** 2026-09-15

## Context

The target environments range from an ESP32 and freestanding WASM to Node, Ruby, Python, and servers with millions of actors. Owning threads, files, sockets, or an event loop would make the library less portable and less SQLite-like.

## Decision

Tinyactors is an in-process library. Calls execute on the caller's thread. The embedder decides when actors run, how external events are queued, how storage works, how time advances, and which I/O implementation is used.

## Consequences

The same core can be embedded in very different runtimes. The host must establish its own scheduling and synchronization discipline. Convenience adapters may be built outside the core.
