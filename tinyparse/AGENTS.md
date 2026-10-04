# Working on tinyparse

tinyparse is a freestanding C parser runtime: it compiles grammar data (`.scm`)
into a running parser, a concrete syntax tree and diagnostics, and its CLI
(`tinyparse`) also generates C parsers from a grammar. It is a base library
and tool beside `tinyxml`: it depends on nothing else in this repository, and
nothing here depends on it yet. It came from the `ax` repository, where it was
called pocketparse; `HISTORY.md` tells how it moved. The design is in
[tinyparse-design.md](tinyparse-design.md), the contributor's guide in
[DEVELOPMENT.md](DEVELOPMENT.md).

## Hard constraints

These are not style preferences. A change that breaks one of them is wrong,
even when the suite passes; most have a gate that fails when it is broken.

1. **The library is freestanding.** `src/*.c` other than `src/cli.c` include
   only `stddef.h` and `stdint.h` (through `tinyparse.h`; `vm.c`'s
   `assert.h` is behind the debug-only `TP_DEBUG_INTERN_GUARD`), and the archive may leave undefined only
   `memcpy`, `memset`, `memmove` and `memcmp`, which a compiler emits calls
   to (`mise run dist` checks it). No allocator, I/O, filesystem, threads or
   clock. The CLI is the hosted shell around it.
2. **All memory comes from the host's grant** (`tinyparse_memory`); `grow`
   hands back a new block, never a `realloc`, so nothing moves.
3. **No global mutable state.** State lives on the engine or the parser; a
   `static` is a `const` table or a function. `gate:no-statics`.
4. **Depends on neither ax nor tinyactors.** No actor runtime, no symbol of
   one. `gate:vm-diff` checks the sources, `dist/self-contained` the binaries.
5. **The public API is `include/tinyparse.h` alone,** every function declared
   with `TINYPARSE_API`. Strings are length-prefixed `tinyparse_string`s,
   never NUL-terminated. Internal names start with `tp_`; a function the CLI
   shares with the library is internal too. `gate:exports` holds an embedded
   artifact to exactly the API.
6. **Deep input does not overflow the C stack.** The VM runs rule calls on an
   arena-backed frame stack, and a generated parser is bounded the same way.
   `gate:vm-nocrash`, `gate:parser-nocrash`.
7. **Every engine builds the same tree.** The VM with its rule memo off, on
   or adaptive, and every parser `tinyparse c` generates, agree byte for byte
   on tree, diagnostics and exit code. `gate:vm-diff`, `gate:cc-diff`,
   `gate:diag-diff`. A change that alters a tree updates the goldens on
   purpose; a performance change leaves every gate green unchanged.

## Naming

- `tinyparse_` for public types and functions, `TINYPARSE_` for public macros
  and constants, `tp_` for everything internal.
- Spell words out; the repository's acronym rule (root `AGENTS.md`) applies.

## Layout

- `include/tinyparse.h` — the public API, the only public header.
- `src/*.c`, `src/internal.h` — the library; `src/cli.c` — the CLI.
- `grammars/std/` — the standard grammars the CLI embeds (`std.json`,
  `std.javascript`, `std.shell`, ...); our own grammars only. A grammar ported
  from another project is a reference pinned in `reference/PINS.txt`, never
  copied in.
- `examples/` — one lesson each, each pinned by a case under `test/suite/`.
- `test/suite/**/*.test` — the data-driven suite; `test/harness.c` runs it,
  `test/xfail.list` is the ratchet ledger (empty: everything passes).
- `test/gates/` — C harnesses the gates compile.
- `bench/` — benchmark harnesses and scripts (opt-in, never in `test`).
- `docs/` — design notes and the decision log (`docs/decisions.md`).
- `.claude/skills/` — adding a std grammar, left-factoring, profiling.

## Tasks

Every task is a Ruby file task in `mise/tasks/`, with the root's pinned
toolchain (`Toolchain::Target.host` for what runs here, zig for `dist`).

- `mise run build` — the library, CLI, harness and examples into `build/`.
- `mise run test` — the suite, after every gate in `mise/tasks/gate/` (which
  mise runs side by side as dependencies, a second or two each).
- `mise run test --full` — also each gate's exhaustive version (every corpus
  input through both engines, 300 parses for reclaim, ...). The root's
  `mise run test:full` passes it down.
- `mise run gate:<name> [--full]` — one gate.
- `mise run dist`, `amalgamate`, `release` — what is shipped.
- `mise run bench:<name>` — benchmarks and corpus counts; `bench:fetch` first.

## Tests

- A new behaviour gets a case under `test/suite/`. Never delete, skip or
  weaken a case, or add one to `test/xfail.list`, to get green.
- A gate is fast unless `--full` is passed: give a new exhaustive check a
  `#USAGE flag "--full"` and keep a small version of the same property in the
  fast pass, as `cc-diff` and `reclaim` do. A gate works in `gate.scratch`,
  its own temporary directory, so gates run side by side.
- When you add a directory the tests read, add it to `#MISE sources` of
  `mise/tasks/test`.
