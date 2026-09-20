# Internal reference

This directory describes what an implementation of `include/txml.h` must do.
The header states the contract; these files state the mechanism. Where the
header is silent, the choice is recorded here as a decision and is binding
until an ADR changes it.

- `memory-architecture.md` — the arena, the document's two of them, the paged
  columns, who owns what, and the exact lifetime of every borrowed pointer.
- `document-layout.md` — the columns byte by byte, with a worked example of a
  parsed document.
- `cursor.md` — the range algebra behind the cursor, and what makes queries
  cheap.
- `parser.md` — the pipeline from a fed chunk to appended nodes, lexer states,
  normalization, and every way a parse fails.
- `writer.md` — the writer state machine, escaping, prefix selection, and the
  resumable subtree walk.
- `conformance.md` — what txml implements, what it does not, and what survives
  a round trip.
- `public-api.md` — the amalgamation and the API description, and how the
  header's annotations are consumed.
- `invariants.md` — numbered rules an implementation must never violate.
- `testing.md` — the suite and how to read a spec file.

Cross-cutting: `../adr/` records why these decisions were made. `../../AGENTS.md`
carries the hard constraints every change must respect.
