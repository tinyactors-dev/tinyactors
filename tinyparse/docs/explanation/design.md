# Design of tinyparse

This explanation is for a developer deciding whether tinyparse fits a program,
and for a user who wants to know why the library behaves as it does. Most
sections name a choice, the alternative it was weighed against, and what the
choice costs. How the source is organized is in
[Architecture](architecture.md); the tree the library builds is explained in
[How tinyparse shapes the tree](tree-shape.md).

## Grammars compiled at run time

A parser generator such as tree-sitter turns a grammar into C ahead of time:
the grammar is written in JavaScript, a generator writes `parser.c` with the
parse tables, and the program compiles and links that file. Changing the
grammar means running the generator and rebuilding the program.

This library takes the grammar as data instead. A host passes the bytes of a
`.scm` grammar to `tinyparse_compile_scheme`, which reads it, checks it and
lowers each rule to bytecode for a small virtual machine, the VM. The same
call works on a grammar the program generated, received or edited a moment
earlier, and the program parses with it immediately. No generator, compiler or
linker runs between writing a grammar and using it.

The cost of that choice is speed and memory at both ends:

- The compiled grammar is bytecode that the VM interprets. On the React
  source tree the VM parses more slowly than tree-sitter's generated tables:
  4.67 MiB/s against 7.41 MiB/s, measured on 2026-10-04 with
  `mise run bench:parsers`. [Performance](performance.md) gives the
  measurements and explains where the time goes.
- Compiling is work a generator would have done ahead of time. Compiling
  std.javascript needs a grant of at least 12,811 KiB, while loading the same
  grammar already compiled needs 430 KiB
  ([Sizing a grant](../reference/c-api.md#sizing-a-grant)).

Both costs have a way out once a grammar stops changing, described below under
"Three ways to ship a grammar".

## When tinyparse fits, and when it does not

The figures in this section were measured on 2026-10-04 on one machine;
[Performance](performance.md) names the machine and the commands and gives the
full results. The library fits a program in these cases:

- Grammars change while the program runs. A program that accepts, edits or
  generates grammars parses with each one as soon as it has its bytes.
- The environment has no C library. The library allocates only from the memory
  the host grants it and does no I/O, so it can be compiled for a target
  without a C library, such as a kernel or firmware. The build compiles and
  checks it only for x86_64 Linux and aarch64 macOS; see
  [Run tinyparse without libc](../how-to/freestanding.md).
- The program reads its trees more than it builds them. On the tree-sitter
  JavaScript corpus (`mise run bench:javascript`), a cursor walk over a
  tinyparse tree took 19 to 24 ns per node against 154 to 196 ns for
  tree-sitter's cursor.
- One C file is easier to ship than a library. The amalgamation is the whole
  library as `tinyparse.c` and `tinyparse.h`.

tree-sitter fits better in these cases:

- The program is an editor. tree-sitter re-parses only the edited part of a
  document; tinyparse parses every document from the start.
- The language already has a mature tree-sitter grammar. tinyparse ships a small set
  of [standard grammars](../reference/std-grammars.md) and expects other languages
  to be written for it.
- Parse throughput matters and the grammar must stay data. On the React source
  tree (`mise run bench:parsers`), the VM parsed 4.67 MiB/s and tree-sitter
  7.41 MiB/s. A generated tinyparse parser reached 7.80 MiB/s, or 10.88 MiB/s
  reusing one engine, but generating one gives up run-time grammars.

## The host owns the memory

`tinyparse_open` takes a memory grant: a first block, its size and an optional
`grow` callback the engine calls when it needs another block. Every byte the
library uses comes from that grant, and the library never calls `malloc`. That
is what lets it run where there is no `malloc`. It also lets a host cap what a
parse of hostile input can consume: with a fixed grant and no `grow` callback,
the parse fails with `TINYPARSE_OUT_OF_MEMORY` and the process carries on.

The engine never resizes or moves a block, and it never asks the host to.
Compiled grammars, trees, diagnostics and strings point into the grant, and
the host holds those pointers between calls: the name of a node kind returned
as a `tinyparse_string` is a pointer into the compiled grammar. A block that
moved would invalidate every one of them. When an array needs to grow, the
engine copies it to new memory inside the grant and leaves the old copy unused.

The engine also never gives memory back to the host, and that is where sizing
gets its shape:

- `tinyparse_close` and `tinyparse_grammar_free` release nothing. The host
  frees the first block and every block `grow` returned, after its last call on
  the engine.
- `tinyparse_parser_free` returns the parser's memory to the engine, and later
  parses reuse it. Each freed parser leaves a small record in the engine, and
  reused memory fragments, so many parses need more than one. With
  `examples/sexpr/sexpr.scm` and a 19,000-byte input, one parse needed a fixed
  grant of 1,725 KiB, and 1,000 parses, each parser freed before the next,
  needed 3,210 KiB ([Sizing a grant](../reference/c-api.md#sizing-a-grant)).
- A long-running host therefore gives the engine a `grow` callback, or opens a
  new engine when the grant runs low.

The measured sizes for each standard grammar, and the rules of the `grow`
callback, are in the [C API reference](../reference/c-api.md#memory-grant).

## A freestanding library and a hosted command

The library is compiled as freestanding C11 with `-ffreestanding -fno-builtin`.
It does no I/O, reads no clock or environment variable, starts no thread and
has no writable static data, and the only symbols it needs from outside are the
memory functions a compiler emits calls to, such as `memcpy` and `memset`. Its
header needs only `<stddef.h>` and `<stdint.h>`, and every string crosses the
interface as a pointer and a length, so nothing depends on NUL-terminated
strings.

Everything that needs an operating system lives in the `tinyparse` command,
which is an ordinary hosted program built on the library:

| Need | Library | Command |
|------|---------|---------|
| Find the text of an imported grammar | Asks the host's module resolver | Looks for a file beside the grammar, then among the standard grammars built into the command |
| Character classes such as `alpha` | Asks the host's class bindings | Binds `alpha`, `digit`, `alnum` and `space` |
| Report a diagnostic | Returns structured diagnostics | Prints them |
| Turn byte offsets into rows and columns | Returns byte offsets only | Computes rows and columns for the tree dump |
| Force the rule memo on or off | Not offered by the public API | Reads `TINYPARSE_RULEMEMO` |

Keeping these decisions out of the library lets the same source serve a
program without a file system and a command-line tool. A host that has a file system writes a
resolver that reads files, and a host that has none resolves names from a table
compiled into the program. The cost is that each host writes the small amount
of glue the command already has; [Supply imports, classes and parameters from a
C host](../how-to/host-services.md) shows it.

Having no writable static data also decides the threading model. Separate
engines run on separate threads with no locks. One engine, and every grammar,
parser and tree made from it, belongs to one thread at a time.

## Parsing expression grammars

A grammar is matched as a parsing expression grammar (PEG): `choice` is
ordered, the first alternative that matches wins, and a failed alternative
backtracks to where it started. The other common choice for a general parser is
GLR, the generalized LR algorithm tree-sitter uses, which explores ambiguous
alternatives in parallel from generated tables.

The comparison is recorded, with measurements, in
[`docs/peg-vs-glr.md`](../peg-vs-glr.md), and the figures below are its
Findings 1 and 2. Those two findings decided the design:

- Slow PEG parses came from grammar shape. A JavaScript grammar ported from
  tree-sitter kept its choices in tree-sitter's order. tree-sitter's `choice`
  is unordered and its tables merge a shared prefix for free; as an ordered PEG
  choice, the same rule parsed an operand, discarded it, and parsed it again
  under the next alternative, at every nesting level. Left-factoring the rule,
  so it parses the operand once and then an optional tail, cut the parse of one
  React test file from 7.83 s to 0.49 s, measured with the rule memo off.
- The remaining gap came from interpretation. The fastest parsers measured on
  the React source tree were hand-written recursive descent (the TypeScript
  compiler and its Go port), which is the same family as a PEG. GLR tables were
  slower than both. What separated tinyparse from them was that it interpreted
  its grammar while they ran compiled code.

So tinyparse stays a PEG, its standard grammars are left-factored, and the
speed comes from compiling the grammar to C (the generated parser) rather than
from changing the algorithm. GLR would also have produced different trees from
the same grammar, resolved by table precedence instead of by the order the
author wrote.

Left-factoring removes the re-parsing a grammar author can see. For the rest,
the VM keeps a rule memo that records, per rule, position and lexical goal,
whether the rule matched and what it produced. Recording every call would slow
every clean parse, so the memo starts dormant and wakes when the number of rule
calls exceeds 32 times the input length plus 65,536, a count a linear parse
stays below. The memo has a cost a generated parser does not pay: a generated
parser has no memo and relies on the grammar being factored.

## The parser lifecycle

A parser accepts input in chunks with `tinyparse_push` and parses when the host
calls `tinyparse_finish`. The chunks are buffered and parsed together at the
end, because a PEG can backtrack to any earlier byte of the input.

A parser is in one of four states:

| State | Reached by | `tinyparse_push` | `tinyparse_finish` |
|-------|-----------|------------------|-------------------|
| accepting | `tinyparse_parser_new`, and every push that buffered its bytes | buffers the bytes | parses, moves to finished |
| dropped | a push that could not buffer its bytes | still buffers later chunks | parses nothing, returns that push's failure, moves to failed |
| finished | `tinyparse_finish` from accepting | returns `TINYPARSE_INVALID` | parses nothing again; reports whether a tree exists |
| failed | `tinyparse_finish` from dropped | returns `TINYPARSE_INVALID` | returns `TINYPARSE_SYNTAX_ERROR`; there is no tree |

The dropped state exists for one case. When a push fails, typically because the
grant ran out, the buffered input is missing a chunk in the middle or at the
end. Parsing what is left could succeed: a number split across three chunks,
`12`, a dropped chunk and `4`, leaves `124`, which is still a number. The
parser would then report a clean parse of input the host never sent. So a
parser that dropped a chunk refuses to parse at all, even if the host ignored
the failed push's result and pushed more, and `tinyparse_finish` returns the
original failure with the fault `input chunk was dropped before finish`.

Every way of parsing goes through the same finishing step: `tinyparse_finish`
running the VM, a generated parser's run function, and the sub-parse of an
injected region. A truncated input is refused the same way by all three.

## Lexing directed by the parser

Some tokens cannot be recognized from the bytes alone. In JavaScript, `/` starts
a regular expression where a value is expected and is division after a value; a
shell here-document ends at a line that repeats a word given earlier; inside a
template literal, `//` is text. tree-sitter handles such cases
with an external scanner: a C file per language that the parser calls for the
tokens its generated lexer cannot produce.

In tinyparse these decisions are part of the grammar:

- A [lexical goal](../reference/grammar.md#lexical-goals) makes one token scan
  as a different token in part of the grammar: std.javascript reads `/` as the
  start of a regex literal under one goal and as division under another.
- A [parametrized goal](../reference/grammar.md#parametrized-goals) builds a
  token pattern from bytes captured earlier, and
  [deferred slots](../reference/grammar.md#deferred-slots) queue a match and
  parse something for it later. Together they parse nested shell
  here-documents.
- A rule marked `extras: none` turns off whitespace and comment skipping for
  everything it parses, and `extras: all` turns it back on inside, which is how
  a template literal's text differs from the expression in its `${ }` hole.

Because the grammar holds the whole language, a packed grammar or a generated
parser carries everything needed to parse it, and porting a grammar to a new
host or platform means moving the grammar file; no C code changes. None of the
standard grammars needs host code. The cost is in what a grammar can express
cheaply. The standard grammars treat every byte from `0x80` to `0xFF` as an
identifier byte, which accepts every UTF-8 identifier and some strings that are
not identifiers; exact Unicode classification would need a property table the
freestanding library does not carry. A host that needs it binds a [character
class](../reference/grammar.md#classes) to its own table with `tinyparse_bind`.
[Lex tokens that depend on context](../how-to/context-sensitive-lexing.md)
shows the forms at work.

## Injection and registered languages

A document often contains another language: a JSON block in a Markdown fence,
JavaScript in an HTML `<script>`. The [`inject`](../reference/grammar.md#inject)
form marks a region of one grammar as parsed by another, named grammar. When
the grammar that contains the `inject` form is compiled, the named grammar is
resolved through the module resolver and compiled with it, so parsing reads no
files. During the parse, the
VM matches the region, parses the region's bytes with the injected grammar, and
places that tree under an `injection` node. The result is one tree, and each
injected node carries a language tag that the tree dump prints.

`tinyparse_register_language` lets a host substitute what runs for a language
name. A generated parser of the same grammar builds the same tree as the VM,
faster. A different compiled grammar replaces what the region is parsed as, so
the injected subtree changes with it. The standard grammar std.fence uses
injection to parse `js` fences with std.javascript and `json` fences with
std.json; see [std.fence](../reference/std-grammars.md#stdfence).

The injected parse runs to completion inside the outer parse, at finish. Two
consequences follow. A region is parsed only after all the input has arrived,
like the rest of the document. And an injected parse that fails leaves an
`injection` node holding an empty root, with no diagnostic, so a host that needs
to know checks the injected subtree.

## Queries are matched like input

A query finds nodes by shape, in the pattern language tree-sitter made common:
`(List (Form (Atom) @first))` captures the first atom of each list. tinyparse
matches a query by turning the tree into a token stream and matching the pattern
against the stream the way the parser matches tokens against input.
`tinyparse_linearize` produces that stream: each node with children becomes an
ENTER token and an EXIT token around its children, and each leaf becomes one
LEAF token.

Matching at any depth then needs no tree search: before a child pattern, the
matcher may skip any number of whole balanced subtrees, and the `.` anchor turns
the skip off between two children. The `#eq?` and `#match?` predicates use the
library's own byte comparison and regex dialect, so a query needs no host
binding. The query syntax is in the [query language
reference](../reference/query-language.md), and
[Find nodes with a query](../how-to/query.md) shows queries in use.

## Three ways to ship a grammar

A grammar reaches a program in one of three forms, each a different trade
between flexibility, memory and speed:

| Form | Made by | At start-up the program | Parses with |
|------|---------|------------------------|-------------|
| Grammar text (`.scm`) | the author | compiles the text | the VM |
| Packed grammar (`.tpg`) or generated module | `tinyparse pack`, or `tinyparse c` | loads the compiled grammar | the VM |
| Generated parser | `tinyparse c --parser` or `--parser-lib` | loads the compiled grammar | native C, one function per rule and token |

Grammar text keeps every option open and costs the most memory and start-up
time, since every start compiles the grammar again. A packed grammar holds the
compiled result, including the grammars it imports and injects and the effect
of its parameters. Loading it is cheap and calls no module resolver. A
generated module is the same image as a C array, so the program reads no file
at all. A packed image loads only into a library of the same format version and
ABI, so it is rebuilt when tinyparse is upgraded
([Release artifacts](../reference/artifacts.md#packed-grammars)).

A generated parser removes the VM's dispatch loop: each rule becomes a C
function and each token a scanning function, and it builds the same tree and
diagnostics as the VM. It is the fastest form and the least portable one:

- It compiles against the library's private header `src/internal.h` from a
  source tree of the same version.
- It links only against the static library or the amalgamation.
- It has no rule memo, so it relies on the grammar being left-factored.
- It fails with a syntax error past 1,000 nested rule calls, so deep input
  cannot exhaust the C stack.

The steps are in
[Ship a grammar inside your program](../how-to/embed-grammar.md) and
[Ship a generated C parser](../how-to/generated-parser.md).

## Rewriting by re-parsing

The tree is read-only, and the library has no function that adds, removes or
replaces a node. A program that rewrites source emits text. It copies the bytes
of each node it keeps through the node's byte range and writes new text for the
nodes it changes. When it needs a tree for the new text, it parses that
fragment.

An edit API would have to keep a contiguous node array consistent under
insertion, or switch the tree to a linked structure that walks more slowly. It
would also have to decide what an edited node's byte range means once it
matches no input. Text emission has neither problem, and a rewrite's output is
text in any case. `examples/rewriter` is a complete codemod built this way.

## What tinyparse does not do

- Incremental parsing: every parse starts from the first byte.
- Semantic analysis: the library builds trees and diagnostics. Name
  resolution, types and lint rules belong to the host.
- Host-written token scanners: the library has no hook for a C function that
  produces tokens; context-dependent lexing is written in the grammar.
- Rows and columns: positions are byte offsets; the host converts them under
  its own column policy.

[`docs/decisions.md`](../decisions.md) records the decisions behind the
library one by one.
