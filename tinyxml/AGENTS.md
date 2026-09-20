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

- `mise run build` — compile the header checks, the modules, and the test
  binaries and link them (`build:library` and `build:tests` run in parallel).
- `mise run generate` — regenerate `include/txml.c` and
  `include/txml.api.json`; run it after changing `src/` or `include/txml.h`.
- `mise run test` — run the whole suite. The checks are `depends` of the task,
  which mise runs in parallel: `generated` (fails if a checked-in generated file
  is stale), `freestanding` (quick tier), `lint-src`, `build`, `amalgamate-test`
  and `api-test`; the task then runs the test binaries that need running, all at
  once.
- `mise run spec <file>` — run one spec file (or all of them) verbosely.
- `mise run freestanding` — prove constraint 1, quick tier: no libc includes,
  and no undefined symbol in the objects of `src/` at -O0, -O2 and -Os and of
  the amalgamation at -O2. `mise run freestanding:full` is the complete set,
  self-contained: both targets at all five levels (-O0 -O1 -O2 -O3 -Os), a line
  per (target, level) saying whether it ran or was verified from cache, and a
  final `2 targets x 5 levels` line with the wall-clock; run it before a commit
  and in CI. The compiler-emitted `memset` this exists for showed at -O0
  through -O3, so the quick tier catches it at -O0 and -O2. The amalgamation is
  only checked at -O2 in the quick tier because compiling that one file costs
  2 s at -O0, 6 s at -Os, 13 s at -O2 and 20 s at -O3 (the time is spread over
  the whole optimiser, nothing to fix).
- `mise run lint-src` — audit constraints 5 and 10: no recursion, at most
  three parameters per function.
- `mise run generated` — fail if `include/txml.c` or `include/txml.api.json`
  is out of date or was edited by hand (both generators in `--check` mode).
- `mise run amalgamate [--check]` — write or verify `include/txml.c`, the
  single-file library.
- `mise run amalgamate-test` — build the amalgamation strictly and run the
  public-API tests linked against it (its freestanding check is
  `freestanding:amalgamation:*`).
- `mise run api [--check]` — write or verify `include/txml.api.json`, the
  machine-readable API description.
- `mise run api-test` — check the description is current and covers the
  header, and test the extractor against `tests/api/*.h`.

A test whose modules do not exist yet is reported as pending, never made to
pass by weakening it. `include/txml.c` and `include/txml.api.json` are
generated and checked in: after changing `src/` or `include/txml.h`, run
`mise run generate` and commit the regenerated files with the change;
`mise run test` fails with a stale file rather than rewriting it.

## Working rules

- Keep `docs/internal/memory-architecture.md` and the header in agreement. If
  a change makes them disagree, the ADR-backed document wins and the header
  follows.
- The amalgamation is one translation unit, so a file-scope name (a typedef,
  macro, static function or static object) must mean one thing across all of
  `src/`. Prefix a module-local name that another module also uses. Every
  quoted include in `src/` must resolve to a header listed in
  `src/internal.h`.
- Prefer growing the spec suite over adding one-off C tests. A case that can
  be expressed as input plus an expected node table belongs in `tests/spec/`.

The tasks use mise's own machinery: file tasks in `mise/tasks/`, and `depends`
for ordering and parallelism (`mise run --jobs N` or `MISE_JOBS` sets the pool;
the default is 4).

Skipping unchanged work is by content, not modification time, because mise's
`sources` check reruns a task when a file is merely touched or rewritten with the
same bytes. `mise/lib/cache.rb` provides it:

- **Stamps.** A task hashes what its result depends on and records the hash
  under `build/stamps/` only when it succeeds; the record is deleted before any
  work starts, so a failing run never leaves one and the next run repeats it.
- **Compile cache.** Every source is compiled to its own object (with `-MMD`)
  and reused while its contents, its headers' contents and the compile command
  are unchanged. If only comments or layout changed, the preprocessed unit is
  identical and the object is kept without compiling.
- **Content addressing downstream.** Linking, the undefined-symbol check and
  running a test binary key on the bytes of what they consume, so an object that
  came out identical invalidates nothing after it. A test binary runs again only
  when its own bytes, its arguments or `tests/spec/**` changed.
- `TXML_FORCE=1 mise run <task>` ignores every stamp and cached object;
  `rm -rf build` does the same. (`mise run --force` only defeats mise's own
  check, which these tasks no longer use, and mise does not tell a task it was
  given.) Changing `CC` or `CFLAGS` changes the compile command and so
  recompiles everything.
