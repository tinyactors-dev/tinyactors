# tinyparse examples

Each subdirectory is a worked example of one tinyparse capability: a
grammar, sometimes a host program that consumes the tree it produces, and a
README that shows the thing running. This file records what every example
owes the reader, and indexes them by the single lesson each one teaches.

## What an example owes the reader

An example is documentation that compiles. The bar is higher than "it
parses something", and these are the rules a new or revised example follows.

**Teach one thing.** Every example names its single lesson in the first
sentence of its README, and no two examples teach the same lesson. If you
cannot say what an example shows that the others do not, it does not belong.

**Parse nesting as nesting.** When the language nests — brackets inside
brackets, an expression inside an expression — the grammar matches it with
recursive rules. A flat token that scans to the next closing delimiter is
not a parser, and an example that ships one teaches the wrong habit. None of
these grammars carries a "does not nest" disclaimer, because none of them
needs one.

**Read the tree.** A host program walks the concrete syntax tree through
the cursor API and works from what the tree already records: node kinds,
field names, and byte spans. It never re-scans the source text.

> A program that consumes the tree MUST NOT build a parallel AST of its own.
> The CST is the data structure. Codegen recurses over the cursor, a
> rewriter copies the spans the tree hands it, a pretty-printer reads the
> fields. If you find yourself defining a struct that mirrors the grammar's
> node types, the example has lost the plot.

**Keep tokens lexical.** Tokens match regular lexemes; structure lives in
rules; fields and `extras` carry the parts a consumer cares about. A greedy
mega-token standing in for a rule hides the structure the example exists to
show.

**Tell the truth in the README.** Show real input and the real output it
produces, then state the genuine limits of the grammar. Honest scope is not
an apology, and a limit worth naming is one a reader would otherwise trip
over.

**Stay green.** Every example is pinned by a test under `test/suite/` that
exercises its lesson, including the nested case where the lesson is
recursion. `bash ci/tinyparse-metric.sh` runs them all.

## Grammars you run with the CLI

These need only the `tinyparse` binary. Feed input on stdin and dump the
tree:

```sh
printf '(a (b c))' | tinyparse run examples/sexpr/sexpr.scm --input - --dump-tree
```

| Example | The lesson |
|---------|------------|
| [`sexpr`](sexpr/)     | The canonical recursive grammar: `List` holds `Form`s and a `Form` resolves back to a `List`, with fields and `extras` trivia along for the ride. |
| [`tcl`](tcl/)         | Recursive command substitution and brace groups that nest to any depth, alongside the fused-word lexing trick that keeps five substitution forms in one word. |
| [`bindings`](bindings/) | Character classes the host installs at runtime, so `(class alpha)` in a grammar resolves to C code. |
| [`lexgoal`](lexgoal/) | Parser-directed lexing: the same `/` byte scans as a regex literal in value position and as division in operator position, chosen by the active lexical goal. |
| [`import`](import/)   | One grammar pulling tokens and rules from another with `(import …)` at compile time. |
| [`recover`](recover/) | Grammar-authored error recovery: a missing `)` at end of input is repaired with a zero-width node and a diagnostic. |

## Programs that consume the tree

These link the dist archives and walk the CST through the cursor API. Build
them with `examples/build.sh` (the dispatcher passes each a `$TP_DIST`,
`$TP_CLI`, and `$TP_INC`); each also has a `run.sh` that builds on demand.

| Example | The lesson |
|---------|------------|
| [`ebnf`](ebnf/)         | A meta-grammar: tinyparse parses an EBNF document, and the program walks that tree to emit an equivalent `.scm` grammar. |
| [`uri`](uri/)           | Reading named components straight off the tree's fields, never re-scanning the input, using the grammar `ebnf` generated. |
| [`rewriter`](rewriter/) | A codemod that rebuilds its output from the byte spans the tree records, copying every untouched byte through verbatim. |
| [`jit`](jit/)           | Native x86-64 emitted by recursing over the cursor, with no intermediate AST between the parse tree and the machine code. |
| [`embed`](embed/)       | Two separately compiled parsers in one binary: a host fenced-block grammar locates code spans and a second generated parser (std.json) parses the bytes inside each fence, linked without symbol collision via `c --parser-lib --name`. |

The `ebnf` → `uri` pair is one pipeline: `ebnf/run.sh` regenerates
`uri/uri.scm` from `ebnf/uri.ebnf`, and the `uri` program then parses URLs
with it.
