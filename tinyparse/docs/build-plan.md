# tinyparse — build plan and supervised loop

This document is the **durable, resumable schedule** for building `tinyparse`.
A fresh session (or a summarized context) resumes from here. The canonical
design is [`../tinyparse-design.md`](../tinyparse-design.md); the
architecture is [`architecture.md`](architecture.md).

All work lands **directly on `main`**, one commit per workstream, named by its
workstream id (e.g. `tinyparse W4: .scm reader`). Every committed turn is
pushed to `origin/main` by the `push-main.sh` Stop hook.

## The finish line is a program

The finish line is a **data-driven, ratcheting test harness**:

- Test cases are **data**, not code: `test/suite/**/*.test` files, each one a
  CLI invocation plus expected exit code / stdout / stderr. The format is in
  [`test/README.md`](../test/README.md).
- A small C runner, `test/harness.c`, interprets them.
- `test/xfail.list` is the **ratchet ledger**: every test currently allowed to
  fail. We seed the suite with *every intended behaviour* up front, all listed
  in `xfail.list`, so the suite starts at `PROGRESS=0/n` and red.
- `ci/tinyparse-metric.sh` builds everything, runs the harness, and prints
  `PROGRESS=k/n`. It exits `0` **only** when `k == n` and `xfail.list` is
  empty. The number only goes up; a drop is a regression.

A workstream is *done* when its tests pass and its entries have been removed
from `xfail.list` — verified by re-running the metric, not by an agent's claim.

Run the metric:

```sh
bash ci/tinyparse-metric.sh            # structural + harness, fast
RUN_GATE=1 bash ci/tinyparse-metric.sh # also build cross-dist + run examples
```

Arm the autonomous loop with a prompt containing `tinyparse-run`; stop it with
`tinyparse-halt`.

## Toolchain

`tinyparse` builds with **Zig 0.15.1** (`zig cc` / `zig ar`), pinned in
[`../mise.toml`](../mise.toml) — independent of the repo's 0.16 freestanding
toolchain. `scripts/tinyparse-setup.sh` (SessionStart) provisions it. The
portable build engine is [`../build.sh`](../build.sh); the `mise` tasks wrap it.

`tinyparse` **consumes the release artifacts in `dist/`**: it links the
prebuilt `dist/<platform>/libax.a` and `#include`s `dist/<platform>/ax.h`
rather than recompiling `ax`. `build.sh` populates `dist/` (via the repo's
`make libs` / `ci/zig-build.sh`) if it is empty.

## Deliverables (the contract)

- `tools/tinyparse/dist/<platform>/` ships `libtinyparse.{a,so}` for
  `linux-x86_64`, `libtinyparse.{a,dylib}` for `macos-arm64`, and the
  `tinyparse` CLI per platform.
- The CLI links the prebuilt `dist/<platform>/libax.a`.
- Zig 0.15.1 compiler, `mise.toml` for deps + tasks.
- Regular C, **not** literate C.
- Design + architecture documented under `docs/`.
- An embedded manpage (`tinyparse man`), in the spirit of `mdc`.
- `examples/`: TCL parser (pathological `echo {*}"a b"${c}d[expr {1+1}]`),
  a native-codegen JIT for a sane C subset (x86-64 Linux, aarch64 macOS, WASM
  bonus), a C `#define`→`enum` rewriter, and an EBNF→`.scm`→generated-`.c`
  pipeline whose output parses RFC 3986 URIs.

## Status

**Phases 0–5 are complete and the metric is GREEN (`PROGRESS=47/47`, empty
ledger before Phase 6 seeding).** The freestanding `libtinyparse` (`.scm` reader → grammar IR →
token/lexical engine → streaming parse → contiguous CST + cursors →
diagnostics), the CLI with an embedded manpage, the Zig 0.15.1 cross-`dist`
matrix, all four examples, binary `pack`/`load`, the C emitter, the recovery
DSL, module `import`, bindings, plugs, lexical goals, and the 0.1.0
distribution all ship.

**Phases 6–9 are COMPLETE; the metric is GREEN at `PROGRESS=291/291`.** The
grammar-distribution layer shipped (packages, `extends`/parameters with
`(override …)`, the embedded registry, `node-types`, `init`/`vendor`, and the
query engine as a parser over a linearized tree, proven on the json/jsonc/json5
family). The standard grammar family is ported to full corpus compatibility
against the pinned tree-sitter references: **`std.javascript` 116/116** (JSX
included; one deliberately-malformed corpus case is asserted rejected),
**`std.typescript` + `std.tsx` 112/112**. The ports needed three engine
primitives — `(reserved …)`, `extras: none/all`, and `\xHH` byte classes —
all documented in the design. Phase 9 benchmarked `std.javascript` against
tree-sitter (`ci/tinyparse-bench.sh`); the honest result is in the README's
*Compared to tree-sitter* section: tree-sitter is ~45× faster, the structural
cost of pre-generated GLR tables versus runtime grammar execution. W39 (JSX)
folded into W38, since tree-sitter-javascript carries JSX.

The design is in [`../tinyparse-design.md`](../tinyparse-design.md) under
*Grammar packages*, *Grammar composition*, *Queries as parsers over linearized
trees*, *Embedding without a runtime dependency*, and *Seeding a project*.

## Workstreams (dependency-ordered schedule)

Legend: ✅ done.

### Phase 0 — finish line + scaffold (supervisor; landed first)

- ✅ **W0** scaffold: dirs, `mise.toml`, `build.sh`, `README.md`, docs skeleton,
  `.gitignore`.
- ✅ **W1** finish line: `test/harness.c`, the `.test` format, the seeded suite,
  `test/xfail.list`, `ci/tinyparse-metric.sh`, the SessionStart setup and the
  Stop/arm loop hooks.

### Phase 1 — core library (the floor)

- ✅ **W2** public header `include/tinyparse.h` (design API → regular C).
- ✅ **W3** memory/arena + `tinyparse_string` utilities over the host grant
  (reusing `ax` arenas from `dist` `libax.a`).
- ✅ **W4** `.scm` reader (lists, symbols, `keyword:`, strings, integers,
  byte/hex, line comments).
- ✅ **W5** grammar validation, name interning, rule/token IR.
- ✅ **W6** token + lexical engine (literals, small regex subset, state-machine
  tokens, `extras`: trivia/skip/error).
- ✅ **W7** rule combinators → `ax` machine lowering (`seq`/`choice`/`repeat`/
  `optional`/`field`); pump to quiescence.
- ✅ **W8** scanner + streaming `push`/`finish`, contiguous CST arrays, cursor
  API, structured diagnostics.

### Phase 2 — CLI + packaging

- ✅ **W9** CLI: `check`, `run --input --dump-tree`, exit codes, diagnostics
  rendering; links `libtinyparse` + `dist` `libax`.
- ✅ **W10** manpage `tinyparse.1.md` + embed (generated C array) + `man`
  subcommand.
- ✅ **W11** `dist` cross-build with Zig 0.15.1: `libtinyparse.{a,so}`
  (linux-x86_64), `{a,dylib}` (macos-arm64), CLI per platform; consume `dist`
  `libax`. `mise` tasks.
- ✅ **W12** docs: `architecture.md`, `grammar.md`, `cli.md`.

### Phase 3 — examples (each flips its xfail tests green)

- ✅ **W13** TCL parser + pathological substitution case.
- ✅ **W14** C `#define`→`enum` rewriter (lossless CST rewrite).
- ✅ **W15** EBNF→`.scm` converter + generated `.c` URI parser (RFC 3986),
  driven by a second program.
- ✅ **W16** JIT for a sane C subset: native x86-64 (Linux) + aarch64 (macOS)
  codegen, executed via W^X memory; WASM emission as a bonus.

### Phase 4 — grow into the full design doc

Introduced in tranches and ratcheted to zero, same as before.

Tranche 1 (CLI-testable, self-contained):

- ✅ **W21** binary grammar `pack` + `load_binary_grammar`: `tinyparse pack
  g.scm --out g.tpg`; `run` auto-detects a `.tpg` and parses identically.
- ✅ **W22** C emitter: `tinyparse c g.scm --out g.c [--bundle]`; the bundle is a
  standalone parser program linking `libtinyparse`.
- ✅ **W23** recovery DSL: `(recover … (on eof … insert: …))` — a malformed parse
  yields a MISSING/error node plus a diagnostic instead of aborting.
- ✅ **W24** module import: `(import name)` resolved by the CLI from the grammar's
  directory (the host module plug, filesystem-backed).

Tranche 2 (harder; complete):

- ✅ **W25** bindings: `(class NAME)` token primitive + `tinyparse_bind`; the CLI
  installs built-in character classes (alpha/digit/alnum/space).
- ✅ **W26** plugs: `tinyparse_plug_install` + a trace plug; `run --trace` prints
  engine trace messages through it.
- ✅ **W27** parser-directed lexical goals (`lex-goal`): the scanner selects a goal
  from parser context (the JS `/` regex-vs-divide case).

### Phase 5 — distribution (release 0.1.0)

Ship downloadable, installable artifacts; details in
[`distribution.md`](distribution.md). Cross-built from Linux with Zig 0.15.1;
released by pushing a `tinyparse-v*` tag.

- ✅ **W17** `VERSION` (0.1.0) + `release.sh`: per-platform tarballs
  (`bin/lib/include/share/man` + INSTALL) for linux-x86_64 and macos-arm64;
  version consistency across `VERSION` and the CLI.
- ✅ **W18** `amalgamate.sh`: the dumb concatenator → `tinyparse.{c,h}` +
  `tinyparse_cli.c`, with `ax.c` folded in and hidden (visibility pragma) so
  a shared lib exports only `tinyparse_*`; an amalgamation tarball.
- ✅ **W19** `install.sh`: detect OS/arch, download the matching release tarball,
  install into a prefix (`--from-file`/`--prefix` for offline/custom installs).
- ✅ **W20** `.github/workflows/tinyparse.yml`: path-scoped build gate; on a
  `tinyparse-v*` tag, run `release.sh` and publish a GitHub Release with all
  artifacts attached.

### Phase 6 — grammar family foundation (the floor for everything below)

This phase is the floor: packages, composition, and the typed surface every
later phase consumes. It lands and is frozen before the query engine and the
ports start. Built on the existing `import` merge (W24).

- **W28** `extends` + parameters: extend a base grammar and gate productions on
  features. `(when PARAM: X)` includes `X` only when the parameter is set;
  `extends: (base PARAM: #t)` pulls a base in with parameters bound. Reuses the
  W24 compile-time merge. **Collision rule (decided):** new names merge
  additively; redefining a base name is a compile error unless wrapped in
  `(override …)`. Done: tests show a parameter toggling a production on/off, an
  extension adding a new rule, an extension overriding a base rule via
  `(override …)`, and a bare redefinition rejected with a clear diagnostic.
- **W29** package format: read `package.scm` (`name`/`version`/`params`/
  `extends`/`provides`); a package resolver maps a package name to its
  directory and binds its parameters. Done: a multi-file package compiles and
  parses through the CLI.
- **W30** embedded registry: bake the standard packages into the CLI with Zig
  `@embedFile`; `tinyparse grammars` lists them; name resolution falls back to
  the embedded registry when no search path resolves a name, and a path entry
  shadows the embedded copy. Done: resolving `std.json` works with no filesystem
  package present.
- **W31** `node-types.json` generation: `tinyparse node-types g.scm --out
  node-types.json` emits the node schema (kinds, fields, child types, supertypes).
  Done: the schema round-trips the grammar's named nodes and fields.
- **W32** `tinyparse init` / `vendor`: scaffold a project in `--amalgamation`
  (default) or `--lib` mode, optional `--grammar NAME`; the amalgamation is
  embedded in the CLI via `@embedFile`; the starter `main.c` compiles its
  grammar at runtime. Done: `init` produces a tree that builds with one `cc` and
  parses stdin, with no tinyparse install on the path.

### Phase 7 — query engine (queries as parsers over linearized trees)

- **W33** tree linearization: an internal pre-order `ENTER kind field? / LEAF
  text / EXIT kind` stream over any CST. Done: a round-trip walk reproduces the
  tree's named structure.
- **W34** query reader + compiler: an s-expr query compiles to a grammar over
  the linearized stream; captures become fields, descendant matching becomes a
  balanced-subtree skip rule, the `.` anchor disables the skip. Done: a query
  matches the documented `call_expression function: (identifier) @fn` shape.
- **W35** query public API + predicates: `tinyparse_query_*` plus the built-in
  `eq?` and `match?` predicates (byte compare and the engine's regex, no host
  binding); `tinyparse query g.scm q.scm --input f` runs it. Done: captures
  and predicate filtering report through the CLI.
- **W36** prove parameterization end to end: `std.json` → `std.jsonc` /
  `std.json5` as one parameterized family via W28/W29, with conformance tests.
  Done: the three variants share one grammar and each passes its own corpus.

### Phase 8 — reference grammar ports (the long ratchet)

The reference grammars are the first real grammars. **The bar is full
compatibility: every tree-sitter corpus case must parse** — `JS_COVERAGE=116/116`
(accept level: `run` accepts the input with no `ERROR`/`MISSING` node), and the
whole tree-sitter corpus is committed as conformance tests, not a hand-picked
subset. Porting the grammar is mechanical translation of tree-sitter's
productions into `.scm`; where a construct genuinely cannot be expressed in the
current engine, the engine is extended rather than the case skipped. Known
engine work the full port needs (authorized, design updated for any public
change): **keyword reservation** (an identifier token that excludes reserved
words / a word-boundary), **token-scoped trivia** so comments are not consumed
inside a `template_string`, and **Unicode** in character classes plus `\u{…}`
escapes and Unicode whitespace/line terminators. JSX is part of
tree-sitter-javascript, so `std.javascript` includes it. Tree-equality with the
expected S-expressions is the next bar after full acceptance.

- **W37** vendor the references (DONE): `tree-sitter-javascript` and
  `tree-sitter-typescript` cloned under `reference/`, pinned in
  `reference/PINS.txt`, gitignored (multi-MB generated parsers), never under
  `grammars/std`. Their `test/corpus` is the source for the accept tests and the
  Phase 9 benchmark.
- **W38** port `std.javascript`: ASI via a line-terminator lexical goal plus
  recovery; ambiguity via a precedence ladder (no GLR, no external scanner);
  regex-vs-`/` and template literals via lexical goals. Done: the JavaScript
  corpus subset is green.
- **W39** add JSX (`jsx:` parameter) → `std.jsx`: a JSX-text lexical goal and the
  element rules. Done: the JSX corpus is green.
- **W40** port `std.typescript` (`types:` parameter): the type grammar layered on
  the JS base. Done: the TypeScript corpus is green.
- **W41** `std.tsx` (`jsx:` + `types:`): the full family. Done: the TSX corpus is
  green and all four variants share one grammar.

### Phase 9 — benchmark vs tree-sitter (the first question users ask)

- **W42** benchmark harness `ci/tinyparse-bench.sh`: parse the reference
  corpus with both tree-sitter (`libtree-sitter` + the compiled upstream
  grammar) and tinyparse (`libtinyparse` + the port), over the same inputs.
  Measure parse throughput, per-file wall time, and peak memory; take the median
  of N runs; capture hardware (CPU model and core count, RAM, OS) from `/proc`.
  Gated behind `BENCH=1` so the loop stays fast. Done: the harness prints a
  reproducible table plus the hardware line.
- **W43** the README section: generate `## Compared to tree-sitter` in
  [`../README.md`](../README.md) from the harness output — the per-input table,
  the hardware line, and an **honest** framing of the trade-off (tinyparse's
  edge is footprint, freestanding embedding, and runtime grammar compilation;
  tree-sitter's is raw throughput from its GLR tables and hand-tuned scanner).
  The section reports the numbers as measured, win or lose. Done: the section
  exists in the README with reproducible numbers and the hardware stats.

### Phase 10 — close the tree-sitter performance gap

**Status: W44–W46 landed; parse is 122 → 12 ms, the tree-sitter ratio ~45× →
~4.6×, correctness GREEN at 291/291 throughout.** W44 (pre-intern, 1.7×), W45
(token-match memo + first-byte dispatch, 2×), and W46 (first-set prediction +
extras memo, 3×) are done and the perf ceiling is ratcheted to 0.012s. W47
(compiled lexer dispatch) is subsumed by W45's token memo. W48
(`tinyparse_parser_reset`) and a packrat whole-rule memo remain optional; the
profile shows the residual cost is the irreducible recursive-descent IR walk, so
further gains are small and closing the rest would need a generated parse table.

Execute the ranked proposal in [`performance.md`](performance.md). The fair
benchmark put tinyparse ~39× behind tree-sitter, and the callgrind/gprof
profile localized the cost: token matching (`tp_match_ir`, ~45%) and string
interning on the hot path (`tp_intern_find`, a linear scan, ~32–44%), then
ordered-choice backtracking (`tp_parse`, ~13%) and trivia (`tp_consume_extras`,
~6%). The work moves cost from parse time to the one-time grammar compile,
keeping the runtime-compiled, actor-shaped, freestanding design.

**The finish line is two programs.** Correctness is the hard floor; speed is a
ratchet:

- **Correctness invariant.** `ci/tinyparse-metric.sh` MUST stay GREEN at
  291/291 after every workstream. No optimization may break a single corpus
  case — this is non-negotiable and checked first.
- **Perf ratchet.** A new `ci/tinyparse-perf-gate.sh` runs the parse benchmark,
  reads a recorded ceiling from `test/perf-baseline.txt` (median ms), and fails
  if the current median exceeds the ceiling beyond a small noise margin. Each
  landed workstream lowers the ceiling; the number only goes down. It prints
  `PERF=<current_ms> ceiling=<ms> ratio_vs_treesitter=<x>`.
- **Per-workstream proof.** Re-profile with callgrind before/after and record the
  before→after median in the commit. A workstream is done only when correctness
  is GREEN, the ceiling has dropped, and its structural change is verified.

Target: items W44–W46 should bring the gap from ~39× into the
single-digit-to-low-teens × band (the profile's two dominant costs plus
backtracking). Fully closing it would require generating a parse table, which is
tree-sitter's design, not this one; that is out of scope.

Dependency-ordered workstreams:

- **W44** (the floor; biggest win, no public change) pre-intern symbols at
  compile time: after grammar compilation, resolve every name reference in the
  IR to its interned id, and remove `tp_intern`/`tp_intern_find` from the
  `push`/`finish` path. Done: correctness GREEN; a debug assertion that
  `tp_intern` is never called during a parse; median drops (expect the ~32–44%
  share to translate to a ~1.5–1.8× speedup).
- **W45** packrat memoization of rule results per input position: cache
  `(rule, position) → outcome` so ordered-choice re-entry stops re-parsing the
  same span. Done: correctness GREEN; median drops; memo memory bounded by the
  pass. Depends on W44.
- **W46** first-set / lookahead prediction: compute each alternative's first set
  at compile time and skip alternatives whose first set excludes the next token,
  pruning dead branches before descent. Done: correctness GREEN; median drops;
  callgrind shows fewer `tp_parse`/`tp_match_ir` calls. Complements W45.
- **W47** compiled lexer dispatch: a compile-time first-byte table plus a
  per-position token-match memo so `tp_match_ir` is not re-interpreted on each
  ordered-choice attempt. Done: correctness GREEN; median drops (targets the
  ~45% token-match share). Largest engine change; lands after the rule-level
  wins.
- **W48** `tinyparse_parser_reset` / arena reuse: a convenience for embedders
  and a small setup saving (the profile shows setup is not the bottleneck, so
  this is ergonomics, not the speed fix). Public surface change → design doc
  first. Done: correctness GREEN; parser reuse works; design updated.

Teardown keeps `ci/tinyparse-perf-gate.sh` + `test/perf-baseline.txt` as the
ongoing perf-regression gate, so a future change cannot silently slow parsing.

### Phase 11 — lift execution onto ax (make the actor model real)

The honest reckoning: tinyparse's identity is an actor-shaped parser on `ax`,
but the parser has never used `ax` beyond its arena allocator — `tp_execute` is
plain C recursive descent, and the *Actor model* sections of the design were
aspiration that never shipped. This phase makes them true. **Decided direction
(owner), after design exploration:** the first cut in
[`ax-execution.md`](ax-execution.md) — fine-grained statecharts, tokens driving
transitions — was *rejected* as genuine but 5–15× slower. The chosen design is
the composition in [`ax-topologies.md`](ax-topologies.md): a **bytecode VM
managed by one actor** (the dispatch loop runs inside a single transition action;
`ax` sees only `input.chunk`/`input.end`), a **compiler actor-pipeline**, and
**child VM actors for sub-parses and language injection**, with journal/replay
deliberately unused (parsing is ephemeral). **Perf stance: this is now expected
to be FASTER, not slower** — the VM loop is 2–5× over the recursive-descent IR
walk and the per-chunk `ax` cost amortizes away, so the principled choice is also
the quicker one (estimate ~1.5–2.5× tree-sitter, under today's 4.6×). The
benefit is the design fulfilled *and* a speedup, plus the marquee capability the
actor model unlocks: embedded-language injection (SQL-in-strings, code-in-
Markdown) as child VM actors.

**The finish line is the same public surface, a different engine underneath:**

- **Frozen API.** `include/tinyparse.h` MUST be byte-identical to its
  Phase-11 baseline; the benchmarks and `axtest`/the 291-case suite compile and
  run **unchanged**. A check asserts no diff to the public header.
- **Correctness floor.** `ci/tinyparse-metric.sh` GREEN at 291/291, now via
  the `ax` execution path.
- **Genuinely on ax (the point).** A structural check asserts the parse path
  actually exercises the `ax` actor API (`ax_open`/`ax_define`/`ax_spawn`/
  `ax_post`/`ax_step`), the inverse of the gap that started this phase.
- **Perf re-benchmarked, expected at/under 4.6×.** Re-run
  `ci/tinyparse-bench.sh`; the VM-on-`ax` path is expected to *beat* today's
  recursive descent, so the perf-gate still applies — this phase should not
  regress speed and ideally improves it.

Dependency-ordered workstreams. **Phase 11 is complete:** the statechart cut was
rejected, the VM composition was built, and the parse now runs on `ax` by default
behind the frozen API. Status and outcome of each workstream:

- **W57** (design, DONE) two design docs: [`ax-execution.md`](ax-execution.md)
  (the rejected fine-grained-statechart cut, kept as the record of why) and
  [`ax-topologies.md`](ax-topologies.md) (the chosen composition — VM managed by
  an actor, compiler actor-pipeline, child-actor sub-parses and injection,
  no journal/replay). **Outcome:** verdict to build the composition; it set the
  shape every workstream below followed.
- **W58** (compiler actor-pipeline) grammar compilation as a dataflow of actors:
  reader → intern → IR → resolve ids/first-sets → emit bytecode; module-load as
  a plug. **DEFERRED (optional).** Compilation is one-time and off the timed
  path, so staging it as an actor pipeline buys integrity, not speed; the
  compiler stays plain C for now and the workstream is left open as an optional
  follow-up. It does not block the genuinely-on-`ax` goal, which the parse path
  (W60) carries.
- **W59** (bytecode VM, DONE) the ISA from [`bytecode-vm.md`](bytecode-vm.md):
  compile the IR to bytecode plus a dispatch loop — terminals, ordered choice via
  Choice/Commit over explicit backtrack/return stacks, Call/Return, CST
  open/close + fields, `reserved`, lexical-goal switch, recovery. **Outcome:**
  `src/vm.c` + `src/vmcompile.c` run in process behind `TINYPARSE_ENGINE=vm`,
  byte-identical to `tp_execute`.
- **W60** (VM managed by an actor + `ax` harness, DONE) the engine opens `ax_sys`
  (`ax_open`); the parser actor owns the program in `ax_heap`, registers in
  `ax_data`, and `ax_alloc`-grown backtrack/return/CST stacks; `push`/`finish`
  post `input.chunk`/`input.end`; the loop runs inside one action; pumped to
  quiescence. **Outcome:** `src/vmactor.c` parses end-to-end on `ax`
  (`TINYPARSE_ENGINE=vm-ax`); `TINYPARSE_TRACE_ACTOR` proves the action ran.
- **W61** (full grammar on the VM-on-`ax` path, DONE) all combinators, the lexer +
  lexical goals + extras, recovery; the W44–W46 wins carried into the VM.
  **Outcome:** the json/sexpr/JS/TS families parse green via `ax`, with
  `tp_execute` as the byte-identical differential reference.
- **W62** (language injection — the marquee, DONE) parse a span with a *different*
  grammar and splice one composite, language-tagged tree. **Outcome:**
  `src/inject.c` + `grammars/std/fence` implement `(inject grammar: …)` with
  compile-time grammar resolution, offset mapping, name re-interning, and
  per-node language tags surfaced by `--dump-tree`. The child runs as an inline
  sub-VM for this pass (the spawned child-actor shape in
  [`ax-topologies.md`](ax-topologies.md) is the eventual form).
- **W63** (full corpus on `ax`, DONE) **Outcome:** all tests green through the
  VM-on-`ax` path, made the CLI default; `tp_execute` kept as the differential
  oracle and the `TINYPARSE_ENGINE=recursive` opt-out rather than retired.
- **W64** (re-benchmark + docs, DONE) **Outcome:** the actor-hosted VM is now the
  **library** default (set in `tinyparse_open`, not just the CLI), so the
  benchmark harness and any embedder parse on `ax` with no flag; the parser-actor
  spec is defined per engine so re-opening the engine no longer drops off the
  actor path. Re-benchmarked: tree-sitter ~4× on the JS corpus, and the ax VM is
  ~1.1× faster than `tp_execute`. The design doc, README, and `performance.md`
  are reconciled to the real engine, and language injection is documented.

Teardown keeps the frozen-API and genuinely-on-ax structural checks as ongoing
gates, so the engine cannot silently drift back off `ax`.

### Phase 12 — no static mutable state, growable arenas (DONE, W65)

Enforce `ax`'s own "no global mutable state" rule inside tinyparse and remove
the fixed caps that silently truncated large grammars/paths. **Outcome:** the one
remaining mutable static (the CLI module-resolver context) moved to the heap; the
freestanding library holds none; the nodetypes field/type/children tables, the
`token.c` FIRST-set stack, the CLI path buffers, and the bench block registry are
all arena-grown. `ci/tinyparse-no-statics.sh` guards the rule (ast-grep, wired
into the metric); over-cap tests prove no truncation. Byte-identical, API frozen.

### Phase 13 — real-workload benchmark + VM optimization (DONE, W66–W67)

A real benchmark over a real codebase, and the optimization it motivated.
**Outcome:**
- **W66** vendored ReactJS (pinned, gitignored) and built
  `ci/tinyparse-react-bench.sh`: parse the React sources with `std.tsx` and
  collect every function definition (1070 clean-parsing files, 9.5 MB; the same
  set for both engines). `pack` learned to resolve packages so `pack std.tsx`
  produces a self-contained `.tpg`. A token-by-symbol index made
  `tp_vm_find_token` O(1).
- **W67** the benchmark exposed a catastrophic PEG-backtracking blowup on deeply
  nested TS/JSX (a 3.7 KB file: 279 ms / ~100 MiB; worst ~1 GB / 5.7 s). Fixed
  with a rule memo: failure memoization (don't re-descend a doomed alternative)
  plus success memoization (snapshot a built subtree, replay it as fresh nodes).
  The memo is adaptive — dormant until a parse goes super-linear — so the JS perf
  ratchet is preserved; `TINYPARSE_RULEMEMO=always` forces it on (and
  `vm-diff` runs a forced-on pass so the replay path stays byte-identical).
- **Result, as measured (not tuned to a number):** the blowup is gone (bounded
  memory, no OOM); traversal is **2.8× faster** than tree-sitter on the React
  tree; raw parse is **39×** slower (adaptive default) / **11.7×** (memo forced
  on). The ≤1.5× target was not reached — that needs generated parse tables,
  tree-sitter's design, not tinyparse's runtime-compiled one. See
  [`docs/performance.md`](performance.md) and [`docs/decisions.md`](decisions.md).

## The loop (supervisor's turn)

> read the metric → pick a red workstream → delegate it to an agent (disjoint
> files only) → verify by re-running `ci/tinyparse-metric.sh` → remove the
> landed tests from `xfail.list` → commit (named by workstream) and push →
> re-read the metric → repeat until green.

Parallelise only agents whose files **and** verification are disjoint;
serialise anything that recompiles into shared `dist/` paths. If `PROGRESS`
has not risen across two stops, the loop disarms and surfaces the blocker.

## Teardown (when green)

- Fold the durable rules (freestanding constraints, the `dist`-consume seam,
  the data-test discipline) into `architecture.md`.
- Keep `test/harness.c` + `xfail.list` + `ci/tinyparse-metric.sh` as the
  ongoing gate.
- Remove the loop/arm scaffolding; keep `push-main.sh`.
