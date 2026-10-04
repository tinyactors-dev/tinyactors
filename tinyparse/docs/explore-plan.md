# pocketparse — exploration project: backtracking, grammar shape, and where speed really lives

This is an **exploration**, run before any decision about a GLR/table engine.
It answers four questions with evidence and fixes one real limitation, then
recommends a direction. The conclusion (build GLR? compile the PEG? left-factor
every grammar?) is a stop-and-ask; the exploration itself runs autonomously.

The questions, from the owner:

1. Can we restructure the JavaScript grammar so it does not backtrack so much on
   the React case — i.e. is the blowup a grammar-shape artifact we can fix at the
   source, separate from the W67 memo?
2. How does pocketparse parse speed compare to the TypeScript compiler's parser
   (`tsc`, via the compiler API) and to `tsgo` (the Go port)?
3. Is the claim "PEGs can be faster than GLR in specific cases" true, and what
   does it mean for us — given a PEG is a different encoding of recursive descent?
4. (pre-authorized) Fix `parser_free` so the arena reclaims: a whole codebase can
   be parsed in one engine session without accreting every tree.

## The finish line is a program

`ci/pocketparse-explore-metric.sh` prints `PROGRESS=k/5` and exits non-zero until
every workstream's artifact exists AND every standing gate is green. The standing
gates (unchanged from the optimization project) are the floor under all of it:

- conformance `ci/pocketparse-metric.sh` 296/296,
- differential `ci/pocketparse-vm-diff.sh` byte-identical (adaptive + forced),
- `ci/pocketparse-no-statics.sh` clean,
- `ci/pocketparse-perf-gate.sh` within ceiling,
- public header `include/pocketparse.h` unchanged **unless a workstream explicitly
  records an API addition with rationale** (W5 may, for `parser_free`).

Per-workstream artifacts the metric checks for:

- W1 → `ci/pocketparse-tsc-bench.sh` (and its tsgo arm) exists and runs.
- W2 → the left-factored grammar lands with conformance still green and a
  recorded before/after backtracking measurement (memo OFF) in `docs/`.
- W3 → a findings section in `docs/peg-vs-glr.md`.
- W4 → an arena-reclaim test (`ci/pocketparse-reclaim.sh`) green.
- W5 → a recommendation section in `docs/peg-vs-glr.md`.

## Workstreams (dependency-ordered)

### W1 — benchmark vs tsc and tsgo (research; concrete, do first)

Extend the React real-workload benchmark with two more reference parsers, parsing
the **same** clean-parsing file set:

- **tsc**: Node + the TypeScript compiler API, `ts.createSourceFile(..., /*setParentNodes*/ false)` — parse only, no type-checking — timed in a warm loop.
- **tsgo**: the Go TypeScript port (`github.com/microsoft/typescript-go`, or the
  `@typescript/native-preview` npm build), driven to parse-only the same files.

Both report total parse time over the corpus so the table reads pocketparse vs
tree-sitter vs tsc vs tsgo. These are production-grade hand-written recursive-
descent parsers (tsc is the canonical TS parser; tsgo its native rewrite), so they
calibrate where pocketparse sits against the *non-table* state of the art, not just
against tree-sitter's GLR. **Done:** `ci/pocketparse-tsc-bench.sh` runs and the
numbers land in `docs/performance.md`.

### W2 — left-factor the JS expression ladder (research + engineering)

The blowup's source is the precedence ladder of **ordered** choices, where
`assignment_expression`'s alternatives (`arrow_function`, `lhs_expression "="…`,
`ternary_expression`) each re-parse the shared operand. Left-factor so the operand
is parsed **once** and the following token selects the production. Measure with the
memo OFF (`POCKETPARSE_RULEMEMO=off`) so the grammar effect is isolated from the
memo. **Done:** conformance 296/296, differential byte-identical, and a recorded
before/after of React parse time / rule-call attempts with the memo OFF. (Trees may
change shape; conformance is accept-level, so the bar is "still accepts the whole
corpus and the differential agrees vm==recursive".)

### W3 — PEG vs GLR, honestly (research)

Separate the *paradigm* from our *implementation*. A PEG is recursive descent with
ordered choice + memoization; with good prediction and no ambiguity it has no
table-lookup or GSS overhead and is cache/branch friendly — which is the kernel of
"PEG can beat GLR." Our slowness is largely (a) re-parsing from grammar shape (W2),
(b) **interpreting** IR/bytecode rather than running compiled descent, (c) the
token matcher. Write `docs/peg-vs-glr.md`: what the claim means, where each wins,
and what our numbers (W1, W2) say about which of (a)/(b)/(c) dominates. **Done:**
the findings section exists and cites our measurements.

### W4 — fix `parser_free` (engineering; pre-authorized)

Today `parser->arena == engine->arena`; nothing is reclaimed on free, so a codebase
walk accretes every tree. `ax_arena` is bump-only with no mark/rewind. Plan:

- Add a **savepoint primitive** — `ax_arena_mark` / `ax_arena_rewind` on the bump
  arena (general, low-risk; lives in the ax literate core) **or**, if touching ax
  is undesirable, a pocketparse-side savepoint over a parser-owned sub-arena.
- `parser_new` marks; `parser_free` rewinds, reclaiming that parser's allocations
  for reuse by the next parse (memory returns to the engine, not the host — the
  freestanding model never frees to the host).

**Open decision (flag for the owner):** rewind reclaims the parser's **tree** too,
so the contract tightens to "the tree is valid until `parser_free`," and parsers
must be freed in LIFO order (the codebase-walk pattern: one parser at a time). If
that breaks the frozen-API/lifetime contract or any test, surface it instead of
shipping it. **Done:** `ci/pocketparse-reclaim.sh` shows bounded peak memory across
many parses in ONE engine; all standing gates green; byte-identical.

### W5 — synthesis + recommendation (research)

Tie W1–W4 together into a recommendation in `docs/peg-vs-glr.md`: given the
measured gap to tsc/tsgo and tree-sitter, and the grammar-shape vs interpretation
split, what is the highest-leverage next step — a *compiled, well-factored,
memoized PEG*, or the *runtime-GLR* engine — and why. This is the input to the
owner's go/no-go on the bigger engine work. **Done:** the recommendation section
exists; the owner reviews before any engine is built.

## Loop & discipline

Work on `main`, commit per workstream (named `Wn`), push every landed turn, rebase
on `origin/main`. Standing gates are the floor on every commit. Stop-and-ask points:
the W4 tree-lifetime/API decision, and the W5 recommendation (the whole point is to
report findings, not to start building GLR). Tear down: fold any durable rule into
the guidelines; keep `peg-vs-glr.md` and the new benchmark scripts; drop this plan
when the questions are answered.
