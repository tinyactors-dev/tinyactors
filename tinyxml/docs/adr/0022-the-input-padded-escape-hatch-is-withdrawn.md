# ADR 0022: The input-padded escape hatch is withdrawn

**Status:** Accepted  
**Date:** 2026-09-21  
**Amends:** [ADR 0008](0008-input-is-copied-into-a-padded-staging-window.md) and [ADR 0017](0017-the-parser-produces-a-concrete-syntax-tree.md)

## Context

ADR 0008 offered `TXML_INPUT_PADDED` as an escape hatch: a caller who guaranteed 64 readable bytes past every fed chunk, and kept the chunk alive until the next step, would let the parser scan it in place and skip the copy into the staging window. After ADR 0017 it was the parser's only flag.

It was never implemented. The parser accepted the flag and ignored it, so a documented promise of zero-copy input quietly copied. Since ADR 0021 the scanner is a table of resumable states that carries partial characters, delimiters, references and carriage returns across chunk boundaries and validates UTF-8 as bytes are fed; all of that assumes the parser owns the bytes it is looking at. Reading a caller's buffer in place would need a second source for every state, a copy of just the carried bytes, and a lifetime rule the parser cannot check, for a saving that the measurements do not show: the copy is small next to per-token work, and the padding it existed to exploit is only useful to a vectorized scanner that does not exist (ADR 0015).

## Decision

`TXML_INPUT_PADDED`, the `txml_parser_flags` set, and `txml_parser_config.flags` are removed. The parser has no flags. `txml_parse_feed` copies, always, and the caller may reuse its chunk when the call returns.

The staging window stays padded: that is an internal property, kept so a future block scanner can over-read within the library's own memory (ADR 0015). If such a scanner ever earns a zero-copy path, the option returns with an implementation behind it and a test that shows the parser never reads outside the range the caller promised.

The spec format loses its `@flags` directive, which had no names left.

## Consequences

The configuration struct is smaller and says only things that happen. Code that set `.flags` on a `txml_parser_config` no longer compiles, which is the point: nothing that worked is lost, because nothing behind the flag ever worked. The lifetime table loses its one row that depended on the caller's chunk outliving the feed call.
