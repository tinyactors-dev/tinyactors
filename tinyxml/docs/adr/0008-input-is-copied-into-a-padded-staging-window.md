# ADR 0008: Input is copied into a padded staging window

**Status:** Accepted  
**Date:** 2026-09-20

## Context

A chunk-fed parser has to decide who owns the input bytes while they are being scanned. Borrowing the caller's chunk avoids a copy but imposes a rule that is easy to get wrong: the chunk must stay alive and unmodified until the parser says it is finished with it, which for a token spanning a boundary means until some later call. That rule is particularly awkward across an FFI boundary, where the caller's buffer may be a garbage-collected object.

There is a second, unrelated pressure. A vectorized scanner wants to read fixed-width blocks — 16, 32, or 64 bytes — past the current position. Reading past the end of the caller's buffer is undefined behaviour, so a scanner either handles every tail with scalar code or requires padding the caller cannot be assumed to provide.

## Decision

`txml_parse_feed` copies. It takes up to `chunk.length` bytes into the parser's staging window, returns how many it accepted, and the caller may reuse or free its buffer the moment the call returns. When the window cannot take the whole chunk, the caller feeds the rest after the next `txml_parse_step`; a `final = true` that accompanied a partially accepted chunk is ignored and repeated with the remainder. The window is sized by `txml_parser_config.staging_length`, defaulting to 64 KiB, and it is allocated with padding past its end so a scanner may over-read within it without leaving the library's own memory.

The copy out of the staging window into the pool is where normalization happens, in one pass: predefined entities and character references are expanded, internal-subset entities are expanded under `TXML_INTERNAL_SUBSET`, `\r\n` and lone `\r` become `\n`, and attribute-value whitespace is normalized as XML 1.0 requires. Names are assembled and interned in the same pass.

`TXML_INPUT_PADDED` is the escape hatch for callers who can do better: it asserts that every fed chunk has 64 readable bytes past its end and that the caller keeps it valid, letting the parser scan the chunk in place and skip the staging copy.

## Consequences

The ownership rule becomes the simplest one available — the parser never holds the caller's memory — which removes an entire category of use-after-free and buffer-reuse bugs, and makes the FFI story trivial.

Because normalization happens during the copy, chunk boundaries and line endings are invisible in the finished document: one text run is one contiguous `txml_str` regardless of how the input was split (ADR 0003). This is what lets the spec suite assert that all chunk splittings of an input produce identical documents.

The cost is that text bytes are copied twice, from the caller's chunk to the staging window and from there to the pool. For markup-dense documents this is noise next to per-tag work; for text-heavy documents it is a real but modest cost, and `TXML_INPUT_PADDED` removes it for callers who can meet the contract.

Padding the window is the load-bearing detail for ADR 0015: a vectorized scanner can be dropped in later without adding a tail special case and without ever reading memory the library does not own.
