# Working on txml

txml is a freestanding XML reader and writer. It is a sibling of
`tinyactors-core`, follows the same C rules, and exists to give an eventual
SCXML importer/exporter (ADR 0025 of the core) an XML front end that is as
embeddable as the core itself.

## Hard constraints

These are not style preferences. A change that breaks one of them is wrong,
even when it passes the tests.

1. **No C library.** `stdint.h`, `stddef.h`, and `stdbool.h` only. No
   `malloc`, `memcpy`, `strlen`, `printf`, `assert`, or anything else from
   libc. Write the byte loops the library needs.
2. **No I/O.** The library never opens, reads, or resolves anything. External
   entities, external DTD subsets, and XInclude are not fetched. This is what
   makes XXE impossible rather than merely defended against.
3. **All memory comes from the embedder** through `txml_allocator`
   (`alloc`/`free`, no `realloc`).
4. **Nothing is ever relocated.** The arena grows by linking a new chunk; an
   allocation keeps its address until its owner is destroyed. Any design that
   would need `realloc` or a pointer fix-up pass is rejected.
5. **No recursion, anywhere.** Parsing, writing, and cursor walks are
   iterative and use explicit, bounded stacks. Stack depth must not depend on
   document nesting.
6. **Everything unbounded in XML is bounded here.** Every construct that a
   document controls has a `txml_limits` value; reaching one produces a
   `TXML_DOC_LIMIT` document error, never unbounded memory use.
7. **Document errors are data; C errors are results.** A malformed document is
   `TXML_STEP_FAILED` plus a `txml_document_error` with a position.
   `txml_result` is for API misuse, allocation failure, and limits.
8. **No global mutable state.** Everything lives behind a handle the caller
   owns. Two parsers in two threads never interact; there is no locking.
9. **The public API is `include/txml.h` only.** Declare every public function
   there with `TXML_API`. `src/*.h` are private headers.
10. **At most three parameters per function.** Introduce a `_params`/`_config`
    struct instead.
11. **Cursors and name IDs belong to one document.** Never compare or mix them
    across documents.
12. **No VLAs, no `alloca`, no compound-literal lifetimes that outlive a
    statement, no undefined behaviour on over-reads.** A SIMD scanner may read
    only within the staging window, which is padded for exactly that reason.

## Naming

- Prefix `txml_` for types and functions, `TXML_` for macros and constants.
- Byte ranges are always `offset` and `length`, never `off`, `len`, `pos`, or
  `size`. Buffer capacity is `capacity`.
- Index fields that may be absent use `TXML_INDEX_NONE` or `TXML_NAME_NONE`,
  never `0` as a sentinel.

## Layout

- `include/txml.h` — the authored public API, and the only public header.
- `src/*.{c,h}` — implementation modules; headers are private.
- `tests/` — the suite described in `docs/internal/testing.md`.
- `tests/spec/*.spec` — data-driven parser cases, readable without C.
- `docs/adr/` — design decisions. Change behaviour, write an ADR.
- `docs/internal/` — implementation reference and invariants.

## Tasks

- `mise run build` — compile the header checks, any implementation modules,
  and the test binaries that can be linked.
- `mise run test` — run the whole suite.
- `mise run spec <file>` — run one spec file (or all of them) verbosely.

The suite is written against the header and is expected to fail until the
implementation exists. Report it as pending, never by weakening a test.

## Working rules

- Keep `docs/internal/memory-architecture.md` and the header in agreement. If
  a change makes them disagree, the ADR-backed document wins and the header
  follows.
- Do not add an amalgamation or API-extraction step until `src/` has modules;
  when it arrives, mirror `tinyactors-core/mise/tasks/amalgamate` and
  `.../api`.
- Prefer growing the spec suite over adding one-off C tests. A case that can
  be expressed as input plus an expected node table belongs in `tests/spec/`.
