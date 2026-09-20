# The public API, the amalgamation and the API description

`include/txml.h` is the complete, authored public API (ADR 0014). Two files are
generated from it and from `src/`, checked in, and verified by `mise run
build`:

| File | Task | What it is |
|---|---|---|
| `include/txml.c` | `mise run amalgamate` | the whole library as one translation unit |
| `include/txml.api.json` | `mise run api` | the API as data, for bindings and tools |

Both tasks take `--check`, which fails when the checked-in file is stale. After
a change to `src/` or the header: `mise run build`, then commit the regenerated
files with the change. A diff of `txml.api.json` is an API change to review.

## The amalgamation

`amalgamate` writes `include/txml.h` plus `include/txml.c`, the only two files
an embedder needs. It concatenates the private headers in the order listed by
`src/internal.h`, then every `src/*.c` by name, with quoted includes and
`#pragma once` removed. Definitions of public functions (those the header
declares `TXML_API`) get `TXML_API`; every other function, definition or
prototype, becomes `TXML_PRIVATE`, which is `static` unless the embedder
defines it otherwise, so a private helper never becomes a linker symbol.

What the rewrite cannot do is rename. The result is one translation unit, so a
typedef, macro, static function or static object that two modules define
differently is a redefinition. That is a rule for `src/` (AGENTS.md): prefix a
module-local name that another module also uses. The compiler reports a
violation the moment the amalgamation is built. State tables that are shared
between modules (`extern` data) stay external; they are data, not functions, and
are not exported by the API.

`amalgamate-test` is the proof. It builds `include/txml.c` under `-Wall -Wextra
-Werror -pedantic`, holds it to the freestanding check, and links every test
that uses only the public API (all but `doc_test`) against it instead of the
modules.

## The freestanding and source checks

Two checks keep AGENTS.md's hard constraints from rotting; both run first in
`mise run test`.

- `freestanding` compiles every module with `-ffreestanding -fno-builtin
  -fno-stack-protector` at each optimisation level, links them with `ld -r`, and
  fails on any undefined symbol. The compiler calls `memset`/`memcpy` on its own
  for large struct initialisers, which is why the library zeroes through
  `txml_text_zero` and why only an object-level check can see it. Unoptimised
  clang is excluded, because it lowers every aggregate copy through `memcpy`;
  see the task header. It also rejects any include but `stdint.h`, `stddef.h`
  and `stdbool.h`.
- `lint-src` enforces constraint 10 (at most three parameters, counted on every
  function, prototype and function-pointer declarator in `src/` and the header)
  and constraint 5 (no recursion: a cycle in the direct-call graph built from
  the sources). Calls through function pointers are not edges; the parser's
  state dispatch is a loop.

## The API description

`api` runs the core's pipeline (`tinyactors-core/docs/internal/api-ir.md`): clang
parses the header for pinned targets with `TXML_API_EXTRACT` turning the
annotation macros into attributes, structural rules and annotations become a
model, layouts and constants are measured per target, the result is validated
against `include/txml.api.schema.json`, and it is compiled back against the
header. "Implemented" comes from the symbols `include/txml.c` exports. The
Ruby is `mise/lib/api*`, copied from the core with the prefix changed and the
adaptations below; the core's copy is the original.

Where txml's vocabulary differs from the core's:

- Strings and bytes are `{ptr, length}` (the core's are `{ptr, len}`).
- `TXML_BUFFER(capacity)` takes one argument; there is no "needed" output.
- `TXML_VALUE` marks a copyable value type and states no facts.
- Handle outputs (`T **`) carry no annotation; the release function is the one
  whose parameter is marked `TXML_DESTROYS`. `txml_arena` is a plain struct, not
  a handle, so `txml_arena_destroy` is unmarked.
- Lifetime scopes add `document`, `parser`, `drain` and `process` to the core's.
  `TXML_LIFETIME` also applies to a function that returns a string or bytes view.
- `TXML_FLAGS(set)` may annotate a function that returns integer flags.
- A function returning `bool` reports success as `true`; an enum constant marked
  `TXML_SUCCESS` (`TXML_MOVED` for `txml_move`) does the same for enums.
- Compound-literal macros such as `TXML_LIMITS_DEFAULT` are values and are not
  extracted as constants.

`api-test` checks the description is current; that every `TXML_API` function is
in it, public, implemented, and taken by `tests/api_surface.c`, and the reverse;
and that the extractor rejects what it should, against the fixture headers in
`tests/api/` (each states its expectations in `// expect` lines).
