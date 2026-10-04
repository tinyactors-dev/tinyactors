# tinyparse performance

This document is the meter and the map. It states what the
tinyparse-versus-others benchmarks measure, reports the current numbers,
profiles where the engine spends its time, and explains the residual gap to
table-driven and compiled parsers. Every figure here was measured on the
hardware named below and is reproducible from the [appendix](#appendix-reproduce);
the deeper round-by-round history lives in [`decisions.md`](decisions.md) and
[`peg-vs-glr.md`](peg-vs-glr.md).

All numbers below were re-measured on **2026-06-27** on one host: Intel(R)
Xeon(R) @ 2.80GHz (4 cores), 15.7 GiB RAM, Linux 6.18.5 x86_64, while tinyparse
was still pocketparse inside the `ax` repository. The orchestrators are now
`mise run bench:javascript` ([`bench/javascript.sh`](../bench/javascript.sh), the
JavaScript corpus) and `mise run bench:parsers`
([`bench/parsers.sh`](../bench/parsers.sh), the React workload). The harness
sources are under [`bench/`](../bench/).

The move out of ax did not change these numbers. On one machine, in one
session, the CLI built before and after the migration parsed 464 KB of
JavaScript and 503 KB of TypeScript with best-of-10 times within 0.5% of each
other, at the same peak RSS (106,472 KB before, 106,524 KB after). A
`mise run bench:javascript` after the move, on an Intel Xeon 2.10GHz with 4
cores, measured a 3.774 ms median parse against tree-sitter's 2.784 ms (1.36x)
and a walk of 19.19 against 154.16 ns per node.

## The headline

tinyparse runs two interchangeable engines over one grammar. The **bytecode
VM** is the default: it accepts a grammar as data and parses with no build step.
The **generated C parser** (`tinyparse c --parser-lib`) is the ahead-of-time
path: it compiles a settled grammar to recursive-descent C, byte-identical to the
VM. They trade differently against the field.

- **The generated C parser now beats tree-sitter on raw parse.** On the real
  React/TSX workload it runs at 8.32 MB/s parsing each file in a fresh engine,
  against tree-sitter's 6.84 MB/s, and at 12.00 MB/s reusing one engine across
  the corpus, level with the TypeScript compiler's own parser (tsc, 12.34 MB/s).
  It is 1.78× the throughput of the VM it is generated from.
- **The VM trails on parse and that is the runtime-compiled tax.** On the small
  JavaScript corpus the VM parses about 1.5× slower than tree-sitter; on React it
  is 1.45× slower. The VM interprets a grammar it compiled at runtime; tree-sitter
  runs a pre-generated table. The generated C parser is how you buy that back when
  the grammar has stopped changing.
- **Traversal is tinyparse's to win, on either engine.** The CST is one
  contiguous array and a cursor is an index into it, so a walk is a linear sweep.
  Collecting every function definition from the parsed React tree, tinyparse is
  **5.5× faster** than tree-sitter (43.7 ms against 239.3 ms over ~29,000
  functions).

The rest of this document backs each of these with the full table, the
methodology that makes the comparison fair, and a profile of where the remaining
time goes.

## The JavaScript corpus

The fixed corpus is the input halves of all 116 cases in the vendored
tree-sitter JavaScript test suite, 11,481 bytes total. `std.javascript` parses
**115 of 116** cleanly and correctly rejects the one deliberately malformed case
(its tree-sitter reference output is itself an `(ERROR …)` recovery tree). Both
runtimes parse in-process; the one-time grammar/runtime load is excluded from the
timed loop; the median is over 200 iterations. The tree-sitter runtime is pinned
at `d861eab2`.

This corpus measures the **default VM**, used per its API (a parser per document,
since tinyparse has no parser-reset). tree-sitter is reported both ways: reusing
one parser (idiomatic) and creating one per document (symmetric), so the
parser-setup cost both sides pay is visible rather than hidden.

| variant   | parser      | median parse | throughput | gap          |
| --------- | ----------- | ------------ | ---------- | ------------ |
| idiomatic | tinyparse | 4.84 ms      | 2.26 MB/s  | —            |
| idiomatic | tree-sitter | 3.28 ms      | 3.34 MB/s  | 1.5× faster  |
| symmetric | tree-sitter | 3.65 ms      | 3.00 MB/s  | 1.3× faster  |

### Traversal

Parsing is one axis; walking the built tree is another, and a host that parses
once and queries many times pays only the second. The benchmark parses each
document **untimed**, then times **only** a preorder depth-first cursor walk that
touches every node. The two trees differ in shape, so each side reports its own
node count and the comparable figure is nanoseconds of walk work per node.

| parser      | nodes | median walk | ns/node |
| ----------- | ----- | ----------- | ------- |
| tinyparse | 8,139 | 0.20 ms     | 24.2    |
| tree-sitter | 5,465 | 0.52 ms     | 95.8    |

tinyparse walks at about **24 ns/node against tree-sitter's 96**, roughly 4×
faster per node. It freezes its CST into a single contiguous array of records; a
cursor is an index into it and a depth-first walk is a near-linear sweep the
prefetcher loves. tree-sitter's pointer-linked tree reconstructs node state as
the cursor moves, which costs more per node. This advantage is entirely separate
from the parse-time gap: the slow part of the VM is building the tree, not walking
it.

### Footprint

Measured fairly (the base grant plus every block the engine actually grew, rather
than the over-provisioned grant size or process RSS), one corpus pass has a **14.1
MiB high-water mark, of which 13.5 MiB is the one-time grammar compile** and 0.6
MiB is the parse arena for all 116 documents. A single document's parse is about
0.1 MiB; the per-pass figure is larger only because `tinyparse_parser_free`
does not return memory to the arena, so a pass accretes. tree-sitter, carrying
pre-generated tables, grows on demand to a 2.9 MiB peak. The grammar compile
dominates tinyparse's footprint, and it is paid once.

## The React/TSX workload

The corpus above is small, tidy documents. The second benchmark runs a real task
over a real codebase: parse the ReactJS source tree with `std.tsx` and collect
every function definition — the pass a linter, a doc generator, or a codemod
makes. React is pinned, gitignored reference data. The corpus is every
`packages/*/src/*` file `std.tsx` parses cleanly: **1070 of 1482** files,
9,546,659 bytes. React's sources are Flow-typed, and Flow is not TypeScript, so
the files `std.tsx` cannot accept are skipped; every engine parses the exact same
set.

### Parse: six parsers, one corpus

Parse-only, warm, median of 30 (in-process arms) / 15 (tsc, meriyah). The two
tinyparse C arms parse the identical bytes — `gate:cc-diff` proves
the generated parser and the VM build byte-identical trees — and differ only in
whether the engine and grammar are reused across files (persistent) or reopened
per file to bound peak memory (fresh).

| parser (parse-only)        | median  | throughput  | peak RSS |
| -------------------------- | ------- | ----------- | -------- |
| meriyah (JS+JSX subset)    | 0.198 s | 29.21 MB/s  | —        |
| tsc (TypeScript compiler)  | 0.738 s | 12.34 MB/s  | —        |
| tinyparse (C, persistent)| 0.759 s | 12.00 MB/s  | 150.8 MB |
| tinyparse (C, fresh)     | 1.094 s | 8.32 MB/s   | 98.8 MB  |
| tree-sitter (TSX, GLR)     | 1.330 s | 6.84 MB/s   | 22.4 MB  |
| tinyparse (bytecode VM)  | 1.949 s | 4.67 MB/s   | 50.4 MB  |

Read it carefully, because the ordering is the whole story:

- **The generated C parser passes tree-sitter** at 8.32 MB/s fresh (1.22×) and
  12.00 MB/s persistent (1.75×). The persistent arm is **level with tsc** (12.34
  MB/s), the production TypeScript parser. This is a measured win on a real
  workload, and it crossed the line that the round-by-round history in
  [`peg-vs-glr.md`](peg-vs-glr.md) records tinyparse chasing for a dozen rounds.
- **The C parser is 1.78× the VM** (8.32 vs 4.67 MB/s, the same fresh-per-file
  methodology). Compiling the grammar to native recursive descent removes the
  per-opcode interpreter dispatch and the per-node frame machine; what remains is
  parse work a hand-written parser also does.
- **meriyah is fastest but does a smaller job.** It parses only the 541 of 1070
  files that are plain JS+JSX, rejecting the TS/Flow the others accept, so its
  29 MB/s is over a different (easier) subset and is not a like-for-like row.
- **`tsgo` (the Go TypeScript parser) was not re-run on this pass.** Its harness
  needs a full `typescript-go` Go build that is not staged in this environment;
  prior calibration rounds measured it around 29 MB/s, the fastest full-TS arm.
  It is omitted here rather than reported from a stale run.

The C parser's higher peak RSS is the cost of its speed: the persistent arm holds
every parse's arena across the corpus (150.8 MB), and even the fresh arm
pre-sizes generously (98.8 MB). tree-sitter's 22.4 MB is the leanest. Memory, not
throughput, is now the axis where tree-sitter leads the generated parser.

### Collect: traversal on real code

The same run then walks each parsed tree and collects every function definition.
Both sides do the same walk; the function counts are a cross-check and differ by a
handful because the grammars categorize a few constructs at different granularity.

| parser      | median collect | functions | functions/sec |
| ----------- | -------------- | --------- | ------------- |
| tinyparse | 43.7 ms        | 29,093    | 665,125       |
| tree-sitter | 239.3 ms       | 29,079    | 121,501       |

The contiguous-array traversal advantage holds on a 9.5 MB tree and is larger
than on the small corpus: **5.5× faster**. The slow part of tinyparse is
building the tree; once built, it is the faster tree to read.

### The rule memo and PEG backtracking

A PEG with ordered choice can re-parse a shared prefix and blow up (exponential
time, runaway memory) on deeply nested input. tinyparse bounds it with a rule
memo (packrat-style failure and success memoization, byte-identical to the
recursive oracle). The memo ships **adaptive**: dormant until a parse goes
super-linear, so clean parses pay nothing and the VM numbers above are
memo-dormant. It can be forced on from the first call with
`TINYPARSE_RULEMEMO=always`, which is slower on clean input and was not
re-measured on this pass (forcing it across the whole React corpus is
impractically slow, which is exactly why the default is adaptive). The
left-factoring that makes the std grammars linear, so the generated parser needs
no memo at all, is documented in [`peg-vs-glr.md`](peg-vs-glr.md).

## What the benchmarks measure

Each harness receives a manifest of document paths and an iteration count, loads
every document into memory once, parses them all in a warm loop, and reports the
median per-iteration wall time, the aggregate throughput, and a footprint figure.
The one-time cost of loading the runtime and compiling the grammar is excluded
from the timed loop.

The tree-sitter side links the official runtime and the generated grammar
(`tree-sitter-javascript` or `tree-sitter-typescript` TSX) plus its hand-written
scanner, and parses with `ts_parser_parse_string`. The tinyparse side links
`libtinyparse.a` and `libax.a`, compiles the `.scm` grammar once, then for each
document creates a parser, pushes the whole input, and calls `tinyparse_finish`.
The generated-C arm links the unit `tinyparse c --parser-lib` emits and calls
its entry point. The governing rule is that **both sides do the same job**: parse
a document to a tree, then either discard it or walk it identically.

## Fairness audit

Three asymmetries once charged tinyparse for work tree-sitter never did, or hid
work it did, or reported a footprint tinyparse never used. Each is corrected in
the harnesses, and the corrections stand.

- **The extra tree walk (removed).** The old tinyparse harness walked the whole
  CST after every parse while the tree-sitter harness did not. Because
  `tinyparse_finish` builds the tree eagerly — every node record, span, and
  link is populated before it returns, and a cursor only navigates an array that
  already exists — that walk was pure extra work, not deferred parse work. It is
  deleted, so on the parse benchmark **neither side traverses**. (Traversal is
  measured separately, above, with both sides walking.)
- **Parser lifecycle (reported both ways).** tree-sitter reuses one parser;
  tinyparse has no reset and creates one per document. That is a genuine API
  difference, so the JavaScript corpus reports tree-sitter both reusing one parser
  (idiomatic) and creating one per document (symmetric), and the React C arms
  report both persistent and fresh-per-file.
- **Footprint (measured directly).** The harness fronts a small base grant
  and supplies the real grow callback, then sums the base plus every grown block
  into a high-water figure split into the one-time compile and the parse arena.
  That sum, not an over-provisioned grant and not process RSS, is the honest
  footprint.

## Profile: where the VM spends its time

A fresh callgrind run of the default VM over the JavaScript corpus (177.5M
instructions in the steady state) puts the cost here:

| self cost | function | what it is |
| --------- | -------- | ---------- |
| 24.2% | `tp_vm_run` | the bytecode dispatch loop |
| 18.8% | `__memset_avx2` (libc) | zeroing freshly granted arena blocks |
| 18.4% | `tp_token_match_memo` | matching a token at the cursor (memoized) |
| 7.9%  | `tp_vm_consume_extras` | skipping whitespace and comments between tokens |
| 5.3%  | `tp_vm_call_enter` | the per-rule CALL prologue (node open) |
| 3.2%  | `tp_tree_builder_push` | appending a packed node to the CST array |
| 3.0%  | `ax_alloc_raw` | bumping the arena pointer (now `tp_alloc_raw`) |
| 2.5%  | `tp_intern` | symbol interning |

Two things stand out against the engine's history. **Interning is no longer the
bottleneck.** It was about a third of every parse before symbols were resolved to
ids at grammar-compile time; it is now 2.5%. And the cost has spread: the dispatch
loop, the token matcher, and arena zeroing are each roughly a fifth, with no
single slice dominating. Grouped by phase, the VM spends about 37% in dispatch and
the rule lifecycle, 20% matching tokens, 22% allocating and zeroing arena memory,
8% on trivia, and the small remainder building the tree. The arena-zeroing slice
(`memset`) is the clearest remaining lever: much of it is granting blocks the
parse then only partly fills.

The generated C parser deletes the dispatch loop and the frame machine entirely —
its per-rule functions are each under 1% of instructions — which is why it runs
1.78× the VM. What is left there is the genuine parse work tree-sitter's
pre-generated DFA lexer and table-driven core avoid: token matching, the
node-open/close lifecycle, extras, and the tree build.

## The structural floor

The residual gap from the VM to tree-sitter, and from the generated parser to
nothing, is structural and named honestly. tree-sitter ships pre-generated GLR
parse tables and a lexer compiled to a C `switch`; at parse time it is mostly
table lookups and pointer bumps. The tinyparse VM carries no generated tables;
it compiles a grammar to flat bytecode at runtime and interprets it, trading
throughput for the ability to accept a brand-new grammar as data. The generated C
parser closes most of that distance by baking the grammar into native recursive
descent, and on the React workload it crosses tree-sitter; closing the rest would
mean generating a parse table, which is tree-sitter's design.

This project records the floor rather than tuning to a marker: where a generated
parser did not yet beat its target, the history in
[`peg-vs-glr.md`](peg-vs-glr.md) and [`decisions.md`](decisions.md) says so
plainly and leaves the win unrecorded until it is earned. The numbers in this
document are reported as measured, with the one arm that could not be reproduced
here (`tsgo`) called out rather than carried over.

## How the gap closed

The VM did not start here. Under a fair methodology the JavaScript corpus once
parsed about 45× slower than tree-sitter; it is now 1.5×. The path, each step tied
to a profiled cost and gated against regression, is recorded in full in
[`peg-vs-glr.md`](peg-vs-glr.md) and [`decisions.md`](decisions.md). In brief:

- **Pre-interning symbols at grammar-compile time** removed the linear interner
  scan that was a third of every parse.
- **A token-match memo with first-byte dispatch** stopped the token interpreter
  re-matching the same bytes on every ordered-choice retry.
- **First-set prediction** skips the alternatives the next byte cannot begin.
- **Lowering the grammar to a bytecode VM** replaced the original
  recursive-descent executor. At the time the VM ran inside an `ax` actor, which
  cost about 1% over the bare loop; the executor was later retired, and the
  migration out of ax replaced the actor with a plain lifecycle in
  `src/parse.c`.
- **The generated C emitter** then took the settled grammars to native recursive
  descent — per-token baked lexers, compile-time FIRST tests, a native call graph
  with locals instead of an operand-reading loop — which is the arm that now
  passes tree-sitter.

Every step held the tree byte-identical across the differential and golden gates;
the conformance suite (now `mise run test`) stayed green throughout.

## Appendix: reproduce

Run everything from `tinyparse/`. The benchmarks need the pinned references
listed in [`reference/PINS.txt`](../reference/PINS.txt);
`mise run bench:fetch` downloads each as a tarball at its pinned commit. The
tree-sitter runtime is cloned on first run. Benchmarks measure time, so run them
alone, never beside `mise run test`.

The JavaScript corpus benchmark (extracts the corpus, runs idiomatic +
symmetric + traversal + the footprint accounting):

    mise run bench:javascript
    # override the iteration count with ITERS=, e.g. ITERS=200 mise run bench:javascript

The React six-parser parse-only benchmark (needs the pinned React source and
`tree-sitter-typescript`; installs typescript and meriyah into a gitignored
scratch directory; the corpus manifest is built by the React workload bench):

    REGEN=1 mise run bench:react    # builds the manifest + parse/collect table
    mise run bench:parsers          # the six-parser parse-only table

The suite and the gates must stay green:

    mise run test --full

Profile the VM with callgrind over the JavaScript corpus (the harness binary
carries function symbols, so no special build is needed):

    G=grammars/std/javascript/grammar.scm
    M=bench/corpus/MANIFEST.txt     # produced by bench:javascript
    valgrind --tool=callgrind --callgrind-out-file=callgrind.out \
      bench/bench_tinyparse "$M" 2 "$G"
    callgrind_annotate --threshold=92 callgrind.out

Everything the benchmarks and the profile produce is gitignored scratch.
