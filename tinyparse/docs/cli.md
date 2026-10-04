# tinyparse CLI guide

The CLI is the hosted shell around the freestanding library: it reads files,
drives the parser, walks the CST through the public cursor API, and renders
diagnostics. Only the CLI touches libc. `tinyparse man` prints the full manual
page, whose source is [`../tinyparse.1.md`](../tinyparse.1.md).

## Commands

```sh
tinyparse check <grammar>                                   # validate a grammar
tinyparse run <grammar> --input <file|-> [--dump-tree] [--chunk N] [--trace]
tinyparse pack <grammar> --out <file.tpg>                   # write a packed grammar
tinyparse c <grammar> --out <file.c> [--bundle|--parser|--parser-lib] [--name P]
tinyparse linearize <grammar> --input <file>                # the tree as ENTER/LEAF/EXIT
tinyparse query <grammar> <query.scm> --input <file>        # print a query's captures
tinyparse node-types <grammar>                              # the node-types schema, as JSON
tinyparse init <dir> [--amalgamation|--lib] [--grammar NAME]
tinyparse vendor <dir> [--amalgamation|--lib]
tinyparse grammars                                          # list the embedded grammars
tinyparse man | version | --help
```

## Naming a grammar

Every command that takes `<grammar>` resolves it the same way:

- **A `.scm` file** is compiled at runtime.
- **A directory, or its `package.scm`**, is a grammar package: the manifest's
  `params:` are bound as compile-time parameters and the sibling `grammar.scm`
  is compiled with them.
- **A bare name** such as `std.json` is looked up on the filesystem first, then
  in the registry of standard grammars compiled into the binary. A package
  found on disk shadows the embedded copy, and `(extends …)` and `(import …)`
  resolve the same way.
- **A packed `.tpg` file** written by `pack` is loaded without compiling
  (`run` recognises it by its `TPG1` magic).

`--param NAME[=VALUE]`, repeatable, sets a compile-time parameter that a
`(when NAME: …)` combinator gates a production on. A bare `--param NAME` sets it
truthy, `--param NAME=0` clears it, and a flag overrides a manifest default.

## `check <grammar>`

Compiles the grammar and reports whether it is well formed. A valid grammar
exits `0`; a malformed one prints a diagnostic to stderr (containing `error`)
and exits `1`.

## `run <grammar> --input <file|->`

Parses the input against the grammar; `--input -` reads standard input.

- **`--dump-tree`** prints the concrete syntax tree in pre-order, one node per
  line, indented by depth. Each line shows the node's kind (and its field name,
  for a field-bound child), its byte range, and its 0-based
  `[line, column]-[line, column]` range, tree-sitter style. Columns are byte
  offsets within the line. A region parsed by an embedded grammar is marked
  with its language.
- **`--chunk N`** feeds the input in `N`-byte pushes. The tree must not depend
  on how the bytes were split, and the suite holds it to that.
- **`--trace`** prints the VM's trace events to stderr (`rule.enter`, `token`,
  and the rule memo's `rule.replay` and `rule.memo-on`).

```sh
$ printf '(a (b c))' | tinyparse run examples/sexpr/sexpr.scm --input - --dump-tree
Source [0-9] [0,0]-[0,9]
  Form [0-9] [0,0]-[0,9]
    List [0-9] [0,0]-[0,9]
      open: ( [0-1] [0,0]-[0,1]
      Form [1-2] [0,1]-[0,2]
        Atom [1-2] [0,1]-[0,2]
      ...
```

A syntax error prints a diagnostic to stderr and exits `1`. A parse that runs
out of memory prints `error: out of memory` and exits `1`.

## `pack <grammar> --out <file.tpg>`

Compiles the grammar, resolving its imports and base, and writes it as one
binary image. `run`, and a host through `tinyparse_load_binary_grammar`, load it
without reading `.scm` source.

## `c <grammar> --out <file.c>`

Emits C. The mode decides what the file contains:

| Flag | Output |
|------|--------|
| (none) | module mode: the packed grammar as a byte array and an accessor; the host loads it with `tinyparse_load_binary_grammar` and runs the VM |
| `--bundle` | the same, plus a `main` that parses standard input and dumps the tree |
| `--parser` | a recursive-descent parser generated from the grammar, with a `main` |
| `--parser-lib` | the same parser as a library: `<prefix>_load`, `<prefix>_run`, and `<prefix>_run_chunk` for a buffer that is still growing; no `main` |

Every mode links against `libtinyparse` (or the amalgamation): module and bundle
output for the VM, generated parsers for the tree builder, diagnostics and
arena.

`--name P` prefixes every emitted file-scope symbol with `P_`, so two generated
parsers link into one program. A generated parser builds the same tree as the
VM; `gate:cc-diff` checks that on every corpus input. It includes the library's
internal header, so compile it with `-I src -I include` from a source tree.

## `linearize`, `query`, `node-types`

- **`linearize <grammar> --input <file>`** prints the tree as the token stream a
  query parses: `enter KIND`, `leaf KIND TEXT`, `exit KIND`, one per line.
- **`query <grammar> <query.scm> --input <file>`** runs a query over the tree
  and prints one line per capture, `@NAME START-END TEXT`. The query language is
  in the design document's
  [Queries as parsers](../tinyparse-design.md#queries-as-parsers-over-linearized-trees)
  section.
- **`node-types <grammar>`** prints the grammar's node types in tree-sitter's
  `node-types.json` shape.

## `init` and `vendor`

`tinyparse init <dir>` seeds a project with a starter `grammar.scm`, a `main.c`
that compiles it and dumps the parse of standard input, and a one-line
`build.sh`:

- **`--amalgamation`** (the default) also writes `tinyparse.c` and
  `tinyparse.h`, so `cc main.c tinyparse.c -o app` builds it with nothing
  installed.
- **`--lib`** copies no engine source; `build.sh` links an installed
  `libtinyparse` with `-ltinyparse`.
- **`--grammar NAME`** starts from a standard grammar such as `std.json`.

`tinyparse vendor <dir>` writes only the engine source into an existing
project: `tinyparse.c` and `tinyparse.h`, or with `--lib` only the header. The
CLI carries the amalgamation inside the binary, so neither command needs the
network or an install prefix.

## `grammars`, `man`, `version`, `--help`

`grammars` lists the standard grammars compiled into the binary: `std.fence`,
`std.javascript`, `std.json`, `std.json5`, `std.jsonc`, `std.shell`, `std.tsx`
and `std.typescript`. `man` prints the manual page in mdoc form (pipe it to
`mandoc -a`). `version` prints `tinyparse 0.1.0`. `--help`, or no arguments,
prints the usage summary.

## Environment

| Variable | Effect |
|----------|--------|
| `TINYPARSE_RULEMEMO` | `always` forces the VM's rule memo on from the first call; `off` turns it off. Unset, the memo is adaptive. `gate:vm-diff` compares all three. |
| `TINYPARSE_TRACE_LIFECYCLE` | after `run`, print the parser's final lifecycle state to stderr (`lifecycle: finished`) |

## Exit codes

| Code | Meaning |
|------|---------|
| `0` | success |
| `1` | a grammar error, a syntax error, an unreadable file, a missing required option, or running out of memory |
| `2` | an unknown command |
