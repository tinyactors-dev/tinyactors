# ADR 0002: One linked-chunk arena per document; nothing relocates

**Status:** Accepted  
**Date:** 2026-09-20

## Context

A document's size is not known when parsing starts. The obvious C answer — grow arrays with `realloc` — is unavailable and undesirable here. It is unavailable because `txml_allocator` offers only `alloc` and `free`, matching the core's allocator (core ADR 0023); a fixed pool, a WASM linear-memory allocator, and an RTOS block allocator have no meaningful `realloc`. It is undesirable because relocation invalidates every pointer the library has handed out, which would force either a pointer fix-up pass or a rule that no borrowed string survives the next parse step. Both are bad: the first is complexity with no user-visible benefit, the second makes a streaming consumer copy everything it wants to keep.

## Decision

Everything a document owns lives in one `txml_arena`: node, attribute, name, and namespace columns, the byte pool that holds text and name characters, and the optional position column.

The arena is a bump allocator over a singly linked list of chunks. `txml_arena_alloc` hands out the next aligned bytes in the newest chunk. When a request does not fit, the arena asks the backing allocator for a new chunk and links it in front of the old ones; existing allocations are neither copied nor readdressed. A request larger than `chunk_size` gets a chunk sized to that request, so every single allocation is contiguous — which is how a text run longer than a chunk still arrives as one `txml_str`. Chunk sizes start at `chunk_size` (`TXML_ARENA_CHUNK_DEFAULT` when the config says 0) and grow up to a cap, so a large document does not accumulate thousands of chunks.

`txml_arena_position` and `txml_arena_release` mark and unwind the arena; `txml_doc_rewind` is built on them (ADR 0013). `txml_arena_reset` frees everything and keeps the arena usable; `txml_arena_destroy` returns every chunk to the backing allocator.

The arena is public. `txml_parser_config.arena` lets a caller supply one, in which case the parser borrows it and `txml_doc_destroy` releases the document's chunks without destroying the arena itself. `txml_doc_arena` exposes `used` and `reserved` for accounting.

## Consequences

Every `txml_str` a document hands out stays valid for that document's lifetime — while parsing continues, not merely after it finishes. That single property is what makes cursors cheap to use: a consumer can hold a name, an attribute value, or a text run without copying it, and it can do so in the middle of a streaming parse. It is also what makes `TXML_LIFETIME(document)` an honest annotation rather than an aspiration.

There is no relocation pass, no pointer table, and no generation counter to validate. Column addressing stays simple because pages are arena allocations that never move.

A fixed-budget embedder needs no special support: it supplies a bump allocator over a static buffer. When that allocator refuses, `txml_arena_alloc` returns NULL and the parse reports `TXML_STEP_NEED_MEMORY`, which is resumable — the caller may extend the budget and call `txml_parse_step` again, or give up and destroy the parser. Memory exhaustion is a pause, not a failure, and it is distinct from both a malformed document and an API misuse.

The cost is bounded internal waste: the tail of a chunk that the next request did not fit into. It is at most one chunk's worth per chunk boundary, it is visible in `reserved` minus `used`, and `chunk_size` tunes it.
