# tinyparse

The tinyparse library compiles grammar data at run time into a streaming
parser. A host program passes it the bytes of a `.scm` grammar and then the
bytes of an input, and gets back a concrete syntax tree (CST) and structured
diagnostics. The library is freestanding C: it allocates only from
the memory its host grants it, does no I/O, and needs from the platform only
the memory functions listed in
[Release artifacts](docs/reference/artifacts.md#library-symbols). A grammar
that has stopped changing can also be compiled to C ahead of time with
`tinyparse c`. The `tinyparse` command wraps the library for use from a shell.

## A first look

This grammar describes s-expressions such as `(a (b c))`:

```scheme
(grammar sexpr
  start: Source

  (token Atom (regex "[^() \t\r\n;]+"))

  (rule Source
    (seq (repeat Form) EOF))

  (rule Form
    (choice Atom List))

  (rule List
    (seq "(" (repeat Form) ")")))
```

Saved as `sexpr.scm`, it parses input with no build step. The command needs
`tinyparse` on your `PATH`; [Install tinyparse](docs/how-to/install.md) shows
how to get it.

```sh
printf '(a)' | tinyparse run sexpr.scm --input - --dump-tree
```

The command prints the tree, one node per line, with each node's byte range
and its start and end points as `[row,column]`:

```text
Source [0-3] [0,0]-[0,3]
  Form [0-3] [0,0]-[0,3]
    List [0-3] [0,0]-[0,3]
      ( [0-1] [0,0]-[0,1]
      Form [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      ) [2-3] [0,2]-[0,3]
```

[Write your first grammar](docs/tutorials/first-grammar.md) continues from
here.

## When to use it, and when not

- Use it when grammars change while the program runs, or are written by its
  users: a grammar is usable as soon as the program has its bytes.
- Use it when the target has no C library, or when a hostile input must not
  take more than a fixed amount of memory.
- Use it when a program walks its trees more often than it builds them.
- Prefer tree-sitter for an editor, which needs incremental re-parsing, or for
  a language that already has a mature tree-sitter grammar.

[Design of tinyparse](docs/explanation/design.md) gives the trade-offs and the
measurements behind them.

## Documentation

The full index is [docs/README.md](docs/README.md).

- Tutorials: [Write your first grammar](docs/tutorials/first-grammar.md),
  then [Parse input from a C program](docs/tutorials/embed-in-c.md).
- How-to guides: [Install tinyparse](docs/how-to/install.md),
  [Ship a grammar inside your program](docs/how-to/embed-grammar.md),
  [Ship a generated C parser](docs/how-to/generated-parser.md),
  [Run tinyparse without libc](docs/how-to/freestanding.md),
  [Supply imports, classes and parameters from a C host](docs/how-to/host-services.md),
  [Recover from and report syntax errors](docs/how-to/handle-errors.md),
  [Lex tokens that depend on context](docs/how-to/context-sensitive-lexing.md),
  [Split, extend and combine grammars](docs/how-to/compose-grammars.md),
  [Find nodes with a query](docs/how-to/query.md), and the worked
  [examples](examples/README.md).
- Reference: [tinyparse(1)](tinyparse.1.md),
  [Grammar language](docs/reference/grammar.md),
  [Query language](docs/reference/query-language.md),
  [C API](docs/reference/c-api.md),
  [Diagnostics](docs/reference/diagnostics.md),
  [Standard grammars](docs/reference/std-grammars.md),
  [Release artifacts](docs/reference/artifacts.md).
- Explanation: [Design of tinyparse](docs/explanation/design.md),
  [How tinyparse shapes the tree](docs/explanation/tree-shape.md),
  [Performance](docs/explanation/performance.md).

## Performance

On 2026-10-04, on an Intel Xeon at 2.10GHz with 4 cores, parsing 1,189 files of
the React source tree (`mise run bench:parsers`), the VM parsed 4.67 MiB/s,
tree-sitter 7.41 MiB/s, and a generated tinyparse parser 7.80 MiB/s, or
10.88 MiB/s reusing one engine. A cursor walk took 19 to 24 ns per node against
154 to 196 ns for tree-sitter. [Performance](docs/explanation/performance.md)
has the method, memory figures and full results.

## Contributing

[Developing tinyparse](DEVELOPMENT.md) covers building from source, the tests,
the correctness gates and the benchmarks.

## License and version

The MIT license in [LICENSE](../LICENSE) covers tinyparse. The current
version is in [VERSION](VERSION).
