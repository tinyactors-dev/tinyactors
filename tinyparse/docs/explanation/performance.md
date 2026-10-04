# Performance

This explanation is for a developer deciding whether tinyparse is fast enough
for a program, and for a contributor working on its speed. It says what the
benchmarks measure, records their results, and explains why the numbers come
out as they do. The commands that reproduce the numbers are in
[`DEVELOPMENT.md`](../../DEVELOPMENT.md); the history of each optimization is in
[`docs/decisions.md`](../decisions.md).

## What the benchmarks measure

Two workloads compare tinyparse with tree-sitter, each parser linked into a
small C program that runs in one process:

- The JavaScript corpus (`mise run bench:javascript`): the input halves of the
  116 test cases in the tree-sitter JavaScript grammar, 11,481 bytes in all,
  parsed with std.javascript and with tree-sitter's JavaScript parser.
  std.javascript parses 115 of them; the 116th is a malformed case for which
  tree-sitter's own expected output is an error-recovery tree.
- The React source tree (`mise run bench:react`, `mise run bench:parsers`):
  every file under `packages/*/src` of React at the commit pinned in
  `reference/PINS.txt` that std.tsx parses without error, parsed with std.tsx
  and with tree-sitter's TSX parser. `bench:parsers` adds the generated
  tinyparse parser, the TypeScript compiler's parser (`tsc`) and meriyah, a
  JavaScript parser written in JavaScript.

Each harness reads every document into memory first, excludes loading the
runtime and compiling or loading the grammar from the timed loop, and reports
the median over many passes. Three rules keep the two sides doing the same job:

- Neither side walks the tree while parsing is timed. `tinyparse_finish` builds
  the whole tree before it returns, so a walk after it would measure the walk,
  which tree-sitter's harness does not do. Walking is timed separately.
- Parser setup is counted both ways. tinyparse has no way to reset a parser, so
  its harness creates one per document. tree-sitter is measured both reusing
  one parser and creating one per document.
- Memory is what the library took. The tinyparse harness gives the engine a
  4 MiB first block and a `grow` callback. It reports the first block plus every
  block `grow` hands out while compiling, and every block `grow` hands out while
  parsing. Process peak RSS
  would also count the corpus, the C library and grant memory the library never
  touched. tree-sitter allocates with `malloc` as it goes, so its harness
  reports process peak RSS, and the two figures measure different things.

Throughput is in MiB/s: bytes parsed divided by 1,048,576, per second. The
harnesses compute it that way and label it `MB/s` in their output. Memory is in
KiB and MiB.

## Results, 2026-10-04

All the numbers below come from one machine on one day:

| Item | Value |
|------|-------|
| Date | 2026-10-04 |
| Machine | Intel Xeon @ 2.10GHz, 4 cores, 15.7 GiB RAM, Linux 6.18.44, x86_64 |
| tinyparse | commit `afdef55`, built with `mise run build` |
| tree-sitter runtime | commit `2ef426e1` |
| Grammars | tree-sitter-javascript and tree-sitter-typescript, and React, at the commits in `reference/PINS.txt` |
| Load | other processes ran during the measurements; the load average was 0.2 to 1.9 |

### JavaScript corpus

`mise run bench:javascript`, 200 passes, run six times. tinyparse creates a
parser per document; tree-sitter is shown reusing one parser and creating one
per document. Each cell is the median time of one pass over the corpus:

| Run | tinyparse | tree-sitter, reused | tree-sitter, per document |
|-----|----------:|--------------------:|--------------------------:|
| 1 | 3.796 ms | 4.063 ms | 3.241 ms |
| 2 | 3.955 ms | 2.930 ms | 4.884 ms |
| 3 | 3.749 ms | 4.025 ms | 4.021 ms |
| 4 | 5.867 ms | 3.125 ms | 3.323 ms |
| 5 | 3.746 ms | 2.865 ms | 3.250 ms |
| 6 | 4.760 ms | 2.731 ms | 3.188 ms |

On 11 KiB a pass takes 3 to 6 ms, and the medians of both sides moved by up to
1.6 times between runs. tree-sitter, reusing its parser, was faster in four of
the six runs, by 1.3 to 1.9 times, and slower in two. The corpus is too small to
rank the two parsers; the React tree below does.

The same runs timed a depth-first cursor walk over each tree already built.
The two grammars build trees of different shapes, so each side's time is divided
by its own node count:

| Run | tinyparse, 8,139 nodes | tree-sitter, 5,465 nodes |
|-----|-----------------------:|-------------------------:|
| 1 | 20.15 ns/node | 164.68 ns/node |
| 2 | 24.45 ns/node | 196.21 ns/node |
| 3 | 20.05 ns/node | 194.44 ns/node |
| 4 | 19.09 ns/node | 157.58 ns/node |
| 5 | 23.75 ns/node | 161.69 ns/node |
| 6 | 20.21 ns/node | 154.42 ns/node |

Memory: the grant figures were the same in all six runs; process peak RSS was
recorded for both sides in runs 4 to 6:

| Measure | tinyparse | tree-sitter |
|---------|----------:|------------:|
| First block plus grown blocks, compiling std.javascript | 12.5 MiB | |
| Grown blocks, parsing all 116 documents | 0.6 MiB | |
| Process peak RSS | 15.5 MiB | 2.9 to 3.0 MiB |

Only the process peak RSS row compares the two. tinyparse's figure includes
compiling the grammar at start-up, work that tree-sitter's generator does
before the program is built.

### React source tree

`REGEN=1 mise run bench:react`, 50 passes, over 1,189 files and 10,026,971
bytes. Parsing creates a parser per file; collecting walks each parsed tree and
counts function definitions. The two grammars classify a few constructs
differently, so the counts differ by 14.

| Measure | tinyparse VM | tree-sitter |
|---------|-------------:|------------:|
| Parse, median | 1,884.69 ms (5.07 MiB/s) | 1,240.03 ms (7.71 MiB/s) |
| Collect, median | 43.41 ms | 442.39 ms |
| Functions found | 29,862 | 29,848 |
| Process peak RSS | 50.7 MiB | 23.2 MiB |

`mise run bench:parsers`, over the same files, parse only, 30 passes for the
programs linked in process and 15 for `tsc` and meriyah:

| Parser | Median | Throughput | Peak RSS |
|--------|-------:|-----------:|---------:|
| meriyah 7.3.3, on the 645 files it accepts | 0.216 s | 28.30 MiB/s | |
| tsc, TypeScript 5.9.3 | 0.781 s | 12.25 MiB/s | |
| tinyparse generated parser, one engine for all files | 0.879 s | 10.88 MiB/s | 151.5 MiB |
| tinyparse generated parser, new engine per file | 1.226 s | 7.80 MiB/s | 96.2 MiB |
| tree-sitter | 1.291 s | 7.41 MiB/s | 23.2 MiB |
| tinyparse VM | 2.047 s | 4.67 MiB/s | 50.7 MiB |

The two tables time the VM on the same files in separate harnesses and
processes, with 50 passes in one and 30 in the other, and their medians differ
by 9%. [Design of tinyparse](design.md) quotes the `bench:parsers` figures so
that every parser it compares comes from one run.

meriyah parses JavaScript and JSX only and rejects the 544 files that use
TypeScript or Flow syntax, so its row covers a different, smaller set of files.
`tsc` was installed as TypeScript 5: the benchmark installs the newest
`typescript` package, and TypeScript 7 lacks the API the harness calls.
The task can also run `tsgo`, the Go port of the TypeScript compiler, which was
not built on this machine.

## Why walking is fast

A tinyparse tree is one array of 40-byte node records, and each record holds
the indices of its parent, first child and next sibling. Nodes are appended as
the parse opens them, so a depth-first walk moves mostly forward through the
array. A cursor is an index: moving it reads one
record. [How tinyparse shapes the tree](tree-shape.md) describes the layout.

tree-sitter stores a tree as shared subtrees that hold their sizes but not
their positions, so that an edited tree can reuse unchanged subtrees. Its
cursor keeps a stack of the nodes above it and computes each node's position
from the sizes of the nodes before it as it moves. That design is what makes
tree-sitter's incremental re-parsing cheap; tinyparse does not re-parse
incrementally, and stores positions directly.

The walk took 7 to 10 times less per node on the JavaScript corpus, and
collecting functions from the React trees took 10 times less. The tinyparse
tree has more nodes for the same input, because whitespace and comments are
leaves, and it is still the faster tree to walk.

## Why the VM parses more slowly

tree-sitter's generator turns a grammar into parse tables and a lexer written
out as C `switch` statements; at parse time it follows tables. The tinyparse
VM runs bytecode it compiled from the grammar when the program started, so
every step of the grammar costs an instruction dispatch. That is the cost of
taking a grammar as data.

A callgrind profile shows where the VM's time goes. On 2026-10-04 it counted the
instructions executed inside `tinyparse_finish` while the JavaScript corpus
harness, `bench/bench_tinyparse`, ran 464 parses: two warm-up passes and two
timed passes over the 116 documents, 137.8 million instructions in all. The
command is

```sh
valgrind --tool=callgrind --toggle-collect=tinyparse_finish \
  bench/bench_tinyparse bench/corpus/MANIFEST.txt 2 grammars/std/javascript/grammar.scm
```

and "Run the benchmarks" in [`DEVELOPMENT.md`](../../DEVELOPMENT.md) describes
the harness. The functions above 2% of the count:

| Share | Function | Work |
|------:|----------|------|
| 38.1% | `tp_vm_run` | the bytecode dispatch loop |
| 22.5% | `tp_token_match_memo` | matching a token at the current byte |
| 10.4% | `tp_vm_consume_extras` | consuming whitespace and comments between tokens |
| 7.3% | `tp_vm_call_enter` | entering a rule and opening its node |
| 4.5% | `tp_vm_call_exit` | leaving a rule and closing its node |
| 4.1% | `tp_tree_builder_push` | appending a node record |
| 3.1% | `tp_alloc_raw` | allocating from the grant |
| 2.5% | `tp_slot_save_push` | saving state for backtracking |
| 2.4% | `tp_vm_terminal` | matching a literal or token in a rule |

Instruction dispatch and the rule calls around it take half the parse. A
generated parser removes the dispatch loop: each rule is a C function, a rule
reference is a direct call, and the bytes that select an alternative are tested
in straight-line code. What remains is the work any parser of this grammar
does, matching tokens, consuming trivia and building the tree. On React that
makes the generated parser 1.67 times as fast as the VM with a new engine per
file, level with tree-sitter.

The generated parser gets faster again when one engine parses every file. A new
engine per file loads the grammar and starts from an empty grant each time; a
persistent engine loads the grammar once and reuses the memory
`tinyparse_parser_free` returned. It reached 10.88 MiB/s, with a peak RSS of
151.5 MiB against 96.2 MiB.

The algorithm is not where the remaining time goes.
[`docs/peg-vs-glr.md`](../peg-vs-glr.md) measured hand-written recursive
descent, the family a parsing expression grammar belongs to, as the fastest
approach on the React tree, ahead of tree-sitter's GLR tables. Its conclusion,
that compiling the grammar and left-factoring it gain more than changing the
algorithm, is why the generated parser exists; [Design of tinyparse](design.md)
summarizes it.

## Memory

Compiling a large grammar dominates tinyparse's memory. On the JavaScript
corpus, compiling std.javascript took 12.5 MiB of the grant and parsing 11 KiB
took 0.6 MiB.
A program that loads a packed grammar or a generated module skips the compile;
the [C API reference](../reference/c-api.md#sizing-a-grant) gives the measured
grant for compiling, loading and parsing with each standard grammar.

A parse needs memory in proportion to its input, and `tinyparse_parser_free`
gives it back to the engine for the next parse. `mise run gate:reclaim` checks
that: it parses one 264,060-byte JavaScript file repeatedly in one engine,
freeing each parser, and measures the process's peak RSS. On 2026-10-04 one
parse peaked at 108,524 KiB, and 12 parses and 300 parses both peaked at
142,572 KiB. The gate fails when the repeated parses need more than twice what
one parse needs.

## Worst cases

A parsing expression grammar backtracks, and backtracking over nested input can
take exponential time. Two mechanisms bound it, and the tests check them on
these inputs:

- Left-factored grammars: std.typescript and std.tsx parse 3,000 unclosed `(`
  with the rule memo off, adaptive and forced on, and fail with a syntax error
  each time; the test (`test/suite/tsfamily/unclosed-parens-linear.test`) gives
  each run 20 seconds of CPU time. On 2026-10-04, each `tinyparse run` of that
  input took 0.06 to 0.09 s, grammar compile included, for std.javascript,
  std.typescript and std.tsx.
- The rule memo: for a grammar that is not factored, the VM's memo wakes when
  the number of rule calls exceeds 32 times the input length plus 65,536. From
  then on, a repeated call of a rule at the same position under the same
  lexical goal replays the earlier result.
  `test/suite/core/rule-memo-grows.test` forces the memo on from the first call
  with `TINYPARSE_RULEMEMO=always`, so it does not exercise the wake rule. It
  checks that the memo table grows: a grammar trying nine rules per nesting
  level makes 720 entries on 80 unclosed `(` against a table of 256 slots, and
  the parse stays linear.

Neither engine lets deep nesting exhaust the C stack:

- The VM keeps its rule frames in the grant. `mise run gate:vm-nocrash` parses
  200,000 unclosed `[` with std.json under a 512 KiB C stack; the parse fails
  with a syntax error and no crash. On 2026-10-04 that run took 0.41 s and
  peaked at 605 MiB RSS. The same gate parses a 400,000-byte number as one
  token.
- A generated parser recurses in C, and fails with a syntax error past 1,000
  nested rule calls. `mise run gate:parser-nocrash` runs the generated std.json
  parser on 200,000 unclosed `[` and the std.shell parser on 100,000 unclosed
  `$(` under a 512 KiB C stack; with `--full` it adds std.javascript on 200,000
  `[` and std.tsx on 200,000 `(`. Each ends with exit status 1 and no crash.

These are the inputs the tests cover. Under the VM, no test covers deep nesting
for std.json5, std.jsonc, std.javascript, std.shell or std.fence.
