# PEG vs GLR, and where tinyparse's speed actually lives

This note answers, with measurements, three questions raised before any decision
about building a GLR/table parser:

1. Is the React backtracking blowup a grammar-shape artifact we can fix at the
   source? (workstream W2)
2. How does tinyparse compare to the production hand-written recursive-descent
   TypeScript parsers, `tsc` and `tsgo`? (W1)
3. Is "a PEG can be faster than GLR" true, and what does it mean for us? (W3)

It ends with a recommendation (W5). Everything here is measured on one corpus —
the 1070 ReactJS source files `std.tsx` parses cleanly, 9.5 MB — parse-only,
warm, median of several iterations, on an Intel Xeon @ 2.80GHz (4 cores), Linux
6.18.5. Reproduce with `ci/tinyparse-tsc-bench.sh` and
`ci/tinyparse-react-bench.sh` (both `BENCH=1`).

## The numbers

| parser | algorithm | median parse | throughput |
| ------ | --------- | ------------ | ---------- |
| tsgo (typescript-go) | hand-written recursive descent (Go, compiled) | 0.31 s | 29.2 MB/s |
| tsc (compiler API) | hand-written recursive descent (TS on V8, JIT) | 0.73 s | 12.5 MB/s |
| tree-sitter (TSX) | **GLR** parse table + DFA lexer (compiled C) | 1.10 s | 8.3 MB/s |
| tinyparse — before W2 | interpreted PEG, un-factored grammar | ~43 s | 0.2 MB/s |
| tinyparse — after W2 | interpreted PEG, left-factored grammar | 32 s | 0.28 MB/s |

Two cross-checks make this apples-to-apples: tsc and tsgo both report
`top_statements=10633` over the corpus (same bytes, comparable trees), and the
tinyparse/tree-sitter walk both collect ~23–29k function definitions.

## Finding 1 — the blowup is grammar shape, and it is fixable at the source (W2)

The pathological files were not slow because the engine is slow; they were
*exponential* because the JavaScript grammar's expression precedence is a ladder
of **ordered** choices in which alternatives re-parse a shared operand prefix.
Concretely, `assignment_expression` tried `lhs_expression "=" rhs` first and, on a
non-assignment, threw the whole operand parse away and re-parsed it as a ternary —
on every level of a nested expression. That shape was inherited from the
tree-sitter grammar, whose `choice(...)` is *unordered* and whose LR table merges
the shared prefix into one state for free; our PEG port turned it into ordered
choice with repeated work.

Left-factoring that one rule — parse the operand **once** as a ternary, then an
optional assignment tail — removed it. Measured with the rule memo **OFF**, so the
grammar effect is isolated (W2-MEASURED):

| React file | before | after | memory |
| ---------- | ------ | ----- | ------ |
| inspectedElement-test.js | 7.83 s | 0.49 s (16×) | 1 GB → 112 MB |
| store-test.js | 4.74 s | 0.55 s (8.6×) | 649 MB → 124 MB |
| ReactHooksInspectionIntegrationDOM-test.js | 0.32 s | 0.033 s (10×) | 54 MB → 9 MB |

Conformance stayed 296/296 and the engines stayed byte-identical. So the answer to
"can we restructure to avoid backtracking" is **yes**, at the source, independent
of the W67 memo — they are complementary: left-factoring removes the re-work where
we can express it, the memo is the general safety net for whatever re-work remains
in any grammar we cannot or have not factored.

## Finding 2 — the residual gap is interpretation, not the algorithm (W1, W3)

With the grammar factored, the blowup is gone — a corpus pass peaks at ~350 MB
with the memo off and ~570 MB adaptive, down from ~1.6 GB, and no single file runs
away — and tinyparse settles at **~0.24 MB/s (memo off) / ~0.28 MB/s (adaptive)**,
the same order as its rate on the small JS corpus (~0.9 MB/s). That is its
*steady-state interpretation throughput*: the cost of walking an IR/bytecode
program in a dispatch loop, byte by byte, token by token. It is no longer
backtracking-bound — note the memo now *costs* a little time-for-memory rather than
saving an exponential (38 s/350 MB off vs 32 s/570 MB adaptive), exactly what you
expect once the re-work is gone from the grammar itself.

Now place that against the field. The fastest parser measured is **tsgo — a
hand-written recursive-descent parser — at 29 MB/s, 3.7× faster than tree-sitter's
GLR.** tsc, also recursive descent, beats GLR too. So on this real workload the
GLR table is *not* the fast option; a well-engineered recursive descent is. That is
the kernel of "a PEG can beat GLR": a PEG *is* recursive descent with ordered
choice (plus memoization for the pathological cases), and recursive descent carries
no graph-structured-stack and no table-indirection overhead. For a grammar with low
local ambiguity and good first-set prediction — which JS/TS is, in practice — the
straight-line descent is branch- and cache-friendly in a way the GLR driver is not.
GLR earns its keep on genuine ambiguity and on incremental re-parse; it does not
make a one-shot parse faster here.

So tinyparse's distance from tsgo/tsc is **not** that it chose PEG over GLR — it
chose the same family as the *winner*. The distance is that tinyparse
**interprets** its grammar where tsc/tsgo are **compiled** (native Go; JITed V8).
The ~20–100× gap is the interpreter, plus the cost of a runtime-compiled grammar
that carries no generated tables. tree-sitter sits in between: compiled tables, but
GLR generality it does not need here.

## Recommendation (W5)

**Do not build a GLR engine to chase speed.** The measurements say it would not get
us there: the GLR parser in the comparison is the *slowest* of the three production
parsers, and a GLR engine would also cost us the most — it breaks the byte-identity
discipline (it produces different trees, resolved by precedence/tables rather than
ordering), needs a token-driven driver with a graph-structured stack, and forces a
conflict/precedence model back onto grammars we wrote as ordered PEGs. GLR remains
interesting for *incremental* re-parse (editor latency), which is a different goal;
it is not the lever for batch throughput.

**The high-leverage direction is a faster PEG**, in three moves, in order of
payoff and increasing effort:

1. **Left-factor the remaining hot rules** (the W2 pattern). Cheap, grammar-only,
   keeps conformance and byte-identity. Removes re-work at the source so the memo
   rarely has to fire.
2. **Compile the grammar instead of interpreting it.** This is the big multiple.
   tsgo proves the paradigm reaches 29 MB/s when the descent is *compiled*; our
   IR-walking dispatch loop is the floor we keep hitting. The runtime-compilation
   identity is preserved — we already turn `.scm` into an IR/bytecode at load; the
   change is the *target*: emit threaded code, or tighter specialized bytecode, or
   (via the existing `tinyparse c` path) actual C compiled at install time. The
   no-native-scanner property (lexical goals) and the actor hosting carry over
   unchanged, and a compiled descent can stay byte-identical to `tp_execute`.
3. **Keep the memo as the safety net**, adaptive as it ships, for any grammar or
   input that still backtracks.

In one line: tinyparse already picked the fast paradigm (recursive descent /
PEG); the win is to *compile* it and *factor* the grammars, not to swap it for the
slower-here GLR. The GLR exploration can be parked unless a specific grammar proves
un-factorable or incremental re-parse becomes a goal.

## W1 left-factoring

The exploration (W2-MEASURED, above) factored `assignment_expression`. W1 carries
the same transform across the rest of the JavaScript and TypeScript grammars:
wherever an ordered `(choice …)` re-parses a shared leading sub-parse, the prefix
is parsed once and the alternatives branch on what follows. The method, the
verification bar, and the traps are captured in the `left-factor` skill
(`tools/tinyparse/.claude/skills/left-factor/SKILL.md`).

**Rules factored**

- `javascript / ternary_expression` — the dominant nested re-parser. Was
  `(choice (seq nullish "?" … ":" …) nullish)`: on a non-ternary it parsed the
  whole `nullish_expression` condition, failed for lack of `?`, and re-parsed the
  same operand as a bare nullish — the assignment ladder's blowup repeated one
  rung down, on every nested level. Now `(seq nullish (optional (seq "?" … ":" …)))`.
- `javascript / export_statement` — four `export`-led alternatives re-matched the
  `export` keyword; parse it once, branch on the next token.
- `javascript / method_head` — the two `async`-led method forms shared their
  `async` head; folded into `async (optional "*") name`.
- `javascript / arrow_function` — the parameter list (parenthesized
  `formal_parameters` or a bare `identifier`) was spelled twice per `async`/non-
  `async` prefix; folded into one inner `(choice formal_parameters identifier)`
  whose arms have disjoint first tokens.
- `typescript / export_statement` — same `export`-head factor as JavaScript,
  across the seven TS export forms.
- `typescript / arrow_function` — same `async`-head + parameter-head factor as
  JavaScript, carrying the TS type-parameter / return-type additions.

(`typescript / conditional_or_type` was already written in the factored shape —
`(seq union_type (optional (seq "extends" … "?" … ":" …)))` — so it needed no
change. The TS expression ladder inherits the now-factored JS `ternary_expression`
through `(extends std.javascript)`.)

**Deliberately left alone**

- `for_statement`'s `(choice for_in_of for_classic)` does re-parse a shared head,
  but the two branches bind structurally different headers (an `in`/`of` clause
  versus three classic clauses) and the choice is reached once per loop, never
  nested in the operand ladder — factoring it cleanly risks an accept change for
  no hot-path payoff.
- `pattern_property` and `import_specifier`'s name-versus-`name as alias` choices
  re-parse a single identifier token, not a nested operand, and sit off the hot
  path; the first-set overlap is one token, so the cost is negligible.
- Choices whose alternatives already lead with distinct first tokens
  (`update_expression`, `unary_expression`, `import_clause`, `meta_property`, the
  `json` family's `Value`, `tsx`'s single-alternative `expression`) are first-set
  predicted and never re-parse — left untouched.

**Before / after (memo OFF, deeply-nested inputs, `TINYPARSE_RULEMEMO=off`).**
Self-contained generated inputs (a chain of nested ternaries; a parenthesized
binary chain that re-descends the whole ladder each level; a mixed
ternary+call+member+binary chain), timed via the CLI before W1 (commit `1388b6c`)
and after. `W14-1-MEASURED`:

| input (depth) | before W1 | after W1 |
| ------------- | --------- | -------- |
| nested ternary (N=17) | 4.34 s | 0.014 s (~310×) |
| parenthesized binary (N=17) | 6.07 s | 0.013 s (~470×) |
| mixed ternary+call+member+binary (N=12) | killed (OOM) after ~117 s | 0.016 s |

The deeper inputs the exploration used (N≈140) OOM-kill the un-factored grammar
within two minutes; the factored grammar parses them in milliseconds. The blowup
was exponential in nesting depth, and left-factoring the ladder removes it at the
source — no single nested expression is backtracking-bound any more. Conformance
held at 296/296 and the engine differential stayed green at every commit.

## W14-3 — `tinyparse c --rd` compiles the PEG to recursive-descent C

The compiled backend the recommendation called for (move 2, "compile the grammar
instead of interpreting it") now exists. `tinyparse c --rd <grammar> --out x.c`
emits C that reads like a hand-written recursive-descent parser: one static
function per lowered program (so one per grammar rule body, plus one per nested
combinator), ordered choice as sequential attempts with save/restore + early
return, repetition as a loop, a rule reference as a function call. The rule-level
interpreter dispatch (`tp_vm_run`'s switch + return stack) is gone — it is baked
into C control flow.

It is **byte-identical to the bytecode VM** because it does not re-derive the
runtime: the emitter (`src/emitrd.c`) walks the SAME lowered program the VM
interprets (`tp_compile_program`) and, for each instruction, emits the C its
`tp_vm_run` case would execute, calling the SAME shared helpers
(`tp_vm_do_match_pattern`, `tp_vm_consume_extras`, `tp_vm_run_call_body`,
`tp_vm_do_inject`, the token matcher, the tree builder). vm.c's helpers and state
(`tp_vm_state`) were lifted into `internal.h` so both executors share one source
of truth; the VM's own `tp_vm_run_call` now delegates to the factored
`tp_vm_run_call_body`, so the CALL/rule-memo semantics cannot drift between them.
The tp_ir pattern pointers a MATCH/BYTE/RECOVER/INJECT carries are runtime
addresses, so the generated parser re-lowers the embedded packed grammar at
startup and reads each instruction's operands from `program->instructions[N]`;
the indices line up because `tp_compile_program` is deterministic.

`ci/tinyparse-cc-diff.sh` is the new independent oracle: it generates the RD C
for each grammar, compiles it against the runtime, and asserts `--dump-tree` is
byte-identical to `TINYPARSE_ENGINE=vm-ax` over the corpus.

**cc-diff-green grammars (W14-3): std.json, std.jsonc, std.json5, the family base
fixture, std.javascript, std.typescript, and std.tsx — 267/267 inputs
byte-identical.** Every construct the corpus exercises is covered: rule/CALL, SEQ,
ordered CHOICE with first-set prediction, REPEAT (zero-width guard + bounds),
OPTIONAL, FIELD, ALIAS, MATCH/BYTE/EOF, extras, GOAL (lexical goals), NO_EXTRAS,
RECOVER (MISSING insertion + diagnostic), INJECT (language injection), and the
adaptive rule memo (the shared CALL helper carries it, exercised by the vm-diff
forced-on pass). New CLI flag `--rd` / `TINYPARSE_EMIT_RD` recorded in
decisions.md. Standing gates green at the commit: build clean, metric 296/296,
vm-diff GREEN, no-statics GREEN.

## W14-6: the bytecode VM is iterative, and the tail-call evidence

The bytecode VM used to C-recurse per rule call (`tp_vm_run` -> `tp_vm_run_call`
-> `tp_vm_run`), so a deeply nested input overflowed the C stack — the VM
SEGFAULTED under `ulimit -s 512` on a ~200k-deep input where the W14-3 compiled
parser (already iterative) exits cleanly. A bytecode VM that recurses defeats the
point of bytecode. W14-6 makes the VM iterative and shares ONE driver.

### The one driver

`tp_vm_run` (src/vm.c) is now the single iterative driver: a root frame is pushed
onto an explicit, arena-backed frame stack (`tp_vm_stack` / `tp_vm_frame`, in
`internal.h`) and the loop pumps it until it resolves — each step either RESOLVES
the top frame (set the signal, pop) or DESCENDS (save the resume `step`, push a
child). SEQ / CHOICE / REPEAT / OPTIONAL / CALL all iterate on the arena stack, so
there is **no C recursion across rule calls**; a deep input grows the arena, never
the C stack. The recursive `tp_vm_run` / `tp_vm_run_call` are gone. The CALL
prologue/epilogue were factored into `tp_vm_call_enter` / `tp_vm_call_exit` (the
old `tp_vm_run_call_body`, split at the body-run point) so the body runs on the
same stack. The emitter (`emitparser.c`) no longer emits its own copy of the loop:
the generated parser CALLS the shared `tp_vm_run`, so the VM and every compiled
parser run the identical loop and cannot drift (cc-diff still 267/267,
byte-identical).

`ci/tinyparse-vm-nocrash.sh` proves the fix: the VM (`vm` and `vm-ax`) parses a
~200k-deep input under `ulimit -s 512` and exits WITHOUT a crash signal (exit 1, a
graceful syntax error), and the script CROSS-CHECKS that the recursive reference
(`tp_execute`) still crashes (139) on the same input under the same stack, so the
test is meaningful, not a no-op.

### Tail-call analysis over the real grammars (regenerate with `analyze-tailcalls`)

`tinyparse analyze-tailcalls <grammar>` lowers a grammar and reports how many
rule CALLs sit in TAIL position (the CALL's signal is the enclosing rule's signal
with no epilogue — propagated structurally through SEQ-last-child and every CHOICE
alternative, the right-recursive operator ladders and rule aliases). Measured on
the supplied grammars:

| grammar         | tail CALLs | total CALLs | fraction |
|-----------------|-----------:|------------:|---------:|
| std.javascript  |         52 |         122 |    42.6% |
| std.typescript  |         97 |         193 |    50.3% |
| std.tsx         |         96 |         192 |    50.0% |
| std.json        |          2 |           4 |    50.0% |
| std.json5       |          2 |           4 |    50.0% |
| std.jsonc       |          2 |           4 |    50.0% |

Tail calls are NOT rare — they are ~43–50% of all CALLs. The lowerer
(`vmcompile.c`, `tp_mark_tail_calls`) marks them with a `tail` flag on the CALL
instruction; `tp_count_tail_calls` reports the counts, so the evidence is
reproducible, not a one-off.

### What the bytecode exploits — measured, not guessed

A CALL builds a distinct tree node, so its epilogue (link children, close span) is
irreducible: reusing the CALL frame itself would change the tree, which vm-diff
forbids. The depth-reducing reuse available is the **pure pass-through wrappers**
that have NO epilogue — FIELD and ALIAS, which only decorate their child's node
with a field/alias symbol and then forward its signal. The driver now REUSES the
wrapper frame in place for its child (rewrite `program`/`field`/`alias`, no second
frame) instead of pushing a child and keeping a do-nothing continuation. A
field/alias-decorated rule reference in tail position — the common shape in these
grammars — is exactly where this fires.

Measured with the `TP_VM_PROBE_DEPTH` hook (peak live frames + total frame
pushes), trees byte-identical throughout:

- **Right-recursive assignment ladder** (`a = a = … = 1`, 3000 levels, the
  tail-call-dense case): peak frame depth **18077 → 15058 (-17%)**, frame pushes
  **429298 → 327231 (-24%)**.
- **Deep nested arrays** (`[[[…1…]]]`, 2000 levels): peak depth unchanged (12004 —
  the CALL epilogue chain dominates and is irreducible there), frame pushes
  **26005 → 22005 (-15%)**.

So the tail-call evidence drove a real, sound optimization: on the right-recursive
ladders the analysis flagged, frame depth and pushes both drop meaningfully; on
nesting that bottoms out in irreducible CALL epilogues, peak depth is already
minimal and the reuse still trims the hot loop. The perf gate moved with it
(JS-corpus parse 5.5 ms, was ~9–10 ms range). The `tail` flag is recorded and
exploited; the honest limit (CALL epilogues cannot be elided without changing the
tree) is documented rather than papered over.

Standing gates green at the commit: build clean, metric 296/296, vm-diff GREEN,
cc-diff GREEN, no-statics GREEN, vm-nocrash GREEN.

## W15-1 — cut the per-parse zeroing (the 22.5% memset)

A callgrind profile of the bytecode VM parsing a real 6.6 KB TS file named
`memset` (via `ax_alloc`, which zeroes every block) the single largest cost. On
the JS corpus the profile put it at 22.5%; on a single representative React file
parsed many times in one engine (`bench/reclaim_test.c` over a 6 KB
`react-server` source, grammar `std.tsx`) it was **48.5%** — the per-parse memo
tables, sized to the input and zeroed every parse, dominate as the input grows.

The caller attribution (`callgrind_annotate --inclusive=yes`) was unambiguous:
the **token-match memo** (`tp_match_memo_init`, ~input×4 slots × 24 B ≈ 768 KB for
a 6 KB input) at **25.9%**, and once it was fixed the **rule memo**
(`tp_rule_memo_init`, ~input×2 slots × 48 B ≈ 768 KB) surfaced at **32.2%** —
the rule memo is *dormant* on a clean parse, so its zeroing was pure waste. Two
init loops (the extras memo, the rule memo) also re-zeroed what `ax_alloc` had
already cleared.

The fix splits occupancy from data. Each memo's slot table is allocated with a
new non-zeroing `ax_alloc_raw` (added to the ax core, `src/ax/`), and a parallel
one-byte-per-slot `live` column — a 24th (match) / 48th (rule) of the slot bytes —
is the only thing zeroed: `live[i] == 0` means slot `i` is empty, so the probe
reads occupancy from `live` and never touches the uninitialised slot. This is
provably correct because a slot's fields are read **only** after `live` reports it
occupied, and `live` is zeroed at allocation; valgrind memcheck (token path and
rule-memo-forced replay path) reports **0 uninitialised reads**. The redundant
init loops were deleted (`ax_alloc` already zeroes). The fix reduces the
O(input) zeroing of a SINGLE parse, so it moves the React benchmark even though
that harness opens a fresh engine per file.

Measured, trees byte-identical throughout (cc-diff GREEN, vm-diff GREEN,
metric 296/296):

- **`memset` share** (callgrind, 6 KB React file × 30 parses, one engine):
  **48.5% → 6.0%** (48.7M → 3.0M Ir, −94%); total instructions 100.6M → 50.3M.
  The interpreted lexer (`tp_match_ir`, 74%) is now the top cost — the W15-2
  target.
- **VM throughput** (React corpus, 9.5 MB, median of 5, fresh engine per file):
  **2.78 → 3.20 MB/s (+15%)**; median parse 3.27 s → 2.84 s; peak RSS
  194,884 → 143,060 KB (−27%, the unwritten pages are never committed).
- JS-corpus perf gate unchanged at 5.6 ms (its files are tiny, so the input-sized
  tables — and their zeroing — are small there; the win concentrates on real
  files).

The same `ax_alloc_raw` + `live`-column shape helps the C-compiled parser, which
shares the VM's memo helpers.

Standing gates green at the commit: build clean, metric 296/296, cc-diff GREEN,
vm-diff GREEN, no-statics GREEN, vm-nocrash + parser-nocrash GREEN, perf within
ceiling, and a valgrind memcheck of a parse reports no uninitialised-value errors.

## W15-2 — compile the lexer (the recursive token IR walk leaves the hot path)

After W15-1 cut the zeroing, the profile named the lexer the top cost: token
matching still bottomed out in `tp_match_ir`, a **recursive tree-walk** over the
token-pattern IR, called per byte from `tp_token_match_memo`. W15-2 compiles each
token pattern, once, to a flat **jump-threaded** program and matches it on the hot
path with an **iterative** register loop (`tp_token_run`): one program counter, one
cursor, and a small fixed save-stack for the backtrack points an ordered choice /
optional / repeat-exit needs. The recursive `tp_match_ir` stays only as the cold
reference (query.c) and a byte-identical fallback for the few `not`-bearing tokens
(comments, strings, the regex literal); it is off the parser hot path for every
common token (keyword, identifier, number, whitespace, punctuation).

### The token bytecode

The token matcher is **pure** — `(input, length, start) -> matched + end`, no tree
build, no extras, no goal — and token matching is **possessive PEG** (greedy with no
cross-element backtracking: `tp_match_ir`'s repeat never gives bytes back, its choice
commits to the first matching alternative, its seq never retries an earlier element).
That makes a jump-threaded encoding direct and frame-free on the linear spine:

- **leaf ops** `LITERAL / ANY / CLASS / CLASS_NAME` match one atom; on failure they
  backtrack to the nearest save point (restore its cursor, jump to its handler).
- **`SEQ`** is its children back to back — straight-line code, no per-element state.
- **`CHOICE`** is a `SAVE / <alt> / COMMIT` chain: each alternative but the last is
  guarded by a `SAVE` whose target is the next alternative; a matching alternative
  `COMMIT`s (drops its save) and jumps past the rest — first match wins, possessively.
- **`REPEAT` of a single leaf** — the dominant shape (an identifier's class run, a
  digit run, whitespace) — fuses into ONE `STAR` op that loops in tight C, exactly
  what a hand-written lexer does; the single hottest lexer loop.
- **`OPTIONAL`** and a general (compound-body) `REPEAT` are `SAVE / body / COMMIT`
  loops, with the zero-width guard living in the runner (a `COMMIT` that did not
  advance the cursor exits the loop, matching `tp_match_ir`'s `if (next==pos) break`).
- **`NOT_KEYWORD`** (the `reserved` identifier wrapper — hot, on every identifier)
  records the match start in a `MARK` register, then a check op rejects the slice if
  it equals a reserved word.

The lowering mirrors `tp_match_ir` node-for-node, so the end offset is **byte-
identical** — proven by cc-diff (267/267), vm-diff, and conformance (296/296). It is
a one-time, compile-time recursion over the AUTHORED pattern (bounded by the grammar,
like the rule lowering in vmcompile.c); the runtime matcher it produces is iterative.

The generated C parser gets this for free: it already embeds the packed grammar,
re-lowers it at startup, and calls the shared library runtime — so once token
matching is bytecode in the library, the compiled parser matches tokens with it too,
with no separate emitter and no risk of drift (cc-diff stays byte-identical).

### Before / after (callgrind, the W15-1 setup)

A callgrind profile of the bytecode VM parsing the 6.6 KB `react-server`
`ReactFizzComponentStack.js` file 30 times in one engine, grammar `std.tsx`
(`bench/prof/prof_react.c`):

- **Recursive token walk on the hot path collapses.** Before, `tp_match_ir`
  (`'2` recursion clone + self) was the single largest function at **~25% self**
  (71.9% inclusive); the whole lexer entry `tp_token_match_memo` was **32.5%
  inclusive**. After, `tp_token_run` (inlined into `tp_token_match_memo`) is **21.1%
  self**, and the residual `tp_match_ir` is **8.4%** — purely the cold fallback for
  the `not`-bearing string/comment/regex tokens. The recursive walk is gone from the
  common-token hot path.
- **Total instructions: 272.9M → 261.4M (−4%).**
- **Wall time** (same machine, 500 parses of the 6.6 KB file): the recursive matcher
  0.553 s, the threaded VM **0.548 s** — on par to slightly faster, where a naive
  per-node frame VM (the first attempt) regressed to 0.647 s. The jump-threaded
  register design, not a generic frame stack, is what keeps the iterative form from
  costing more than the recursion it replaces.

### Throughput (`ci/tinyparse-bench5.sh`, React corpus, std.tsx)

Hardware: Intel Xeon @ 2.80GHz (4 cores), Linux 6.18.5 x86_64. Corpus: 1070 React
files, 9.5 MB. Parse-only, median; in-process arms 30 iters, tsc/tsgo 15.

| parser (parse-only)        | median parse | throughput  | ratio |
|----------------------------|--------------|-------------|-------|
| tsgo (typescript-go)       | 0.280 s      | 32.53 MB/s  | 1.0x  |
| tsc (compiler API)         | 0.722 s      | 12.60 MB/s  | 2.6x  |
| tree-sitter (TSX, GLR)     | 1.367 s      | 6.66 MB/s   | 4.9x  |
| tinyparse (bytecode VM)  | 2.827 s      | 3.22 MB/s   | 10.1x |
| tinyparse (C-compiled)   | 2.895 s      | 3.14 MB/s   | 10.4x |

Honest standing: the **C-compiled parser improved to 3.14 MB/s (+11% over the 2.83
MB/s it measured before W15-2)** because it now matches tokens with the shared lexer
bytecode; the VM holds at 3.22 MB/s (no regression vs W15-1). tinyparse is still
~2x tree-sitter — **W2 alone does not beat it**; the next-largest interpreted slice
the profile now names is rule dispatch (`tp_vm_run`, ~24% self), which **W3** bakes.

### No-crash note

The runtime token matcher is now iterative: the fused `STAR` loop consumes an
arbitrarily long token (an identifier, a 400 KB number) in constant C-stack space, so
`ci/tinyparse-vm-nocrash.sh` adds a single-huge-token case that survives under
`ulimit -s 512`. The threat the lexer posed was never input-driven — `tp_match_ir`'s
recursion depth was the (fixed, authored) token-pattern nesting, never the input — so
this is an iterative-by-construction guarantee, not a fix for an observed input crash;
a deeply nested *grammar* still recurses at compile-time lowering, identically to the
existing rule lowering, and the parser hot path stays bounded.

Standing gates green at the commit: build clean, metric 296/296, cc-diff GREEN
(byte-identical), vm-diff GREEN, no-statics GREEN, vm-nocrash (incl. the new big-token
case) + parser-nocrash GREEN, perf within the ratcheted 6.5 ms ceiling.

## W15-3 — bake the rule dispatch (the generated parser stops interpreting)

After W15-1 (zeroing) and W15-2 (lexer), the profile of the *generated* C parser
named rule dispatch the top cost: the compiled parser still **called the shared
interpreter** `tp_vm_run` (the W14-6 arrangement — one loop, one source of truth),
so its rule dispatch was a generic loop that, per step, *fetched* an instruction
from `state->program->instructions[fr->program]` and *switched* on its opcode.
W15-3 removes that interpreter from the generated parser: the emitter
(`src/emitparser.c`) now bakes the rule control flow into specialized C.

### What is baked

`tinyparse c <grammar> --parser` emits `tp_parser_run` in place of the call into
`tp_vm_run`. It keeps the SAME explicit, arena-backed frame stack (`tp_vm_frame`/
`tp_vm_stack`) and the SAME leaf helpers (the token matcher, the tree builder,
extras, recovery, the rule memo, inject) — those stay in the library, one source of
truth — but the dispatch is `switch (fr->program)` over **one baked case per program
index**, each the specialization of the matching `tp_vm_run` arm with every
structural operand (`target`/`start`/`count`/`min`/`max`/`symbol`/`aux`/`tail`)
inlined as a compile-time constant. SEQ / CHOICE / REPEAT / OPTIONAL / FIELD /
ALIAS / GOAL / NO_EXTRAS / RECOVER / CALL / INJECT all become inlined control flow;
the only runtime reads left in a case are the `tp_ir` pattern/source pointers a
MATCH/BYTE/RECOVER/INJECT needs (runtime addresses, read from
`state->program->instructions[N]`, since they cannot be serialized into the C). The
CALL prologue/epilogue (`tp_vm_call_enter`/`exit`) and the rule-memo logic stay the
shared library functions, so the memo verdicts and the tree build are byte-for-byte
the VM's. **The generated parser no longer calls `tp_vm_run` for rule dispatch**
(`grep tp_vm_run` of the emitted file finds only comments).

It reads like a hand-written recursive descent but is **iterative and bounded**: no
combinator recurses in C — every descent pushes a frame on the arena stack — so a
deeply nested or malformed input grows the arena, never the host C stack
(`ci/tinyparse-parser-nocrash.sh`: a 200k-deep input under `ulimit -s 512` exits
cleanly). The W14-6 tail-call frame reuse carries over verbatim: the baked FIELD and
ALIAS cases rewrite the frame in place for their child instead of stacking a
do-nothing continuation. The output is **macro-free** — it is generated, so the
loop is expanded once at generation time, with zero `#define`/`#undef` in the
emitted C (an `inline` frame-push helper is emitted locally so the C compiler folds
it into the descent sites; the profile showed an out-of-line cross-TU call to the
library push was the one cost baking *added*).

### Tail-call analysis (owner directive), reproduced

`tinyparse analyze-tailcalls <grammar>` over the real grammars (the W14-6 count,
unchanged — the lowering is the same): std.javascript **52/122 (42.6%)**,
std.typescript **97/193 (50.3%)**, std.tsx **96/192 (50.0%)**, std.json **2/4
(50.0%)**. Tail calls are common, so the baked parser exploits them exactly as the
VM does — the right-recursive operator ladders and the pure FIELD/ALIAS pass-through
wrappers reuse the frame rather than stacking a forwarding continuation, keeping the
hot stack at the true nesting depth.

### Before / after (callgrind, the W15-1/2 setup)

The 6.6 KB `react-server` `ReactFizzComponentStack.js` file parsed 30 times in one
engine through the COMPILED parser (`bench/prof/prof_compiled.c`, grammar std.tsx):

- **The interpreter leaves the generated parser.** Before, the compiled parser's
  dispatch was `src/vm.c:tp_vm_run` at **62.4M Ir (23.9% self)**. After, it is the
  generated `tp_parser_run` at **61.3M Ir (23.5%)** — the per-step instruction
  fetch + opcode switch is gone; what remains is the frame push/pop and the leaf
  helper calls, which baking does not remove.
- **Total instructions: 261.4M → 261.4M (neutral).** Baking the dispatch trades the
  interpreter's fetch-and-switch for an equal-cost direct jump table; the dispatch
  was never the majority of the work. The honest read: removing the interpreter is
  the deliverable and a structural cleanup (the generated parser is now standalone
  C, not a driver over a shared loop), but it is **not, by itself, a throughput
  win** — the profile already said dispatch was ~20%, and the larger slices
  (`tp_token_match_memo` 21%, `memset` ~10%, `tp_vm_consume_extras` 8%) are
  elsewhere.

### Throughput (`ci/tinyparse-bench5.sh`, React corpus, std.tsx)

Hardware: Intel(R) Xeon(R) @ 2.80GHz (4 cores), 15.7 GiB, Linux 6.18.5 x86_64.
Corpus: 1070 React files, 9.55 MB. Parse-only, median; in-process arms 25 iters,
tsc/tsgo 12.

| parser (parse-only)        | median parse | throughput  | ratio |
|----------------------------|--------------|-------------|-------|
| tsgo (typescript-go)       | 0.322 s      | 28.28 MB/s  | 1.0x  |
| tsc (compiler API)         | 0.751 s      | 12.12 MB/s  | 2.3x  |
| tree-sitter (TSX, GLR)     | 1.340 s      | 6.79 MB/s   | 4.2x  |
| tinyparse (bytecode VM)  | 2.740 s      | 3.32 MB/s   | 8.5x  |
| tinyparse (C-compiled)   | 2.815 s      | 3.23 MB/s   | 8.8x  |

The baked parser is **byte-identical to the VM** (cc-diff 267/267) and runs at
**3.23 MB/s, within noise of the VM's 3.32 MB/s** (0.97x) — as the instruction
count predicts. **It does not beat tree-sitter (6.79 MB/s); the gap is ~2.1x.**

### This is the floor for the interpret-a-runtime-compiled-PEG approach (honest)

W15-3 baked the rule dispatch, the last interpreted slice the phase plan named, and
the meter says the generated parser is now within noise of the VM and still ~2x
tree-sitter. Per the integrity-first stance, the FASTER goal is recorded as **NOT
met**, with the evidence for why it is a structural floor, not a missing
optimization:

- **No single remaining cost is half the work.** After baking, the profile spreads
  across `tp_token_match_memo` (~21%), the baked dispatch (~20%, irreducible frame
  push/pop + the leaf-helper calls every node makes), `memset`/allocation (~10%,
  the per-parser builder + tree + frame-stack growth copies), `tp_vm_consume_extras`
  (~8%), `tp_choice_first_admits` (~6%), and the CALL prologue/epilogue (~8%). To
  reach tree-sitter (2.1x) would mean roughly *halving the total*, which no one
  item affords. W1/W2/W3 each removed their named slice; the remainder is
  fundamental to the design.
- **The gap is the design difference this exploration already established** (see
  "Finding 2" above and `docs/decisions.md`): tinyparse runs a PEG it
  **compiled at runtime** — every node visits a generic frame, matches through a
  per-position memo, and appends to a contiguous tree builder — while tree-sitter
  runs a **pre-generated GLR parse table + DFA lexer** with no runtime frame
  machine and no per-position memo. Baking the *rule structure* into C removes the
  opcode interpreter but not the per-node frame/memo/tree work, which is where the
  time now lives. Matching tree-sitter would mean generating parse tables — its
  design, not tinyparse's — the same conclusion W5 reached and parked GLR over.
- **What W3 *did* buy, beyond the cleanup:** the generated parser is now genuinely
  self-contained C control flow (the embedding deliverable — a reader sees the
  grammar's rules as baked C, not a call into a black-box loop), it stays
  byte-identical and crash-bounded, and the tail-call frame reuse is inlined per
  rule. It is faster than the *original* (pre-W15) compiled parser, just not faster
  than tree-sitter.

Standing gates green at the commit: build clean, metric 296/296, cc-diff GREEN
(byte-identical), vm-diff GREEN, no-statics GREEN, vm-nocrash + parser-nocrash GREEN
(the baked parser survives 200k-deep input under `ulimit -s 512`), perf within the
6.5 ms ceiling. The emitted C is macro-free and calls no `tp_vm_run`.

## W16-1 — the design-independent wins, both engines (lexer/extras/prediction/allocation)

Phase 16 ports the fast-parser recipe (tsc/tsgo/meriyah). W16-1 lands the
*design-independent* slices first — the ones that help BOTH the bytecode VM and the
generated C parser, since they share the `token.c`/`vm.c` runtime — and adds
**meriyah** to the benchmark, the owner's bar ("if we can't get close to meriyah
we're not in a good spot"). The per-node frame machine (the largest single slice)
is W16-2's native-recursion rewrite, NOT this workstream.

### Profile, before → after (callgrind, one comment/string-heavy React file ×3)

Representative file `react-client/.../ReactFlight-test.js`, std.tsx grammar, the
W15 profiling setup (`bench/prof/prof_react`). Total instructions retired:

| stage | I refs | vs W15-3 baseline |
|-------|--------|-------------------|
| W15-3 baseline | 725.2 M | — |
| + extras lead-byte fast path | 634.8 M | −12.5% |
| + raw (non-zeroing) hot allocations | 549.9 M | −24.2% |
| + inlined choice predictor | **516.5 M** | **−28.8%** |

The named slices fell exactly as the meter predicted:

| slice (self %) | before | after |
|----------------|--------|-------|
| `tp_vm_consume_extras` | 11.45% (83.0 M) | 8.58% (47.2 M) |
| `tp_token_match_memo` | 22.68% (164.5 M) | 21.29% (110.0 M)* |
| `__memset_*` (zeroing) | 10.78% (78.2 M) | dropped out of the top list |
| `tp_choice_first_admits` | 5.80% (42.1 M) | folded inline (call elided) |

(* `tp_token_match_memo`'s self % barely moved, but its *total* fell 164→110 M:
much of it was the per-extra token matching the new lead-byte fast path skips.)

### The three wins (each measured, byte-identical, both engines)

1. **Extras lead-byte fast path.** `tp_vm_consume_extras` runs before every
   terminal, and at the overwhelming majority of positions the cursor byte begins
   no extra at all (it is an identifier/operator/string byte, not whitespace or a
   comment opener) — yet the scanner still looped over every extras entry and ran a
   full token match for each. The grammar now caches a 32-byte (`[256]`-bit) UNION
   of every extra token's first-byte set (`tp_extras_lead_build`, computed once per
   grammar like `vm_program`/`token_index`); a cursor byte outside it provably
   begins no extra, so the whole loop is one bitset probe. Sound by construction: an
   extra whose lead byte cannot be pinned down sets `extras_lead_open`, which
   disables the fast path. The single biggest slice: −12.5% of all instructions.

2. **Raw (non-zeroing) hot allocations.** The W15-1 lesson — prefer raw allocation
   of a fully-overwritten buffer over a zeroing `ax_alloc` whose result is
   discarded — applied to the remaining per-rule hot allocations the profile still
   showed: the per-CALL children-sink struct (`tp_vm_call_enter`, ~386 k/parse, all
   three fields overwritten next line), and the geometric-growth buffers whose live
   prefix is copied in and whose tail is written before any read (the child sink
   `items`, the VM frame stack, the tree-builder node array). Each became
   `ax_alloc_raw`. The 78 M-instruction `memset` slice (10.8%) left the top list.
   Verified with **valgrind memcheck clean** on both the default and the
   `TINYPARSE_RULEMEMO=always` replay path (no uninitialised read), the W15-1 bar.

3. **Inlined choice predictor.** `tp_choice_first_admits` already cached its FIRST
   set on the node (computed once, not per visit — the brief's "re-derive per visit"
   was already addressed in W46). The residual cost was the cross-translation-unit
   *call*: its own prologue/epilogue outweighed the cached bitset test it guarded.
   A `static inline tp_choice_first_admits_fast` in `internal.h` does the cached
   case in place (same open/past-end/bit-test verdict) and falls back to the full
   mutating function only on the cold first visit. Inline in the header so the VM
   and the generated parser both fold it in. −6% net, byte-identical.

### Sub-wins considered and NOT taken (integrity-first)

- **A table-driven `[256]` lexer is already present.** The brief's sub-win 2
  ("replace per-byte pattern stepping with a `[256]` class table + ASCII fast-path
  loop") describes the state BEFORE W15-2. As of W15-2 the dominant tokens
  (identifier, number, whitespace) already run as a fused `STAR` op looping in tight
  C over `tp_class_has`, which is a 256-bit class-table probe — exactly the
  table-driven fast path. The only residual `tp_match_ir` (recursive) lexer cost is
  the `not`-guard tokens (comments, strings, the regex literal), which W15-2
  *deliberately* declined to lower for byte-identity (an inverted-lookahead
  sub-evaluation risks changing a tree). That risk is unchanged, and the cost is
  file-dependent (comment density), so re-deriving a fused `not`-scan was NOT taken
  here — it is not a design-independent, low-risk win. Recorded as a known residual.

### The benchmark, with meriyah (the owner's bar) now visible

Six parsers, parse-only, ReactJS (std.tsx corpus), per-file-fresh engine, only the
parse timed. Hardware: Intel Xeon @ 2.80 GHz (4 cores), 15.7 GiB, Linux x86_64.
Corpus: 1070 files, 9.55 MB clean-parsing under std.tsx. meriyah parses only the
JSX subset it accepts (541/1070; it rejects the 529 TS/Flow files), so its MB/s is
over that subset — an honest figure, not byte-for-byte comparable with the
full-corpus arms.

| parser (parse-only) | median | throughput | vs fastest |
|---------------------|--------|------------|-----------|
| tsc (compiler API) | 0.778 s | 11.71 MB/s | 2.6× |
| tsgo (typescript-go) | 0.298 s | 30.60 MB/s | 1.0× |
| **meriyah (JS+JSX subset)** | 0.226 s | **25.52 MB/s** | 1.2× |
| tree-sitter (TSX, GLR) | 1.388 s | 6.56 MB/s | 4.7× |
| tinyparse (bytecode VM) | 2.568 s | **3.55 MB/s** | 8.6× |
| tinyparse (C-compiled) | 2.803 s | 3.25 MB/s | 9.4× |

The VM rose **3.32 → 3.55 MB/s** (+7%) from W15-3; the C-compiled arm held (3.23 →
3.25) because its bottleneck is the per-node frame machine W16-2 will remove, not
the slices W16-1 trimmed. **W16-1 alone does not beat tree-sitter** — expected, and
recorded honestly; the point is the named slices shrank on both engines and meriyah
is now the visible target (~7× the VM, the gap W16-2+ must close).

Standing gates green at the commit: build clean, metric 296/296, cc-diff GREEN
(byte-identical — the faster lexer/extras/allocations change no tree), vm-diff
GREEN, no-statics GREEN (the new `[256]` set is `static`-free, cached on the
arena-owned grammar), vm-nocrash + parser-nocrash GREEN under `ulimit -s 512`, perf
ratcheted 6.5 → 5.5 ms (JS corpus ~4.8 ms).

## W16-2 — the generated parser becomes a depth-guarded native recursive-descent parser

This is the phase's lever. Through W15-3 the generated parser drove an **explicit
per-node frame stack** — a `switch (fr->program)` loop that pushed one arena frame
per combinator (SEQ stepping its children, CHOICE trying alternatives, REPEAT
iterating, a rule body running). W15-3 baked the *dispatch* (one case per program
index instead of an opcode interpreter) but kept the *frame machine*, and the
profile said that machine was the ~2.1× wall: the per-node push/pop + the
continuation-state bookkeeping cost about a fifth of all instructions, work the
fast hand-written parsers (tsc/tsgo/meriyah) never pay because they recurse on the
native C call stack with a few C locals per rule.

W16-2 does what those parsers do. The emitter (`src/emitparser.c`) now generates
**true predictive recursive descent**.

### Design

- **One C function per program index.** Each lowered instruction `N` becomes a
  `static int tp_parser_p<N>(tp_vm_state *state, tp_vm_sink *sink, uint32_t field,
  uint32_t alias)` whose body is the straight-line C its `tp_vm_run` arm would
  execute, with every structural operand (target/start/count/min/max/symbol/aux)
  inlined as a literal. There is no frame struct and no dispatch loop in the
  emitted parser.
- **Intra-rule structure is ordinary control flow.** SEQ is a sequence of
  statements; ordered CHOICE is mark / try / rewind (with the W16-1 inlined
  first-set predictor `tp_choice_first_admits_fast` gating each alternative);
  REPEAT is a `while`; OPTIONAL is a try-and-restore; FIELD/ALIAS are tail calls
  that forward the decoration; GOAL/NO_EXTRAS/RECOVER are save / set / call /
  restore; INJECT forwards to the shared splice through a region dispatcher.
- **Rule→rule is a native C call.** A CALL calls `tp_parser_p<target>` directly —
  no frame push. The CALL prologue/epilogue stay the shared library functions
  `tp_vm_call_enter` / `tp_vm_call_exit` (over a C-stack-local `tp_vm_frame`), so
  the rule-node open/close and the rule-memo verdicts have **one source of truth**
  shared with the VM. That is what keeps the generated tree byte-identical.
- **Cheap mark/rewind.** A CHOICE/OPTIONAL/REPEAT backtrack saves three scalars in
  C locals — the cursor (`state->position`), the builder length
  (`builder.count`), and the sink count — and on a failed alternative restores
  them and truncates the builder (rolls `count` back; no arena rollback). This is
  exactly the saved_pos / saved_count / saved_sink the VM's arms keep.
- **The VM is unchanged.** `tp_vm_run` keeps its explicit-stack iterative engine;
  it stays the runtime-grammar engine and the cc-diff byte-identity reference. Only
  the *generated* parser went native.

The output is macro-free (it is generated, so the per-rule control flow is expanded
once at generation time) and holds no static mutable state (no-statics GREEN).

### Crash-safety: a measured depth guard

Native C recursion can overflow the host C stack on adversarial deep input, which
is not tolerable. The generated parser carries a recursion-depth counter in
`tp_vm_state` (`depth` / `depth_limit`): each rule-body entry — the CALL site, the
one true recursion point — bumps `depth`, and when it would exceed `depth_limit`
the rule returns `TP_VM_FAIL` instead of recursing. A pathological input fails
**gracefully** (a clean syntax error, exit < 128) rather than crashing. The VM
leaves `depth_limit` at `TP_NO_INDEX` (no guard — it never recurses).

The limit is **measured, not guessed** (per the profiling skill: a no-crash claim
is a justified bound, never a magic number found by a depth sweep). On this
hardware (Intel Xeon @ 2.80 GHz, std.json / std.tsx generated parsers):

| measurement | std.json | std.tsx (worst-framed shipped grammar) |
|-------------|----------|----------------------------------------|
| C-stack per CALL-depth increment | ~200 bytes | ~273 bytes |
| CALL-depth per input nesting level | 2 | ~22 |
| C-stack overflow point under `ulimit -s 512` | ~2560 CALL-depth | **~1920 CALL-depth** (~273 B × 1920 ≈ 512 KiB) |
| deepest CALL-depth any **real** corpus file reaches | (tiny fixtures) | **313** (a React test file) |

So the deepest real input reaches CALL-depth **313** (std.tsx, the full React
corpus; the conformance fixtures are far shallower), and the C stack overflows
around CALL-depth **~1920** under the 512 KiB no-crash budget. The chosen limit is
**1000**:

- **~3× above** the deepest real input (313) — so no valid file ever false-bails,
  and cc-diff / conformance stay byte-identical (verified: cc-diff 267/267, metric
  296/296);
- **~2× below** the 512 KiB overflow point (1000 × 273 B ≈ 273 KiB ≪ 512 KiB) — so
  a pathological deep input bails gracefully well before the C stack runs out.

`ci/tinyparse-parser-nocrash.sh` proves it: a ~200k-deep `[` input under
`ulimit -s 512` now exits **1** (graceful syntax error, no signal) where the same
generated parser without the guard segfaulted (139). The guard is the crash-
prevention mechanism; the explicit-stack arena bound that protected the W15-3
parser is no longer present (the parser is native), so the depth guard is now
load-bearing.

### Before / after (callgrind, instruction counts, one comment/string-heavy React file)

The frame machine vanishes. Total instructions over the file drop **−21%**
(23.87 M → 18.77 M for 3 passes); the steady-state self-cost table (30 passes,
150.6 M Ir) shows the per-node dispatch loop is *deleted*, not reduced:

| function | W15-3 (frame machine) | W16-2 (native RD) |
|----------|----------------------|-------------------|
| `tp_parser_run` (per-node frame dispatch loop) | **25.8% + 3.0% inlined ≈ 28%** | **gone** |
| `tp_token_match_memo` | 14.3% | 22.6% |
| `tp_match_ir` (not-guard lexer fallback) | 9.4% | 15.0% |
| `tp_vm_consume_extras` | 6.9% | 11.0% |
| `tp_vm_call_exit` / `tp_vm_call_enter` | 5.4% + 4.3% | 8.5% + 6.8% |
| `tp_tree_builder_push` | 4.2% | 6.6% |
| each generated `tp_parser_p<N>` | (n/a — one loop) | **< 1% each** |

The slices that *rose* in percentage did not get slower — they are the same genuine
parse work now occupying a smaller total. The dispatch overhead the whole phase
chased is essentially free: the generated per-rule functions are each under 1%.

### Throughput (`ci/tinyparse-bench5.sh`, React corpus, std.tsx)

Six parsers, parse-only, per-file-fresh engine, only the parse timed. Hardware:
Intel Xeon @ 2.80 GHz (4 cores), 15.7 GiB, Linux x86_64. Corpus: 1070 files,
9.55 MB. meriyah parses the 541/1070 JSX subset it accepts (MB/s over that subset).

| parser (parse-only) | median | throughput | vs fastest |
|---------------------|--------|------------|-----------|
| tsc (compiler API) | 0.765 s | 11.90 MB/s | 2.4× |
| tsgo (typescript-go) | 0.313 s | 29.07 MB/s | 1.0× |
| meriyah (JS+JSX subset) | 0.232 s | 24.92 MB/s | 1.2× |
| tree-sitter (TSX, GLR) | 1.418 s | 6.42 MB/s | 4.5× |
| tinyparse (bytecode VM) | 2.506 s | 3.63 MB/s | 8.0× |
| **tinyparse (C-compiled, native RD)** | 2.033 s | **4.48 MB/s** | 6.5× |

The native rewrite lifts the C-compiled arm **3.25 → 4.48 MB/s (+38%)** and, for
the first time, it **beats the VM** (1.23×: 4.48 vs 3.63 MB/s) — precisely because
the frame machine the VM still runs is gone from the generated parser.

### Does it beat tree-sitter? Not yet — the honest floor, and the next cost

No: 4.48 MB/s against tree-sitter's 6.42, ~1.43× slower. So `W16-FASTER` is **not**
recorded in `docs/performance.md` — the win was not earned, and per the project's
integrity-first stance the floor is recorded rather than the marker faked. W16-2 is
the largest single step of the phase (the structural overhead the Phase 15 floor
blamed is genuinely removed), but the residual gap is the design difference the
exploration established and is now concentrated in **token matching** (~37%: the
`tp_token_match_memo` core plus the recursive `tp_match_ir` fallback W15-2 left in
place for the `not`-guard comment/string/regex tokens, for byte-identity) and the
shared CALL prologue/epilogue (~15%, the rule-node build + memo bookkeeping). Those
are exactly the costs tree-sitter's pre-generated DFA lexer and table-driven core
avoid. The meter now names the **lexer** as the next lever — not the parser control
flow, which W16-2 has made free — and lowering the `not`-guard tokens to the token
bytecode (a separately-verified byte-identity change W15-2 deferred) is the next
attack. Reaching tree-sitter from there, or recording a final evidenced floor, is
the remaining phase work.

### Standing gates green at the commit

Build clean (0 warnings under the engine's `-Werror`), metric 296/296, cc-diff
**byte-identical 267/267** (the native parser builds the exact same tree as the VM
on every corpus file), vm-diff GREEN, no-statics GREEN, vm-nocrash + parser-nocrash
GREEN under `ulimit -s 512` (the parser-nocrash case is now the depth-guard proof),
perf gate within ceiling (VM JS corpus 4.9 ms).

## W16-3 — two generated parsers in one binary (a per-parser symbol prefix)

The phase's first packaging deliverable: the generator must emit **independent,
non-colliding parsers that coexist in one program**. The motivating case is
embedding — a host markup grammar recognizes fenced code blocks and the code
INSIDE each fence is parsed by a SECOND, separately compiled grammar. That was
impossible before this workstream because every emitted parser used FIXED
file-scope symbol names (`tp_parser_p<N>`, `tp_compiled_parser_load`,
`tp_compiled_parser_run`, the emitted statics). Link two generated parsers into
one binary and those symbols collide.

### The capability

`tinyparse c <grammar> --parser[-lib] --name <prefix>` renames **every** emitted
file-scope symbol to `<prefix>_…`. With no `--name` the parser keeps its
historical default names, so existing callers — and `ci/tinyparse-cc-diff.sh`,
which compiles the default-name path — are byte-for-byte unaffected (verified:
cc-diff 267/267 byte-identical, the generated default output is unchanged). Two
parsers compiled with distinct prefixes carry disjoint symbol sets and link into
one program with no collision and no duplicate-definition error.

The shared runtime is not duplicated: the leaf helpers, the CALL prologue/epilogue
(`tp_vm_call_enter`/`exit`), the tree builder, the lowering, and ax all live in
`libtinyparse` + `libax`, ONE archive set linked once and shared between the two
generated translation units. Only each parser's own per-program functions, its
embedded grammar bytes, its driver, and its two callable entries are per-TU, and
those are exactly what the prefix disjoints.

### Design

- The CLI's `c` subcommand parses `--name P` and sets it on the engine through a
  library-private setter (`tp_engine_set_parser_prefix`, declared in `internal.h`,
  defined in `engine.c`) — the same seam `TINYPARSE_ENGINE=vm` uses, so the
  public header stays frozen. The prefix points at host-owned `argv` storage that
  outlives the synchronous emit; nothing is copied, and an empty string is treated
  as no prefix.
- `src/emitparser.c` threads the prefix through its emit helpers. A single helper,
  `tp_parser_name(cursor, prefix, default_name, suffix)`, emits `default_name`
  verbatim when `prefix` is NULL and `<prefix>_<suffix>` otherwise. That one helper
  guarantees the default path is byte-identical and maps the historical names —
  which are NOT a uniform stem (`tp_parser_p<N>`, `tp_execute_parser`,
  `tp_compiled_parser_load`) — onto one tidy prefixed scheme.
- The actual collision was only ever the two EXPORTED entries (the per-program
  functions and the grammar bytes are `static`, internal linkage, never colliding
  across TUs), but the prefix renames all of them so a reader sees one consistent
  scheme and a future non-static symbol is covered for free.

### The example and the gate

`examples/embed/` is the worked example. A host grammar (`host.scm`) delimits
```` ``` ````-fenced blocks and exposes each fence's body as a `code` field; the
driver (`embed.c`) parses a document with the host parser, reads each `code`
span's byte range straight off the host tree (never re-scanning the source), and
runs a second compiled parser — `std.json`, emitted with its own prefix — on those
bytes. `build.sh` generates the two parsers with `--name host_parser` and
`--name json_parser`, so their entries become `host_parser_load`/`_run` and
`json_parser_load`/`_run`, and links both with the driver into one binary. This is
deliberately NOT the W62 `inject` demo (`std.fence`), which parses both languages
inside one grammar; here the embedding is two separate parsers driven by the host
program.

`ci/tinyparse-multiparser.sh` is the gate: it builds the example (a symbol
collision would fail the link), runs it on a committed sample, asserts the output
equals the golden `expected.txt` (the host tree AND both embedded json trees), and
checks with `nm` that the binary carries both parsers' four distinct entries and no
leftover default-name collision symbol.

### Standing gates green at the commit

Build clean (0 warnings under `-Werror`), metric 296/296, cc-diff **byte-identical
267/267** (the `--name` option does not change the default-name output), vm-diff
GREEN, no-statics GREEN, vm-nocrash + parser-nocrash GREEN under `ulimit -s 512`,
perf gate within ceiling, and the new `ci/tinyparse-multiparser.sh` GREEN (two
parsers, two trees, one binary).

## W16-4 — hide ax inside the generated parser's embeddable artifact

The phase's second packaging deliverable. The generated C parser uses the ax actor
engine internally — ax is the freestanding runtime the compiled parser links
against — but a host that drops the parser into its own application must not see ax
leak out. The embeddable object or shared library should export ONLY the parser's
own public entries (its `<prefix>_load` / `<prefix>_run`) plus the public
`tinyparse_*` API the host calls. No `ax_*` symbol, and no tinyparse-internal
`tp_*` symbol, belongs in the exported table.

### The leak, measured

Before this workstream the `examples/embed` binary — the driver plus two generated
parsers, linked against `libtinyparse.a` and `libax.a` as separate archives —
exported the whole engine:

```
$ nm examples/embed/embed | grep -cE ' [TDB] ax_'
56
$ nm examples/embed/embed | grep ' T ax_' | head -3
0000000000013b10 T ax_alloc
00000000000139d0 T ax_alloc_raw
0000000000013ca0 T ax_arena_open
```

Fifty-six `ax_*` symbols, every one external and defined. An embedder linking that
object inherits all of them — name clashes with their own allocator, a confusing
symbol table, an implementation detail turned into part of the surface.

### The fix: amalgamation (ax already hidden) + a linker version script

The mechanism is "hidden in the generated code" in the sense the owner asked for —
a self-contained amalgamation the embedder compiles — finished with a link-time
filter. Two pieces were already in place:

- `dist/amalgamation/tinyparse.c` folds the ax engine in under
  `#pragma GCC visibility push(hidden)`, so `ax_*` never carries default visibility
  in the first place; and
- the generated parser-lib's own per-rule functions are `static` (the no-statics
  gate proves it), so only its two `<prefix>_load` / `<prefix>_run` entries are
  external.

What remained was the amalgamation's tinyparse section, which sits under
`#pragma GCC visibility push(default)` — public `tinyparse_*` AND internal `tp_*`
both default. That is correct for the standalone shared library, which must export
its public API; the embedding build simply wants a tighter surface. So the
embeddable artifact links behind a **linker version script** that exports exactly
`{ <prefix>_load, <prefix>_run, …, tinyparse_* }` and localizes everything else —
which makes both `ax_*` and the internal `tp_*` local.

Two roads were rejected:

- **`-fvisibility=hidden` alone does not work.** The amalgamation's
  `push(default)` pragma overrides the command-line flag, so the internal `tp_*`
  stay exported under it (measured: 62 `tp_*` survived). The version script is
  applied at link, after the pragmas, and localizes them cleanly.
- **A pure single-translation-unit `.o` with everything `static` was not chosen.**
  Distinguishing public `tinyparse_*` from internal `tp_*` inside one file needs
  an in-source seam, and the FROZEN public header carries no visibility annotation;
  adding one would change the contract and risk the byte-identity gates. Keying on
  the symbol names at link — the same approach amalgamated libraries like SQLite
  ship — needs no header change and no emitter change.

Because the fix is a link-time recipe, **the emitter is untouched**: the generated
C is bit-for-bit what W16-3 emitted, so `ci/tinyparse-cc-diff.sh` stays
byte-identical and every existing caller is unaffected.

### The recipe and the result

`examples/embed/hide-ax.sh OUT.so PREFIX:GRAMMAR …` is the documented build recipe:
it compiles the amalgamation once, generates and compiles each parser-lib TU with
its `--name PREFIX`, generates the version script from the prefixes (GNU `ld`
`--version-script`, or the macOS `-exported_symbols_list`), and links the ax-hidden
shared object. On the two-parser embed example its exported table is exactly:

```
$ nm -D --defined-only libembed.so | grep -cE ' ax_'
0
$ nm -D --defined-only libembed.so | grep -cE ' tp_'
0
$ nm -D --defined-only libembed.so | grep -E ' (host_parser|json_parser)_'
... T host_parser_load
... T host_parser_run
... T json_parser_load
... T json_parser_run
```

Zero `ax_*`, zero tinyparse-internal `tp_*`, both parsers' four entries plus the
public `tinyparse_*` API — and nothing else. The 56-symbol ax leak is gone.

### The gate

`ci/tinyparse-hide-ax.sh` builds the embeddable artifact two ways and asserts,
with `nm`: a SINGLE generated parser (std.json, prefix `jp`) exports `jp_load` /
`jp_run` and the public API and zero `ax_*` / zero `tp_*`; the TWO-parser embed
example exports both parsers' four entries, the public API, and again zero `ax_*`;
and a hardened EXECUTABLE of the two-parser case (behind the same version script,
zero exported `ax_*`) still parses the committed sample to the golden tree byte for
byte — proving that hiding ax does not break the parse.

### Standing gates green at the commit

Build clean (0 warnings under `-Werror`), metric 296/296, cc-diff **byte-identical**
(the emitter is unchanged), vm-diff GREEN, no-statics GREEN, vm-nocrash +
parser-nocrash GREEN under `ulimit -s 512`, perf gate within ceiling, the W16-3
`ci/tinyparse-multiparser.sh` still GREEN (no regression), and the new
`ci/tinyparse-hide-ax.sh` GREEN (no exported `ax_`, parser API present, parse
correct).

## W16-GOAL — the last IR interpreter leaves the parse; the token memo retired from the clean path

W16-2 made the generated parser's control flow essentially free and named the
residual cost: token matching ~37% (including the deferred `not`-guard fallback to
the recursive `tp_match_ir`), the CALL prologue/epilogue ~15%, `consume_extras` ~11%,
the tree builder + its allocation ~12%. W16-GOAL goes after those, profile-ranked,
**measuring every change** — instruction count AND wall-clock A/B on a real React
file — and discarding the measured non-wins.

### The not-guard tokens are lowered — `tp_match_ir` is off the parse hot path

Comments (`line_comment`, `block_comment`), `hash_bang`, the HTML-comment trivia,
and the `regex` literal all scan with a `(repeat (not GUARD any))` shape that W15-2
left on the recursive `tp_match_ir` fallback to keep byte-identity. `TP_OP_NOT` now
lowers to a flat negative-lookahead guard region in the token bytecode:

- `TP_TOK_NOT_BEGIN` pushes a *negative* save whose target is the continuation
  reached when the guard FAILS (so the `(not GUARD …)` succeeds); a leaf failure
  inside the guard pops it and resumes there, exactly like an ordinary save.
- `TP_TOK_NOT_END` is reached only when the guard MATCHED, so the construct must
  fail: it drops the negative save and backtracks to the enclosing save.

This is the textbook PEG negative-lookahead compilation, and it mirrors
`tp_match_ir`'s `TP_OP_NOT` node-for-node, so the end offset is byte-identical
(cc-diff stays GREEN across all 267 differential cases, which exercise comments,
regex, and strings). The profile confirms the goal: `tp_match_ir`, the project's last
IR interpreter, went from ~3% of the parse to **zero instructions on the hot path** —
it is no longer called during a parse. It remains only as the cold reference (query.c)
and the save-stack-overflow / unlowerable fallback, so it is reduced to cold rather
than deleted (a safety net for no measured gain).

A dedicated fused "scan to delimiter" op (`TP_TOK_SCAN_UNTIL`) was tried and
**discarded**: it measured ~4% MORE instructions and no wall-time win (comments are a
small fraction of real TS files, and the per-scan lead-byte warm-up cost more than
the plain NOT loop). The meter said it was not a win, so it is gone.

### The per-position token memo is dropped from the clean parse path

The single biggest win, and a cache-behaviour one the instruction profile actively
*mis*-predicts. On a left-factored, first-set-predicted parse the token matcher
rarely re-runs at the same offset, yet the memo paid a hash + random-access slot
probe and store on every match attempt. Measured, head-to-head on a real file
(`ReactDOMFloat-test.js`, 80 parses):

| token memo on the parse path | wall-clock (80 parses) |
| ---------------------------- | ---------------------- |
| consulted (terminal + extras + byte ops) | ~2.58 s |
| dropped (every caller passes 0) | ~1.40 s |

That is **~38% faster** despite a *higher* instruction count — the memo's churn
thrashed the cache faster than re-running the jump-threaded token bytecode
(`tp_token_run`). The verdict is identical (a memo is a pure cache), so cc-diff stays
byte-for-byte; the per-position memo table is no longer even allocated. The token
memo measured a **43% hit rate** on this corpus, so it *was* doing work — it just cost
more than it saved on the fast path. Pathological backtracking is still bounded: the
RULE memo (`tp_rule_memo`) wakes on a super-linear parse and short-circuits
re-descent, so a terminal is not re-matched unboundedly. Verified fast on adversarial
deep / long / wide inputs and on the no-crash gates.

The rule memo, meanwhile, is confirmed **dead on the clean path and already skipped**
(gated behind the super-linear-attempt threshold) — it is left intact as the
backtracking backstop, not removed.

### Two more measured wins

- **Identifier choice-leaf fusion (~3–4%).** `(repeat (choice LEAF alt…))` — the
  `identifier` / `identifier_name` body, the most common token shape — now fast-loops
  the leading character class as a tight `STAR` and only falls to the rare `\u`
  alternatives when the class stops matching. Byte-identical (same ordered,
  possessive semantics).
- **Per-rule child sink uses inline frame storage on the generated path.** The
  generated native parser's `tp_vm_frame` is a stable C local, so `tp_vm_call_enter`
  uses the frame's inline `children_storage` and skips a per-rule heap allocation
  (`state->children_inline`). The VM keeps the separate allocation — its frames live
  on a relocatable arena stack where a pointer into a frame would dangle.

### Before / after profile (callgrind, ReactDOMFloat-test.js, the C-compiled parser)

| function | W16-2 (before) | W16-GOAL (after) |
| -------- | -------------- | ---------------- |
| `tp_token_run` (jump-threaded token bytecode) | 14.1% | 24.6% |
| `tp_vm_consume_extras` | 14.0% | 13.4% |
| `tp_token_match_memo` (now memo-free wrapper) | 12.6% | 9.5% |
| `tp_vm_call_enter` | 8.0% | 7.5% |
| `tp_tree_builder_push` | 6.0% | 5.7% |
| `tp_vm_call_exit` | 5.8% | 5.4% |
| `ax_alloc_raw` | 5.4% | 5.1% |
| **`tp_match_ir` (the recursive IR interpreter)** | **3.0%** | **0% (off the hot path)** |

The instruction count barely moved (1.665 B → 1.653 B) — the wins are in the *shape*
of the work (the memo's cache thrash gone, the not-guard recursion gone), which
wall-clock shows and callgrind cannot. `tp_token_run` rising to ~25% is the memo no
longer short-circuiting it; in wall time that trade is strongly positive.

### The 6-arm benchmark (measured post-commit, the full corpus)

The micro-benchmark wall-clock win translates to the full corpus
(`BENCH=1 ci/tinyparse-bench5.sh`, 1070 React files, 9.55 MB, parse-only, median;
Intel Xeon @ 2.80 GHz):

| parser (parse-only)        | median  | throughput | ratio (×slower than fastest) |
| -------------------------- | ------- | ---------- | ---------------------------- |
| meriyah (JS+JSX subset)    | 0.208 s | 27.72 MB/s | 1.0× |
| tsgo (typescript-go)       | 0.354 s | 25.71 MB/s | 1.1× |
| tsc (compiler API)         | 0.750 s | 12.14 MB/s | 2.3× |
| tree-sitter (TSX, GLR)     | 1.469 s | **6.20 MB/s** | 4.5× |
| **tinyparse (C-compiled)** | 1.545 s | **5.89 MB/s** | 4.7× |
| tinyparse (bytecode VM)  | 2.065 s | 4.41 MB/s  | 6.3× |

W16-GOAL moved the C-compiled arm **4.48 → 5.89 MB/s (+31%)** — now **0.95× of
tree-sitter** (just ~5% short) and **1.34× the VM**. The headline goal (strictly
faster than tree-sitter) is therefore NOT yet met and `W16-FASTER` is deliberately
not recorded; this is the clean-lexer base for the holistic hand-written-RD round.

### Standing gates green at the commit

Build clean (0 warnings under `-Werror`), metric 296/296, cc-diff **byte-identical**
(VM and generated parser share `token.c`/`vm.c`, so the lexer/memo changes move both
in lockstep), vm-diff GREEN, no-statics GREEN, vm-nocrash + parser-nocrash GREEN under
`ulimit -s 512` (the not-guard scan is a bounded loop — no new recursion), perf gate
ratcheted 5.5 → 5.2 ms and within it, multiparser + hide-ax GREEN (no W16-3 / W16-4
regression).

## W17-1 — the preservation net, hardened (and a real hole closed)

Before the Phase 17 emitter rewrite, three gates now pin the tinyparse-special
capabilities that `cc-diff`/conformance did not yet pin, so the rewrite cannot
silently drop them. Hardening the net surfaced a genuine pre-existing hole.

- **The hole:** the packed grammar (`.tpg`) a compiled-to-C parser embeds did NOT
  serialize the embedded grammars referenced by `TP_OP_INJECT`, nor the `lex_goals`
  table. So a generated C parser silently dropped **language embedding** (every inject
  loaded with `embedded_grammar == 0`) and **lexical goals** (heredoc/context). Fixed
  in `pack.c` (symmetric write+read): a trailing embedded-grammars section writes each
  embedded grammar as a full nested `.tpg` IMAGE, recursively, and the loader wires
  every inject node's `embedded_grammar` from it with no module resolver; `lex_goals`
  serialize alongside. `tinyparse c std.fence --parser` now bakes in std.javascript
  + std.json and the generated parser preserves injection and goals.
- **diag-diff gate** (`ci/tinyparse-diag-diff.sh`): for `(recover …)` grammars, the
  generated parser's diagnostics (message/help/label/severity + missing-node
  positions) must match the VM's on a broken-input corpus. **GREEN 8/8.** (cc-diff
  pins the recovered tree; this pins the diagnostics.)
- **import→C gate** (`ci/tinyparse-import-c.sh`): a grammar composed via `(import …)`
  compiles to a standalone C parser whose tree matches both a committed golden and the
  VM — the "build from std pieces, ship native" path, end to end. **GREEN.**
- **inject cc-diff coverage**: `std.fence` (injecting std.json + std.javascript) is now
  in the cc-diff differential corpus; the generated parser is byte-identical to the VM
  on fenced documents, injection subtrees and `[lang]` tags included. cc-diff
  **271/271**.

Standing gates green: build clean, metric 296/296, cc-diff 271/271 byte-identical,
vm-diff, no-statics, vm-nocrash + parser-nocrash, perf within ceiling, multiparser +
hide-ax. The net is complete; the W17-2 emitter rewrite runs under it.

## W17-2 — the IR-walking emitter (the generated parser is hand-written, by name)

### The design

W16-2 made the generated parser true native recursive descent, but the emitter still
walked the **lowered bytecode program** (`tp_program` / `tp_instruction`) and emitted
one function per program index — `tp_parser_p<N>` — that read structural operands from
`program->instructions[N]` at run time. It was "bytecode rendered as C": `rule_0`/
`rule_N`, an opcode dispatched per node, the program array on the hot path.

W17-2 re-grounds the emitter **on the IR, by name** (`docs/archive/codegen-transform.md`, the
owner-signed blueprint). The generator now walks the grammar's IR — `grammar->rules[i].body`
and `grammar->tokens[i].pattern`, each a `tp_ir` tree — and emits the parser a person
would write:

- one function **per named rule**, `<prefix>_parse_<rule>` (`tp_parse_Value_14`), not
  `rule_0`/`rule_N`;
- one function **per named token**, `<prefix>_scan_<token>` (`tp_scan_String_2`);
- `TP_OP_REF` → a **direct named call** (`parse_<name>` for a rule, `scan_<name>` for a
  token);
- SEQ/CHOICE/REPEAT/OPTIONAL/FIELD/ALIAS/GOAL/NO_EXTRAS/RECOVER/INJECT/EOF → idiomatic
  inline C control flow, the FIRST sets baked as `tp_choice_first_admits_fast` byte
  tests keyed on the child's own IR node.

There is no program array, no opcode dispatch, no `program->instructions[N]` read and no
`rule_0`/`rule_N` in the generated parser. A leaf finds its run-time `tp_ir` by
**navigating the loaded grammar** — `state->grammar->rules[i].body->children[a]->…` —
which round-trips in order through `pack.c`, so no instruction indices are needed; the
structure is the C, the few run-time operands are read in place off the named tables.

Byte-identity is kept by the **bake-vs-substrate split** the blueprint prescribes: the
grammar is baked away, but the CST builder and every special feature stay the shared
runtime the VM calls — `tp_vm_call_enter`/`exit` (CST + rule memo), the terminal/byte/
EOF/extras/recover/inject helpers. The measured depth guard (W16-2) wraps each rule's
body recursion. cc-diff stays **271/271 byte-identical**, std.fence injection included.

### The per-token baked lexer (the throughput win)

The steady-state callgrind profile of the generated parser (std.javascript, the largest
corpus file ×200) named the generic token VM as the dominant cost:

| function (self Ir)              | before W17-2 lexer |
|---------------------------------|--------------------|
| `tp_token_run` (the token VM)   | 30.7%              |
| `tp_token_match_memo`           | 13.5%              |
| `tp_vm_consume_extras`          | 14.4%              |

So the emitter now compiles each token's pattern to **straight-line C**, the code a hand
lexer is: a literal → byte compares; a byte class → a `static const uint8_t [256]` table
indexed in one step; a class repeat → a tight loop; `seq`/`choice`/`optional`/`repeat`
over those → idiomatic C; `not_keyword` → the inner match plus whole-slice reserved-word
compares (so the hot `identifier` token bakes); `not` (negative lookahead) → a scratch-
cursor guard (so comments, strings and slash bake). `scan_<token>` routes a bakeable
token through a new shared `tp_vm_terminal_baked` (extras + lexical-goal redirect + leaf
push + advance, byte-identical to `tp_vm_terminal`) calling that pure matcher; any
unbakeable shape keeps the generic matcher — byte-identity is total. The **extras** scan
(whitespace/comments), which runs before every terminal off `tp_vm_consume_extras`, is
baked the same way: a `<prefix>_extras_match` scanner the driver wires into a new
`state->extras_match` hook, used under no lexical goal, with the policy/node/diagnostic
logic shared verbatim.

The cumulative effect on the same profile:

| function (self Ir)         | W17-2 emitter | + leaf lexers | + extras baked |
|----------------------------|---------------|---------------|----------------|
| total instructions         | 106.4M        | 95.3M         | **69.8M**      |
| `tp_token_run`             | 30.7%         | 22.4%         | **6.8%**       |

**~34% fewer instructions**; `tp_token_run` collapsed from 30.7% to 6.8% (only rare
non-extras generic tokens remain). The top costs are now the irreducible recursive-
descent + CST work — `tp_vm_consume_extras` bookkeeping, `tp_tree_builder_push`,
`tp_vm_call_enter`/`exit`, `tp_vm_sink_push` — exactly what a hand-written parser pays.

### The 6-arm benchmark (ReactJS, std.tsx, parse-only, this hardware)

Intel(R) Xeon(R) @ 2.80GHz (4 cores), 15.7 GiB, Linux 6.18.5 x86_64. Corpus: 1070 React
files, 9,546,659 bytes (clean-parsing under std.tsx). Parse-only, warm, median of 30
(in-process arms) / 15 (tsc/tsgo/meriyah). Both tinyparse arms parse the same bytes,
per-file-fresh-engine, only the parse call timed — cc-diff proves they build the same
tree.

| parser (parse-only)        | median   | throughput  | ratio |
|----------------------------|----------|-------------|-------|
| tsgo (typescript-go)       | 0.298 s  | 30.58 MB/s  | 1.0x  |
| meriyah (JS+JSX subset)    | 0.205 s  | 28.13 MB/s  | 1.1x  |
| tsc (compiler API)         | 0.745 s  | 12.21 MB/s  | 2.5x  |
| **tinyparse (C)**        | 1.382 s  | **6.59 MB/s** | 4.6x |
| tree-sitter (TSX, GLR)     | 1.449 s  | 6.28 MB/s   | 4.9x  |
| tinyparse (bytecode VM)  | 2.056 s  | 4.43 MB/s   | 6.9x  |

The C-compiled parser is **strictly faster than tree-sitter** (6.59 vs 6.28 MB/s) — the
phase floor crossed — and **1.49× the VM** (6.59 vs 4.43). It is not yet tsc-class
(12.21); the honest next costs are named above (the CST build and the per-rule
call_enter/exit machinery now dominate, not the lexer). This is a real, measured,
byte-identical gain past the tree-sitter bar, recorded without faking the phase marker.

Reproduce: `BENCH=1 bash ci/tinyparse-bench5.sh`; the profile per the Appendix, with
`bench/prof/prof_compiled.c` driving the generated `--parser-lib` over the largest
corpus file under callgrind.

## W17-3 — configurable prefix by grammar name, and native nesting through the ax inject hook

W17-2 made the generated parser hand-written C, but two capabilities the blueprint
demands for real multi-language work were still half-built: the symbol prefix was
*configurable* (W16-3's `--name`) but not *defaulted*, so two parsers collided unless
the caller remembered to name them; and language injection still ran the embedded
region through the bytecode VM, so a generated host parser could not nest a *generated*
embedded parser. W17-3 closes both.

### The prefix scheme — defaulted from the grammar name

Every emitted file-scope symbol already routes through `tp_parser_name`, which renders
`<prefix>_<suffix>` when a prefix is set and the historical flat name (`tp_parse_…`,
`tp_compiled_parser_load/run`) when it is not. W16-3 set that prefix only from an
explicit `--name`. W17-3 captures the grammar's own name — the `NAME` in the source's
`(grammar NAME …)` form — onto `tinyparse_grammar.name_symbol` at compile time, and
the emitter defaults the prefix from it when no `--name` is given:

- `std.json`'s grammar is `(grammar json …)` → prefix `json` → `json_load`, `json_run`,
  `json_parse_Value_…`, `json_scan_String_…`;
- `std.javascript` → `javascript_…`; the `host.scm` fenced grammar → `host_…`.

The name is sanitized exactly like the readable rule slug (every non-`[A-Za-z0-9_]`
byte becomes `_`, a leading digit gains an `s`), so any grammar name yields a legal C
identifier. `--name` still overrides outright, and a grammar built with **no** captured
name (an old binary `.tpg` that predates the field) falls back to the flat default
names — so legacy callers, the profiling harnesses that link `tp_compiled_parser_*`, and
the cc-diff oracle are byte-for-byte unaffected.

Two consequences matter. First, **multiple languages coexist in one project with no
flag**: parsers compiled from differently named grammars carry disjoint symbol sets by
default, because their grammars are named differently. Second, the change is invisible to
cc-diff: that gate compares the generated standalone parser's `--dump-tree` against the
VM, and renaming internal symbols does not move a single output byte — 271/271 byte-
identical holds, std.fence injection included.

### The inject hook — a language registry the engine holds, resolved by name

`(inject grammar: NAME content: REGION)` bakes, on the host side, into "match the
region, drop the raw region nodes, hand the span to the inject runtime" — and the host
parser **names** `NAME` as a compile-time constant but is **not linked** against the
parser for it. Resolution, execution, and isolation are the runtime's job. Before W17-3
the runtime (`tp_inject_run`) only ever re-parsed the span with the grammar resolved at
compile time, through the in-process VM. That cannot compose two separately-compiled
*generated* parsers: the host binary would have to link every embeddable grammar.

W17-3 adds a **language registry on the engine**. A host registers an embedded language
by name:

```c
tinyparse_language_entry entry = { json_grammar, json_lang_run };
tinyparse_register_language(engine, str("std.json"), &entry);
```

An entry carries the embedded grammar (whose interner names the spliced nodes) and an
**optional** parse function. When `parse` is NULL the region is parsed by the bytecode VM
over the entry's grammar — the dynamic case, byte-identical to before. When `parse` is
set it is a separately-compiled generated parser's `<name>_run` entry. `tp_inject_run`
now resolves the language **by name in that registry first**, and only falls back to the
compile-time `embedded_grammar` (the in-grammar injection case) when nothing is
registered. So the host binary names the language; the registration wires the two
generated parsers at runtime.

The embedded parse stays exactly the supervised, isolated child it always was: its own
builder, diagnostics, grammar, and input window, sharing only the host arena so its nodes
outlive the call for the splice (offset-mapped, re-interned, language-tagged). A failing
embedded parse is contained — its tree, with its own ERROR/diagnostics, is spliced under
the language-tagged `injection` node regardless, so the host tree and the process are
untouched. Injection depth is bounded by injection nesting, not input depth: the
generated host parser runs the registered parser **synchronously** over the fully-known
span, with no suspension — the fast pre-compiled mode the blueprint names. (Streamed,
message-coordinated embedding remains the VM/actor engine's job, the same split as the
buffer-friendly boundary, deferred to W17-4.)

### Why ax, not a bare function-pointer table

A function-pointer table would cover only the static, in-binary case. The registry is
keyed by *name* and resolved at *parse* time, which is what lets a host compose
separately-compiled parsers it never linked, swap an embedded parser for a newer one, or
register a VM grammar and a generated parser interchangeably under one name. It rides the
same session abstraction that already owns the arena, the builder, and streaming — the
parse-as-actor the engine runs on — so the supervision and isolation that make
complicated nesting safe come for free, and the streamed-embedding path has a home. The
per-grammar prefix keeps two generated parsers collision-free in one binary; the registry
keeps them composable at runtime.

### The demonstration

`examples/nesting` links three parser-lib translation units into one program: a
**generated** `std.fence` host parser, a **generated** `std.json` parser, and a
**generated** `std.javascript` parser, each with its own `--name` prefix. The driver
loads the host grammar, registers the two embedded generated parsers on the engine, and
parses a fenced document. The generated host parser's baked inject sites resolve each
fence's language in the registry and drive the generated embedded parser over the fence
content — native nesting, no VM in the loop.

`ci/tinyparse-nesting.sh` pins it: the composite tree the generated host+embedded
parsers build is **byte-identical** to the VM's `run std.fence --dump-tree` on the same
document (the JSON subtree under the json fence, the JavaScript subtree under the js
fence, every node tagged with the language that produced it); all three parsers' entries
link with no collision; and a hide-ax shared object of the set exports the parser entries
and the public `tinyparse_*` API and **zero** `ax_` symbols. Generated host nesting
generated embedded yields the same tree as the dynamic VM — nesting changed nothing about
the CST.

## W17-4 — the buffer-friendly boundary: the generated parser over a growing buffer

The generated parser is native recursive descent (W16-2, W17-2): one C function per
rule, the call stack is the parse stack. That is the throughput win, but it is also a
constraint — a native call stack cannot be frozen and resumed, and ax offers no
stackful fiber to freeze it into (ax's continuation is its journal, not a saved C
stack). So the generated parser cannot suspend partway down a rule the way the
explicit-stack VM can, where the arena frame stack *is* the continuation between
`tinyparse_push` calls. W17-4 gives the generated parser the one suspension point
it *can* honor, and wires a session to drive it over input that arrives in pieces.

### The boundary is the shallow top-level-item loop

A grammar whose start rule is a repeat of top-level items — `sexpr`'s
`Source = (seq (repeat Form) EOF)`, a JSON value sequence, the import/words example —
has exactly one place where the C stack is empty: the top of each iteration of that
repeat, before the parser commits to the next item. Between items there is no
half-finished rule on the stack to lose. That is where the generated parser suspends.

Within an item it runs at full native speed, recursing as deep as the item needs; it
never checks for "more input" mid-item, so the hot path is unchanged. The boundary
check lives only in the top-level loop, and only when the driver has said more input
may still come (`parser->input_complete == 0`). The rule is conservative and sound:
an item is committed only when it completes **strictly** before the buffer end — a
following byte then proves the item is fully delimited. If parsing an item reaches the
buffer end (success *or* the loop-terminating failure) while the input is incomplete,
the loop rolls the cursor back to that item's start, records the boundary, and returns
`TINYPARSE_MORE`. No matcher changes were needed: the boundary is a comparison of
the cursor against the buffer length at the item edge, nothing deeper.

Holding back any item that touches the very end of the buffer is what makes the result
byte-identical to a whole parse. Such an item might have continued had more bytes been
present (an atom `foo` extending to `foobar`, a list still open), so it is never
committed early. On the final chunk the driver sets `input_complete`, the held-back
check is disabled, and the loop parses the tail exactly as the whole-input driver
does — the same items, in the same order, into the same root.

### Resumption keeps the tree, not just the position

`TINYPARSE_MORE` is not a restart. The root node, the child sink, and the cursor
boundary persist on the parser across calls (`bf_root`, `bf_children`, `bf_position`).
Each `run_chunk` re-initializes the throwaway parse state (memos are caches; the token
index and extras lead-set are grammar-scoped and already built) but resumes the
top-level loop at `bf_position` into the *same* root and sink. Items committed on
earlier calls stay; only the unfinished tail is re-attempted with the new bytes. When
the input completes, the loop runs the identical finalization the whole-input driver
runs — trailing extras, the `EOF` check, link children, freeze — so the frozen tree is
the one a single whole parse would have built.

This is a shared runtime helper, `tp_parser_chunk_loop`, parameterized by the
generated per-item function, whether the start body ends in `EOF`, and the repeat's
lower bound. The emitter recognizes the top-level-item shape, emits a per-item
function (the exact body of one whole-input REPEAT iteration, so an item parsed in
chunks is byte-identical to one parsed whole), a chunk driver that sets up the state
the same way the whole-input driver does, and the `<prefix>_run_chunk` entry. The
whole-input driver and `<prefix>_run` are untouched — the change is purely additive,
and cc-diff stays 271/271 byte-identical.

### How ax coordinates it, and the split it preserves

The parse is already an ax session (W60): the parser actor owns the arena, the tree
builder, the diagnostics, and the input buffer; `tinyparse_push` routes bytes to it
as `input.chunk` events. The buffer-friendly path rides that same session. A host that
pulls input drives the generated native parser over the session's growing buffer:
push the bytes available, mark whether the input is now complete, call `run_chunk`;
on `TINYPARSE_MORE`, append more and call again. The generated parser runs *inside*
the session, against the session's state — ax owns the buffer and the arena, the
native parser owns the per-item descent.

ax picks the mode, exactly as the blueprint's reconciliation states. The generated
native parser serves the buffer/pull case with per-item streaming: the span between
suspensions is a whole top-level item, fully known, parsed synchronously at native
speed. Arbitrary-granularity **async push** — chunks that arrive in the middle of an
item — stays the explicit-stack VM/actor's job, because only its arena frame stack can
suspend mid-item. `tinyparse_push` streaming on the VM path is unchanged by this
workstream (vm-diff GREEN). It is the same split as the inject hook's synchronous
(generated, known span) versus message-coordinated (VM/actor, streamed span) modes
recorded in W17-3.

### The proof: chunked == whole == VM

`examples/bufferfriendly` drives the generated `sexpr` parser over a document in
fixed-size chunks through the `TINYPARSE_MORE` loop and dumps the resulting tree.
`ci/tinyparse-bufferfriendly.sh` parses the same document at eight chunk sizes — 1,
2, 3, 5, 8, 17, 64, and one larger than the file — and asserts every result is
byte-identical to the VM's whole-input `--dump-tree` of the same grammar. At chunk
size 1 the parser genuinely suspends 189 times (the gate fails if it does not, so the
boundary cannot be silently bypassed); at the largest size it suspends zero times and
runs as one whole parse. Chunked equals whole equals VM, byte for byte, across the
whole granularity range — the depth guard and byte-identity the phase protects are
preserved, and the generated parser is now drivable over a buffer it does not have all
of yet.

## W17-GOAL — profile-driven: toward tsc-class throughput

W17-2 made the generated parser hand-written C and crossed the tree-sitter floor
(6.59 MB/s vs 6.28). The bar is tsc-class. This workstream is profile-driven: each
change is measured (callgrind instruction counts and the 6-arm wall-clock bench), and
only measured wins are kept. The setup is the W17-2 one — `bench/prof/prof_tsx.c`
driving the generated `std.tsx` parser (`typescript_load`/`typescript_run`) over the
largest React file (`ReactDOMFloat-test.js`, 311 KB) under callgrind, ×40 — except the
profile now times **push + run** exactly as the bench does, so a cost in the input
buffering shows up.

### The starting profile (callgrind, generated std.tsx parser, 311 KB ×40)

| function (self Ir)           | % of 634.3M |
|------------------------------|-------------|
| `tp_parser_buffer` (push)    | 15.70%      |
| `__memset_avx2` (arena zero) | 11.94%      |
| `tp_vm_consume_extras`       | 11.79%      |
| `tp_vm_call_enter`           | 7.50%       |
| `tp_vm_call_exit`            | 5.72%       |
| `tp_tree_builder_push`       | 5.71%       |
| `tp_vm_sink_push`            | 3.61%       |
| `tp_vm_link_children`        | 3.22%       |

The W17-2 profile (std.javascript, run-only) had named the per-rule lifecycle and the
CST build as the gap; profiling the bench's actual timed region (push + run) named a
new top cost the run-only profile had hidden: **the input buffer copy**.

### Change 1 — buffer the input with a bulk copy, not a byte loop

`tp_parser_buffer` (the body of `tinyparse_push`) copied the pushed bytes into its
growable arena buffer **one byte at a time** in a C `for` loop, and allocated the
buffer with the zeroing `ax_alloc` even though every byte is immediately overwritten.
On the 311 KB file that byte loop was the single largest cost in the timed region
(15.70%). Replaced both copies with `__builtin_memcpy` (lowered to the vectorised libc
`memcpy` already linked through ax — no new header dependency in the freestanding
source) and switched the buffer allocation to `ax_alloc_raw` (the buffer is fully
written before it is read, so zeroing it was a discarded memset).

| metric (callgrind, 311 KB ×40) | before | after  | delta   |
|--------------------------------|--------|--------|---------|
| total instructions             | 634.3M | 526.2M | −17.0%  |
| `tp_parser_buffer`             | 15.70% | off top| —       |
| `__memset_avx2`                | 11.94% | 10.41% | −1.5pp  |

Pure harness/push cost, no parser logic touched, so cc-diff stays byte-identical
(271/271) and every net gate is green. A −17% instruction cut in the bench's timed
region from the first measured change.

### Change 2 — allocate the rule-memo tables lazily, only when the memo wakes

After change 1 the `__memset_avx2` slice was still 10.41% (54.8M instructions), called
only ~144×/parse — a few large block zeroings, not per-node. Tracing the callers named
the **rule-memo `live` column**: `tp_rule_memo_init` allocated and zeroed an
input-scaled occupancy table (`pow2(2·input + 64)` bytes, ~1 MB for the 311 KB file) at
the start of *every* parse. But the rule memo is the super-linear backstop: it is
dormant until `tp_vm_call_enter`'s attempt counter crosses the threshold
(`memo_active`), which a clean, linear parse never does — so on the common path that
million-byte table was allocated and zeroed and never read.

Split the init into a no-allocation `tp_rule_memo_init` (record the arena and the
intended size, leave capacity 0) and a lazy `tp_rule_memo_ensure` called exactly when
`memo_active` flips on (and on the differential's force-on path, so the replay path is
still exercised). A clean parse now never touches the table; a parse that goes
super-linear builds it on the first activation, identically to before.

| metric (callgrind, 311 KB ×40) | before | after  | delta   |
|--------------------------------|--------|--------|---------|
| total instructions             | 526.2M | 484.3M | −8.0%   |
| `__memset_avx2`                | 10.41% | 2.65%  | −7.8pp  |

The verdict is unchanged (the memo is a pure cache, allocated at the same moment it is
first usable), so cc-diff stays byte-identical 271/271 — confirmed both on the default
path and with `TINYPARSE_RULEMEMO=always`, which forces the memo on and exercises the
full probe/store/replay through `ensure`. The pathological-input nocrash gates (where
the memo genuinely activates) stay GREEN.

Cumulative across changes 1–2: **634.3M → 484.3M, −23.6%** in the bench's timed region.

### Change 3 — inline the per-rule CALL lifecycle into the generated functions

With the lexer baked (W17-2) and the two memset costs cut (changes 1–2), the profile's
top remaining costs were the per-rule lifecycle the generated parser pays through the
shared helpers: `tp_vm_call_enter` (9.82%) and `tp_vm_call_exit` (7.49%) — ~17% combined.
Their self-cost was the memo bookkeeping every rule entry wrote (the `target`/`memo_pos`/
`diag_before`/`seen_success` stores, the `memoable` test) — all dead on a clean parse,
where the memo never wakes — plus the cross-TU function-call overhead, ~587K enters and
~488K exits over the file.

Specialized the lifecycle into two `static inline` wrappers (`tp_vm_enter`/`tp_vm_exit`
in `internal.h`) that the emitter now calls instead of `tp_vm_call_enter`/`exit`. On the
overwhelmingly common steady state — memo dormant, no recovery armed, no `no_extras`
scope, the frame's inline sink, no trace plug — they do **only** what a hand-written
parser does, as straight-line C the compiler inlines into the rule function with the
rule's kind/field/alias in registers: save the marks, consume leading extras, open the
node; and on exit link the children, close the span, push the node. The instant any
condition fails — the parse goes super-linear (the memo about to wake), recovery is
armed, a `no_extras` scope is in force, the VM's relocatable sink is in use, or a trace
plug is installed — they **delegate** to the shared `tp_vm_call_enter`/`exit`, so the
tree is byte-for-byte the VM's in every case. The memo-waking attempt counter is
threaded so the crossing entry always goes through the shared helper (which owns the
lazy table allocation and the first probe), and `call_attempts` is incremented exactly
once per entry on both paths. The VM (`tp_vm_run`) keeps calling the shared functions
directly — it is unchanged, the cc-diff reference intact.

| metric (callgrind, 311 KB ×40) | before | after  | delta   |
|--------------------------------|--------|--------|---------|
| total instructions             | 484.3M | 442.4M | −8.7%   |
| `tp_vm_call_enter` (self)      | 9.82%  | inlined| —       |
| `tp_vm_call_exit` (self)       | 7.49%  | inlined| —       |

Both lifecycle functions left the top of the profile — folded into the generated
`parse_<rule>` functions and the remaining shared CST helpers. cc-diff byte-identical
271/271 on both the default and `TINYPARSE_RULEMEMO=always` paths (the latter
exercises the delegation), and the pathological-input nocrash gates (which wake the
memo) stay GREEN.

Cumulative across changes 1–3: **634.3M → 442.4M, −30.3%** in the bench's timed region.

### Change 4 — inline the extras fast-reject at the hot call sites

With the lifecycle inlined, `tp_vm_consume_extras` was the runaway top cost (16.91%,
74.8M) — it runs before every terminal and on every rule entry, and at the dominant
position the cursor byte begins no extra, which the function already proves with one
lead-byte bitset probe and returns. But it returned as a real cross-TU call, ~millions
of times. Added an inline `tp_vm_extras` wrapper that does exactly that O(1) reject
(byte-for-byte the function's own fast path, same order: `no_extras` first, then the
lead-byte set) inline at every hot call site, falling through to the full
`tp_vm_consume_extras` only when a byte could begin an extra (or a `no_extras` scope or
an unpinnable lead set is in force). Wired into the inline rule lifecycle
(`tp_vm_enter`), the generated ordered-choice prologue, and the generated terminal path
(`tp_vm_terminal_baked`). The VM's own terminal keeps calling `tp_vm_consume_extras` —
the cc-diff reference unchanged.

| metric (callgrind, 311 KB ×40) | before | after  | delta   |
|--------------------------------|--------|--------|---------|
| total instructions             | 442.4M | 421.6M | −4.7%   |
| `tp_vm_consume_extras` (self)  | 16.91% | 7.14%  | −9.8pp  |
| `tp_vm_extras` (inline reject) | —      | 3.26%  | new     |

The extras work fell from 74.8M to ~43.8M (the inline reject plus the residual real
scans). cc-diff byte-identical 271/271 default and `TINYPARSE_RULEMEMO=always`; all
net gates GREEN.

Cumulative across changes 1–4: **634.3M → 421.6M, −33.5%** in the bench's timed region.

### Change 5 — bake the inline rule-body literals (drop the last generic-token-VM slice)

The profile's largest remaining slice was the generic terminal: a bare `TP_OP_LITERAL`
in a rule body — the punctuation a grammar matches directly (`(`, `;`, `=>`, `{`) —
still ran through `tp_vm_do_match_pattern` → `tp_vm_terminal` → the generic token VM
(`tp_token_run` + `tp_token_match_memo`), called ~320K times (≈19% inclusive). The
per-token *scanners* were baked in W17-2, but these anonymous inline literals were not.

The emitter now bakes each DISTINCT inline literal (deduped by bytes) to a pure matcher
(`<prefix>_litmatch_<n>`: straight-line byte compares, reusing the token-matcher
emitter) and routes the literal site through `tp_vm_terminal_baked` with that matcher —
the same shared extras-consume / leaf-push / `token` trace / cursor-advance as the
generic path. A bare literal is never goal-redirected (its kind is the literal text, not
a token symbol, so the redirect probe in both paths finds nothing), so
`tp_vm_terminal_baked` is byte-identical to `tp_vm_do_match_pattern` here. An empty
literal keeps the generic path. A literal-registry pre-pass assigns each distinct text a
stable matcher index before emission, so a site looks it up; the common punctuation
shares one matcher (664 literal sites collapsed to a handful of distinct matchers in
std.tsx).

| metric (callgrind, 311 KB ×40) | before | after  | delta   |
|--------------------------------|--------|--------|---------|
| total instructions             | 421.6M | 383.8M | −9.0%   |
| `tp_token_run` (generic VM)    | 3.56%  | off top| —       |
| `tp_token_match_memo`          | 5.21%  | off top| —       |

The generic token VM left the top of the profile — every terminal, token and inline
literal alike, is now baked straight-line C. cc-diff byte-identical 271/271 default and
`TINYPARSE_RULEMEMO=always`; all net gates GREEN.

Cumulative across changes 1–5: **634.3M → 383.8M, −39.5%** in the bench's timed region.

### Change 6 — allocate the extras memo lazily too

The last large memset (3.34%, ~311 KB zeroed per parse) was the **extras memo**'s marks
array — `tp_extras_memo_init` allocated and zeroed one byte per input offset at the
start of every parse. That memo only short-circuits a position re-visited on a
*backtrack* (a comment-opener `/` that turned out to be a divide, re-scanned), which a
clean predictive parse never does — so on the common path the array was zeroed and never
hit. Deferred it exactly like the rule memo (change 2): `tp_extras_memo_init` now only
records the size, and `tp_extras_memo_ensure` allocates the marks lazily on the same
`memo_active` wake (the parse going super-linear is precisely when backtracking — and
the memo — starts to matter). Until then marks is NULL, which disables the memo;
`tp_consume_extras` is unchanged and the memo is a pure cache, so the verdict is
byte-identical.

| metric (callgrind, 311 KB ×40) | before | after  | delta   |
|--------------------------------|--------|--------|---------|
| total instructions             | 383.8M | 370.9M | −3.4%   |
| `__memset_avx2`                | 3.34%  | off top| —       |

The arena-zeroing memset left the top of the profile entirely. cc-diff byte-identical
271/271 default and `TINYPARSE_RULEMEMO=always`; the pathological-input nocrash gates
(which wake the memo and now exercise the lazy extras-memo allocation) stay GREEN.

Cumulative across changes 1–6: **634.3M → 370.9M, −41.5%** in the bench's timed region.
The remaining top costs are the irreducible CST construction a hand-written parser pays
— `tp_tree_builder_push` (9.8%), `tp_vm_sink_push` (6.2%), `tp_vm_link_children` (5.5%)
— and the one-time input copy (`memcpy`, 6.7%). The generic token VM, the per-rule
lifecycle bookkeeping, and every input-scaled zeroing are gone from the hot path.

## W17-GOAL2 — de-packrat the generated parser (no memo on the hand-written path)

The owner's reframing (`docs/decisions.md`, "the formalism"): the generated C parser is
**not** a packrat-PEG-generated parser. It is a code generator that types what a human
would type by hand, using ax to manage the logical processes (the parse session, nested
parsers, streaming). A hand-written recursive-descent parser does not carry a packrat
memo — so neither should the generated parser. The VM (`tp_vm_run`) keeps the memo, because
it must bound super-linear backtracking for arbitrary runtime-supplied grammars; it stays
the dynamic engine and the cc-diff byte-identity oracle.

### What was removed

The generated parser pays the per-rule CALL lifecycle through two generated-only inline
wrappers, `tp_vm_enter`/`tp_vm_exit` (`internal.h`). Before this round they carried the
adaptive-memo bookkeeping: `++state->call_attempts`, the `state->call_attempts >=
state->memo_threshold` threshold compare, the `state->memo_active` check, and a delegation
to the shared `tp_vm_call_enter` whose body runs the probe/store/snapshot/replay. The memo
was already *dormant* on the std corpora (left-factored to linear, the W17-GOAL profile
showed zero rule-memo cost), but the bookkeeping ran on every rule entry.

- `tp_vm_enter`/`tp_vm_exit` now carry **no** memo bookkeeping: no `call_attempts`, no
  `memo_threshold`, no `memo_active`. They do only the hand-written per-rule work (marks,
  leading extras, node open/link/close/push) and delegate to the shared helper **only**
  for the genuinely non-trivial lifecycle (recovery armed, a `no_extras` scope, the VM's
  relocatable sink, a trace plug).
- The generated driver pins the memo OFF (`memo_threshold = 0xFFFFFFFF`,
  `memo_active = 0`) right after `tp_vm_state_init`, so even a delegated CALL lifecycle
  cannot wake it. (The lazy memo allocation introduced at W17-GOAL means no input-scaled
  memo table is allocated for the generated parser either — it was already dormant; now it
  is provably absent.)

The generated `std.tsx` parser now contains **zero** memo machinery — `grep memo` over the
emitted file finds only the two explicit pin-off lines and their comment.

### Measured (callgrind, std.tsx over the largest React file ×8)

| metric (callgrind, 311 KB ×8)  | before        | after         | delta   |
|--------------------------------|---------------|---------------|---------|
| total instructions             | 1,456,543,386 | 1,437,366,138 | −1.32%  |

Wall-clock on the prof driver (60 reps) is flat within noise (~1.085 s → ~1.083 s): the
removed work is a handful of scalar ops per rule entry, so it shows in the instruction
count more than the wall-clock. The 6-arm bench (per-file-fresh-engine, which masks
per-rule wins behind engine setup) holds the C-compiled arm at ~9.78 MB/s, **1.20×
tree-sitter, 1.64× the VM**, byte-identical. The honest framing: the win is small and
mostly masked at the bench level, but it is real in the timed region, and the larger point
is correctness-of-form — the generated parser now reads and runs like the hand-written
recursive descent it is meant to be, with the memo a VM-only concern.

### The audit for other interpreter-generality residue

With the memo gone, the profile's top costs are the irreducible hand-written-parser floor:
`tp_token_run` (the baked lexer, 10.6%), `tp_tree_builder_push` (9.3%, the packed node
write), `tp_vm_consume_extras` (7.2%, the trivia policy), `tp_vm_terminal_baked` (6.6%),
`tp_vm_sink_push` (5.8%) and `tp_vm_link_children` (4.9%) — the CST build, the lexer, and
the extras policy, exactly what a hand parser does. No further generic per-node
bookkeeping that serves only the interpreter's generality remains on the generated path;
the `tp_vm_frame` fields the memo used (`target`/`memo_pos`/`diag_before`/`seen_success`)
are written only by the shared VM/delegated path, never by the inline hand-written path,
and removing them would touch the shared VM struct for no measured win.

### Gates

build clean; cc-diff **271/271 byte-identical** (and fast, ~27 s — no exponential blowup
without the memo, the std grammars being left-factored); metric 296/296; vm-diff GREEN
(the VM keeps its memo, including the `TINYPARSE_RULEMEMO=always` replay path); diag-diff
8/8; import-c, nesting 1/1, bufferfriendly 8/8, no-statics 0, multiparser 1/1, hide-ax 1/1;
parser-nocrash + vm-nocrash GREEN under `ulimit -s 512` (the depth guard, not the memo, is
the crash-safety mechanism — unchanged); perf-gate 3.7 ms within the 5.0 ms ceiling.

## W17-CST — the CST build path: incremental sibling linking + a pre-sized node array

W17-GOAL2 left the profile's top costs on the generated path as the **CST construction a
hand-written parser pays**. This round attacked that build path directly — the per-node
allocation and the per-rule child linking — measuring each change with callgrind and a
same-machine wall-clock A/B, holding the `--dump-tree` output and the cursor API invariant
(cc-diff is the byte-identity oracle).

### The profile, before (callgrind, generated `std.tsx` over the largest React file ×20, load amortized)

The build trio dominated. Ranked self-cost (of 184.57 M total instructions):

| rank | function                       | self Ir   | %      | what it is |
|------|--------------------------------|-----------|--------|------------|
| 1    | `tp_tree_builder_push`         | 18.11 M   | 9.81%  | append the 40-byte packed node + grow the node array |
| 2    | `__memcpy` (libc)              | 12.45 M   | 6.75%  | input-into-arena copy + array doublings |
| 3    | `tp_vm_sink_push`              | 11.44 M   | 6.20%  | append a child index to the per-rule sink |
| 4    | `tp_vm_terminal_baked`         | 10.45 M   | 5.66%  | baked leaf scan + emit |
| 5    | `tp_vm_link_children`          | 10.22 M   | 5.54%  | **the second pass** wiring parent/sibling links at rule close |
| 6    | `typescript_extras_match` + `tp_vm_consume_extras` | ≈16.6 M | ≈9.0% | the trivia policy |
| 7    | `tp_choice_first_admits_fast`  | 8.15 M    | 4.41%  | baked FIRST-set choice prediction |
| 8    | `ax_alloc_raw`                 | 6.15 M    | 3.33%  | node/sink allocations |
| 9    | `tp_vm_enter`                  | 4.23 M    | 2.29%  | per-rule node open |

The build trio (`tp_tree_builder_push` + `tp_vm_sink_push` + `tp_vm_link_children`) was
**≈21.5%** of all instructions, plus most of the `memcpy` and `ax_alloc_raw`. The extras
scan and the baked lexer/choice prediction are the *other* hand-parser costs, out of scope
for this structural round (they are not the CST build).

### Change 1 — incremental sibling linking (drop the `link_children` second pass)

`tp_vm_link_children` walked a rule's whole child array **again** at close, cold, to wire
each child's `parent`/`next_sibling` and stamp `first_child`/`child_count`/span on the
parent. The sink's `items[]` array exists for exactly two jobs: it is the **rollback
ledger** an ordered-choice attempt truncates (`sink->count = mark`), and it is the
direct-child list that second pass reads.

The fix: wire each child's `parent` and forward `next_sibling` **in `tp_vm_sink_push`**, at
the moment the child is appended — while the node `tp_tree_builder_push` just wrote is still
hot in cache. The previous sibling is read from `items[count-1]` (the ledger), not a cached
pointer, so a failed-attempt rollback that truncated `count` re-derives the correct last
sibling on the next push automatically. `tp_vm_sink_close` then finalises in **O(1)**: stamp
`first_child`/`child_count`, terminate the final sibling's `next_sibling` (left dangling
during forward linking), extend the span — reading only `items[0]`, `items[count-1]`, and
`count`. The sink carries the parent node index; the VM and the generated parser share the
same helpers, so the tree is byte-identical. `tp_vm_sink_push`/`tp_vm_sink_close` are
`static inline` at the emit sites (one push per node is the hottest CST call); only the
geometric grow stays out of line (`tp_vm_sink_grow`).

First cut (non-inline helpers) **regressed** to 187.59 M instructions — the new
`tp_vm_sink_close` call and the per-push branch cost more out-of-line than the tight
`link_children` loop. Inlining both recovered it: the linking folds into `tp_vm_exit`.

| metric (callgrind, ×20)        | before W17-CST | after change 1 | delta   |
|--------------------------------|----------------|----------------|---------|
| total instructions             | 184,570,014    | 178,639,100    | −3.21%  |
| `tp_vm_link_children`          | 10.22 M        | 0 (gone)       | —       |
| `tp_vm_sink_push`/`_close`     | 11.44 M        | folded into `tp_vm_exit` | — |

### Change 2 — pre-size the node array from the input length

`tp_tree_builder_push` grew the node array by doubling from 32, and ax arenas never realloc
in place, so each doubling copied every node already built — the copy loop (and the
`memcpy` it drove) was a top slice. A real parse settles near one node per few input bytes,
so `tp_vm_state_init` now reserves `input_length / 4` nodes up front, when the builder is
still empty (the first push of the parse), absorbing essentially all the growth into one
allocation. It is a pure capacity hint — the tree contents are byte-identical, only the
realloc schedule changes — skipped for tiny/empty inputs and for an already-grown builder
(the buffer-friendly resume path keeps its array).

| metric (callgrind, ×20)        | after change 1 | after change 2 | delta   |
|--------------------------------|----------------|----------------|---------|
| total instructions             | 178,639,100    | 175,684,364    | −1.65%  |
| `tp_tree_builder_push`         | 18.11 M        | 15.16 M        | −16.3%  |

Peak RSS for the compiled bench arm also **dropped** (55.8 MB → 45.2 MB): the doubling
schedule over-allocated more than `input/4`.

### End-to-end A/B (same machine, same moment)

Built the pre-W17-CST baseline (commit `b17d56a`) in a worktree and timed both binaries
on the profile file:

| measure                                  | baseline  | W17-CST   | delta    |
|------------------------------------------|-----------|-----------|----------|
| callgrind total, ×3 (incl. load)         | 31.55 M   | 30.22 M   | −4.2%    |
| callgrind total, ×20 (parse, load amortized) | 184.57 M | 175.68 M | −4.8%    |
| wall-clock, best-of-5, 400 reps          | 0.4076 s  | 0.3655 s  | **−10.3%** |

The wall-clock win is **larger** than the instruction-count win — the structural payoff of
writing links while the node is hot and removing the cold second pass and the doubling
copies, which the cache-blind Ir count understates.

### The 6-arm bench (ReactJS, std.tsx, parse-only; this host)

| parser (parse-only)        | median   | throughput  | ratio to tsgo |
|----------------------------|----------|-------------|---------------|
| tsgo (typescript-go)       | 0.242 s  | 37.55 MB/s  | 1.0×          |
| meriyah (JS+JSX subset)    | 0.153 s  | 37.59 MB/s  | 1.0×          |
| tsc (compiler API)         | 0.614 s  | 14.84 MB/s  | 2.5×          |
| **tinyparse (C)**        | 0.893 s  | **10.19 MB/s** | 3.7×       |
| tree-sitter (TSX, GLR)     | 1.127 s  | 8.08 MB/s   | 4.6×          |
| tinyparse (bytecode VM)  | 1.533 s  | 5.94 MB/s   | 6.3×          |

The C-compiled arm is **10.19 MB/s — 1.26× tree-sitter, 1.72× the VM, 0.69× tsc, 0.27×
tsgo**, byte-identical. (The per-file-fresh-engine bench masks some of the per-parse win
behind engine setup; the timed-region A/B above is the cleaner signal.)

### The honest floor

`tp_vm_link_children` is gone and the node-array doubling is absorbed; the build cost is now
a cache-hot core (`tp_vm_exit`/`tp_tree_builder_push`). The genuine irreducible floor: per
node the 40-byte packed record is written and linked (one parent + one sibling store, now
while the node is hot), and per rule a node is opened and closed — the minimum a flat-array,
cursor-navigable CST builder does. **Dropping the child-index sink entirely** was
re-examined and stays out of reach without changing the byte-identical contract: `items[]`
is the rollback ledger a failed ordered-choice attempt truncates, and the incremental link
reads `items[count-1]` to recover the previous sibling *after* such a rollback, so it cannot
collapse to a bare first/last pair on the frame without also rewriting the generated
rollback marks (each `*_sink_NNN = sink->count` save/restore) and re-validating cc-diff —
a change with real regression risk for a sink that is already a single inline append.

### Gates

build clean; cc-diff **271/271 byte-identical** (the dump-tree did not move); metric
296/296; vm-diff GREEN; diag-diff 8/8; import-c, nesting 1/1, bufferfriendly 8/8 (the
resume path's deferred root link is unchanged), no-statics 0, multiparser 1/1, hide-ax 1/1;
parser-nocrash + vm-nocrash GREEN under `ulimit -s 512`; perf-gate within the ceiling.

## W17-FLAT — hidden rules: collapsing the single-child wrapper ladder to tree-sitter's shape

A node census of the 1070-file React corpus (9.04M CST nodes) found **~57% of all nodes
were single-child PASS-THROUGH wrappers** from the expression-precedence ladder. To reach a
bare `identifier`, tinyparse's left-factored grammar materialized ~20 chained nodes:

```
as_satisfies_expression -> assignment_expression -> ternary_expression ->
nullish_expression -> … -> unary_expression -> update_expression ->
postfix_expression -> member_or_call -> call_head -> primary_expression -> identifier
```

tree-sitter never emits these. It marks such rules **hidden** (a leading `_` on the rule
name) so they are omitted from the tree and their children promote to the parent. This is
why a tree-sitter TSX tree for `x` is `program > expression_statement > identifier` while
ours was 25 levels deep.

### The marker and its semantics (the PEG twist)

The `.scm` marker is `(hidden (rule …))` / `(hidden (override (rule …)))`, carried as a
`TP_RULE_HIDDEN` flag on `tp_rule_def`. The keyword form was chosen over tree-sitter's
leading-underscore convention because **our left-factored ladder rules do double duty**:
`additive_expression ::= mult (('+'|'-') mult)*` is a bare wrapper when zero operators
matched but a real binary node when one matched. A leading-underscore name would bury the
descriptive name in the case where the node survives.

So a hidden rule **collapses only when it added no structure** — exactly one child, no
alias, no operator / second child:

- A *pure dispatch* rule (`expression ::= (choice …)`) is always single-child → always
  collapses.
- An *operator-ladder* rule collapses only on its no-operator (pass-through) path; when an
  operator matched it has 3+ children and keeps its node, so `a + b * c` nests
  `additive_expression > multiplicative_expression` exactly as tree-sitter nests
  `binary_expression > binary_expression`.

This is the difference from tree-sitter's hidden rule, which always inlines: tree-sitter's
binary node is a *separate* named rule, so its hidden precedence rules carry no operator.
Ours merge the two, so the collapse must be conditional. When a hidden rule collapses, its
single child is re-stamped with the field the hidden rule's own call site wanted (the
ladder's inner `left:`/`condition:` gives way to the outer caller's field — tree-sitter
likewise attaches a field only to the surviving node).

### The mechanism: shared close, so the engines flatten identically

The collapse lives in the **shared node-close** — `tp_vm_exit` (the generated parser's
inline CALL epilogue) and `tp_vm_call_exit` (the VM's). When a hidden rule turns out to be
a pass-through, neither pushes the wrapper node; both call `tp_vm_collapse_hidden`, which
re-stamps and pushes the single child in the wrapper's place, leaving the wrapper node
orphaned (nothing links to it; the cursor never reaches it). Because the mechanism is in the
shared close, **the VM and the generated parser flatten identically by construction** —
`cc-diff` stays 271/271 with no new generated-side code beyond the flag riding the rule. The
hidden-by-symbol lookup is an O(1) byte array built once per grammar (like the token index),
consulted at CALL entry to set a frame flag. A collapsed success is never memoized (the memo
would snapshot the orphaned un-collapsed subtree); the memo is a pure cache, so the skip is
sound. Never collapsed: the root, injection (W62) nodes, aliased nodes, recovery/MISSING
nodes, leaves/tokens.

### Before / after (`tinyparse run std.tsx --dump-tree`)

`x` — the whole 25-deep ladder collapses to tree-sitter's three nodes:

```
  before                              after
  program                            program
    statement                          expression_statement
      expression_statement               identifier
        expression_sequence
          expression: expression
            as_satisfies_expression
              … (20 more wrappers) …
                identifier
```

`a + b * c` — the operator-ladder rules KEEP their node (operator matched), nesting like
tree-sitter's `binary_expression`:

```
  additive_expression
    left: identifier
    operator: +
    right: multiplicative_expression
      left: identifier
      operator: *
      right: identifier
```

### Node-count drop and bench

**60.8% of reachable nodes removed** on a 30-file React sample (57,024 → 22,336 dump
lines), matching the census's ~57% prediction. The 6-arm React bench (this host) is flat on
parse throughput — C-compiled **10.10 MB/s** (vs 10.19 pre-W17-FLAT, within variance), 1.25×
tree-sitter, 0.68× tsc — because the collapse *orphans* the wrapper rather than reclaiming
its slot: the node is still opened by `tp_tree_builder_push` at entry (the collapse is
decided at exit), and only the per-collapsed-node sink-push + O(1) close are saved. The
tiny, setup-dominated JS corpus the perf-gate measures does show the saving (5.0 → 3.7 ms;
ceiling ratcheted 5.0 → 4.0 ms). Reclaiming the slots — skipping the node entirely for the
always-collapsing dispatch rules — is the next lever, left as evidenced future work rather
than rushed (the Phase-15 honest-floor stance). The tree-shape result is real and gated.

### Gates

build clean; cc-diff **271/271 byte-identical**; vm-diff, diag-diff 8/8, nesting (golden
re-baselined), bufferfriendly GREEN; metric 296/296 (one inject `call_head` assertion
updated to the surviving `member_or_call`); import-c, multiparser, hide-ax, no-statics GREEN;
parser-nocrash + vm-nocrash GREEN under `ulimit -s 512` (the collapse only ever makes the
tree shallower); perf-gate 3.7 ms within the 4.0 ms ceiling.

## W17-RECLAIM — reclaiming the wrapper allocation W17-FLAT only orphaned

W17-FLAT made the tree right but kept the wrapper *allocation*: a hidden rule still opened
its node at entry (`tp_tree_builder_push`), and the exit-time collapse merely declined to link
it. The ~57% of nodes that collapse were still allocated and written (40 bytes each) — pure
waste, which is exactly why W17-FLAT's throughput was flat. W17-RECLAIM takes that allocation
back where it is provably never needed, **without changing one byte of output**.

### Stage 1 — skip the node at entry for transparent-always rules

A hidden rule splits into two kinds. A *pure dispatch* wrapper (`expression ::= (choice
assignment_expression)`, `primary_expression`, `lhs_expression`, `call_head`,
`member_property`, `postfix_expression`, `argument`, …) yields **exactly one child by every
path** — it *always* collapses. An *operator ladder* (`additive_expression` …) yields one
child on its pass-through path but a real node when an operator matched — it collapses only
*sometimes*. Stage 1 reclaims the first kind.

The grammar compiler proves the property structurally and marks the rule
`TP_RULE_TRANSPARENT_ALWAYS` (`tp_ir_yields_one_child`: a `ref`, a `(field …)`/`(goal …)`
whose child yields one, or a `(choice …)` whose alternatives *all* yield one;
`tp_grammar_mark_transparent_rules`, run after refs resolve so it can follow a `(choice ref)`
through the rule table). The flag implies `TP_RULE_HIDDEN` and rides the same packed flags
field, so the `.tpg` load path and the generated parser inherit it for free.

At runtime the CALL prologue (`tp_vm_enter` inline for the generated parser, `tp_vm_call_enter`
for the VM) takes a no-node path for a transparent rule with no alias: consume the leading
extras into the **caller's** sink (as the node-open would have), point the body's child sink AT
the caller's sink, and run the body — so its single child lands in the wrapper's place with no
node ever opened. The exit re-stamps the call's field onto that child, reproducing
`tp_vm_collapse_hidden` exactly. The one subtlety is leading extras: the re-stamp must target
the body's *significant* child, so the re-stamp index is captured *after* the extras consume,
not before (a Space landing first would otherwise wear the field — caught immediately by the
byte-identical reference dump). On the VM the fast path is gated `!memoable`: a memoed
transparent success would have no node to snapshot, so the rare memo-active path keeps the
exit-time collapse, leaving the memo's contiguous-subtree snapshot untouched.

**Probe folding.** The first cut read two index bytes per entry (hidden, then transparent),
both out-of-line. Callgrind showed the pair cost *more* than the node it reclaimed — a +1.0%
regression. The hidden-by-symbol index already packs both marks in one byte (bit 0 hidden,
bit 1 transparent), so reading it once through the inline `tp_grammar_rule_marks` and
extracting both bits turned the regression into a win.

### Stage 1 A/B (callgrind, deterministic instruction counts)

315 KB expression-heavy JS file, generated `std.javascript` parser, 3 passes:

| variant                       | instructions   | vs baseline |
|-------------------------------|----------------|-------------|
| baseline (W17-FLAT, b52cbde)  | 1,481,620,816  | —           |
| Stage 1, two probes           | 1,496,782,243  | **+1.0%** (regression) |
| **Stage 1, one probe**        | 1,425,982,163  | **−3.75%**  |

311 KB React file (`ReactDOMFloat-test.js`), generated `std.tsx` parser, 3 passes:

| variant                       | instructions   | `tp_tree_builder_push` |
|-------------------------------|----------------|------------------------|
| baseline (W17-FLAT)           | 552,253,572    | 168.7M                 |
| **Stage 1**                   | 547,018,544 (**−0.95%**) | 151.2M       |

The React win is smaller than the pure-expression win because React files are JSX/statement-
heavy, so the transparent expression-dispatch rules fire less proportionally. Wall-clock over
80 reps of the React file is a consistent ~3–4% faster (cache effects from fewer allocations).
Peak RSS for the compiled React arm dropped 45.2 → ~42.4 MB — the reclaimed allocations made
visible. The 6-arm bench C-compiled arm measures ~9.5 MB/s (vs 14.58 MB/s tsc = **0.65× tsc**).

### Stage 2 — lazy node-open for the operator ladders: the array-invariant finding, declined

The ladders' pass-through allocation (the bulk of what W17-FLAT orphans that Stage 1 does not
reclaim) needs *lazy open*: run the body with no wrapper, and create the node at exit only when
an operator matched. The blocking question is the **builder array invariant**. A lazily-created
wrapper sits at a *higher* index than its already-emitted children, inverting the
parent-precedes-children ordering the array has always held.

The investigation traced every consumer of the built array:

- the **cursor** (`tree.c`), the **linearizer** (`query.c`), the **inject subtree-copy**
  (`inject.c` — which in fact re-establishes preorder in the host builder), and **node-types**
  all navigate by *index links* (`first_child`/`next_sibling`/`parent`) and are
  order-independent;
- the **only** consumer requiring contiguous `[root, count)` preorder with root = min index is
  the **rule memo** — `tp_vm_snapshot_subtree` relocates by `parent -= root`, and
  `tp_vm_replay_success` re-bases the same way. The memo is off on the generated parser and
  wakes on the VM only after super-linear blowup, which the left-factored linear ladders never
  trigger.

So lazy-open **can be made safe** by gating it `!memoable` (eager open when the memo is armed),
exactly as Stage 1 gates the transparent path. But the reward is small. After Stage 1, only
**~5.2% of node pushes** remain orphaned — ≈7,300 of 140,830 pushes per parse of the 311 KB
file (the ladder pass-throughs), worth ≈0.5% of total instructions. Against that sits the
intricate earned-case child-adoption (allocate the wrapper above its children, re-parent and
re-link the sibling run, splice it into the caller's sink in place of the run) and the
inversion of a structural invariant a future feature could come to rely on. Per the Phase-15
honest-floor stance: **keep Stage 1, stop Stage 2, report the partial win** rather than risk
the byte-identical guarantee for half a percent.

### Did we pass tsc?

No. tsc (V8 JIT) measures 14.58 MB/s on the React corpus; the C-compiled arm measures
~9.5 MB/s = **0.65× tsc**. Stage 1 converts the W17-FLAT node-count drop from a flat result
into a small, real, byte-identical throughput gain — past tree-sitter, closing on tsc, not
yet past it.

### Gates

build clean; **reference dump byte-identical** to b52cbde across 345 jsfamily/tsfamily/bench
files; cc-diff **271/271**; vm-diff, diag-diff, nesting, bufferfriendly GREEN with goldens
**unchanged**; metric **296/296** goldens unchanged; import-c, no-statics, multiparser,
hide-ax GREEN; parser-nocrash + vm-nocrash GREEN under `ulimit -s 512`; perf-gate within the
4.0 ms ceiling.

## W18-WORDCACHE — the scan-once cursor cache, measured and declined at the thesis gate

A feasibility study proposed a one-entry cursor cache for the identifier-or-keyword "word"
class. The premise: word-class terminals (`identifier`, `jsx_identifier`, `identifier_name`,
the `kw_*` choices) re-scan the same word at the same cursor two or three times, so caching
the maximal word span once per `(post-extras position, lexical goal)` and letting each
word-class terminal reuse it would reclaim 8–12% of instructions, byte-identically. The design
named the smallest slice — the `jsx_name` choice, `(choice jsx_identifier identifier)` — as the
thesis-proving minimum: if a cache there moves no number, nothing larger should be built.

We instrumented the slice before writing any engine code, and the premise did not survive the
measurement.

### The jsx_name slice has no double-scan to remove

Parsing every `.tsx` file in the React reference corpus, and separately the 311 KB
`ReactDOMFloat-test.js`, with counters around the generated `jsx_name` choice:

```
all .tsx files:     jsx_name calls=3221   jsx_identifier ok=3221   identifier reached=0
ReactDOMFloat-test: jsx_name calls=28103  jsx_identifier ok=28103  identifier reached=0
```

`jsx_identifier` matches outright on every call; the `identifier` alternative is never tried.
The 28,103 figure the study read as a double-scan is 28,103 *single* successful scans. The
`tp_choice_first_admits_fast` predictor already gates the second alternative, and the two
classes are not even nested — `jsx_identifier` admits `-` but not high bytes, `identifier`
admits high bytes and escapes but not `-`, so neither is a strict widening of the other and a
shared "maximal word" does not exist for this choice. There is nothing at `jsx_name` for a
cursor cache to collapse.

### The whole word class is 1.45% of instructions, not the 26% bucket

The study's 26% token bucket is real on the package-built TSX parser (callgrind, 3 passes of
the 311 KB file: `tp_token_run` 10.46%, `tp_vm_consume_extras` 7.09%, `tp_token_match_memo`
6.63%, `tp_vm_terminal_baked` 6.26%+3.09%). But that bucket is **extras scanning and the
terminal driver**, not word re-scans. The baked word matchers themselves cost:

| matcher | Ir share |
|---|---|
| `scan_jsx_identifier_54_match` | 0.78% |
| `scan_identifier_14_match` | 0.59% |
| `scan_identifier_name_18_match` | 0.08% |
| `kw_*` matchers | below threshold (single-digit calls) |

The entire pool a word cache could draw from is **~1.45% of instructions**. Counting probes
through `tp_vm_terminal_baked` on the 311 KB file confirms the asymmetry: of 190,641 terminal
probes, **20.3% are word-class** (38,719) and 79.7% are contended (`<`, `/`, punctuation,
strings, numbers) — the class the cache must never touch. Among the word-class probes, 23,867
are same-position repeats and 22,879 of those re-probe the *same* token kind: a genuine
backtracking re-scan a cache could fold, but bounded above by the repeat fraction of 1.45% —
**under ~0.8% of instructions**, before the cache's own lookup, fill, and goal/cursor
invalidation cost. The killed per-position token memo (W16-GOAL) is the standing precedent that
this bookkeeping outweighs re-running a cheap baked matcher.

### Verdict

The thesis gate failed on its own terms. The `jsx_name` slice has zero removable scans, and the
broadest defensible reading of the word class caps the reward below one percent against a cache
whose cost the engine has already measured and rejected once. Per the round's mandate — *if the
slice does not move the needle, stop and commit nothing speculative* — we built no cache and
lifted no scanner (a refactor with no cache to follow is churn against the byte-identical
oracle for no gain). The corpus reference dump is unchanged and every gate stays green at its
current ceiling; this round changed no code.

### Where the cost actually is

The study's profile predates W17-FLAT and W17-RECLAIM, which already collapsed the wrapper-node
ladder the token bucket used to sit beneath. On the current engine the largest costs are
`tp_vm_exit`/`tp_vm_enter` and `tp_tree_builder_push` — tree construction — and
`tp_vm_consume_extras` (the whitespace/comment scanner between tokens, the true source of
`tp_token_run`'s 10.46%). A future round chasing tsc should attack extras consumption or the
enter/exit frame overhead, not word re-scans.

### Baseline

JS corpus: tinyparse 3.5 ms, **1.2× tree-sitter**, within the 4.0 ms perf-gate ceiling.
React TSX corpus (9.55 MB, 50 iterations): tinyparse 6.19 MB/s vs tree-sitter 7.49 MB/s
(0.83×); the C-compiled arm is **0.42× tsc** (tsc 14.58 MB/s). No B-side: the thesis failed at
the gate, so no change was made.

## W18-EXTRAS — the baked extras scanner, freed from the lexical-goal guard

W18-WORDCACHE named the standing target out loud: *attack extras consumption.* This round does,
and it lands a single-line change for a **−17.4% instruction cut** on the React file — the
biggest single-round win in the series. The whole `tp_token_run` / `tp_token_match_memo` subtree,
the profile's largest mass, vanishes from the cost map.

### The path that was paying for trivia

Between every token, the parser scans whitespace and comments. There are two scanners. The fast
one is baked: `tp_emit_parser` emits `<prefix>_extras_match`, a pure straight-line C function that
tries each extras token's baked matcher in table order and returns the first non-empty span — the
hand-lexer the generic loop never was. The slow one is the generic fallback in
`tp_vm_consume_extras`, a `for` over the extras table that re-runs `tp_token_match_memo →
tp_token_run` (the token bytecode VM) for every extra def at every cursor.

The fast scanner was wired behind a guard: `state->extras_match != 0 && state->active_goal ==
TP_NO_SYMBOL`. Under any active lexical goal it fell to the slow loop. In TSX a lexical goal is
live almost everywhere — `div`/`slash` (regex-vs-divide), the JSX text and template-body goals —
so the fast scanner barely fired (121 K of its potential calls), and the slow loop carried the
trivia. The profile pinned the cost precisely: `tp_vm_consume_extras` drove `tp_token_match_memo`
(620,544 of its 620,550 calls) which drove `tp_token_run` (all 194,727 of its calls). On the
311 KB React file that subtree was **~24.6% self / 26.7% inclusive** of all instructions — and
`tp_token_run`'s entire 10.46% self was trivia scanning, not terminal matching.

### Why the guard was conservative, not load-bearing

The guard reads as caution — *a goal could redirect a lexeme, so fall back to the goal-aware
loop.* The investigation shows the caution is misplaced: **the goal cannot change which bytes are
trivia.**

A lexical goal is purely about a *terminal reference inside a rule body*. The `(goal …)`
combinator sets `state->active_goal`; when a rule body then references a trigger terminal,
`tp_vm_goal_redirect` rewrites that reference to a different token's pattern (the same `/` byte
scanned as a divide operator in one goal, a regex opener in another). That is the whole of the
mechanism — a `(goal_symbol, trigger_symbol, target_symbol)` table consulted only at a terminal
reference.

An extra is never a goal trigger nor a goal target. The extras loop matches each extra's *fixed*
pattern directly; it never calls `tp_vm_goal_redirect`. And the `goal` argument the slow loop
forwards to `tp_token_match_memo` is dead there: the loop passes `memo = 0`, and
`tp_token_match_memo` uses `goal` *only* as a memo-probe key — the match itself
(`tp_token_run` / `tp_match_ir`) takes no goal at all. The baked scanner takes no goal argument in
its signature. So both scanners compute a span that is a pure function of `(input, position)`,
identical in the goal and no-goal cases. Whitespace is whitespace and a comment is a comment under
every goal; the goal only decides what the *next significant* token is.

The guard was therefore safe to drop. The change is exactly that:

```c
/* before */ if (state->extras_match != 0 && state->active_goal == TP_NO_SYMBOL) {
/* after  */ if (state->extras_match != 0) {
```

The per-position extras memo (`extras_memo.marks`) stays gated to the no-goal case — its
`memoable` flag is computed there and the baked branch's memo write is simply inert under a goal,
matching the slow loop's behaviour exactly. The lead-byte bitset reject and the memo are untouched
and still run alongside.

### A/B — callgrind, deterministic instruction counts

311 KB React file (`ReactDOMFloat-test.js`), generated `std.tsx` parser, 3 passes:

| variant                   | total Ir        | vs baseline |
|---------------------------|-----------------|-------------|
| baseline (W18-WORDCACHE)  | 546,772,019     | —           |
| **W18-EXTRAS**            | 451,775,969     | **−17.4%**  |

What moved (self Ir, 3 passes; copies merged):

| function                | baseline self      | after self        |
|-------------------------|--------------------|-------------------|
| `tp_token_run`          | 57.2M (10.46%)     | ~0 (below threshold) |
| `tp_token_match_memo`   | 36.3M (6.63%)      | ~0 (below threshold) |
| `tp_vm_consume_extras`  | 41.0M (7.49%)      | 25.3M (5.60%)     |

`tp_vm_consume_extras` inclusive falls from **26.71% to ~12.2%**. The `token_run` /
`token_match_memo` subtree it drove is gone — the baked scanner replaces the per-extra-def
bytecode loop with straight-line C.

### Throughput

Single 311 KB React file (`prof_tsx`, 500 reps, wall-clock, best of three):

| engine        | MB/s  |
|---------------|-------|
| baseline      | 15.3  |
| **W18-EXTRAS**| 17.8  |

A **~16% wall-clock speedup** on a real file, tracking the −17.4% instruction cut.

The whole-corpus bench5 number is flat (C-compiled 9.71 MB/s vs baseline ~9.87; VM 6.30 vs 6.26).
That is expected and honest: the corpus is 1070 mostly-small files where per-file `parser_new` /
`parser_free` setup dominates, so a per-byte lexer win amortises away — the same caveat the
profile recorded. The win lands on real files, which the single-file figure measures.

For the record at this run's tsc (typescript@6.0.3, 16.58 MB/s on the corpus): C-compiled is
9.71 MB/s = **0.59× tsc, 1.22× tree-sitter** on the whole corpus; on the single 311 KB file the
C-compiled/VM-shared engine reaches 17.8 MB/s, **past this tsc on that file**.

### Output-invariant

Pure speedup. A `--dump-tree` reference over the React TSX sample (171 files) plus crafted files
that exercise the exact trap — regex-vs-divide with interleaved comments, template literals with
substitutions and comments, JSX text with embedded expressions and ternary regex/divide — is
**byte-identical before and after** for both the vm-ax arm and the C-compiled arm
(md5 `5cd6a76…` unchanged). Every gate stays green with no golden re-baseline: cc-diff 271/271,
vm-diff, diag-diff, nesting, bufferfriendly, metric 296/296, import-c, no-statics, multiparser,
hide-ax; vm-nocrash and parser-nocrash under `ulimit -s 512`; perf-gate 3.5 ms within the 4.0 ms
ceiling.

## W18-PRATT — a precedence-climbing grammar primitive, built, proven byte-identical, declined at the thesis gate

The binary-operator precedence ladder is a stack of fifteen-odd `(hidden)` rules,
loosest at the top. Reaching any operand descends every rung as a real rule frame
(`tp_vm_enter`/`tp_vm_exit`) whether or not an operator is present, and the profile
put that descent at roughly a fifth of all instructions. This round built the
engine-level answer tree-sitter spells `prec.left`/`prec.right`: a
`(precedence-climb …)` grammar primitive that collapses the pure-binary rungs into
one Pratt loop, executed once per operand chain instead of N nested frames. The
primitive is correct and byte-identical. It is not shipped, because on this engine
the loop costs *more* instructions than the frames it removes. The negative result,
and why, is the deliverable.

### What the ladder actually builds

The first surprise was in the tree the rungs already build, measured with
`--dump-tree` before any change. The rungs are not the textbook recursive
`binary_expression` chain. Each is already left-factored:

```
(hidden (rule LEVEL
  (seq (field left: NEXT)
       (repeat (seq (field operator: OP) (field right: NEXT))))))
```

so the concrete tree has three properties a naive Pratt rewrite would break:

- **Each level is its own named node** — `additive_expression`,
  `multiplicative_expression`, `exponent_expression` — not a uniform
  `binary_expression`. A rung is `(hidden)` and promotes to a node named for the
  rule only when its `repeat` fires at least once.
- **A same-precedence run is flat.** `a + b + c` is one `additive_expression`
  with fields `left, operator:+, right:b, operator:+, right:c`. This holds even for
  right-associative `**`: `a ** b ** c` is one flat `exponent_expression`, not
  `a ** (b ** c)`. The CST encodes associativity by which level nests inside which,
  never by regrouping within a level.
- **A rung with no operator vanishes.** `a` alone is an `identifier`, with no rung
  node at all. The descent still pays a frame per rung to discover this.

A textbook precedence-climb builds uniformly nested `BinaryExpression` nodes
(`a-b-c` → `((a-b)-c)`), which would change the node names, the within-level
grouping, and every intermediate span. So the primitive had to reproduce the
ladder's exact grouping, not the canonical Pratt grouping. The whole value was
narrowed up front to one thing: collapse the frame-per-rung descent while emitting
the identical CST.

### The primitive and its driver

`(precedence-climb operand: NAME (level NODE PREC OPERATOR) …)` names the tightest
operand rule and one descriptor per level: the node it promotes to, its precedence
(higher binds tighter), and the operator pattern, which carries its own `(goal …)`
where division needs the `div` goal. It compiles to a new IR op
(`TP_OP_PRECEDENCE_CLIMB`) and a shared C driver, `tp_vm_prec_climb`, that both
engines call: the bytecode VM through a new `TP_VM_PRECEDENCE_CLIMB` dispatch arm,
the generated parser through one emitted call with the operand's `parse_<rule>`
function passed as a callback. One driver, so the cc-diff differential holds by
construction rather than by parallel maintenance.

The driver climbs precedence but groups its output to match the ladder: on an
operator of level *p* it opens one node named for *p*, re-stamps the operand as
`left:`, then runs a flat same-level loop appending `operator:`/`right:` pairs
while the next operator stays in level *p*, each right operand parsed at *p+1*
(strictly tighter — uniform across all levels, including `**`, because the measured
exponent tree is itself a flat repeat). Node open and close reuse the same
`tp_tree_builder_push` / `tp_vm_sink_close` / collapse helpers the rungs use, so
spans, child linking, and field stamping are the rungs' own code.

### Byte-identical: proven

The design carried a proof obligation for four cases — a single operator, mixed
precedence (`a + b * c`), a same-precedence left-associative run (`a - b - c`),
and right-associative `**` — plus the collapse case. All reproduce the ladder
exactly; the worked proof is in the design note. The implementation then earned the
claim empirically. A thesis slice replaced the additive and multiplicative rungs
with one `precedence-climb` over the exponent operand:

```
(hidden (rule additive_expression
  (precedence-climb operand: exponent_expression
    (level multiplicative_expression 11 (choice "*" "%" (goal div slash)))
    (level additive_expression       10 (choice "+" "-")))))
```

A `--dump-tree` reference over 131 React files (167,821 lines) is **byte-identical**
before and after on the vm-ax arm. Every gate passed with no golden re-baseline:
cc-diff 271/271 byte-identical (the generated-C climb matches the VM), metric
296/296. The primitive reproduces the ladder's tree across the corpus, in both
engines, down to the byte.

### The instruction count went up

On the 311 KB React file (`ReactDOMFloat-test.js`, 3 passes), against the current
post-W18-EXTRAS baseline:

| variant                          | total Ir        | vs baseline |
|----------------------------------|-----------------|-------------|
| baseline (W18-EXTRAS ladder)     | 451,776,574     | —           |
| W18-PRATT thesis slice (2 levels)| 481,704,930     | **+6.62%**  |

Where it moved (self Ir, 3 passes):

| function                | baseline self | climb self | delta |
|-------------------------|---------------|------------|------:|
| `tp_vm_enter`           | 37.24M        | 27.44M     | **−9.81M** |
| `tp_vm_exit`            | 49.24M        | 49.93M     | +0.70M |
| `tp_vm_consume_extras`  | 23.20M        | 28.29M     | **+5.09M** |
| `tp_tree_builder_push`  | 46.53M        | 48.21M     | +1.68M |
| climb driver + operator matcher (new) | 0 | ~9.3M | **+9.3M** |

The descent saving is real: collapsing two rung frames per operand cut `tp_vm_enter`
by 9.81M, the largest single move. It is also the *only* saving, and three new costs
swamp it. The driver peeks operators speculatively — at every operand it tries each
level's operator pattern and rolls back the miss — so `tp_vm_climb_match_operator`
runs **84,834 times**, far more than the ladder's ~5,200 per rung, and each attempt
consumes extras, which is the +5.09M on `tp_vm_consume_extras`. Moving a level's
children from the caller's sink into its node, plus the per-chain climb bookkeeping,
is the +9.3M of new self cost.

### Why the engine resists it

The profile blamed the descent frames, and removing them does help. But this engine
already paid for the rungs cheaply in the two places a Pratt loop cannot. A rung's
operator check is a lead-byte bitset reject inside `tp_vm_consume_extras` — one
probe, no node built, no rollback — so the common operand-with-no-operator costs
almost nothing per rung. And each rung is already a flat `repeat`, not recursion, so
a same-level run never nested in the first place. The ladder's expensive part was
the frame *enter/exit*, which W17-FLAT and W17-RECLAIM had already stripped down to
near the metal.

A precedence-climb trades those N cheap rejections for one loop with O(levels)
speculative matching per operand, and on a left-factored ladder with a cheap
lead-byte reject, that trade loses. A lead-byte filter on the peek could claw some
of it back, but the ceiling is the arithmetic: two levels can recover at most ~2% of
instructions (the binary slice is ~10.8% across all eleven levels), and the driver's
per-operand overhead already exceeds that. More levels would amortise the fixed climb
setup, but the peek cost scales with level count too, so the overhead grows with the
saving. The win does not arrive by adding rungs.

### Decision

Declined at the thesis gate, by its own rule: a slice that does not reduce
instructions stops the round. The primitive is correct and the byte-identical proof
holds, so the engine and grammar are reverted to the ladder rather than shipped with
a regression — never trade output or a green gate for a number, and here the number
went the wrong way. The reusable finding: on an engine whose rung frames are already
this thin and whose per-rung operator rejection is already a single bitset probe, the
operator ladder is not the instruction sink the frame-attribution suggested, and a
Pratt loop is the wrong tool for it. The standing highest-value target named by the
profile — the trivia/extras path — remains the one to attack.

## W18-PARITY — the persistent-engine corpus number, re-validated on the fixed engine

The W18 throughput study found that the React corpus shortfall against tsc was not a
parse-speed deficit but a measurement choice: reopening the engine and reloading the
grammar per file. Reuse one engine across the corpus and the same timed region jumped
from ~10 MB/s to ~15.7 MB/s, which the study called parity with tsc. That number was
measured on the engine that **silently corrupted reused parses** — reuse across an arena
grow collapsed trees to a single node while the API reported success — so it may have
been measured over fast-but-wrong parses, and could not be trusted. With the corruption
fixed (`86c4317`), this round re-validates the figure honestly and lands the
persistent-engine arm in the benchmark with a guard so a corrupt parse can never be
timed silently again.

### The two arms, and why both belong

The benchmark measures the corpus two ways, both timing only `push` + `run`:

- **fresh-per-file (RSS-bounded).** Each file gets a new engine, opened and grammar-loaded
  off the meter, then closed before the next. Peak resident memory stays bounded to one
  file's tree. This is the historical bench5 policy — a fair memory-isolation choice, and
  the right number when the question is peak footprint.
- **persistent-engine (whole-codebase).** One engine, grammar loaded once, reused across
  the corpus through the public API: `tinyparse_parser_new` per file, then
  `tinyparse_parser_free`, which resets the parser's child arena so each parse's memory
  returns to the engine pool for the next. This is how a real host parses a codebase, and
  the faster, like-for-like figure — no per-file grammar reload runs underneath the timed
  region.

The persistent pattern is the one `parse.c` was built for: the parser struct lives in the
engine arena, the parse data in a child arena sharing the engine's pool, so a whole
codebase parses in one engine without accreting every tree. Its cost is that peak RSS is
no longer bounded to one file (it high-waters at the largest tree). The fresh arm keeps
that bound; the persistent arm trades it for throughput.

### The byte-identical assertion

Before timing, the harness asserts the two arms build the same tree. For every file it
fingerprints the fresh-arm tree and the persistent-arm tree — node count plus an FNV-1a
hash folding each node's kind, field, and byte span in pre-order — and aborts the bench on
any divergence. A persistent parse that collapsed to a degenerate tree, dropped a chunk, or
diverged in any span turns the assertion red and stops the run before a number is reported.
The standing oracle for the fix is `ci/tinyparse-reuse-regression.sh`; this assertion is
the bench's own in-line guard, so neither path can ever measure corrupt parses again. It
passes for all 1070 files.

### The re-validated numbers

Same host, same run, median of 30 (in-process) / 15 (tsc/tsgo/meriyah); 1070 files,
9,546,659 bytes. Intel Xeon @ 2.80 GHz (4 cores), 15.7 GiB, Linux 6.18.5 x86_64.

| arm (parse-only)                  | MB/s   | vs tsc | vs tree-sitter | peak RSS |
|-----------------------------------|--------|--------|----------------|----------|
| persistent-engine, whole-codebase | 16.48  | 1.13×  | 2.03×          | 158 MB   |
| fresh-per-file, RSS-bounded       | 11.04  | 0.76×  | 1.36×          | 103 MB   |
| tsc (typescript@6.0.3)            | 14.60  | 1.00×  | 1.80×          | —        |
| tree-sitter (TSX, GLR)            | 8.11   | 0.56×  | 1.00×          | 22 MB    |
| tinyparse (bytecode VM)         | 6.16   | 0.42×  | 0.76×          | 50 MB    |

On correct parses the persistent engine is past tsc on the whole corpus, 1.13×, and twice
tree-sitter. The fresh arm trails tsc at 0.76×; the gap between the two arms is the entire
corpus shortfall, and it is the per-file engine-reopen / grammar-reload artifact, not parse
speed.

### Honest against the buggy number

The buggy study reported "~15.7 MB/s = 0.99× tsc" with tsc measured at 15.9 that run. The
fixed engine measures **16.48 MB/s** of correct parses against a tsc of 14.60 this run, so
1.13×. The raw throughput is at least as high as the buggy figure: correct parses are not
slower here, and the ratio shifted because this run's tsc was slower, not because the engine
regressed. The honest reading is parity-or-better with tsc on the corpus, past it on every
single-file size, and short of the native-Go and JS-in-JS parsers (tsgo and meriyah, ~37
MB/s) by a wide margin.

### Where this leaves tinyparse, and the one structural lever left

On the fixed engine tinyparse is no longer behind tsc on the corpus when driven the way a
host actually parses a codebase, and it is ahead of tsc on every file size measured. The
remaining distance is to the table-and-DFA parsers. Closing it would mean a generated DFA
lexer in place of the per-byte trivia scanner — the token layer tsc's JIT and tree-sitter's
table both have and tinyparse does not. That is a separate project: a maximal-munch DFA is
unsafe to bake naively from the grammar's terminals, and it is recorded here as future work,
not committed. The persistent-engine arm and its assertion are the deliverable of this round;
the lexer architecture is the next one's question.
