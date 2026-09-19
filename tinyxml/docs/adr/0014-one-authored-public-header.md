# ADR 0014: One authored public header

**Status:** Accepted  
**Date:** 2026-09-20

## Context

The core learned this the hard way (core ADR 0041): a public header that includes private module headers makes everything reachable from it de facto API, so implementation layouts leak, bindings cannot tell what is public, and an API change is invisible in review. SQLite's answer — one hand-maintained `sqlite3.h`, private headers that include it, opaque handles, and an amalgamation that gives private functions internal linkage — is the precedent the core adopted.

txml starts from an empty `src/`, which is the cheapest possible moment to adopt the same rule rather than to retrofit it.

## Decision

`include/txml.h` is authored by hand and is the complete public API. Every public function is declared there, and only there, with `TXML_API`. Types whose layout is private are opaque typedefs: `txml_parser`, `txml_doc`, `txml_writer`, and `txml_arena_chunk`.

Three types are deliberately transparent, because callers must place them: `txml_arena`, whose fields are accounting a caller reads; `txml_cursor` and `txml_attr_cursor`, which are two-word values passed by value (ADR 0005); and the plain data of `txml_str`, `txml_qname`, `txml_position`, `txml_limits`, and the config and view structs. Everything that describes how a document is stored — columns, pages, name hashing, lexer state — stays behind the handles (ADR 0004).

`src/*.h` are private module headers. They include `txml.h` and add layouts and internal functions; an umbrella header lists them in dependency order, as `src/internal.h` does in the core.

The header also carries the `TXML_*` annotation macros, which record what C types cannot: which pointers are single elements or caller buffers, which may be null, which outputs are valid after which result, who releases what, and how long borrowed data lives. They expand to nothing unless `TXML_API_EXTRACT` is defined, so they never change the compiled API.

The amalgamation and the machine-readable `api.json` extraction are deferred until `src/` has modules. When they arrive they mirror `tinyactors-core/mise/tasks/amalgamate` and `.../api`, including the `--check` modes that fail when a public prototype lacks `TXML_API` or a private header redeclares a public function.

## Consequences

The public surface is one reviewable file, and an API change is a diff to it. A binding generator — the same one the core's `api.json` feeds — parses one header. Shipping the eventual `txml.h` plus `txml.c` exports exactly the `TXML_API` symbols.

Internal layouts stay changeable, which the rest of the design depends on: ADR 0004's column split and ADR 0015's vectorized scanning are private changes precisely because no caller ever holds a pointer into a column.

The header is written before the implementation, which is deliberate. It is the specification the spec suite tests against, and the annotations are part of that specification rather than decoration added afterwards.
