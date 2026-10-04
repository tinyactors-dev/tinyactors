# tinyparse

**A parser runtime for C that turns grammar *data* into a running parser, with
no code generation and no build step.** Hand `tinyparse` the bytes of a `.scm`
grammar and the bytes of your input; it hands you back a streaming parser, a
concrete syntax tree, and structured diagnostics. The core is freestanding (no
libc, no `malloc`, no filesystem, no threads, no clock, no IO), so it runs
inside whatever memory you grant it.

It is a pocket knife for programs that need to parse something without dragging
in a generator toolchain, a scripting runtime, or a hosted C environment. The
execution model is actor-shaped internally and powered by [`ax`](../../src/ax/),
but the public surface is ordinary C: include one header, link one library.

tinyparse was called pocketparse until it moved out of the ax repository into
the tinyactors monorepo; HISTORY.md tells how it moved.

## Quickstart

Compile a grammar, parse some input, and dump the tree, all from the CLI:

```sh
printf '(a (b c))' | tinyparse run examples/sexpr/sexpr.scm --input - --dump-tree
```

```text
Source [0-9] [0,0]-[0,9]
  Form [0-9] [0,0]-[0,9]
    List [0-9] [0,0]-[0,9]
      open: ( [0-1] [0,0]-[0,1]
      Form [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      ...
```

The grammar that produced it is a few lines of S-expressions. A list contains
forms, and a form can be a list again, so the recursion in the rules is the
recursion in the language:

```scheme
(grammar sexpr
  start: Source

  (token Atom (regex "[^() \t\r\n;]+"))

  (rule Source (seq (repeat Form) EOF))
  (rule Form   (choice Atom List))
  (rule List   (seq (field open: "(") (repeat Form) (field close: ")"))))
```

Validate a grammar without parsing anything with `tinyparse check <grammar>`,
and read the full manual with `tinyparse man`.

## Embedding it

The library is the product; the CLI is just a hosted shell around it. A host
grants memory, compiles a grammar once, then pushes input and walks the tree
through the cursor API:

```c
#include <tinyparse.h>

tinyparse_engine *engine = tinyparse_open(configuration);

tinyparse_grammar *grammar = 0;
tinyparse_compile_scheme(engine, grammar_source, &grammar);

tinyparse_parser *parser = 0;
tinyparse_parser_new(engine, grammar, start_rule, &parser);

tinyparse_push(parser, first_chunk);   /* feed bytes as they arrive */
tinyparse_push(parser, next_chunk);
tinyparse_finish(parser);

const tinyparse_tree *tree = tinyparse_parser_tree(parser);

tinyparse_tree_cursor *cursor = 0;
tinyparse_tree_cursor_new(tree, &cursor);
if (tinyparse_tree_cursor_goto_first_child(cursor)) {
    tinyparse_symbol kind = tinyparse_tree_cursor_kind(cursor);
    uint32_t start = tinyparse_tree_cursor_start_byte(cursor);
    /* read node kinds, field names, and byte spans straight off the tree */
}
```

`tinyparse` never allocates outside the memory grant you hand it, and the
public API takes length-prefixed slices rather than null-terminated strings, so
nothing in it assumes a C standard library underneath.

## Generating a C parser

Runtime compilation is the default and the point. But once a grammar is settled
you can compile it to C ahead of time, and the emitted parser is faster than the
runtime VM:

```sh
tinyparse c grammar.scm --parser-lib --name mylang --out mylang.c
```

The output is the recursive-descent parser a person would write from the grammar
by hand. Each rule and token becomes one C function, named after it
(`parse_arrow_function`, `scan_identifier`); the literal bytes, FIRST-set branch
tests, and per-token lexers all bake into straight-line C. It carries
no bytecode interpreter and no packrat memo, so it reads, and runs, like
hand-written descent. It produces the same trees as the runtime VM, so you can
develop against the VM and ship the compiled parser, and it is depth-guarded:
adversarial deep input returns a parse error rather than overflowing the C stack.

`--name <prefix>` renames every emitted symbol, so two generated parsers can link
into one program without colliding: a host markup grammar and the language inside
its code fences, for instance. The [`embed`](examples/embed/) example does exactly
that. `tinyparse c` also has a `--parser` mode (a standalone driver) and a
default *module* mode that emits just the grammar tables to link against
`libtinyparse`.

To ship with **no dependency on tinyparse at all**, seed the amalgamation:

```sh
tinyparse init my-parser --amalgamation   # drops tinyparse.c + tinyparse.h
cc main.c tinyparse.c -o my-parser         # one cc, no library, no .scm reader, no CLI
```

The amalgamation folds the engine and `ax` into a single `tinyparse.c`. After
that one generation step, the artifact is ordinary C.

## When to use tinyparse, and when not to

`tinyparse` sits at roughly tree-sitter's level of abstraction but with a
different center of gravity. Tree-sitter ships pre-generated language modules and
a compact runtime; `tinyparse` compiles and runs grammars as data. That trade
is the whole decision, so here is the honest version of it.

**Reach for `tinyparse` when:**

- **Grammars are data at runtime.** You want to accept, edit, or generate a
  grammar while the program runs and parse with it immediately, with no
  `grammar.js` → generated `parser.c` → C compiler → linked artifact pipeline in
  the loop.
- **The environment is constrained.** A sandbox, a kernel module, a Wasm guest,
  or bare-metal firmware where there is no libc, no `malloc`, and no filesystem
  to assume. `tinyparse` runs inside an allocator you pass it.
- **You want one file.** The library amalgamates to a single `tinyparse.c`
  plus a header.
- **You traverse more than you parse.** The CST is one contiguous array and the
  cursor is an index into it, so walking the tree is a linear sweep. On pure
  traversal `tinyparse` is faster than tree-sitter (see below).

**Reach for [tree-sitter](https://tree-sitter.github.io/) instead when:**

- **You need maximum parse throughput from grammars supplied at runtime.**
  Tree-sitter parses about 1.5× faster than tinyparse's runtime VM, because it
  runs pre-generated GLR tables while the VM interprets bytecode it compiled from
  the grammar on the fly. (Once a grammar is fixed, tinyparse's generated C
  parser passes tree-sitter — see Performance — so this edge is really about the
  runtime-compiled path.)
- **You are building an editor or IDE.** Incremental reparsing — re-parsing only
  the edited region of a huge file on every keystroke — is tree-sitter's reason
  to exist. `tinyparse` parses a document from scratch.
- **You want an existing language off the shelf.** Tree-sitter has a large
  ecosystem of mature, battle-tested grammars. `tinyparse` ships a handful
  (`std.javascript`, `std.tsx`, `std.json`) and expects you to bring or write
  your own.

## Performance, honestly

We measured rather than guessed, and report what the meter said. Against the
[tree-sitter JavaScript test corpus](reference/tree-sitter-javascript/) (116
documents, both runtimes in-process, median over 200 iterations):

| What            | tinyparse  | tree-sitter  | gap             |
| --------------- | ------------ | ------------ | --------------- |
| Parse to a tree | 4.8 ms       | 3.3 ms       | 1.5× slower     |
| Walk every node | 24 ns/node   | 96 ns/node   | **4.0× faster** |

On parse, tree-sitter still wins, but the gap is small and structural: it runs
pre-generated GLR parse tables, while `tinyparse`'s VM compiles the grammar to
bytecode at runtime and interprets it. That is the price of taking a grammar as
data; the generated C parser below pays it back.

On traversal `tinyparse` wins, and that is the contiguous CST earning its
keep. It builds its whole tree eagerly at `finish` and freezes it into one
array, so a cursor walk is a linear sweep the prefetcher loves; tree-sitter's
pointer-linked tree costs more per node to navigate.

Those numbers are the runtime VM. On a real codebase — the ReactJS source tree,
1070 TypeScript/JSX files, 9.5 MB — the **generated C parser** changes the parse
picture (parse-only throughput, higher is better):

| parser (React/TSX, parse-only)        | throughput |
| ------------------------------------- | ---------- |
| tsc (the TypeScript compiler)         | 12.3 MB/s  |
| tinyparse (generated C, persistent) | 12.0 MB/s  |
| tinyparse (generated C, fresh)      | 8.3 MB/s   |
| tree-sitter                           | 6.8 MB/s   |
| tinyparse (bytecode VM)             | 4.7 MB/s   |

The generated C parser **passes tree-sitter** (1.2× parsing each file in a fresh
engine, 1.8× reusing one across the tree) and draws level with the TypeScript
compiler's own parser, at 1.78× the VM. (meriyah is faster still at 29 MB/s, but
only on the ~half of the corpus that is plain JS+JSX, so it is not a like-for-like
row; `tsgo` was not re-run this pass.) The traversal lead also widens on real
code: collecting ~29,000 function definitions, tinyparse walks **5.5× faster**
than tree-sitter (44 ms vs 239 ms). So tree-sitter's parse edge holds against the
runtime-compiled VM, not against the ahead-of-time parser; on traversal
tinyparse wins outright.

The cost is footprint at compile time. One corpus pass has a 14 MiB high-water
mark, but 13.5 MiB of that is the one-time grammar compile; the parse arena is
0.6 MiB. Tree-sitter, carrying pre-generated tables, peaks at 2.9 MiB.

Deeply nested input that would make a naive PEG backtrack catastrophically stays
bounded, so parses do not blow up in time or memory. The full methodology, the
six-parser React comparison, and the profile are in
[`docs/performance.md`](docs/performance.md); reproduction is in
[`DEVELOPMENT.md`](DEVELOPMENT.md).

## Build

Install a release tarball and link against it as shown in
[`INSTALL.md`](INSTALL.md), or build it from source: plain C, built with
**Zig 0.15.1**:

```sh
mise install && mise run build    # libtinyparse + the CLI + examples
bash build.sh all                 # the same, without mise
```

The test suite, the correctness gates, and the benchmarks are in
[`DEVELOPMENT.md`](DEVELOPMENT.md).

## Deliverables

- `dist/<platform>/libtinyparse.{a,so|dylib}` plus the `tinyparse` CLI for
  `linux-x86_64` and `macos-arm64`.
- A single-file amalgamation, `tinyparse.c` plus the header.
- An embedded manpage: `tinyparse man`.

## Examples

Each subdirectory under [`examples/`](examples/) teaches one capability with a
grammar that runs and a README that shows it running. Some need only the CLI;
others are host programs that link the library and walk the tree.

| Example | The lesson |
|---------|------------|
| [`sexpr`](examples/sexpr/)     | The canonical recursive grammar; read this one first. |
| [`tcl`](examples/tcl/)         | Command substitution and brace groups nesting to any depth. |
| [`recover`](examples/recover/) | Grammar-authored error recovery with a zero-width repair node. |
| [`import`](examples/import/)   | One grammar pulling tokens and rules from another at compile time. |
| [`ebnf`](examples/ebnf/) → [`uri`](examples/uri/) | A meta-grammar: parse EBNF, emit a `.scm`, then parse RFC 3986 URIs with it. |
| [`rewriter`](examples/rewriter/) | A codemod that rebuilds output from the spans the tree records. |
| [`jit`](examples/jit/)         | Native x86-64 emitted by recursing over the cursor, with no intermediate AST. |
| [`embed`](examples/embed/)     | Two separately compiled parsers in one binary, linked without symbol collision. |

The full index, with the rest of the examples, is in
[`examples/README.md`](examples/README.md).

## Where things live

| File | Contents |
|------|----------|
| [`tinyparse-design.md`](tinyparse-design.md) | canonical design: API, memory model, tree storage, the actor topology |
| [`include/tinyparse.h`](include/tinyparse.h) | the public header, the only file you include |
| [`INSTALL.md`](INSTALL.md) | installing a release tarball and linking against it |
| [`DEVELOPMENT.md`](DEVELOPMENT.md) | building from source, the test suite, the gates, and the benchmarks |
| [`docs/`](docs/) | architecture, the bytecode VM, the grammar reference, the PEG-vs-GLR rationale, and the performance write-up |

This is V1, pinned at version `0.1.0`. It parses real languages correctly and
honestly; the design notes mark what is in scope and what is deferred.
