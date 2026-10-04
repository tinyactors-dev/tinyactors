# Architecture

This document is for a contributor who is about to change the library. It
explains how the pieces of tinyparse fit together and why they are shaped the
way they are, so that you can tell where a change belongs and which properties
it has to keep. The functions a host program calls are specified in the
[C API reference](../reference/c-api.md); the design choices behind the
grammar language and the tree are in [Design of tinyparse](design.md). The
commands that build and test the project are in
[Developing tinyparse](../../DEVELOPMENT.md).

## Layers

```text
        ┌─────────────────────────────────────────────┐
        │  tinyparse CLI  (hosted: libc, argv, I/O)   │  src/cli.c
        ├─────────────────────────────────────────────┤
        │  libtinyparse  (freestanding: no libc)      │  src/*.c, include/tinyparse.h
        │   reader → IR → bytecode → VM → CST         │
        │   arenas over the host's memory grant       │  src/arena.c
        └─────────────────────────────────────────────┘
```

The middle row of the library is the pipeline described below: the reader's
forms become an intermediate representation (IR), the IR becomes bytecode,
and the VM runs the bytecode to build the concrete syntax tree (CST).

The library is freestanding C11. A host program may run where there is no C
library, may want every allocation to come from memory it controls, and may
run two engines in one process. A library that called `malloc`, read files or
kept global state would fail all three, so the library leaves every decision
about memory and I/O to the host. Its sources include only `stddef.h` and
`stdint.h`, through `include/tinyparse.h`. The one exception is `src/vm.c`,
which includes `assert.h` when `TP_DEBUG_INTERN_GUARD` is defined, a
debugging switch that no build task sets. The compiled archive may leave
undefined only `memcpy`, `memset`, `memmove` and `memcmp`, the functions a
compiler emits calls to for aggregate copies and zeroing.

The CLI is the hosted shell around the library: it reads files, finds grammar
packages on disk, renders diagnostics and prints trees. Every libc call in
`src/` is in `src/cli.c`.

The library depends on no other code. It began inside another project, which
supplied its allocator and ran each parse in an actor; [HISTORY.md](../../HISTORY.md)
records how both were replaced.

## Memory

All memory comes from the `tinyparse_memory` the host passes to
`tinyparse_open`: a base block, its size, and an optional `grow` callback that
returns a fresh block. `grow` never reallocates. Tree nodes, interned names,
compiled grammars and cursors point into memory the library handed out
earlier, and a `realloc` that moved a block would leave those pointers
dangling. With fresh blocks only, an address stays valid until the arena that
owns it is reset.

[`src/arena.c`](../../src/arena.c) turns the grant into arenas with three
structures:

- **`tp_pool`** owns the grant. It bump-allocates from the current host block,
  calls `grow` when the block is spent, and keeps a free list of chunks that
  arena resets hand back.
- **`tp_chunk`** is one contiguous run the pool hands to an arena.
- **`tp_arena`** bump-allocates through a chain of chunks drawn from one pool.

An arena never frees one object; it is reset as a whole. The engine owns one
long-lived arena for grammars and other engine-lifetime data. Each parser gets
a child arena on the same pool, and `tinyparse_parser_free` resets it, which
returns its chunks to the free list for the next parser. An engine that
parses many documents therefore stays near the memory of one: `gate:reclaim`
holds 12 parses of a 264,060-byte JavaScript file to under twice the peak of
one parse. On 2026-10-04, on a 4-core 2.10 GHz Xeon running x86-64 Linux, one
parse peaked at 108,396 KB of resident memory and 12 parses at 142,444 KB.

## The parser lifecycle

A parser is a four-state machine over the parser struct, declared as
`tp_parser_state` in [`src/internal.h`](../../src/internal.h) (with a diagram)
and driven in [`src/parse.c`](../../src/parse.c):

| State | Reached by | What happens there |
|-------|------------|--------------------|
| `ACCEPTING` | `tinyparse_parser_new` | `tinyparse_push` copies each pushed block of input into the parser's arena |
| `DROPPED` | a push that cannot buffer its bytes | later pushes still buffer, but the input is known to be incomplete |
| `FINISHED` | finishing from `ACCEPTING` | the executor ran; the tree and diagnostics exist |
| `FAILED` | finishing from `DROPPED` | the executor never ran; finishing returns the push's failure |

`FINISHED` and `FAILED` are final: a push in either returns
`TINYPARSE_INVALID`.

`tp_parser_run(parser, executor)` is the one finishing transition. Three
callers share it with different executors: `tinyparse_finish` runs the
bytecode VM, a generated parser's `<prefix>_run` runs its generated code, and
an injected region runs the generated parser registered for its language, or
the VM when none is registered. Because all three go through the same
transition, a generated parser and the VM refuse a dropped input the same way.
Running an executor over a buffer with pushed input missing would produce a tree
of the wrong input and report success.

## The pipeline

A grammar's text becomes a compiled grammar, and the compiled grammar turns
an input into a tree, in seven stages:

1. **Reader** ([`reader.c`](../../src/reader.c)) reads `.scm` bytes into
   S-expression forms. It reads data and evaluates nothing.
2. **Grammar** ([`grammar.c`](../../src/grammar.c)) validates the forms,
   interns every name to a dense symbol, resolves `import` and `extends`
   through the host's module resolver, and builds the IR: named rules and
   tokens.
3. **Tokens** ([`token.c`](../../src/token.c)) compiles token patterns and
   lexical goals into matchers, with a regex engine of its own.
4. **Bytecode** ([`vmcompile.c`](../../src/vmcompile.c)) lowers the IR to a
   flat instruction array, one instruction per IR node. The first parse
   lowers it and caches it on the compiled grammar for its start rule.
5. **VM** ([`vm.c`](../../src/vm.c)) runs the bytecode over the buffered
   input. Rule calls run on a frame stack in the parser's arena, so nesting
   depth costs arena memory, not C stack. A rule memo bounds the cost of PEG
   backtracking. It stays off until a parse has made more than
   32 × input length + 65,536 rule calls, so a parse that does not backtrack
   heavily never pays for it.
6. **Tree** ([`tree.c`](../../src/tree.c)) appends node records to one
   contiguous array in pre-order, each linked to its parent, first child and
   next sibling, so a cursor walks the tree without allocating.
7. **Diagnostics** ([`diag.c`](../../src/diag.c) and the collector in
   [`util.c`](../../src/util.c)) record each problem as a structure: severity,
   code, message, labeled byte ranges, help and note. The library never
   formats them for display.

Around the pipeline:

| File | What it does |
|------|--------------|
| [`engine.c`](../../src/engine.c) | opens and closes an engine over the grant, holds the fault text, and stores class bindings, parameters, the module resolver and registered languages |
| [`parse.c`](../../src/parse.c) | the parser lifecycle and the input buffer |
| [`package.c`](../../src/package.c) | reads a grammar package's `package.scm` and applies its `params:` before the grammar compiles |
| [`util.c`](../../src/util.c) | string comparison, the interner, the node builder and the diagnostic collector |
| [`inject.c`](../../src/inject.c) | parses an injected region with another grammar and splices the result into the host tree |
| [`query.c`](../../src/query.c) | flattens a tree to an enter, leaf and exit token stream and runs each query pattern as a parser over it |
| [`pack.c`](../../src/pack.c) | writes and reads packed `.tpg` grammars |
| [`nodetypes.c`](../../src/nodetypes.c) | renders a compiled grammar's node kinds and fields as JSON |
| [`emit.c`](../../src/emit.c) | writes a generated module: the packed grammar as a C array |
| [`emitparser.c`](../../src/emitparser.c) | writes a generated parser |
| [`version.c`](../../src/version.c) | the version string |

Each generator builds its C text in two passes, a measuring pass and a
filling pass, because an arena block cannot grow once it is handed out.

## Generated parsers

`tinyparse c --parser` and `--parser-lib` walk the IR of named rules and
tokens and emit the recursive-descent parser a person would write: one C
function per rule (`json_parser_parse_Value_15` in the std.json parser of
`examples/embed`) and one per token (`json_parser_scan_String_3`), with
sequences, choices and repeats as ordinary control flow. A generated parser
carries no rule memo.

It recurses natively, so deep input could overflow the C stack. Each rule
body is wrapped in a depth guard, `TP_PARSER_DEPTH_LIMIT` (1000 nested rule
calls). The limit is measured against the generated parsers with the largest
frames, std.javascript and std.tsx, which need between 256 and 384 KiB of C
stack at the limit, under the 512 KiB bound the gates run with. Past the limit
the rule fails and the parse ends in a syntax error instead of a crash.

A generated parser includes `src/internal.h` and links against
`libtinyparse.a`, calling the same tree builder, token matcher, extras
handling, recovery and injection functions the VM calls. That shared code is
what makes the next constraint achievable: every generated parser builds the
VM's tree, byte for byte.

## Hard constraints

A change that breaks one of these is wrong even when the test suite passes.
Most of them have a gate that fails when they break; the gates are listed in
[Developing tinyparse](../../DEVELOPMENT.md).

| Constraint | What it means | Checked by |
|------------|---------------|------------|
| The library is freestanding | no allocator, I/O, filesystem, threads or clock outside `src/cli.c`; only the four memory functions left undefined | `mise run dist` |
| All memory comes from the grant | `grow` returns a new block; nothing moves | no gate of its own; `gate:reclaim` and `gate:reuse-regression` check reuse of the grant |
| No global mutable state | a `static` in the library or in a generated parser is a `const` table or a function | `gate:no-statics` |
| The library depends on no other project | the library links no other project's code | `gate:vm-diff`, the `dist/self-contained` case |
| The public API is `include/tinyparse.h` alone | every public function is declared with `TINYPARSE_API`; strings are length-prefixed `tinyparse_string` values; internal names start with `tp_` | `mise run dist`, `gate:exports` |
| Deep input does not overflow the C stack | the VM's frames live in the arena; a generated parser stops at its depth guard | `gate:vm-nocrash`, `gate:parser-nocrash` |
| Every engine builds the same tree | the VM with its rule memo off, adaptive or forced on, and every generated parser, agree on the tree, the diagnostics and the exit status | `gate:vm-diff`, `gate:cc-diff`, `gate:diag-diff` |

## Toolchain

tinyparse is one project of the tinyactors monorepo and uses its toolchain:
the root `mise.toml` pins clang 22, LLVM's binutils, zig and Ruby, and every
task is a Ruby file task under [`mise/tasks/`](../../mise/tasks/) that shares
[`mise/lib/tinyparse.rb`](../../mise/lib/tinyparse.rb). Nothing is taken from
`PATH`, so every machine builds with the same compiler and tools.

The library and the programs around it are compiled with different flags
because they make different promises. The library adds `-ffreestanding
-fno-builtin` to the common `-std=c11 -O2 -Wall -Wextra -pedantic -Werror`,
so the compiler assumes no hosted C library and gives no library function
name a built-in meaning. It can still emit calls to the four memory
functions for struct copies and zeroing, which is why those four are the
only undefined symbols allowed. The CLI, the test harness and the examples
are hosted programs and use the common flags alone.

What is shipped is cross-built with zig cc for `x86_64-linux` and
`aarch64-macos` from any host, so one machine produces every release
artifact. The build then reads the artifacts' symbol tables to check the
freestanding and public-API constraints above.

The CLI embeds three generated headers, which `mise run generate` writes to
`build/generated/`: the manual page, the standard grammars and the library
amalgamation. They make the CLI one self-contained file: `tinyparse man`
needs no man page on disk, a grammar name such as `std.json` resolves without
a grammar directory, and `tinyparse init` and `tinyparse vendor` write the
library source into a project from bytes in the binary.
