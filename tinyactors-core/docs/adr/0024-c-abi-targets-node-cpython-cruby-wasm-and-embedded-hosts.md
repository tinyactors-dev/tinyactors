# ADR 0024: C ABI targets Node, CPython, CRuby, WASM, and embedded hosts

**Status:** Accepted  
**Date:** 2026-09-15

## Context

The library should be callable from C-family programs and easy to bind into common dynamic-language runtimes without changing semantics.

## Decision

Keep the public ABI C-compatible, use fixed-width IDs and explicit slices, expose opaque top-level objects, and use `{self, vtable}` for host interfaces. Do not expose C++ exceptions, STL containers, POSIX handles, or host-specific runtime types in the core ABI.

## Consequences

N-API, CPython C extensions/cffi, CRuby extensions/Fiddle, and WASM exports/imports can all adapt the same ABI. Host object wrappers live in bindings rather than the core.
