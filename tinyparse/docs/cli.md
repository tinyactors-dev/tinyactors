# pocketparse CLI guide

The CLI is the hosted shell around the freestanding library: it reads files,
drives the parser, walks the CST through the public cursor API, and renders
diagnostics. The library itself stays freestanding; only the CLI touches libc.

## Commands

```sh
pocketparse check <grammar>                                  # validate a grammar
pocketparse run <grammar> --input <file|-> [--dump-tree] [--chunk N]
pocketparse man                                              # print the manual page
pocketparse version                                          # print the version
pocketparse --help                                           # usage summary
```

### `check <grammar>`

Compiles the `.scm` grammar and reports whether it is well-formed. Exit `0` if
valid; on a malformed grammar it prints a diagnostic to stderr (the message
contains `error`) and exits `1`.

### `run <grammar> --input <file|-> [--dump-tree] [--chunk N]`

Parses input against the grammar. `--input -` reads standard input. With
`--chunk N` the input is fed to the streaming parser in `N`-byte pushes, which
exercises the chunk-independence of `pocketparse_push` (parsing the same bytes
one at a time must yield the same tree). With `--dump-tree` it prints the
concrete syntax tree as a pre-order, source-order traversal — one node per
line, indented by depth, each line showing the node's kind name (and, for a
field-bound child, its field name), then the node's span: the byte range and the
0-based `[line, column] - [line, column]` range, tree-sitter style. On a syntax
error it prints a diagnostic to stderr (containing `error`) and exits `1`.

Example:

```sh
$ printf '(a (b c))' | pocketparse run examples/sexpr/sexpr.scm --input - --dump-tree
Source [0-9] [0,0]-[0,9]
  Form [0-9] [0,0]-[0,9]
    List [0-9] [0,0]-[0,9]
      open: ( [0-1] [0,0]-[0,1]
      Form [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      ...
```

The span columns are byte offsets within the line (0-based), matching
tree-sitter's `[row, column]`.

### `man`

Prints the embedded manual page (rendered mdoc, generated from
[`../pocketparse.1.md`](../pocketparse.1.md) at build time and compiled into the
binary, mirroring `mdc`). Pipe it to a pager or `mandoc` to format it.

### `version` / `--help`

`version` prints `pocketparse <semver>`; `--help` (or no arguments) prints the
usage summary.

## Exit codes

| Code | Meaning                                                      |
|------|-------------------------------------------------------------|
| `0`  | success                                                      |
| `1`  | a grammar error (`check`) or a syntax error (`run`)        |
| `2`  | a usage error (unknown command, missing argument)          |

## Deferred to Phase 4

`pocketparse pack <grammar> --out g.ppg` (binary grammar) and
`pocketparse c <grammar> --out g.c` (the C emitter) are described in the design
doc and are not yet implemented.
