# Examples

This index is for a developer looking for a worked example of one tinyparse
technique. Each directory under `examples/` holds a grammar, sometimes a C
host program that reads the tree, and a README that runs it on real input
and states its limits.

## Prerequisites

- Run the commands from the `tinyparse/` directory of a source checkout.
- `mise run build` builds the library, the `tinyparse` command at
  `build/bin/tinyparse` and the example programs. Put the command on your
  `PATH`:

  ```sh
  mise run build
  export PATH="$PWD/build/bin:$PATH"
  ```

  [Install tinyparse](../docs/how-to/install.md) covers the other ways to
  get the command; the examples that run with the command alone work with
  any of them.
- `mise run examples` builds the C host programs alone, each into
  `build/examples/<name>/`. No `run.sh` builds anything: those of the C
  examples run the program `mise run examples` built, and
  `examples/tcl/run.sh` runs the `tinyparse` command.
- The `jit` example emits x86-64 machine code, and its program builds and
  runs only on an x86-64 host. On another host `mise run examples`, and with
  it `mise run build`, stops with an error at `examples/jit/jit.c`.

## Grammars you run with the command

These need only the `tinyparse` command. Each README feeds input to
`tinyparse run` and prints the tree.

| Example | Lesson |
|---------|--------|
| [sexpr](sexpr/README.md) | a recursive grammar: lists inside lists, with fields and trivia |
| [tcl](tcl/README.md) | nested command substitutions and brace groups, and words joined from several parts |
| [bindings](bindings/README.md) | a character class whose bytes the host program supplies |
| [lexgoal](lexgoal/README.md) | one byte, `/`, read as a regular-expression literal or a division depending on the parser's position |
| [import](import/README.md) | a grammar that takes its tokens from another file |
| [recover](recover/README.md) | a tree and a diagnostic when the input ends inside an open list |
| [heredoc](heredoc/README.md) | here-documents whose closing line the input chooses, nested inside each other |

## C host programs

These link `build/lib/libtinyparse.a` and walk the tree with the cursor
functions. Each runs from `build/examples/<name>/` after
`mise run examples`, directly or through the `run.sh` in its directory.

| Example | Lesson | How it runs |
|---------|--------|-------------|
| [ebnf](ebnf/README.md) | read a tree and print text: convert EBNF to a tinyparse grammar | `examples/ebnf/run.sh FILE.ebnf` |
| [uri](uri/README.md) | read named fields from a tree | `examples/uri/run.sh URL` |
| [rewriter](rewriter/README.md) | rewrite source by copying the byte ranges the tree records | `examples/rewriter/run.sh FILE.c` |
| [jit](jit/README.md) | generate x86-64 code by recursing over the tree | `examples/jit/run.sh SOURCE FUNCTION ARGUMENT` |
| [embed](embed/README.md) | link two generated parsers into one program | `build/examples/embed/embed FILE` |
| [nesting](nesting/README.md) | generated parsers for the regions another grammar injects | `build/examples/nesting/nesting FILE` |
| [bufferfriendly](bufferfriendly/README.md) | feed a generated parser a buffer that grows | `build/examples/bufferfriendly/bufferfriendly FILE [CHUNK]` |

`ebnf` writes the grammar that `uri` reads: `examples/ebnf/run.sh` converts
`examples/ebnf/uri.ebnf` and saves the result as `examples/uri/uri.scm`,
and `examples/uri/run.sh` runs that conversion first when
`examples/uri/uri.scm` is missing.

Each example is checked by a case under `test/suite/` or by a gate under
`mise/tasks/gate/`, and `mise run test` runs both.
[Developing tinyparse](../DEVELOPMENT.md) describes how to add an example.
