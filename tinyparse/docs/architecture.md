# tinyparse architecture

This document describes how `tinyparse` is built and how its pieces fit. The
*what* and *why* of the public contract live in
[`../tinyparse-design.md`](../tinyparse-design.md); this is the *how*. The
hard constraints a change must keep are in [`../AGENTS.md`](../AGENTS.md).

## Layers

```
        ┌─────────────────────────────────────────────┐
        │  tinyparse CLI  (hosted: libc, argv, I/O)   │  src/cli.c
        ├─────────────────────────────────────────────┤
        │  libtinyparse  (freestanding: no libc)      │  src/*.c, include/tinyparse.h
        │   reader → IR → bytecode → VM → CST         │
        │   arena allocator over the host's grant     │  src/arena.c
        └─────────────────────────────────────────────┘
```

The library is freestanding C11. It includes only `stddef.h` and `stdint.h`,
and the archive leaves undefined only `memcpy`, `memset`, `memmove` and
`memcmp`, which a compiler may emit calls to. It receives a memory grant from
the host and never allocates outside it. The CLI is the hosted shell around it:
it reads files, renders diagnostics, and prints. Nothing under `src/` except
`cli.c` calls libc.

The library depends on no other code. It came out of the `ax` repository, where
it linked ax for its arena allocator and ran each parse inside an ax actor;
[`../HISTORY.md`](../HISTORY.md) tells how both were replaced.

## Memory

All memory comes from `tinyparse_memory`: a base block, its size, and an
optional `grow` callback that hands back a fresh block (never a `realloc`, so
nothing moves). [`src/arena.c`](../src/arena.c) turns the grant into arenas:

- **`tp_pool`** owns the grant. It bump-allocates from the current host block,
  asks `grow` for a new one when it is spent, and keeps a free list of chunks
  that arena resets hand back.
- **`tp_chunk`** is one contiguous run the pool hands an arena.
- **`tp_arena`** bumps through a chain of chunks from one pool.

The engine owns one long-lived arena. Each parser gets a child arena on the same
pool, and `tinyparse_parser_free` resets it, so its chunks go back on the free
list and the next parser reuses them. An engine that parses many documents stays
near the memory of one (`gate:reclaim` holds it to twice one parse).

## The parser lifecycle

A parser is a four-state machine over the parser struct, declared as
`tp_parser_state` in [`src/internal.h`](../src/internal.h) (with a diagram) and
driven in [`src/parse.c`](../src/parse.c):

| State | Reached by | What happens there |
|-------|------------|--------------------|
| `ACCEPTING` | `tinyparse_parser_new` | `tinyparse_push` buffers each chunk into the parser's arena |
| `DROPPED` | a push that cannot buffer its bytes | later pushes still buffer, but the input is known incomplete |
| `FINISHED` | finish or run from `ACCEPTING` | the executor ran; the tree and diagnostics exist |
| `FAILED` | finish or run from `DROPPED` | the executor never ran; finish returns the push's failure |

`tp_parser_run(parser, executor)` is the one finishing transition. Three callers
share it with different executors: `tinyparse_finish` runs the bytecode VM, a
generated parser's `<prefix>_run` runs its generated code, and an injected
sub-parse runs whichever of the two is registered for its language. So a
generated parser and the VM refuse a dropped input the same way.

## The pipeline

1. **Reader** ([`reader.c`](../src/reader.c)): parse `.scm` bytes into
   Scheme-shaped forms.
2. **Grammar** ([`grammar.c`](../src/grammar.c)): validate, intern names, resolve
   `(import …)` and `(extends …)` through the module resolver, and build the IR:
   named rules and tokens.
3. **Tokens** ([`token.c`](../src/token.c)): compile token expressions and
   lexical goals into matchers.
4. **Bytecode** ([`vmcompile.c`](../src/vmcompile.c)): lower each rule to flat
   bytecode.
5. **VM** ([`vm.c`](../src/vm.c)): run the bytecode over the buffered input on
   an arena-backed frame stack (no C recursion per rule call), with an adaptive
   rule memo that wakes only when a parse goes super-linear.
6. **Tree** ([`tree.c`](../src/tree.c)): append node records to one contiguous
   array and freeze it at finish; cursors walk it.
7. **Diagnostics** ([`diag.c`](../src/diag.c)): structured errors with spans and
   recovery nodes.

Beside the pipeline: [`inject.c`](../src/inject.c) re-parses a span with an
embedded grammar and splices the result, [`query.c`](../src/query.c) runs
queries as parsers over the linearized tree, [`pack.c`](../src/pack.c) reads and
writes packed `.tpg` grammars, and [`emit.c`](../src/emit.c) and
[`emitparser.c`](../src/emitparser.c) generate C.

## Generated parsers

`tinyparse c --parser-lib` walks the IR rather than the bytecode and emits the
recursive-descent parser a person would write: one C function per rule and
token. It recurses natively under a depth guard (`TP_PARSER_DEPTH_LIMIT`),
measured to fit a 512 KiB stack. A generated parser includes `internal.h` and
links against `libtinyparse.a` for the tree builder, the diagnostics and the
arena. Every generated parser must build the VM's tree byte for byte, which
`gate:cc-diff` and `gate:diag-diff` check.

## Toolchain

tinyparse is a project of the tinyactors monorepo and uses its toolchain: the
root `mise.toml` pins clang 22, LLVM's binutils, zig 0.16 and Ruby. Each task is
a Ruby file task in [`../mise/tasks/`](../mise/tasks/), sharing
[`../mise/lib/tinyparse.rb`](../mise/lib/tinyparse.rb):

- What runs on this machine (the library, CLI, harness and examples in
  `build/`) is compiled by the pinned clang with `-std=c11 -O2 -Wall -Wextra
  -pedantic -Werror`; the library adds `-ffreestanding -fno-builtin`.
- What is shipped (`dist/x86_64-linux`, `dist/aarch64-macos`) is cross-built
  with zig cc from any host and its symbols are checked: the shared library
  exports exactly the 48 functions `include/tinyparse.h` declares with
  `TINYPARSE_API`.
- The CLI embeds three generated headers, written by `mise run generate` into
  `build/generated/`: the manual page, the standard grammars, and the
  amalgamation.

## Tests

`mise run test` runs the 13 correctness gates in
[`../mise/tasks/gate/`](../mise/tasks/gate/) side by side, then the suite of
`test/suite/**/*.test` cases interpreted by `test/harness.c` against the empty
ledger `test/xfail.list`. [`../DEVELOPMENT.md`](../DEVELOPMENT.md) lists what each
gate holds.

## Source layout

```
tinyparse/
  tinyparse-design.md   the design (the contract)
  include/tinyparse.h   the public API
  src/                  libtinyparse, and cli.c
  grammars/std/         the standard grammars the CLI embeds
  examples/             one lesson each, each pinned by a suite case
  test/                 harness.c, suite/**/*.test, xfail.list, gates/
  bench/                benchmark harnesses and scripts (opt-in)
  mise/                 the tasks and their Ruby library
  docs/                 this document, the references, the decision log
  docs/archive/         the design notes from before the move out of ax
  reference/PINS.txt    pinned third-party references (fetched, gitignored)
```
