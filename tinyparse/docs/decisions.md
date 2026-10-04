# tinyparse — hard decisions log

A running record of load-bearing decisions and their rationale, so they are not
re-litigated. Newest at the bottom of each section.

## Grammar / language
- **extends collisions require explicit `(override …)`** — new names merge; a
  bare redefinition of a base name is a compile error. Prevents a typo silently
  shadowing a base rule.
- **Conformance is accept-level, not tree-equality** — JS/TS ports count a
  corpus case as covered when `run` accepts it with no ERROR/MISSING, not by
  matching tree-sitter's exact S-expression. Full tree-equality is future work.
- **The malformed `destructuring__5` corpus case asserts rejection** — its
  tree-sitter reference is itself an error-recovery tree; tinyparse rejects it
  (exit non-zero). Full error-recovery-tree parity is separate future work.
- **Language injection uses an inline sub-VM, not a child ax actor (for now)** —
  suspending/resuming the host VM across `ax_step` to await a child outcome is
  large; the inline sub-VM yields the identical composite, isolated, offset-
  mapped, tagged tree. Promoting to a spawned child actor touches only inject.c.

## Engine / performance
- **Phase 10 stopped at ~4.6× tree-sitter on the recursive descent** — interning
  pre-resolved, token-match memo + first-byte dispatch, first-set prediction.
  Going below needed a different engine (the VM).
- **Phase 11 topology: a bytecode VM managed by ONE ax actor** — not
  fine-grained statecharts (genuine but 5–15× slower) and not one actor per
  rule. The dispatch loop runs in one transition action; ax sees only
  `input.chunk`/`input.end`. Compiler stays plain C for now (W58 deferred).
- **PEG backtracking lives in an `ax_data` stack, not ax's configuration** — ax
  cannot rewind its active configuration, so choice/commit/fail manage explicit
  marks (cursor, builder, sink) in actor data.
- **The ax VM is the library default; `tp_execute` is the opt-out** via
  `TINYPARSE_ENGINE=recursive`, kept as the byte-identical differential oracle.

## Open issues / follow-ups
- **Flaky `tsfamily/declarations__18`** — intermittent (~1 in 5+), engine-
  consistent (recursive and vm-ax agree), so it is nondeterminism in a SHARED
  path (matcher/memo/first-set/tree-builder), not a VM bug. Steady-state green.
  To investigate: reproduce in a loop, diagnose for uninitialized memory.
- **W58 compiler-as-actor-pipeline** — optional; compilation is one-time, so the
  actor dataflow is organizational, not a perf lever.

## Autonomous project queue (overnight, owner-directed)
- **Project A — no statics, growable arenas** (Phase 12) — DONE (W65, 3dc83f1).
  One remaining mutable static (cli.c's module-resolver context) moved to the
  heap; the freestanding library holds none. The fixed caps (nodetypes field/
  type/children [64], token.c FIRST-set stack[64], cli.c path char[4096], bench
  block registry) are now arena-grown. `ci/tinyparse-no-statics.sh` guards the
  rule (ast-grep, wired into the metric). New tests prove no truncation past the
  old caps. Byte-identical (vm-diff 289/289), API frozen, no features cut.
  - **`static const` ROM and `static` functions stay** — the guard exempts both;
    the rule is "no mutable global state," not "no `static` keyword."
  - **`cli.c` may use libc (malloc/realloc)** — it is the hosted CLI, not the
    freestanding library; the no-statics rule still applies to it (caught the bug)
    but the heap is fair game there, matching the rest of the CLI's leak-to-exit.
- **Project B — real benchmark + VM optimization** (Phase 13) — IN PROGRESS.
  - **React benchmark corpus = the files std.tsx parses cleanly** (1070 of 1482;
    Flow ≠ TS, so ~28% are skipped). Both engines parse the EXACT same set, so it
    is a fair like-for-like subset of a real codebase. Vendored React is pinned +
    gitignored reference data (W66).
  - **The benchmark parses each file in a fresh, immediately-closed engine.**
    tinyparse's arena does not reclaim on parser_free, so a whole-codebase
    session accretes every tree; per-file engines bound peak memory to one file.
    The per-file engine open + grammar load is OFF the meter (only the parse/walk
    is timed), matching the methodology. tree-sitter reuses one parser, timing only
    the parse, symmetrically.
  - **`pack` resolves packages (W66)** so `pack std.tsx` packs the fully-resolved
    grammar; the benchmark loads that .tpg via tinyparse_load_binary_grammar.
  - **Root cause of the gap was catastrophic PEG backtracking, not slow code.**
    The profile-guided wins (token-index O(1), W66) were minor; the React files
    exposed an exponential blowup (a 3.7 KB file: 279 ms / 100 MiB; worst ~1 GB /
    5.7 s) from re-parsing and re-building sub-derivations on every ordered-choice
    retry.
  - **The fix is rule memoization (packrat), in two parts, VM-only.** (a) FAILURE
    memo (W67): cache "rule fails here" so doomed alternatives are not re-descended
    -- safe, no nodes. (b) SUCCESS memo (W67+): cache the built subtree as a
    self-contained SNAPSHOT and, on a later attempt, copy it in as FRESH nodes
    (never share -- a shared node's parent/sibling links, which the caller mutates,
    would corrupt goto_parent). Guards: keyed on (program, offset, goal, field,
    alias); cached only when recover==0, no_extras==0, and the body emitted no
    diagnostic (so a replay reproduces no missing side effect).
  - **Snapshot only on the 2nd attempt; the memo is ADAPTIVE.** A clean parse
    touches each key once and must pay nothing, so the first success is only marked
    "seen" and the memo stays dormant until a parse exceeds 32x input-length
    rule-call attempts (proof of super-linearity). This keeps the JS perf gate
    green (11.9 ms) while killing the React blowup. TINYPARSE_RULEMEMO forces it
    on/off; vm-diff runs a forced-on pass so the replay path stays byte-identical.
  - **≤1.5× was not reached on the React workload; integrity-first stands.**
    Measured (1070 files, 9.5 MB, ax-hosted VM): parse is 39x tree-sitter with the
    memo adaptive (the default) and 11.7x with TINYPARSE_RULEMEMO=always (1.1s
    tree-sitter; 43s / 13s tinyparse). The COLLECT/traversal pass is 2.8x FASTER
    than tree-sitter (82ms vs 229ms) -- the contiguous-array cursor wins, as on the
    JS corpus. So the exponential blowup is gone (bounded memory, no OOM) and
    traversal leads, but raw parse stays structurally slower: matching
    tree-sitter's pre-generated GLR tables to 1.5x would mean generating parse
    tables (its design, not tinyparse's runtime-compiled one). Reported as
    measured, not tuned to a number.
  - **The perf ratchet only goes down, so the memo ships ADAPTIVE by default.**
    Forcing it always-on regresses the JS micro-corpus gate (the dormant-until-
    super-linear default keeps it at ~11.9ms). Real-code consumers can opt into the
    3.3x-faster always-on mode with TINYPARSE_RULEMEMO=always.

## Exploration: grammar shape, tsc/tsgo, PEG-vs-GLR, parser_free (DONE)

Run before any GLR decision (docs/archive/explore-plan.md, docs/peg-vs-glr.md). Hard
decisions that came out of it:

- **The React blowup is grammar shape, fixable at the source.** Left-factoring the
  JS `assignment_expression` ladder (parse the operand once, then an optional
  assignment tail) cut the worst files 8-16x with the memo OFF, byte-identical,
  conformance 296/296. Complementary to the W67 memo: factoring removes re-work
  where we can express it, the memo is the general safety net.
- **PEG is the right paradigm; the gap is interpretation, not the algorithm.**
  Measured parse-only over the React corpus: tsgo 29.2 MB/s, tsc 12.5, tree-sitter
  (GLR) 8.3, tinyparse 0.28. The FASTEST is recursive descent (tsgo), 3.7x faster
  than GLR. tinyparse already picked the fast family; it is slow because it
  INTERPRETS an IR where tsc/tsgo are compiled.
- **Do NOT build GLR for speed.** It is the slowest production parser here, breaks
  byte-identity (different trees, precedence/table conflict model), and needs a
  token-driven GSS driver. The high-leverage path is a compiled + left-factored +
  memoized PEG. GLR is parked unless incremental re-parse becomes a goal.
- **parser_free reclaims via two new ax public primitives** — `ax_arena_child`
  (a child arena on a shared pool) and `ax_arena_reset` (recycle chunks to the
  pool), exposing machinery the core already used for actor heaps and the step
  arena. The parser struct stays in the engine arena (stable for the actor
  back-pointer); the parse data + tree live in a per-parser child reset on free.
  - **Tree lifetime tightened to "valid until parser_free"** (the header never
    promised otherwise; nothing in-repo relied on it). Copy out what you need first.
  - **vm_program must be cached on the grammar's arena, not the parser's** — a
    per-parser arena is reset, so a cross-parse cache pointing into it would dangle
    (the bug ci/tinyparse-reclaim.sh caught). It proves the fix: a 264 KB file
    parsed 300x in one engine peaks identically to 10x.
- **Perf ratchet 12 -> 10 ms.** Left-factoring + per-parser arena locality dropped
  the JS-corpus parse to ~9 ms (2.9x tree-sitter, was 4.2x).

## Phase 14: compiled PEG — left-factor, one engine, C codegen, 5-way bench

- **W1: left-factored the JS/TS grammars at the source.** Beyond the exploration's
  `assignment_expression`, factored `ternary_expression` (the dominant nested
  re-parser — 310-470x on deeply nested inputs, memo OFF), `export_statement`,
  `method_head`, `arrow_function` (JS) and `export_statement`, `arrow_function`
  (TS). The method/traps/verification bar are captured in the `left-factor` skill
  (tools/tinyparse/.claude/skills/left-factor/). Each verified 296/296 +
  vm-diff before commit. Deliberately left: `for_statement` in/of-vs-classic
  (structurally distinct headers, off the hot path, factoring risks an accept
  change), pattern/import name-vs-alias (single token, off hot path), and
  first-set-disjoint choices (predicted, never re-parse).
- **W2: the bytecode VM is the sole production engine.** inject.c's sub-parse moved
  off `tp_execute` onto `tp_execute_vm` (an inline sub-VM, no nested actor,
  byte-identical). After W2 no production path uses `tp_execute`; it survives only
  as the differential oracle + the `TINYPARSE_ENGINE=recursive` opt-out.
- **`tp_execute` deletion is sequenced LAST (W5), not at W2.** It is the
  independent differential oracle; deleting it before W3's C-compiled parser exists
  would leave the engine with no independent cross-check (only the conformance
  suite). So W2 removes it from production paths; W5 deletes it once the
  C-compiled parser is the new VM-vs-C oracle.
- **W3 plan: `tinyparse c` will emit recursive-descent C** — one C function per
  rule, ordered choice as if/return, over the existing token matcher + tree
  builder, self-contained (no libtinyparse link). This is the speed lever the
  exploration identified (compile the PEG, do not interpret it) and the embedding
  deliverable. A new ci/tinyparse-cc-diff.sh asserts the generated parser is
  byte-identical to the VM — becoming the independent oracle.

### W3 design: compile the rule structure, reuse the runtime
- **The generated C emits one function per RULE (native recursive descent), but
  calls the existing runtime primitives** — the token matcher, the contiguous tree
  builder, extras, the cursor. The interpretation cost the exploration measured was
  the rule-level dispatch (tp_vm_run walking the lowered program); compiling THAT to
  C functions removes it. Compiling the token matcher too is a later step, not this
  one. "Self-contained / no libtinyparse link" = ship the amalgamation + the
  generated rule functions; the rule logic is native C, the runtime is shared.
- **Byte-identical to the VM is the bar**, enforced by ci/tinyparse-cc-diff.sh
  (generate -> compile -> `--dump-tree` == the VM's, over the corpus). That gate is
  the new independent oracle that lets W5 delete tp_execute.
- **New flag `tinyparse c --rd`** (TINYPARSE_EMIT_RD), recorded as a deliberate
  CLI/emit-API addition.

### W3 done — the compiled RD backend ships, byte-identical
- **`TINYPARSE_EMIT_RD` is the one permitted public-header addition** — a single
  enum value (`0x2u`) alongside `TINYPARSE_EMIT_BUNDLE`, driven by `tinyparse c
  --rd`. The emitter lives in `src/emitrd.c` (freestanding, two-pass cursor like
  emit.c, no static mutable state); `tinyparse_emit_c` branches to it on the flag.
- **`command_emit` now resolves grammars exactly like `check`/`run`** — package
  dir, plain file, or embedded name, with the module resolver installed — so a
  grammar that `extends` a base (std.typescript, std.tsx) compiles. Previously `c`
  read a path only and failed on extends with "requires a module resolver".
- **The generated RD parser reuses the VM runtime, it does not reimplement it.**
  vm.c's helpers + `tp_vm_state` moved to internal.h; the VM's `tp_vm_run_call`
  delegates to a factored `tp_vm_run_call_body(... body callback ...)` that the
  generated code also calls, so CALL/rule-memo semantics have ONE source of truth.
  The emitter walks the SAME lowered program (`tp_compile_program`) and emits, per
  instruction, the C its `tp_vm_run` case would execute — that is what makes it
  byte-identical, proven by `ci/tinyparse-cc-diff.sh` (the new independent oracle).
- **The tp_ir pattern pointers are not baked; the parser re-lowers at startup.** A
  MATCH/BYTE/RECOVER/INJECT carries a runtime tp_ir address, so the generated file
  loads the embedded packed grammar and reads operands from `program->instructions[N]`;
  indices line up because the lowering is deterministic. Only the rule STRUCTURE is
  compiled to C; the pattern operands ride along in the lowered program.
- **cc-diff-green: json/jsonc/json5/family-base/javascript/typescript/tsx, 267/267
  byte-identical.** Every corpus construct is covered (incl. GOAL, NO_EXTRAS,
  RECOVER, INJECT, the rule memo). Standing gates green at the commit.

### W3 corrections (owner directives) — rename + make the generated parser iterative
- **No cryptic names in the final product.** `emitrd.c` / `TINYPARSE_EMIT_RD` /
  `--rd` / `tp_rd_*` were cryptic ("rd" = recursive descent). Rename to spelled-out
  names — the emitter is `emitparser.c`, the flag is `--parser`
  (`TINYPARSE_EMIT_PARSER`), generated symbols `tp_parser_*`. Per GUIDELINES:
  spell symbols out. (Cryptic names are tolerable only in throwaway scratch.)
- **The generated parser MUST be iterative, not natively recursive.** The first
  W3 cut emitted one C function per rule that call each other — native C recursion
  proportional to input NESTING DEPTH, so a deeply nested (possibly malformed)
  input overflows the C stack and crashes the host. Not tenable in production.
  Convert the generated parser to the iterative transformation: an EXPLICIT frame
  stack (the work/continuation stack), driven by a loop, with the grammar baked
  into the dispatch. Bound by the stack, which lives in an ARENA and grows — never
  the fixed C stack. Lifetimes are known (the frame stack is per-parse), so use the
  parse arena to keep it fast. cc-diff (byte-identity to the VM) stays the bar.
- **Known follow-up: the bytecode VM itself C-recurses per rule call** (tp_vm_run_call),
  so it has the same deep-nesting crash exposure as the first generated cut. The
  owner's directive named the generated parser; making the VM's dispatch iterative
  too is the natural next step (tracked, not done in W3).
- **The generated parser is macro-free and crash-bounded (W14-3 close-out).** The
  emitter's five dispatch macros (TP_PARSER_TOP/RESOLVE/PUSH/DESCEND in the loop,
  TP_PARSER_GRANT in main) are now inlined at their use sites in the emitted
  strings — the generated output contains zero `#define`/`#undef` (it is already
  generated, so the macros are expanded in the generation phase), byte-identical
  to the VM (cc-diff 267/267). The no-crash guarantee is the arena bound, verified
  ONCE with a tool, not a depth search: ci/tinyparse-parser-nocrash.sh runs a
  ~200k-deep std.json input under `ulimit -s 512` and asserts a clean exit (code
  < 128). Cross-check: the recursive VM segfaults (139) on the same input under
  the same small stack, so the test is meaningful, not a no-op.

### W6 scheduled (owner directive): make the bytecode VM iterative
- **A bytecode VM that C-recurses defeats the point of bytecode.** tp_vm_run /
  tp_vm_run_call recurse per rule call, so deep input overflows the C stack — the
  VM segfaults under `ulimit -s 512` where the W3 compiled parser (iterative,
  arena-backed) survives. W6 drives tp_execute_vm with the same explicit
  arena-backed frame stack (reuse the W3 pattern — likely one shared iterative
  driver). Bounds depth by the arena, not the C stack.
- **Tail-call analysis is part of W6 (owner directive).** The W6 agent must run an
  analysis pass over the REAL grammars (js/ts/tsx/json) to count tail-position rule
  calls (the right-recursive operator ladders especially), then, leveraging the
  fact that we already lower to bytecode, design an instruction-set extension
  (TAILCALL op or a tail flag on CALL) so the iterative driver REUSES the current
  frame for a tail call instead of pushing. Design from the measured evidence, not
  a guess.
- **Sequenced after W4/W5** in the autonomous flow; verified byte-identical
  (vm-diff + cc-diff) + a vm-nocrash gate proving the VM survives deep input.

### W4 done — the five-way benchmark, and the honest C-vs-VM verdict
- **One new permitted public-header addition: `TINYPARSE_EMIT_PARSER_LIB`.** The
  five-way bench needs the C-compiled parser driven IN-PROCESS (a subprocess per
  file would time process startup, not parsing). The existing `--parser` emits a
  standalone program with `main()` reading stdin; there was no callable entry. Added
  the flag (CLI `--parser-lib`, env-free) that emits the SAME compiled parser
  without `main()`/dump and instead exposes `tp_compiled_parser_load(engine,
  &grammar)` + `tp_compiled_parser_run(parser)`. Same family as W3's
  `TINYPARSE_EMIT_PARSER`; the bundle path is untouched (cc-diff still 267/267).
  The emitter stays freestanding and holds no static mutable state (no-statics
  GREEN). Chose a flag over a separate command to keep one emit codepath.
- **The C-compiled arm is measured exactly like the VM arm.** `bench/react_compiled.c`
  is `react_tinyparse.c`'s twin: per-file fresh engine, only the parse call timed,
  grammar load off the meter. It loads the grammar through the generated loader (so
  the baked instruction indices line up) and times `tp_compiled_parser_run`.
- **Verdict (W14-4, honest): C-compiled is NOT faster than the VM — it is ~11%
  SLOWER** (2.71 vs 3.01 MB/s over the 1070-file React corpus). Recorded as-is in
  `docs/performance.md`. The cause is structural and expected: the "compiled" parser
  still reads the lowered program at runtime through one generic dispatch loop
  (that is what keeps it byte-identical to the VM), trading per-op interpreter
  dispatch for near-identical per-frame dispatch plus arena push/pop overhead. Real
  speedups would need the emitter to specialize each rule into straight-line C with
  operands baked as constants — future work, not claimed here.

### W6 done — the bytecode VM is iterative; tail calls measured and exploited
- **The VM now drives parsing through ONE iterative driver, `tp_vm_run` (vm.c).**
  The recursive `tp_vm_run`/`tp_vm_run_call` are gone: a root frame is pushed onto
  an explicit, arena-backed frame stack (`tp_vm_stack`/`tp_vm_frame` in internal.h)
  and a single loop pumps it (RESOLVE-and-pop or DESCEND-and-push). No C recursion
  across rule calls, so a ~200k-deep input grows the arena instead of overflowing
  the C stack. The CALL prologue/epilogue were factored into `tp_vm_call_enter`/
  `tp_vm_call_exit`. The emitter (emitparser.c) stopped emitting its own copy of
  the loop and now CALLS the shared `tp_vm_run`, so the VM and every compiled
  parser run the identical loop (one source of truth; cc-diff stays 267/267).
- **New gate `ci/tinyparse-vm-nocrash.sh`.** The VM (vm and vm-ax) survives a
  ~200k-deep input under `ulimit -s 512` (graceful exit 1, no crash signal); it
  cross-checks that the recursive reference still segfaults (139) on the same input
  under the same stack, so the test is meaningful. Verified the VM crashed (139)
  on this input BEFORE the change.
- **Tail-call analysis (owner directive) — done and reproducible.** `tinyparse
  analyze-tailcalls <grammar>` lowers and counts tail-position CALLs (marked by
  `tp_mark_tail_calls` in vmcompile.c, a `tail` flag on the CALL instruction).
  Measured: std.javascript 52/122 (42.6%), std.typescript 97/193 (50.3%),
  std.tsx 96/192 (50.0%), std.json/json5/jsonc 2/4 (50.0%). Tail calls are common,
  not rare — so the bytecode exploits them.
- **The exploitation is FIELD/ALIAS frame reuse, chosen from the measurement.** A
  CALL builds a tree node, so its epilogue is irreducible and reusing the CALL
  frame would change the tree (vm-diff forbids it). The depth-reducing reuse is the
  pure pass-through wrappers (FIELD/ALIAS) that only decorate and forward: the
  driver rewrites the wrapper frame in place for its child instead of keeping a
  do-nothing continuation. Measured (TP_VM_PROBE_DEPTH, trees byte-identical): the
  right-recursive assignment ladder drops peak frame depth 18077->15058 (-17%) and
  pushes 429298->327231 (-24%); deep arrays keep peak depth (irreducible CALL
  chain) but trim pushes 26005->22005 (-15%). Full numbers + the per-grammar table
  in docs/peg-vs-glr.md (W14-6).

### W6 before W5 (ordering correction)
- Do W6 (make the VM iterative) BEFORE W5 (delete tp_execute). tp_execute is the
  INDEPENDENT recursive oracle vm-diff checks the VM against; W6 rewrites the VM's
  control flow, which is exactly when that independent cross-check matters most.
  Keep tp_execute through W6, delete it in W5 as the final teardown. After W5 the
  standing anchor is the conformance suite (296 independent expected outputs);
  cc-diff/vm-diff become host-consistency checks (they share runtime logic).

### W5 done — the IR interpreter is gone; the bytecode VM is the engine
- **tp_execute and its entire scaffolding were deleted from src/parse.c (W14-5).**
  The recursive-descent IR interpreter and every helper used only by it (tp_parse
  and the combinator drivers tp_parse_rule_ref / tp_parse_terminal, its tp_ctx /
  tp_sink / tp_inject_sink, tp_sink_push / tp_inject_sink_append, tp_find_token /
  tp_find_rule / tp_goal_redirect, tp_consume_extras, tp_recover_insert,
  tp_link_children, tp_trace_symbol, tp_freeze_tree, and the TP_DEBUG_INTERN_GUARD
  snapshot) were removed. parse.c now holds ONLY the parser front end (buffer,
  new/free, push, finish). Nothing was kept-because-shared: the VM (vm.c) carries
  its OWN copies of every one of those helpers (tp_vm_*), so deleting parse.c's
  static set left vm.c / token.c / tree.c / inject.c compiling and linking
  unchanged. `grep -rn 'tp_execute\b' src` now finds only tp_execute_vm /
  tp_execute_vm_actor.
- **The engine is the iterative bytecode VM — in process and actor-hosted.**
  tinyparse_finish prefers the actor-hosted VM (tp_execute_vm_actor, the library
  default) and falls back to the in-process VM (tp_execute_vm) when no ax system
  could be opened; tp_execute_vm always works, so there is no further fallback. The
  CLI's TINYPARSE_ENGINE=recursive selector can no longer name a deleted executor:
  it is accepted as a deprecated alias for the in-process VM (=vm) so old
  invocations still pick a real engine.
- **The intern-guard tripwire moved to tp_execute_vm.** TP_DEBUG_INTERN_GUARD now
  snapshots tp_intern_call_count on entry to tp_execute_vm and asserts it has not
  moved at each return, so the W44 "no interning on the parse hot path" check still
  compiles and holds on the engine.
- **The standing correctness anchor is the conformance suite.** With the
  independent recursive oracle gone, ci/tinyparse-metric.sh (296 independent
  expected outputs) is THE anchor. vm-diff was repointed from "vm/vm-ax vs
  recursive tp_execute" to a host-consistency check (in-process vm vs actor-hosted
  vm-ax, reference held at =vm), keeping the rule-memo-forced pass; cc-diff (the
  C-compiled parser vs the VM, 267/267) remains the other consistency check. The
  vm-nocrash inverse proof (the recursive interpreter segfaulting under ulimit -s
  512) is no longer runnable — its evidence is recorded here (W14-6) and the bound
  is unchanged; the positive VM-survives assertion stays the live gate.
- **Perf ratchet 10ms -> 7ms.** The iterative bytecode VM parses the JS corpus in
  ~5.5ms (1.7x tree-sitter); the ceiling in test/perf-baseline.txt drops to 0.007
  to lock the gain in (the ratchet only ever goes down), verified passing twice.
- **W15-1: `ax_alloc_raw` + a `live` occupancy column kill the per-parse memo
  zeroing.** `ax_alloc` zeroes every block; the token-match and rule memos are
  sized to the input (~768 KB each on a 6 KB file) and were zeroed every parse —
  the largest cost in the profile (memset 48.5% on a single representative React
  file, 22.5% on the JS corpus). A new non-zeroing `ax_alloc_raw` (ax core,
  `src/ax/ax-design.md` + `-implementation.md`; `ax_alloc` is now it plus the
  memset) backs each memo's slot table, and a parallel one-byte-per-slot `live`
  array — the only thing zeroed — carries occupancy. The probe reads `live`, never
  the raw slot, so a slot's fields are read only after `live` proves it occupied:
  provably no uninitialised read (valgrind memcheck clean on both the token path
  and the rule-memo-forced replay path). The two redundant init loops that
  re-zeroed `ax_alloc`'d memory were deleted. Result: memset 48.5% → 6.0%, React
  VM throughput 2.78 → 3.20 MB/s (+15%), peak RSS −27%, trees byte-identical
  (cc-diff/vm-diff GREEN, metric 296/296). Prefer raw allocation of a
  fully-overwritten or self-validated buffer over a zeroing alloc whose result is
  discarded.
- **`tinyparse_memory.grow` migrated to ax's 3-arg shape
  `(user, need, size_t *got)`.** The ax core's `grow` had already gained the
  `*got` true-usable-size out-parameter (ax commit "grow reports its true block
  size"), but the consumed tinyparse dist and `tinyparse_memory.grow` had not
  followed, so a freshly tangled ax dist no longer compiled against `engine.c`'s
  grant bridge. Rebuilding the ax dist for `ax_alloc_raw` (W15-1) surfaced the
  latent drift. The grant is handed straight to the ax arena, so tinyparse's
  grow IS ax's grow; the public struct and every host `grow` callback (CLI,
  examples, benches) now take `size_t *got` and a host that returns exactly `need`
  sets `*got = need`. This realigns one source of truth rather than papering over
  the bridge with a pointer cast.
- **W15-2: the lexer compiles to a JUMP-THREADED token bytecode; the recursive
  `tp_match_ir` walk leaves the hot path.** Each token pattern lowers once
  (lazily, cached on the IR node like the first-byte set) to a flat threaded
  program — leaf ops, `SAVE`/`COMMIT`/`JMP` for ordered-possessive choice and
  optional/repeat tails, a fused `STAR` for the dominant repeat-of-a-leaf, a `MARK`
  + check for the `reserved` not-keyword wrapper — run by an iterative register
  loop (`tp_token_run`: one pc, one cursor, a fixed 64-deep save-stack). It
  replaces the `tp_match_ir` call inside `tp_token_match_memo`. Decisions made:
  - **Jump-threaded, NOT a per-node frame VM.** The first attempt was a generic
    frame stack mirroring the rule VM; it was byte-identical but ~17% SLOWER on the
    React profile (0.553 → 0.647 s) and +9% instructions, because the per-node
    step/resume state machine costs more than the recursion it replaced for shallow
    possessive patterns. The lesson — measured, not guessed — is that an iterative
    token matcher only wins as a threaded register loop where the linear spine
    (keyword/identifier/number) runs as straight-line code. The threaded form lands
    at 261M vs 272M instructions (−4%) and 0.548 s (on par to faster). The frame VM
    was discarded; only the threaded VM ships.
  - **`tp_match_ir` stays as a cold reference + fallback, off the hot path.** The
    lowering declines a `(not …)` guard (it would need an inverted-lookahead
    sub-evaluation that risks byte-identity), so the comment / string / regex-literal
    tokens keep matching with the recursive `tp_match_ir` — a byte-identical
    fallback, ~8% of the React profile, the only residual `tp_match_ir` cost.
    query.c (no parse position to retry) also keeps the recursive path. The brief
    sanctions this; the common tokens are all bytecode.
  - **The compiled C parser gets the lexer bytecode for free.** Since W14-6 the
    generated parser embeds the packed grammar, re-lowers it at startup and calls
    the shared library runtime — no token emitter. So compiling the lexer in the
    library compiles it for the generated parser too, with zero drift risk; cc-diff
    stays byte-identical (267/267) and the C-compiled React throughput rose 2.83 →
    3.14 MB/s (+11%). The VM holds at 3.22 MB/s.
  - **The token-program cache is keyed to `sizeof(tp_ir)`, which the .tpg ABI tag
    embeds.** Adding the cache fields to `tp_ir` changed `TP_PACK_ABI`, so a .tpg
    packed by an older binary is correctly rejected. No .tpg is committed (all are
    gitignored and regenerated by `pack`), so the bench/CI scripts that `pack`
    before use are unaffected; only stale local fixtures need re-packing.
  - **No-crash is iterative-by-construction, not an observed-crash fix.** The
    runtime matcher has no recursion; the fused `STAR` consumes an arbitrarily long
    token in constant C-stack space (the new `vm-nocrash` big-token case, a 400 KB
    number under `ulimit -s 512`, exits cleanly). The recursive `tp_match_ir`'s
    depth was always the fixed authored pattern nesting, never the input, so it was
    never an input-driven crash risk; a deeply nested *grammar* still recurses at
    compile-time lowering, identically to the existing rule lowering in vmcompile.c,
    and is out of scope for this input-hot-path workstream.
  - **Perf ratchet 7ms -> 6.5ms.** The threaded lexer measures ~5.5ms on the JS
    corpus (on par with the W14-5 baseline — those files are tiny, so the lexer win
    lands on real files); the ceiling drops to 0.0065 to lock the small gain in.
- **W15-3: the generated parser bakes the rule dispatch — it no longer calls
  `tp_vm_run`.** Through W14-6 the compiled parser (emitparser.c) CALLED the shared
  interpreter `tp_vm_run` (one loop, one source of truth), so rule dispatch was still
  interpreted: per step it fetched `state->program->instructions[fr->program]` and
  switched on the opcode. W15-3 emits `tp_parser_run` instead — the SAME explicit,
  arena-backed frame stack and the SAME leaf helpers, but the dispatch is
  `switch (fr->program)` over one baked case per program index, each the
  specialization of the matching `tp_vm_run` arm with every structural operand
  (target/start/count/min/max/symbol/aux/tail) inlined as a constant.
  - **Only the rule control flow is baked; the leaf helpers stay shared.** The CALL
    prologue/epilogue (`tp_vm_call_enter`/`exit`), the rule memo, the token matcher,
    the tree builder, extras, recovery and inject remain library functions the
    generated file pulls in through internal.h — so the memo verdicts and the tree
    build have ONE source of truth and the baked dispatch is byte-identical to the VM
    (cc-diff 267/267). The `tp_ir` pattern/source pointers a MATCH/BYTE/RECOVER/INJECT
    needs are runtime addresses, so those few operands are still read from
    `state->program->instructions[N]`; only the structural operands bake.
  - **Iterative and bounded, macro-free, tail-call-reusing.** Every combinator
    descends by pushing a frame on the arena stack (no C recursion across nesting), so
    a 200k-deep input grows the arena, not the C stack (parser-nocrash GREEN under
    `ulimit -s 512`). The emitted C carries zero `#define`/`#undef` — it is generated,
    so the loop is expanded at generation time. The W14-6 FIELD/ALIAS frame reuse is
    emitted per rule (rewrite the frame in place for the child). Tail-call analysis
    re-run (`analyze-tailcalls`): js 42.6%, ts 50.3%, tsx 50.0% tail CALLs — common,
    so the reuse matters; the baked parser inherits it.
  - **An `inline` frame-push helper is emitted locally.** The one cost baking ADDED
    was a cross-translation-unit call to the library `tp_vm_stack_push` (the per-descent
    hot path), which the interpreter inlined within its own TU. The generated file emits
    a byte-identical `static inline tp_parser_push` so the C compiler folds it back into
    the descent sites; with that, instruction count is neutral vs the interpreter.
  - **A leaf-fusion experiment was MEASURED and discarded.** Running leaf children
    (MATCH/BYTE/EOF, ~⅓ of instructions) in place inside the SEQ case to skip the
    frame round-trip was byte-identical but +6.7M Ir (the per-child leaf-op check cost
    more than the frame push it saved). Kept the clean one-case-per-program form.
  - **The goal (beat tree-sitter) is NOT met; recorded as an honest floor, not
    faked.** Measured (bench5, React corpus): baked C-compiled **3.23 MB/s** vs
    tree-sitter **6.79 MB/s** (~2.1x slower) and within noise of the VM (3.32 MB/s) —
    as the neutral instruction count predicts. Baking removes the opcode interpreter
    (the W3 deliverable + the embedding property: standalone C rule control flow) but
    not the per-node frame/memo/tree work, which the profile now spreads so no single
    slice is half the total. The residual gap is the design difference established in
    peg-vs-glr.md / above: tinyparse interprets a runtime-compiled PEG over a
    contiguous-array tree; tree-sitter runs pre-generated GLR tables + a DFA lexer.
    Closing it would mean generating parse tables — its design, not tinyparse's.
    `W15-FASTER` is deliberately NOT recorded in performance.md (the phase15 metric
    greps for it), because the win was not earned. The floor and its evidence are
    in docs/performance.md (`### W15-3`) and docs/peg-vs-glr.md (`## W15-3`).

## Phase 16 — parse like tsgo/meriyah (kickoff)

- **The Phase 15 floor was design-specific, not fundamental — overturned by
  reading the fast parsers.** We read tsc and tsgo (hand-written recursive descent)
  and meriyah (the fastest JS-in-JS parser). The shared recipe: tokens are integers
  in flat scanner state (no per-token allocation); scanning is a byte `switch` +
  `[256]` character-class tables with an ASCII identifier fast-path; ordered
  ambiguity is resolved predictively (O(1) tristate token peeks; meriyah rewrites an
  expression node in place via a `destructible` bitfield) so deep backtracking almost
  never fires; mark/rewind is ~6 scalars + truncate the node array; nodes are
  arena/bump-allocated with deferred parent links. Crucially, **all three use native
  call-stack recursion, not an explicit frame stack.**
- **Our profile says backtracking is NOT our problem and the per-node frame machine
  IS.** Left-factoring + first-set prediction already killed re-parsing on the
  React/TS corpus. The ~2.1x gap is structural overhead the fast parsers never pay:
  per-node frame push/pop + dispatch (~20%), `consume_extras` before every terminal
  (~8%), CALL prologue/epilogue (~8%), allocation/zeroing (~10%), per-choice runtime
  first-set prediction (~6%), with token match (~21%) reducible via table-driven
  scanning. Baking (W15-3) was instruction-neutral because it removed the opcode
  interpreter but not this model.
- **Decision (owner): relax the "explicit stack only" mandate for the GENERATED
  parser.** The explicit per-node stack is what forces the frame machine. The
  generated C parser will become a **depth-guarded native recursive-descent parser**
  — native C recursion with a configurable maximum nesting depth that returns a parse
  error (never crashes) on malformed/deep input. This is the tsc/tsgo/meriyah design
  and is genuinely crash-safe (verified under `ulimit -s 512`). The VM keeps its
  explicit-stack iterative engine: it stays the runtime-grammar engine and the
  cc-diff byte-identity reference.
- **Order (owner): cheap wins first, on BOTH engines, THEN the native-recursion
  rewrite.** W16-1 ports the design-independent lexer/extras/prediction/allocation
  wins to the VM and the generated C (which share the runtime), and adds meriyah +
  tsgo to the benchmark (meriyah is the bar: "if we can't get close to meriyah we're
  not in a good spot"). W16-2 does the native-recursion rewrite of the generated C.
- **Two packaging requirements (owner).** (W16-3) The generator must handle MULTIPLE
  parsers in one program — e.g. a host markup grammar embedding a fenced code block
  parsed by a second grammar — proving the emitted parsers are independent and
  non-colliding. (W16-4) The generated parser may use ax internally (freestanding
  library) but must NOT export ax symbols; the compiled object exposes only the
  parser's own API (`nm`: no `ax_` in the exported table).

### W16-1 done — the design-independent wins on both engines; meriyah is the bar
- **The cheap, design-independent slices fell ~29% on real files, both engines.**
  Profiled before/after (callgrind, a comment/string-heavy React file ×3, std.tsx):
  725.2 M → 516.5 M instructions. Three measured wins, each byte-identical (cc-diff
  267/267, vm-diff GREEN, metric 296/296), applied to the shared `token.c`/`vm.c`
  runtime so the VM and the generated C parser both benefit:
  - **Extras lead-byte fast path (the biggest, −12.5%).** `tp_vm_consume_extras`
    runs before every terminal; at almost every position the cursor byte begins no
    extra, yet it looped over all extras entries running a full token match for
    each. A 32-byte (`[256]`-bit) UNION of every extra's first-byte set is cached
    once per grammar (`tp_extras_lead_build`, like `vm_program`/`token_index`); a
    byte outside it is rejected with one bitset probe. Sound: an extra with an
    unpinnable lead byte sets `extras_lead_open` and the full loop runs.
  - **Raw (non-zeroing) hot allocations (−12%, the memset slice gone).** Extended
    the W15-1 `ax_alloc_raw` discipline to the per-CALL children sink struct (fully
    overwritten on the next line) and the geometric-growth buffers whose live prefix
    is copied and whose tail is written before any read (child-sink items, VM frame
    stack, tree-builder nodes). Verified valgrind-memcheck clean on the default AND
    the `TINYPARSE_RULEMEMO=always` replay path — no uninitialised read.
  - **Inlined choice predictor (−6%).** `tp_choice_first_admits` already cached its
    FIRST set per node (W46); the residual cost was the cross-TU call overhead
    dwarfing the cached bitset test. A `static inline tp_choice_first_admits_fast`
    in `internal.h` does the cached case in place and calls the full (mutating)
    function only on the cold first visit; both engines fold it in.
- **A table-driven `[256]` lexer was found ALREADY PRESENT, not re-derived.** The
  brief's sub-win 2 describes the pre-W15-2 lexer. Since W15-2 the dominant tokens
  (identifier/number/whitespace) run as a fused `STAR` loop over `tp_class_has` — a
  256-bit class-table probe, i.e. the table-driven ASCII fast path. The only
  residual recursive (`tp_match_ir`) lexer cost is the `not`-guard tokens
  (comments/strings/regex), which W15-2 deliberately declined to lower for
  byte-identity; that risk is unchanged and the cost is comment-density-dependent,
  so it was NOT taken as a "design-independent low-risk win." Recorded as a known
  residual for a later, separately-verified change.
- **meriyah added to the benchmark — the owner's bar.** `bench/meriyah_bench.mjs`
  (parseModule, jsx) installs into the gitignored `bench/tsbench` scratch and runs
  as a sixth arm in `ci/tinyparse-bench5.sh` (kept that filename so callers and
  the phase metrics still find it; its header/output now say "six parsers"). meriyah
  parses ECMAScript + JSX, not TypeScript, so it rejects the corpus's TS/Flow files
  (parses 541/1070); its MB/s is over the parsed subset, reported with the
  parsed/failed counts so the figure is honest, not a false byte-for-byte compare.
- **Result is honest: W16-1 alone does not beat tree-sitter.** Bench (1070 files,
  9.55 MB): tinyparse VM **3.32 → 3.55 MB/s** (+7%), C-compiled 3.23 → 3.25 (held
  — its bottleneck is the per-node frame machine W16-2 removes, not these slices),
  vs tree-sitter 6.56 and meriyah 25.52 (the target, ~7× the VM). Perf ratchet
  6.5 → 5.5 ms (JS corpus ~4.8 ms). The slices shrank and meriyah is now visible;
  the frame rewrite (W16-2) is where the structural gap closes.

### W16-2 done — the generated parser is a depth-guarded native recursive-descent parser
- **The generated parser dropped the explicit per-node frame stack for true
  predictive recursive descent.** Through W15-3 `tinyparse c --parser` emitted one
  `switch (fr->program)` loop pushing an arena frame per combinator; the profile put
  that frame machine at ~28% of instructions (the per-node dispatch loop
  `tp_parser_run`), the ~2.1× wall behind tree-sitter. The emitter (emitparser.c)
  now generates one C function per program index (`tp_parser_p<N>`), intra-rule
  structure as straight-line C (SEQ statements, CHOICE mark/try/rewind with the
  inlined first-set predictor, REPEAT `while`, OPTIONAL try-and-restore,
  FIELD/ALIAS tail calls, GOAL/NO_EXTRAS/RECOVER save/set/call/restore), and
  rule→rule as a NATIVE C call. No frame struct, no dispatch loop. The W15-3
  per-node frame dispatch loop is DELETED from the generated output, not reduced
  (each generated `tp_parser_p<N>` is <1% of instructions in the after-profile).
- **The CALL prologue/epilogue and leaf helpers stay shared (one source of truth).**
  A native CALL still calls the library `tp_vm_call_enter`/`tp_vm_call_exit` over a
  C-stack-local `tp_vm_frame`, and the token matcher / tree builder / extras /
  recovery / inject / rule memo remain the shared library functions. So the
  memo verdicts and the tree build are identical to the VM's, and cc-diff stays
  byte-identical (267/267) — the native parser builds the EXACT same tree as the VM
  on every corpus file.
- **The VM (tp_vm_run) is UNCHANGED.** It keeps the explicit-stack iterative engine
  and stays the runtime-grammar engine and the cc-diff byte-identity reference. The
  Phase-16 kickoff decision (relax the explicit-stack mandate for the GENERATED
  parser only; the VM keeps it) is honoured exactly.
- **Crash-safety is a MEASURED recursion-depth guard, not the arena bound.** Native
  recursion can overflow the C stack, so `tp_vm_state` gained `depth`/`depth_limit`:
  each rule-body entry (the CALL site) bumps `depth` and FAILs gracefully when it
  would exceed the limit. The VM leaves `depth_limit` at TP_NO_INDEX (no guard — it
  never recurses). The limit is **1000**, justified by measurement on this hardware
  (the profiling skill's bound-not-sweep rule):
  - per-CALL-depth C-stack cost ~200 B (std.json) to ~273 B (std.tsx, the
    worst-framed shipped grammar);
  - deepest CALL-depth any REAL corpus file reaches = **313** (a React test file,
    std.tsx; conformance fixtures are far shallower);
  - C-stack overflow point under `ulimit -s 512` = **~1920 CALL-depth** (std.tsx,
    ~273 B × 1920 ≈ 512 KiB).
  1000 is ~3× above the deepest real input (313) — so no valid file false-bails and
  cc-diff/conformance stay byte-identical — and ~2× below the 512 KiB overflow point
  (1000 × 273 B ≈ 273 KiB ≪ 512 KiB) — so a pathological deep input bails
  gracefully. parser-nocrash now EXERCISES the guard: a ~200k-deep input exits 1
  (graceful) under `ulimit -s 512` where the unguarded native parser segfaults
  (139). The depth guard is now the load-bearing crash-prevention mechanism.
- **Mark/rewind is cheap C locals.** A CHOICE/OPTIONAL/REPEAT backtrack saves the
  cursor, the builder count, and the sink count in C locals and on failure restores
  them and truncates the builder (rolls `count` back; no arena rollback) — exactly
  the saved_pos/saved_count/saved_sink the VM's arms keep.
- **The goal (beat tree-sitter) is NOT met; recorded as the new floor, not faked.**
  Bench (1070 React files, 9.55 MB): native C-compiled **4.48 MB/s** — **+38%** over
  the W16-1 baked parser (3.25) and, for the first time, **faster than the VM**
  (1.23×: 4.48 vs 3.63). But tree-sitter is 6.42 MB/s (~1.43× faster), so
  `W16-FASTER` is deliberately NOT recorded in performance.md. The structural
  overhead the Phase 15 floor blamed (the per-node frame machine) is genuinely
  removed; the residual gap is now token matching (~37%, incl. the recursive
  `tp_match_ir` not-guard fallback W15-2 deferred for byte-identity) and the shared
  CALL prologue/epilogue (~15%). The meter names the LEXER as the next lever, not the
  parser control flow. Floor + evidence in docs/performance.md (`### W16-2`) and
  docs/peg-vs-glr.md (`## W16-2`).

### W16-3 done — two generated parsers in one binary (per-parser symbol prefix)

The phase's first packaging deliverable: prove the generator emits independent,
non-colliding parsers that coexist in one program (the embedding use case).

- **Decision: a `--name <prefix>` codegen option, default unchanged.** `tinyparse
  c <grammar> --parser[-lib] --name P` renames EVERY emitted file-scope symbol to
  `P_…`. With no `--name`, the parser keeps its historical default names
  (`tp_parser_p<N>`, `tp_compiled_parser_load`/`_run`, the statics), so existing
  callers and `ci/tinyparse-cc-diff.sh` (which compiles the default-name path) are
  byte-for-byte unaffected — verified: the generated default output is byte-identical
  and cc-diff stays 267/267. Two parsers with distinct prefixes link into one binary
  with no collision.
- **Decision: thread the prefix on the engine, not the public header.** The CLI sets
  it through a library-private `tp_engine_set_parser_prefix` (declared in
  internal.h, defined in engine.c) — the same seam `TINYPARSE_ENGINE=vm` uses — so
  the frozen public API is untouched. The prefix points at host-owned `argv`
  storage that outlives the synchronous emit; nothing is copied, and an empty string
  is treated as no prefix.
- **Decision: one emit helper guarantees default byte-identity.**
  `tp_parser_name(cursor, prefix, default_name, suffix)` emits `default_name`
  verbatim when prefix is NULL, else `<prefix>_<suffix>`. The historical names are
  NOT a uniform stem (`tp_parser_p<N>` vs `tp_execute_parser` vs
  `tp_compiled_parser_load`), so passing both the default name and the prefixed
  suffix lets one scheme cover all of them while keeping the default path exact.
- **Shared runtime is deduplicated by being one library, not per-TU.** The only
  symbols that ACTUALLY collided were the two exported entries — the per-program
  functions and the embedded grammar bytes are `static` (internal linkage, no
  cross-TU collision). The leaf helpers, the CALL prologue/epilogue, the tree
  builder, the lowering, and ax all live in libtinyparse + libax, ONE archive set
  linked once and shared between the two generated translation units. The prefix
  still renames the statics too, so a reader sees one consistent scheme and any
  future non-static symbol is covered.
- **The example: `examples/embed/` (host fenced-block grammar + std.json embedded).**
  `host.scm` delimits ```` ``` ````-fenced blocks and exposes each body as a `code`
  field; `embed.c` parses a document with the host parser, reads each `code` span's
  byte range off the host tree (never re-scanning the source), and runs a SECOND
  compiled parser (std.json, its own prefix) on those bytes — printing the host tree
  and the embedded json tree of each fence. Deliberately NOT the W62 `inject` demo
  (`std.fence` parses both languages in one grammar); here it is two separate parsers
  the host program drives, which is what exercises the no-collision linking.
- **The gate: `ci/tinyparse-multiparser.sh`.** Builds the example (a symbol
  collision fails the link), runs it on a committed sample, asserts the output equals
  the golden `expected.txt` (host tree + both embedded json trees), and checks with
  `nm` that the binary carries both parsers' four distinct entries
  (`host_parser_load`/`_run`, `json_parser_load`/`_run`) and no leftover
  `tp_compiled_parser_*` collision symbol.
- **Gates at the commit:** build clean, metric 296/296, cc-diff byte-identical
  267/267, vm-diff GREEN, no-statics GREEN, vm-nocrash + parser-nocrash GREEN under
  `ulimit -s 512`, perf gate within ceiling, multiparser gate GREEN. Evidence:
  docs/peg-vs-glr.md (`## W16-3`).

### W16-4 done — hide ax inside the generated parser's embeddable artifact

The phase's second packaging deliverable: the generated C parser may use ax
internally (it is the freestanding runtime the compiled parser links against), but
an embeddable object/library built from it must export ONLY the parser's own public
API plus the public `tinyparse_*` API the host calls — no `ax_*` symbol, and no
tinyparse-internal `tp_*` symbol, in the exported table.

- **Starting state, measured.** Before this workstream the `examples/embed` binary
  (driver + two generated parsers, linked against `libtinyparse.a` + `libax.a` as
  separate archives) exported **56 `ax_*` symbols as `T`** (external, defined) — ax
  leaked wholesale into the host's symbol table. That is the leak W16-4 closes.
- **Decision: package the embeddable artifact as the amalgamation + a linker
  version script, not a per-symbol annotation of the frozen header.** The owner's
  preferred shape is "hidden in the generated code" — a self-contained amalgamation
  the embedder compiles. The pieces already exist: `dist/amalgamation/tinyparse.c`
  folds the ax engine in under `#pragma GCC visibility push(hidden)`, so `ax_*`
  never has default visibility to begin with; and the generated parser-lib's own
  per-rule functions are already `static` (the no-statics gate proves it), leaving
  only its two `<prefix>_load`/`<prefix>_run` entries external. The remaining leak
  was that the amalgamation keeps the tinyparse part — public `tinyparse_*` AND
  internal `tp_*` — at default visibility (correct for the standalone shared
  library, which must export its public API). The embedding build wants a tighter
  surface, so it links behind a version script that exports exactly
  `{ <prefix>_load, <prefix>_run, …, tinyparse_* }` and localizes everything else
  — which makes both `ax_*` and the internal `tp_*` local.
- **Why a version script and NOT `-fvisibility=hidden` alone.** The amalgamation's
  `#pragma GCC visibility push(default)` around the tinyparse section OVERRIDES a
  command-line `-fvisibility=hidden`, so the flag alone cannot hide the internal
  `tp_*` (measured: 62 `tp_*` stayed exported under `-fvisibility=hidden`). The
  version script is applied at link, after the pragmas, and cleanly localizes the
  internals without touching the shared amalgamation or annotating the FROZEN public
  header (which would change the contract and risk the byte-identity gates).
- **Why a version script and NOT a single-TU all-`static` amalgamation.** A pure
  single-TU `.o` with everything `static` except the entries would need to
  distinguish public `tinyparse_*` from internal `tp_*` inside one file; the
  public header carries no visibility annotation, so there is no in-source seam to
  key on without editing the frozen header. The version script keys on the symbol
  names at link instead — the same mechanism real amalgamated libraries ship — and
  needs no header change.
- **The recipe: `examples/embed/hide-ax.sh`.** `hide-ax.sh OUT.so PREFIX:GRAMMAR …`
  compiles the amalgamation once, generates and compiles each parser-lib TU with its
  `--name PREFIX`, generates the version script from the prefixes (GNU `ld`
  `--version-script`, or the macOS `-exported_symbols_list`), and links the
  ax-hidden shared object. It is the documented embedding build recipe and what the
  gate drives.
- **The gate: `ci/tinyparse-hide-ax.sh`.** Builds the embeddable artifact two
  ways and asserts with `nm`: (1) a single parser (std.json, prefix `jp`) exports
  `jp_load`/`jp_run` + the public API and ZERO `ax_*` and ZERO `tp_*`; (2) the
  two-parser embed example exports both parsers' four entries, the public API, and
  ZERO `ax_*`; (3) a hardened EXECUTABLE of the two-parser case (behind the same
  version script, 0 exported `ax_*`) still parses the committed sample to the golden
  tree byte for byte — proving hiding ax does not break the parse.
- **Gates at the commit:** build clean, metric 296/296, cc-diff byte-identical
  (the emitter is UNCHANGED — packaging is a link-time recipe, so the generated C is
  bit-for-bit what W16-3 emitted), vm-diff GREEN, no-statics GREEN, vm-nocrash +
  parser-nocrash GREEN under `ulimit -s 512`, perf gate within ceiling, multiparser
  gate still GREEN (no regression), and the new `ci/tinyparse-hide-ax.sh` GREEN.
  Evidence: docs/peg-vs-glr.md (`## W16-4`).

### W16-GOAL done — the last IR interpreter is off the parse hot path; the token memo retired from the clean path

The phase's speed push on the generated C parser. After W16-2 the profiler (callgrind,
the React corpus through the C-compiled parser) named the residual costs: token
matching ~37% (including the deferred `not`-guard fallback to the recursive
`tp_match_ir`), CALL enter/exit ~15%, the tree builder ~12%, extras ~11%. Each change
below was MEASURED (instruction count AND wall-clock A/B on a real file); the
measured non-wins were discarded.

- **The `not`-guard tokens are lowered — `tp_match_ir` is off the parse hot path.**
  Comments, the regex literal, `hash_bang`, and the HTML-comment trivia all scan with
  a `(repeat (not GUARD any))` shape that W15-2 deliberately left on the recursive
  `tp_match_ir` fallback (to keep byte-identity). `TP_OP_NOT` now lowers to a flat
  negative-lookahead guard region in the token bytecode (`TP_TOK_NOT_BEGIN` pushes a
  negative save resuming where the guard FAILS; `TP_TOK_NOT_END`, reached only when
  the guard matched, drops the save and fails the construct). The control flow mirrors
  `tp_match_ir`'s `TP_OP_NOT` node-for-node, so the end offset is byte-identical
  (cc-diff GREEN across 267 cases, which exercise comments / regex / strings). The
  profile confirms it: `tp_match_ir` went from ~3% of the parse to **zero instructions
  on the hot path** — the IR interpreter, the last one in the project, is no longer
  called during a parse. It stays only as the cold reference (query.c) and the
  save-stack-overflow / unlowerable fallback, so deleting it outright would remove a
  safety net for no measured gain; it is reduced to cold, not removed.
- **DISCARDED: a fused `TP_TOK_SCAN_UNTIL` "scan to delimiter" op.** A dedicated
  hand-lexer scan for `(repeat (not GUARD any))` measured ~4% MORE instructions and no
  wall-time win on the corpus (comments are a small fraction of real TS files, and the
  per-scan lead-byte warm-up cost more than the plain NOT loop it replaced). Removed —
  the meter said it was not a win.
- **The per-position TOKEN memo is dropped from the clean parse path — the biggest
  win (~38% on a real file).** On a left-factored, first-set-predicted parse the
  token matcher rarely re-runs at the same offset; the memo's hash + random-access
  slot probe/store churned the cache faster than re-running the jump-threaded token
  bytecode (`tp_token_run`) would, even though the matcher re-ran more often.
  Measured: passing the memo through the terminal path (`tp_vm_terminal`) and the
  extras scan (`tp_vm_consume_extras`) as NULL is **byte-identical** (a memo is a pure
  cache) and ~38%+6% faster respectively in wall-clock A/B, despite a HIGHER
  instruction count — a cache-behaviour win callgrind cannot see, exactly the case the
  profiling skill flags for wall-time measurement. Pathological backtracking is still
  bounded: the rule memo (`tp_rule_memo`) wakes on a super-linear parse and
  short-circuits re-descent, so a terminal is not re-matched unboundedly; verified
  fast on adversarial deep/long/wide inputs and on the no-crash gates.
- **The RULE memo is confirmed dead on the clean path and already skipped — left
  intact for the backtracking backstop.** It is gated behind a super-linear-attempt
  threshold (`memo_active`), so a clean React/TS parse never probes or stores it; the
  profile shows zero rule-memo cost. No change needed — it remains the bound for
  pathological inputs.
- **The identifier choice-leaf run is fused (~3-4%).** `(repeat (choice LEAF alt…))`
  — the `identifier` / `identifier_name` body, the single most common token shape —
  now fast-loops the leading character class as a tight `STAR` and only falls to the
  rare `\u` alternatives when the class stops matching. Byte-identical (same ordered,
  possessive semantics), measured ~3-4% faster.
- **Per-rule child sink uses inline frame storage on the generated path.** The
  generated native parser's `tp_vm_frame` is a stable C local, so `tp_vm_call_enter`
  now uses the frame's inline `children_storage` instead of a per-rule heap alloc
  (gated by `state->children_inline`, which the generated driver sets). The VM keeps
  the separate allocation — its frames live on a RELOCATABLE arena stack, where a
  pointer into a frame would dangle when the stack grows. A small, clean structural
  win.
- **`consume_extras` reordered so the lead-byte reject is the first check** (ahead of
  the extras-memo computation) and no longer writes the per-position mark on the
  no-extra fast path (the O(1) bitset reject re-derives it for free on revisit).
  Marginal but cleaner; byte-identical.
- **Honest outcome: NOT `W16-FASTER`.** These wins lifted the C-compiled parser from
  ~4.48 MB/s (W16-2) to ~5.8 MB/s on the React corpus (+~29%), and the VM rode the
  shared `token.c`/`vm.c` changes up too. But tree-sitter remains ~6.2 MB/s on the
  same corpus — ~5-10% ahead, consistently across runs. The `W16-FASTER` marker is
  NOT recorded (it was not earned; the headline goal is not met). The remaining,
  evidenced floor is the per-rule NODE model the generated parser shares with the VM
  for cc-diff byte-identity: CALL enter/exit + the tree builder + the per-rule child
  allocation are ~24% of instructions, and tree-sitter's table-driven core and compact
  incremental tree never pay it. The relocatable-frame VM design also forces the
  per-rule child-sink allocation that a hand parser avoids with a shared child stack.
  Closing the last ~10% means the holistic emitter redesign below (a native parser
  that owns its own tree/child storage), not another micro-optimization. Evidence:
  docs/peg-vs-glr.md (`## W16-GOAL`); before/after profile and the 6-arm bench there.

## Phase 16 — the holistic round (owner principle, next pass)

- **Principle (owner): the generated C parser is guided by how a HAND-WRITTEN
  recursive-descent parser would look — we generate the code a human would write.**
  Not "bytecode lowered to C." The incremental rounds (W15-3 bake, W16-2 native
  recursion, the W16-GOAL lexer lowering) were each grounded in the previous step's
  representation; that lineage is the limiter. The next pass views the parser
  holistically by INTENT: emit idiomatic per-rule recursive descent directly from
  the grammar/IR.
- **We have everything we need in the IR.** Rule structure, first-sets, token
  patterns, field/alias/node-kind constants, AND the material for excellent error
  handling and reporting — all known at generation time. So the generated file is
  self-contained idiomatic C with NO runtime interpretation of the grammar: no
  `program->instructions[N]` operand reads, no generic frame/sink/memo machinery
  called per node — just code, the way tsc/tsgo are written by hand.
- **ax's role is narrowed: the runtime substrate that ORCHESTRATES the state
  machines** (arenas/memory; actor orchestration for streaming/embedding/multi-parser
  coordination) — NOT a per-node parse interpreter the generated code drives. The
  generated parser core is native code.
- **Target: close to tsc/tsgo (native RD).** tsgo ~29 MB/s, tsc ~11.9, tree-sitter
  ~6.42; our bytecode-derived native-RD parser plateaued at ~4.48. The owner's bar:
  "if we cannot get close to native performance when generating native code, we're
  doing something wrong, and we need to rethink the approach." Beating tree-sitter
  is the floor for this round; tsc-class is the aim.
- **Error handling/reporting becomes a first-class output of the generator** (precise
  expected/found diagnostics with location), beyond what the VM produces — the
  generated parser may add diagnostics, but on valid input it must still build the
  same contiguous CST (cc-diff byte-identity stays the correctness anchor) and stay
  crash-safe (depth guard).
- **Sequencing:** let the in-flight W16-GOAL round finish and land first — deleting
  the last IR interpreter (`tp_match_ir`) and giving us a clean compiled lexer is
  exactly the base the hand-written design wants — then do the holistic redesign of
  the emitter on top of it.

## Phase 16 — holistic round: two more principles the hand-written parser must respect

- **The CST stays arena-built and tightly packed.** The generated hand-written
  parser must construct the SAME contiguous, arena-allocated CST it builds today —
  cursor-only navigation, nodes packed in one growing array. This is also exactly
  what `ci/tinyparse-cc-diff.sh` pins (byte-identical tree vs the VM), so it is a
  hard correctness anchor, not a preference.
- **Buffer-friendly: the parser can signal it needs more input.** Desired so the
  parser works over a growing buffer / streamed input rather than requiring the whole
  input up front.
- **The tension, and how it is reconciled.** Native call-stack recursion (the
  "code a human would write") holds parse state on the C stack, which cannot be
  suspended mid-parse — and ax has NO stackful fiber/coroutine/ucontext machinery
  (its continuation is the journal, event-sourced replay), so ax cannot pause a
  native stack between input chunks. Today's streaming (`tinyparse_push`) works
  precisely BECAUSE the VM uses an explicit stack that holds the continuation
  between pushes. Reconciliation for the holistic round:
  - the generated native-RD parser signals "need more input" at a SHALLOW boundary
    (naturally top-level-item granularity): it native-recursion-parses each complete
    item from the buffer at full speed, and when the buffer ends before an item
    completes it returns `NEED_MORE_INPUT`; the orchestrator appends bytes to the
    growable arena buffer and it resumes at the next item. The lexer's `need_bytes`
    is the signal. This keeps native-RD speed AND is buffer-friendly — it suspends at
    item boundaries (stack empty), not at arbitrary depth.
  - the explicit-stack VM/actor stays the engine for true async PUSH streaming
    (chunks arriving mid-item, caller cannot block). ax orchestrates: pick the fast
    native-RD generated parser for buffers/pull sources and per-item streaming, the
    VM for arbitrary-granularity async push. That is "ax orchestrates the state
    machines."
  - honest limitation: the generated NATIVE parser suspends at item boundaries, not
    anywhere; arbitrary-depth suspension would require giving ax a real
    coroutine/fiber capability (a larger, separate lift) — deferred unless asked.
- **Error handling/reporting is a first-class output of the generator** (precise
  expected/found diagnostics with location), built from the IR — beyond what the VM
  produces. On valid input the tree is still byte-identical; diagnostics are additive.

## Phase 17 — hand-written C parser that keeps everything (kickoff)

- **Owner principle: the generated C parser is the code a human would write** —
  idiomatic per-rule recursive descent with the grammar baked away (no
  `program->instructions[N]` runtime reads, no generic per-node frame/sink driver).
  The pre-compiled-grammar use case is the common one and demands a native parser
  that is REALLY fast — past tree-sitter, toward tsc/tsgo. The VM stays for dynamic
  runtime parsing (a reasonable multiple of native is fine there).
- **The rewrite must NOT drop what makes tinyparse special** (owner's explicit
  fear). Non-negotiables, each gated: the tightly-packed arena CST (cc-diff); the
  context-building primitives for heredocs — lexical goals / no-extras / host class
  bindings (conformance + cc-diff); detailed error reporting via `(recover …)`
  (NEW diagnostics-parity gate — cc-diff pins the tree, not the diagnostics);
  language embedding / nesting grammars via `TP_OP_INJECT`/`fence` + multi-parser
  embedding (multiparser + cc-diff on an inject grammar); importing std grammars via
  `(import …)` compile-time merge through to a generated C parser (NEW import→C gate);
  a configurable symbol prefix defaulted from grammar name + language; and ax actors
  for coordination with a buffer-friendly `NEED_MORE_INPUT` signal.
- **Sequencing: harden the net BEFORE the rewrite.** W17-1 adds the gates that pin
  the capabilities cc-diff/conformance miss (diagnostics parity, import→C, inject
  cc-diff coverage) against the CURRENT generated parser, so the W17-2 emitter
  rewrite runs under a complete safety net and cannot silently regress a feature.
  W17-3 makes the prefix name/language-derived and verifies nesting + import on the
  new emitter; W17-4 adds the buffer-friendly signal + actor coordination.
- **Buffer-friendly reconciliation (recap):** native recursion can't suspend
  mid-parse and ax has no stackful fiber, so the generated parser signals
  NEED_MORE_INPUT at the shallow top-level-item boundary (full-speed native recursion
  within an item); the explicit-stack VM/actor remains the engine for
  arbitrary-granularity async push. ax orchestrates the choice.
- **Finish line:** `ci/tinyparse-phase17-metric.sh` GREEN — PROGRESS=4/4, every
  standing + new preservation gate holds, and C-compiled beats tree-sitter (closing
  on tsc), tagged `W17-FASTER`. Do not fake the marker; do not trade a feature for
  speed.

## Phase 17 — W17-1 done: the preservation net (and a real hole closed)

- **Hardening the net found a genuine pre-existing feature-loss in the generated
  parser path, exactly the kind this phase exists to prevent.** The packed grammar
  (.tpg) the compiled-to-C parser embeds did not serialize TP_OP_INJECT embedded
  grammars or the lex_goals table, so a generated C parser silently dropped language
  embedding and lexical goals. Fixed symmetrically in pack.c: a recursive
  embedded-grammars IMAGE section + lex_goals, wired on load with no module resolver.
  This is why hardening the net BEFORE the rewrite was the right call — the hole was
  already there.
- **Three gates now pin what cc-diff/conformance did not:** diag-diff (recover
  diagnostics parity, generated vs VM, 8/8), import-c (an (import …)-composed grammar
  compiles to a C parser matching golden + VM), and fence/inject coverage in cc-diff
  (std.fence byte-identical, 271/271). The phase17 metric runs all three under
  RUN_GATE.
- **W17-2 (the IR-walking emitter) now runs under a complete net:** any regression of
  injection, goals, recover diagnostics, import composition, or the CST trips a gate.

## Phase 17 — W17-2 done: the IR-walking emitter + the per-token baked lexer

- **The emitter walks the IR by name, not the lowered bytecode.** It emits one
  `<prefix>_parse_<rule>` per rule and one `<prefix>_scan_<token>` per token; `TP_OP_REF`
  is a direct named call; the combinators are inline idiomatic C; the FIRST sets are
  baked byte tests. No program array, no opcode dispatch, no `program->instructions[N]`,
  no `rule_0`/`rule_N`. The generated `std.json` parser reads as `tp_parse_Document`,
  `tp_parse_Value` (first-set-guarded choice), `tp_scan_String` — hand-written RD.
- **A leaf finds its run-time `tp_ir` by NAVIGATING the loaded grammar**
  (`state->grammar->rules[i].body->children[a]->…`), which round-trips in order through
  pack.c. This is what lets the emitter walk the IR at emit time yet hand the shared
  leaf helpers a real run-time pointer without serializing addresses or keeping
  instruction indices. The decision: navigation over an index table — the structure
  lives in the C, only the few operands are read in place.
- **Byte-identity is bought by the bake-vs-substrate split, not by reproducing the
  runtime.** The grammar control flow is baked away; the CST builder, the rule CALL
  prologue/epilogue (`tp_vm_call_enter`/`exit`, memo included), the terminal/byte/eof/
  extras/recover/inject helpers stay the SAME functions the VM calls. So cc-diff is
  271/271 byte-identical, diag-diff and the inject/import/goal gates all hold. The rule
  memo is keyed on the rule SYMBOL (a stable, dense per-rule key) instead of the old
  program index — a pure cache keyed consistently across enter/exit, so the verdict is
  identical.
- **The per-token baked lexer is where the speed is.** The profile named `tp_token_run`
  (the generic token VM) at ~31% + `tp_token_match_memo` ~14% + the extras path ~14%.
  The emitter now compiles each token's pattern to straight-line C (keyword byte
  compares, `static const uint8_t [256]` class tables, class-repeat loops, `not_keyword`
  whole-slice compares, `not` lookahead guards) and bakes the whitespace/comment extras
  scan the same way (a `state->extras_match` hook the driver wires; NULL for the VM).
  Result: total instructions 106.4M → 69.8M (~34% fewer), `tp_token_run` 31% → 6.8%,
  all byte-identical. The unbakeable shapes (a host `class` binding) fall back to the
  generic matcher, so correctness is total — bake the common case, keep the substrate
  for the rest.
- **The decision to add shared helpers (`tp_vm_terminal_baked`, the `extras_match`
  hook) rather than fork the runtime.** Both the VM and the generated parser run one
  `tp_vm_consume_extras`; the hook lets the generated parser inject its baked scanner
  with the VM defaulting it off, so there is still ONE extras code path (one source of
  truth for the policy/node/diagnostic logic) — the substrate stays shared, only the
  inner "where does the token end" call is baked.
- **Headline (6-arm bench, ReactJS std.tsx, parse-only, this hardware): the C-compiled
  parser is 6.59 MB/s — strictly faster than tree-sitter (6.28) and 1.49× the VM
  (4.43).** The phase floor (beat tree-sitter) is crossed, honestly and byte-identically.
  It is not yet tsc-class (12.21); the next costs are now the CST build and the per-rule
  call_enter/exit machinery, NOT the lexer — named for the next workstream, not faked as
  the W17-FASTER marker (which is the full phase goal: meaningfully closing on tsc).

## Phase 17 — W17-3 done: defaulted prefix by grammar name + native nesting via the ax inject hook

- **The default symbol prefix is the grammar's own name, captured at compile time.**
  W16-3 prefixed emitted symbols only from an explicit `--name`; W17-3 records the
  `NAME` in `(grammar NAME …)` onto `tinyparse_grammar.name_symbol` and defaults the
  prefix from it (`json` → `json_load`/`json_parse_…`, `javascript` → `javascript_…`).
  The decision: derive collision-freedom from a fact the grammar already states (its
  name) rather than demand a flag, so two parsers from differently named grammars coexist
  in one binary with no `--name`. `--name` still overrides; a grammar with no captured
  name (an old binary `.tpg`) falls back to the historical flat names, so legacy callers,
  the `tp_compiled_parser_*` profiling harnesses, and the cc-diff oracle stay byte-for-
  byte unchanged (cc-diff compares the dumped tree, which internal renaming never moves).
- **Language injection resolves through an engine-held registry keyed by name, not the
  compile-time grammar alone.** `tinyparse_register_language(engine, "std.json",
  {grammar, parse})` maps a name to an entry whose `parse` is NULL (run the VM over the
  grammar — today's dynamic case, unchanged) or a separately-compiled generated parser's
  `<name>_run`. `tp_inject_run` consults the registry first and falls back to the inject
  IR's compile-time `embedded_grammar` when nothing is registered. The decision: name-
  keyed runtime resolution over a static function-pointer table, because only that lets a
  host compose generated parsers it never linked, and it generalizes the existing
  compile-time embedded grammar (which stays a valid entry) instead of replacing it.
- **Native nesting reuses the existing supervised-child splice; it does not add a new
  embedding path.** The registered generated parser runs over a sub-parser that shares
  only the host arena (its own builder/diagnostics/grammar/input window), and the SAME
  `tp_inject_copy_node` offset-maps, re-interns, and language-tags the subtree into the
  host tree under the `injection` node. So a generated host nesting a generated embedded
  parser yields a tree byte-identical to the VM's, and a failing embedded parse stays
  contained — the isolation guarantee is unchanged whether the child is VM or generated.
- **Synchronous over the known span is the generated parser's only nesting mode.** The
  region's bytes are fully known when the host reaches the inject site, so the registered
  parser runs to completion with no suspension — the fast pre-compiled path. Streamed,
  message-coordinated embedding stays the VM/actor engine's job (the same boundary as the
  buffer-friendly signal), deferred rather than forced into native recursion that cannot
  suspend.
- **The capability is gated, not just demonstrated.** `examples/nesting` links a
  generated `std.fence` host with a generated `std.json` and a generated `std.javascript`
  parser in one binary, embedded parsers registered at runtime;
  `ci/tinyparse-nesting.sh` asserts the composite tree is byte-identical to the VM's
  `std.fence --dump-tree`, all three entries link with no collision, and a hide-ax shared
  object of the set exports the parser + `tinyparse_*` API and zero `ax_` symbols.

## Phase 17 — W17-4 done: buffer-friendly generated parser + ax-coordinated chunk loop

- **The generated parser suspends ONLY at the shallow top-level-item boundary,
  because native recursion cannot suspend mid-stack and ax has no fiber.** The
  generated parser is native recursive descent (the call stack is the parse stack),
  which cannot be frozen and resumed; ax's continuation is its journal, not a saved C
  stack, so there is no fiber to freeze it into. The one place the C stack is empty is
  the top of each iteration of the start rule's repeat of top-level items. That is the
  suspension point: within an item the parser runs at full native speed and never
  checks for more input; at the item boundary, with more input still possible, it can
  stop. The explicit-stack VM/actor keeps arbitrary-granularity async push (its arena
  frame stack IS the continuation), the same split the W17-3 inject hook drew between
  synchronous (generated, known span) and message-coordinated (VM, streamed span).
- **An item is committed only when it completes STRICTLY before the buffer end; that
  is what buys byte-identity.** The boundary check is a comparison of the cursor
  against the buffer length at the item edge — no matcher change. An item that reaches
  the buffer end (success or the loop-terminating failure) while input is incomplete
  is rolled back to its start and the loop returns TINYPARSE_MORE, because more bytes
  might have extended it (an atom growing, a list still open). On the final chunk
  input_complete disables the held-back check and the tail is parsed exactly as the
  whole-input driver parses it — same items, same order, same root. The decision:
  conservative hold-back at the edge over a high-water-mark scheme in the matchers,
  because it needs no runtime change and is provably byte-identical.
- **Reuse the existing TINYPARSE_MORE result code — no public-header change.** The
  frozen header already declared TINYPARSE_MORE (unused). It is exactly the
  NEED_MORE_INPUT signal, so the buffer-friendly entry returns it and the contract
  surface is unchanged. The host owns one new internal field, input_complete (default
  1 = whole input, the historical finish/run path), set on the parser before each
  run_chunk; a buffer-friendly host includes internal.h to reach it, as the nesting
  example already does for the language-tag accessor.
- **The resumable loop is ONE shared runtime helper (tp_parser_chunk_loop), not baked
  per grammar.** It is parameterized by the generated per-item function, whether the
  start body ends in EOF, and the repeat's min bound. The root, the child sink, and the
  cursor boundary persist on the parser across calls (bf_root/bf_children/bf_position),
  so a MORE return keeps the committed items and resumes at the boundary rather than
  re-parsing from byte 0. The emitter recognizes the top-level-item shape
  ((repeat ITEM) or (seq (repeat ITEM) [EOF]); a bounded repeat is excluded) and emits
  a per-item function (the exact body of one whole-input REPEAT iteration), a chunk
  driver that sets the state up identically to the whole-input driver, and the
  <prefix>_run_chunk entry. The whole-input driver and <prefix>_run are untouched, so
  the change is purely additive and cc-diff stays 271/271 byte-identical.
- **ax coordinates the choice on the same session that already owns the buffer.** The
  parse is the W60 ax session: the parser actor owns the arena, builder, diagnostics,
  and input buffer, and tinyparse_push routes bytes to it. The buffer-friendly path
  rides that session — push bytes, mark input_complete, call run_chunk; the generated
  native parser runs inside the session against its state. ax picks the native parser
  for buffer/pull + per-item streaming and the VM for async push; this workstream does
  not touch the VM push path (vm-diff GREEN).
- **The capability is gated, not just demonstrated.** examples/bufferfriendly drives
  the generated sexpr parser over a document in fixed-size chunks through the
  TINYPARSE_MORE loop; ci/tinyparse-bufferfriendly.sh asserts the tree is
  byte-identical to the VM's whole-input --dump-tree at eight chunk sizes (1, 2, 3, 5,
  8, 17, 64, and larger-than-file), and that chunk size 1 genuinely suspends (>0
  TINYPARSE_MORE), so the boundary cannot be silently bypassed. Chunked == whole ==
  VM, byte for byte, across the granularity range; depth guard and byte-identity
  preserved.

## Phase 17 — W17-GOAL: profile-driven push toward tsc-class throughput

The four W17 workstreams landed the hand-written generated parser past tree-sitter
(6.59 vs 6.28 MB/s) but short of tsc (12.21). This push is profile-ranked: measure
every change (callgrind + the 6-arm bench), keep only measured wins, keep the parser
byte-identical to the VM (cc-diff 271/271) throughout. Decisions:

- **Profile the bench's actual timed region, not just run-only.** The bench times
  `push + run`; the W17-2 profile timed run only. Profiling push+run (the new
  `bench/prof/prof_tsx.c`, generated std.tsx over the largest React file ×40) named a
  cost the run-only profile hid: the input buffer copy.
- **Buffer the input with a bulk copy.** `tp_parser_buffer` (the body of
  `tinyparse_push`) copied the pushed bytes into its growable arena buffer one byte
  at a time and zeroed the buffer it was about to overwrite. That byte loop was the
  single largest cost in the timed region (15.70%). Switched to `__builtin_memcpy`
  (the freestanding source takes no new header; the compiler lowers it to the
  vectorised libc memcpy already linked through ax) and to `ax_alloc_raw` (the buffer
  is fully written before read, so the zeroing memset was discarded work). −17.0%
  total instructions in the timed region; no parser logic touched, so cc-diff stays
  byte-identical and every net gate is green.

- **Allocate the rule-memo tables lazily.** `tp_rule_memo_init` zeroed an input-scaled
  occupancy column (the `live' byte-per-slot array, ~1 MB for the 311 KB React file)
  at the start of every parse, but the rule memo is dormant until a parse goes
  super-linear (`memo_active`), which a clean parse never does — so that zeroing
  (the largest memset left after change 1, 10.41%) was pure waste on the common path.
  Split init into a no-allocation prepare and a lazy `tp_rule_memo_ensure` called when
  `memo_active` flips on (and on the force-on differential path). −8.0% total
  instructions; memset 10.41% → 2.65%. The memo is a pure cache built at the moment it
  first becomes usable, so the verdict is unchanged: cc-diff byte-identical 271/271
  on both the default and `TINYPARSE_RULEMEMO=always` paths, and the
  pathological-input nocrash gates (which activate the memo) stay GREEN.

- **Inline the per-rule CALL lifecycle into the generated functions.** Once the lexer
  was baked and the memsets cut, `tp_vm_call_enter`/`exit` (the per-rule lifecycle the
  generated parser pays through the shared helpers) were the profile's top costs (~17%
  combined): their self-cost was the memo bookkeeping every entry wrote — dead on a
  clean parse — plus cross-TU call overhead over ~587K entries. Specialized them into
  two `static inline` wrappers (`tp_vm_enter`/`tp_vm_exit`) the emitter calls: on the
  clean steady state they do only the marks / extras / node-open / link / close / push
  a hand-written parser does, inlined with the rule's kind/field/alias in registers;
  they DELEGATE to the shared helpers the instant the lifecycle is non-trivial (memo
  waking, recovery armed, no_extras scope, VM relocatable sink, or a trace plug), so
  the tree is byte-identical. The memo-waking counter is threaded so the crossing entry
  always goes through the shared helper (which owns the lazy allocation and first
  probe) and call_attempts increments exactly once per entry. The VM is untouched
  (cc-diff reference). −8.7% instructions; both functions left the top of the profile.
  cc-diff byte-identical 271/271 default and RULEMEMO=always; nocrash gates GREEN.

- **Inline the extras fast-reject at the hot call sites.** Once the lifecycle was
  inlined, `tp_vm_consume_extras` was the runaway top cost (16.91%): it runs before
  every terminal and rule entry, and the dominant "cursor byte begins no extra" case
  it already proves with one lead-byte bitset probe was paid as a cross-TU call
  millions of times. Added an inline `tp_vm_extras` doing exactly that O(1) reject
  (byte-for-byte the function's own fast path, same order) at the call site, falling to
  the full function only when a byte could begin an extra. Wired into tp_vm_enter, the
  generated choice prologue, and tp_vm_terminal_baked (generated-only paths); the VM's
  terminal keeps tp_vm_consume_extras (cc-diff reference). −4.7% instructions; the
  extras slice fell from 74.8M to ~43.8M. cc-diff byte-identical 271/271 default and
  RULEMEMO=always; all gates GREEN.

- **Bake the inline rule-body literals.** The largest remaining lexer slice was the
  bare TP_OP_LITERAL in a rule body (punctuation matched directly) -- still on the
  generic token VM (tp_vm_do_match_pattern -> tp_token_run), ~320K calls / ~19%
  inclusive, even after W17-2 baked the per-token scanners. The emitter now bakes each
  DISTINCT inline literal (deduped by bytes) to a pure byte-compare matcher
  (<prefix>_litmatch_<n>, reusing the token-matcher emitter) and routes the site
  through tp_vm_terminal_baked. A bare literal is never goal-redirected (its kind is the
  literal text, not a token symbol), so the baked path is byte-identical to
  tp_vm_do_match_pattern -- same extras consume, leaf push, `token' trace, end offset.
  A literal-registry pre-pass assigns stable matcher indices before emission; the common
  punctuation shares one matcher. -9.0% instructions; tp_token_run/tp_token_match_memo
  left the top of the profile -- every terminal is now baked straight-line C. cc-diff
  byte-identical 271/271 default and RULEMEMO=always; all gates GREEN. Cumulative -39.5%.

- **Allocate the extras memo lazily too.** The last large memset was the extras memo's
  marks array (~311 KB zeroed per parse), which only helps on a backtrack a clean
  predictive parse never does. Deferred it exactly like the rule memo (lazy
  tp_extras_memo_ensure on the memo_active wake -- super-linear parsing is when
  backtracking, and the memo, start to matter). marks stays NULL until then, disabling
  the pure-cache memo with no verdict change. -3.4% instructions; the arena-zeroing
  memset left the top of the profile. cc-diff byte-identical 271/271 default and
  RULEMEMO=always; pathological nocrash gates GREEN. Cumulative -41.5%; the remaining
  top costs are the irreducible CST build (tree_builder_push / sink_push /
  link_children) and the one-time input copy -- the hand-written-parser floor.

- **Measured and dropped: an inline sink-append fast path.** After the lazy memos and
  the baked literals, the CST-build helpers (tp_tree_builder_push / tp_vm_sink_push /
  tp_vm_link_children) were the profile's top costs. An inline tp_vm_sink_append (the
  spare-capacity store/increment inlined, growth delegated) was tried to cut the
  per-node sink_push call. It MEASURED A NON-WIN: instructions rose slightly and
  wall-clock stayed flat (~0.397 s on the prof driver) -- the saved call overhead was
  offset by the inlining bloat at every node site (I-cache pressure). Dropped per the
  profiling discipline (keep only measured wins). Writing children straight into the
  builder to eliminate the sink entirely was also rejected: the contiguous pre-order
  CST interleaves a node's descendants with its direct children, so the direct-child
  vector link_children needs cannot be recovered without the sink, and touching it risks
  the byte-identical tree. The remaining CST-build cost is the honest hand-written-parser
  floor (the Phase 15 stance), recorded in docs/performance.md under W17-FASTER.
- **W17-FASTER earned and recorded.** The C-compiled parser reached 7.69 MB/s (up from
  6.59 at W17-2, +16.7%): 1.20x tree-sitter, 1.68x the VM, 0.62x tsc (1.6x short, down
  from 1.85x short). Comfortably past the tree-sitter floor and meaningfully closing on
  tsc, byte-identical (cc-diff 271/271) throughout, -41.5% instructions in the timed
  region. Full 6-arm table + hardware in docs/performance.md. The marker is honest: not
  tsc-class yet, with the irreducible CST-build floor named and evidenced.

## Phase 17 — the formalism: the generated parser is hand-written code, not a packrat engine

- **Owner reframing (decisive):** the generated C parser's formalism is NOT "a
  packrat-PEG-generated parser." It is **a code generator that types what a human
  would type by hand**, using **ax to manage multiple logical processes** (the parse
  session, nested/embedded parsers, streaming coordination). The VM remains the
  packrat-PEG engine for dynamic, runtime-supplied grammars; the GENERATED parser is
  a hand-written recursive-descent parser.
- **Direct consequence — no memo in the generated parser.** A hand-written RD parser
  does not carry a packrat memo. Strip the rule memo entirely from the generated
  code: no memo probe/store/snapshot/replay, no `memo_active`/`call_attempts`/
  threshold bookkeeping, no per-position memo tables. The memo is a pure cache, so
  removing it is byte-identical (cc-diff holds); it only ever bounded pathological
  super-linear *time*, which a hand-written parser also lives with — and our std
  grammars are left-factored to linear, so the cc-diff corpora stay fast. The VM
  keeps the memo (it must handle arbitrary runtime grammars).
- **And any other PEG-engine residue** the generated parser still carries that a
  human would not type (generic per-node bookkeeping that exists only to serve the
  interpreter's generality) is fair game to remove, provided the packed-arena CST
  stays byte-identical and every preservation gate holds.
- **ax's role, restated:** managing multiple logical processes — sessions, the
  language registry for nesting, streaming — NOT per-rule parse machinery.
- This guides the next goal-push round (de-packrat the generated parser); expect both
  a simpler "reads like hand-written" parser AND a speed gain from dropping the memo
  bookkeeping.

## Phase 17 — W17-GOAL2 done: de-packrat the generated parser (no memo, hand-written)

The goal-push round that acts on the formalism above (the generated parser is
hand-written code, not a packrat engine; ax manages logical processes, not per-rule
parse machinery; the memo is VM-only).

- **The rule memo is removed ENTIRELY from the generated path.** The generated-only
  inline CALL lifecycle (`tp_vm_enter`/`tp_vm_exit` in internal.h) dropped all memo
  bookkeeping: no `call_attempts` increment, no `memo_threshold` compare, no
  `memo_active` check, and no delegation-for-memo. It now does only the hand-written
  per-rule work and delegates to the shared `tp_vm_call_enter`/`exit` only for the
  genuinely non-trivial lifecycle (recovery armed, a `no_extras` scope, the VM's
  relocatable sink, a trace plug). The generated driver pins the memo OFF
  (`memo_threshold = 0xFFFFFFFF`, `memo_active = 0`) so a delegated lifecycle cannot
  wake it. The emitted std.tsx parser now contains zero memo machinery (only the two
  explicit pin-off lines). The VM keeps its memo unchanged — it must bound
  backtracking for arbitrary runtime-supplied grammars — verified by vm-diff (incl.
  the `TINYPARSE_RULEMEMO=always` forced-replay path).
- **Byte-identical and fast, because the memo is a pure cache and the grammars are
  left-factored.** cc-diff stays 271/271 byte-identical and completes in ~27 s — no
  exponential blowup appears without the memo, confirming the std corpora are
  left-factored to linear (the precondition the round depended on; had any corpus
  blown up, that grammar would not be left-factored and the report would have stopped
  rather than re-adding the memo).
- **Measured effect: −1.32% instructions in the timed region** (callgrind, std.tsx
  over the largest React file ×8: 1,456,543,386 → 1,437,366,138 Ir). Wall-clock on the
  prof driver is flat within noise (the removed work is a few scalar ops per rule
  entry); the 6-arm per-file-fresh-engine bench holds the C arm at ~9.78 MB/s (1.20×
  tree-sitter, 1.64× the VM) byte-identically. The win is small and mostly masked at
  the bench level but real in the timed region; the larger value is
  correctness-of-form — the generated parser now reads and runs like hand-written RD.
- **Audit: no other interpreter-generality residue left to remove on the generated
  path.** With the memo gone, the profile's top costs are the irreducible
  hand-written-parser floor (the baked lexer `tp_token_run`, the packed-CST build
  `tp_tree_builder_push`/`tp_vm_sink_push`/`tp_vm_link_children`, the extras policy
  `tp_vm_consume_extras`). The `tp_vm_frame` memo fields
  (`target`/`memo_pos`/`diag_before`/`seen_success`/`memoable`) are written only by
  the shared VM/delegated path, never by the inline hand-written path; removing them
  from the struct would touch the shared VM for no measured win (the VM still needs
  them), so they stay. Nothing else on the generated path is generic per-node
  bookkeeping a human would not type.
- **The blueprint's framing was corrected.** docs/codegen-transform.md (now docs/archive/) previously said
  "the rule-memo backstop stays behind `if(memo_active)`" for the generated parser —
  now WRONG. Updated: the bake-vs-substrate table drops the memo from the substrate
  column, and a "Memo: VM-only" section states the generated parser carries none.

## Phase 17 — W17-CST done: the CST build path (incremental linking + pre-sized nodes)

The structural-lever round the owner chose after de-packrat: push past the CST build path
the W17-GOAL/GOAL2 floor named (per-node allocation, the per-rule child sink, the
`tp_vm_link_children` second pass), toward tsc/tsgo, with the hard constraint that
`--dump-tree` stays byte-identical (cc-diff) and the public cursor API / node format stays
unchanged. Profile-driven; each change measured (callgrind + same-machine wall-clock A/B),
non-wins discarded.

- **Profile ranking (before).** Generated std.tsx over the largest React file ×20, load
  amortized (184.57 M Ir): `tp_tree_builder_push` 9.81%, `tp_vm_sink_push` 6.20%,
  `tp_vm_link_children` 5.54% — the build trio ≈21.5%, plus most of the `memcpy`/`ax_alloc`.
  The extras scan (≈9%) and the baked lexer/choice prediction are the *other* hand-parser
  costs, out of this round's scope.
- **Decision 1: incremental sibling linking; the `link_children` second pass is removed.**
  The sink's `items[]` array serves two jobs — the rollback ledger an ordered-choice attempt
  truncates, and the direct-child list `link_children` re-walked cold at close. Now
  `tp_vm_sink_push` wires each child's `parent` and forward `next_sibling` as the child is
  appended (while the node is hot in cache), and `tp_vm_sink_close` finalises in O(1)
  (`first_child`/`child_count`, terminate the last sibling, span). The previous sibling is
  read from `items[count-1]`, so a failed-attempt rollback re-derives the correct last
  sibling for free. The sink gained a `parent` field; the VM and the generated parser share
  the same helpers (lockstep → cc-diff byte-identical). Both helpers are `static inline` at
  the emit sites — a first, non-inline cut REGRESSED (+1.6% Ir: the extra call cost more
  than the tight loop it replaced), so the inline form is load-bearing, not cosmetic.
  Measured: 184.57 M → 178.64 M Ir (−3.21%); `tp_vm_link_children` gone.
- **Decision 2: pre-size the node array from input length.** The node array doubled from 32
  and ax arenas never realloc in place, so each doubling copied every prior node. A parse
  settles near one node per few bytes, so `tp_vm_state_init` reserves `input_length/4` nodes
  up front (builder still empty), absorbing the growth into one allocation. Pure capacity
  hint — byte-identical. Measured: 178.64 M → 175.68 M Ir (−1.65%); `tp_tree_builder_push`
  18.1 M → 15.2 M. Peak RSS for the compiled bench arm DROPPED 55.8 → 45.2 MB (the doubling
  over-allocated more than input/4).
- **End-to-end A/B (baseline `b17d56a` in a worktree, same machine):** −4.8% Ir
  (parse, load amortized) and **−10.3% wall-clock** (best-of-5, 400 reps: 0.4076 → 0.3655 s)
  on the profile file. The wall-clock win exceeds the Ir win — the cache-locality payoff of
  hot-node linking and dropped copies, which the cache-blind instruction count understates.
  6-arm bench: C arm **10.19 MB/s — 1.26× tree-sitter, 1.72× the VM, 0.69× tsc, 0.27× tsgo**.
- **What stayed, and why (the evidenced floor).** The child-index sink is NOT dropped:
  `items[]` is the rollback ledger and the incremental link reads `items[count-1]` after a
  rollback, so a bare first/last pair on the frame would require rewriting the generated
  rollback marks (`*_sink_NNN = sink->count`) and re-validating cc-diff — real regression
  risk for an already-single inline append. The redundant `parent`/`next_sibling` writes in
  `tp_tree_builder_push` (overwritten by the incremental link for every parented child) were
  left in place: they are needed by the root and the non-parented sinks, store-coalesce to
  ~0 Ir, and splitting the push into leaf/open variants is complexity for no measured win.
  The remaining build cost is the genuine floor: write+link the 40-byte node while hot, open
  and close per rule — the minimum a flat-array, cursor-navigable CST builder performs.
- **Root and buffer-friendly sinks keep the legacy full `link_children` pass**
  (`parent = TP_NO_INDEX`): once per parse, negligible, and it avoids touching the
  resume-path's deferred root link (bufferfriendly 8/8 unchanged).
- **Gates:** build clean; cc-diff 271/271 byte-identical (dump-tree did not move); metric
  296/296; vm-diff GREEN; diag-diff 8/8; import-c/nesting/bufferfriendly/multiparser/hide-ax
  GREEN; no-statics 0; parser-nocrash + vm-nocrash GREEN under `ulimit -s 512`; perf-gate
  within ceiling.

## Phase 17 — W17-FLAT: hidden rules collapse the single-child wrapper ladder

A node census of the 1070-file React corpus (9.04M CST nodes) found ~57% of all
nodes were single-child PASS-THROUGH wrappers from the expression-precedence
ladder: reaching a bare `identifier` materialised ~20 chained nodes
(`as_satisfies_expression -> assignment_expression -> ternary_expression -> … ->
primary_expression -> identifier`). tree-sitter never emits these — it marks such
rules hidden (a leading `_`) so they are omitted and their children promote.

**Decision: a `(hidden …)` rule marker, collapsing only in the pass-through case.**
- *Syntax.* `(hidden (rule …))` / `(hidden (override (rule …)))`, carried as a
  `TP_RULE_HIDDEN` flag on `tp_rule_def`. Chosen over the leading-underscore
  convention because our left-factored ladder rules do double duty (a bare wrapper
  with zero operators, a real binary node with one), so the rule's node — and its
  name — must survive whenever it earned structure; `_additive_expression` showing
  in the operator case would only obscure it. The marker hides the *empty node*,
  not the name. The keyword form needs no reference renaming, and the flag rides
  the `.tpg` wire format (version 3) so a packed grammar collapses identically.
- *Semantics.* A hidden rule collapses **only when it added no structure**: exactly
  one child, no alias, no operator/second child. Then its single child is promoted,
  re-stamped with the call's field (the ladder's inner `left:`/`condition:` gives
  way to the outer caller's field, as tree-sitter attaches a field only to the
  surviving node). A pure dispatch rule (`(choice …)`) is always single-child, so
  always collapses; an operator-ladder rule collapses only on its no-operator path.
- *Where it lives.* The shared node-close — `tp_vm_exit` (generated, inline) and
  `tp_vm_call_exit` (VM) — both call `tp_vm_collapse_hidden`, which pushes the child
  in the wrapper's place and orphans the wrapper node (nothing links to it; the
  cursor never reaches it). Because the mechanism is in the shared close, the VM and
  the generated parser flatten identically **by construction** — the differential
  gates (cc-diff, vm-diff, diag-diff, nesting, bufferfriendly) hold without a
  re-baseline. A collapsed success is not memoized (the memo would snapshot the
  orphaned un-collapsed subtree); the memo is a pure cache, so the skip is sound.
- *Never collapsed.* The root/start node, injection (W62 language-tagged) nodes,
  aliased nodes, recovery/`MISSING` nodes, any leaf or token.

**Validation against tree-sitter (built under bench/).** For `x`, `a + b * c`,
`x as A as B`, `f(x) + 2`, and a JSX expression, our flattened `--dump-tree` now
matches tree-sitter's expression nesting (named vs anonymous nodes still differ).
`x` collapsed from a 25-deep ladder to `program > expression_statement >
identifier`, exactly tree-sitter's shape. (The one residual shape difference,
`x as A as B`, is the pre-existing left-factoring artifact: our `as_satisfies_
expression` is a flat repeat where tree-sitter left-nests `as_expression`.)

**Rules marked hidden** (JS base + TS/TSX overrides): the precedence ladder
(`assignment_expression`, `ternary_expression`, `nullish_expression`, the six
bitwise/logical levels, `equality_/relational_/shift_/additive_/multiplicative_/
exponent_expression`, `unary_expression`, `update_expression`), the dispatch and
pass-through spine (`expression`, `primary_expression`, `lhs_expression`,
`postfix_expression`, `member_or_call`, `call_head`, `member_property`,
`statement`, `declaration`, `expression_sequence`, `argument`), plus TS's
`as_satisfies_expression`.

**Measured.** ~61% reachable-node drop on a 30-file React sample (57024 -> 22336
dump lines). JS-corpus parse median 5.0ms -> 3.7ms (1.3× tree-sitter); perf-gate
ceiling ratcheted 5.0 -> 4.0ms. Gates: build clean; cc-diff 271/271; vm-diff,
diag-diff, nesting (golden re-baselined), bufferfriendly GREEN; metric 296/296
(one inject test's `call_head` assertion updated to the surviving `member_or_call`);
import-c/multiparser/hide-ax/no-statics GREEN; vm-/parser-nocrash GREEN under
`ulimit -s 512` (the collapse only ever makes the tree shallower).

## Phase 17 — W17-RECLAIM: skip the wrapper allocation W17-FLAT only orphaned

**Decision.** Take back the entry-time wrapper allocation for hidden rules that
*provably always collapse*, without changing one byte of output, and stop short of
the riskier ladder case rather than weaken the byte-identical guarantee.

- *The problem.* W17-FLAT fixed the tree but not the allocation. A hidden rule still
  opened its node at entry (`tp_tree_builder_push`); the exit-time collapse merely
  orphaned it. The ~57% of nodes that collapse were still allocated and written —
  pure waste, which is why W17-FLAT's throughput was flat.

- *Stage 1 (done).* A new compile-time flag `TP_RULE_TRANSPARENT_ALWAYS` marks a
  hidden rule whose body yields *exactly one child by every path* — a bare dispatch
  wrapper, a `(choice ref …)` of plain refs, a `(field f: ref)`/`(goal …)`, or a
  lone ref (`tp_ir_yields_one_child` / `tp_grammar_mark_transparent_rules`, run after
  refs resolve so the analysis follows `(choice ref)` through the rule table). Such a
  rule always collapses, so the CALL prologue (`tp_vm_enter`, `tp_vm_call_enter`)
  skips its node entirely: leading extras into the caller's sink, body run straight
  into that sink, the call's field re-stamped onto the single child at exit —
  byte-identical to `tp_vm_collapse_hidden`, allocation reclaimed. The flag implies
  `TP_RULE_HIDDEN` and rides the packed flags field, so `.tpg` load and the generated
  parser inherit it. Both marks are read with one inline byte probe
  (`tp_grammar_rule_marks`, bit 0 hidden / bit 1 transparent) — the two-call form
  measured *slower* than the node it saved (+1.0%); folding it to one made it a win.
  Gated `!memoable` on the VM so the rule memo's contiguous-subtree snapshot is never
  asked to relocate a node that was never opened.

- *The leading-extras trap.* The field must land on the body's significant child, not
  a leading trivia node, so the re-stamp index is captured *after* the extras consume.
  The byte-identical reference dump caught the first (pre-extras) cut immediately.

- *Stage 2 (declined, with the array-invariant finding).* Reclaiming the operator
  ladders' pass-through allocation needs *lazy open* — create the wrapper at exit only
  when an operator matched. That puts the wrapper at a *higher* array index than its
  children, inverting parent-precedes-children. The investigation found the cursor,
  linearizer, inject subtree-copy, and node-types are all index-link-driven and
  order-independent; the **only** consumer requiring contiguous `[root, count)`
  preorder is the rule memo (`tp_vm_snapshot_subtree` / `tp_vm_replay_success`), which
  is off on the generated parser and never wakes on the linear left-factored ladders.
  So lazy-open *could* be made safe by gating `!memoable`. But after Stage 1 only
  ~5.2% of node pushes remain orphaned (≈7,300 of 140,830 per parse of a 311 KB React
  file ≈ 0.5% of instructions), against intricate earned-case child adoption and the
  inversion of a long-standing structural invariant. Per the Phase-15 honest floor:
  keep Stage 1, stop Stage 2, report the partial win.

**Measured.** Generated parser, callgrind, 3 passes: 315 KB expression file
1,481,620,816 → 1,425,982,163 instructions (**−3.75%**); 311 KB React file 552.3M →
547.0M (**−0.95%**), `tp_tree_builder_push` 168.7M → 151.2M; React compiled-arm peak
RSS 45.2 → ~42.4 MB; ~3–4% faster wall-clock on the React file. **Did we pass tsc?**
No — C-compiled ~9.5 MB/s vs tsc 14.58 MB/s = 0.65× tsc; past tree-sitter, not past
tsc. Output-invariant: `--dump-tree` byte-identical to b52cbde across 345 corpus
files; gates green with goldens unchanged (cc-diff 271/271, metric 296/296, vm-/diag-
diff, nesting, bufferfriendly, import-c, no-statics, multiparser, hide-ax,
vm-/parser-nocrash under `ulimit -s 512`, perf-gate within ceiling). No golden
re-baseline — this round changed only HOW the tree is built, never WHAT.

## Phase 18 — W18-WORDCACHE: scan-once cursor cache, measured and declined

**Decision.** Decline the proposed one-entry word-class cursor cache. The thesis
gate — a cache on the `jsx_name` choice must measurably move the perf gate — failed
on measurement, so per the round's mandate we built no cache, lifted no scanner, and
changed no code. The full study is in `docs/peg-vs-glr.md` (W18-WORDCACHE).

- *The premise.* Word-class terminals (`identifier`, `jsx_identifier`,
  `identifier_name`, `kw_*`) were thought to re-scan the same word at one cursor 2–3
  times; caching the maximal word span per `(position, goal)` was projected to reclaim
  8–12% of instructions, byte-identically. The smallest slice, `jsx_name =
  (choice jsx_identifier identifier)`, was named the thesis-proving minimum.

- *What the measurement showed.* Instrumenting the generated `jsx_name` choice over
  the React corpus: `jsx_identifier` matches outright on every call (3221/3221 across
  all `.tsx`; 28103/28103 on `ReactDOMFloat-test.js`) and the `identifier` alternative
  is **never reached**. The 28103 the study read as a double-scan is 28103 single
  successful scans. The two classes are not even nested (`jsx_identifier` admits `-`
  but not high bytes; `identifier` the reverse), so no shared "maximal word" exists for
  the choice the cache was meant to collapse.

- *The bucket is not word re-scans.* The 26% token bucket is real but is extras
  scanning plus the terminal driver. The baked word matchers cost ~1.45% of
  instructions total (`scan_jsx_identifier` 0.78%, `scan_identifier` 0.59%,
  `scan_identifier_name` 0.08%, `kw_*` below threshold). Of 190,641 terminal probes on
  the 311 KB file, only 20.3% are word-class; 79.7% are contended tokens the cache must
  never touch. The reclaimable repeat fraction caps the reward below ~0.8% of
  instructions, before the cache's own lookup/fill/invalidation — the cost the killed
  per-position token memo (W16-GOAL) already proved outweighs re-running a cheap baked
  matcher.

- *The standing target.* The study's profile predates W17-FLAT/W17-RECLAIM. On the
  current engine the top costs are `tp_vm_enter`/`tp_vm_exit`, `tp_tree_builder_push`,
  and `tp_vm_consume_extras`. A future tsc-chasing round should attack extras
  consumption or the frame enter/exit overhead, not word re-scans.

**Measured (baseline only; no B-side).** JS corpus 3.5 ms, 1.2× tree-sitter, within
the 4.0 ms perf-gate ceiling. React TSX corpus (9.55 MB, 50 iters): tinyparse 6.19
MB/s vs tree-sitter 7.49 MB/s (0.83×); C-compiled arm 0.42× tsc (tsc 14.58 MB/s).
Output-invariant by inaction: no code changed, so the corpus reference dump is
byte-identical and every golden is untouched.

## Phase 18 — W18-EXTRAS: run the baked extras scanner regardless of lexical goal

**Decision.** Drop the `state->active_goal == TP_NO_SYMBOL` guard on the baked extras
fast path in `tp_vm_consume_extras` (`tools/tinyparse/src/vm.c`). The baked
`<prefix>_extras_match` scanner now runs under an active lexical goal too, which in TSX
is almost everywhere a goal is live. This is the standing target W18-WORDCACHE named —
extras consumption — and it lands a **−17.4% instruction cut** on the 311 KB React file
for a one-condition change. The full study is in `docs/peg-vs-glr.md` (W18-EXTRAS).

- *Why the guard was conservative.* A lexical goal only rewrites a *terminal reference
  in a rule body*: `tp_vm_goal_redirect` maps a `(goal, trigger, target)` triple so the
  same byte scans as a different token in a different context (regex-vs-divide, template
  body, JSX text). An *extra* (whitespace, comment) is never a goal trigger nor target —
  the extras loop matches each extra's fixed pattern directly and never redirects. The
  `goal` the slow fallback forwards to `tp_token_match_memo` is dead there too: the loop
  passes `memo = 0`, and `goal` is used only as a memo-probe key; the match itself
  (`tp_token_run` / `tp_match_ir`) takes no goal. The baked scanner takes no goal in its
  signature. So which bytes are trivia is a pure function of `(input, position)`,
  identical under every goal — the guard guarded nothing.

- *What it removed.* The slow generic loop re-ran `tp_token_match_memo → tp_token_run`
  (the token bytecode VM) for every extra def at every cursor under a goal. That subtree
  was the profile's largest mass (~24.6% self / 26.7% inclusive on the React file);
  `tp_token_run`'s entire 10.46% self was trivia scanning. After the change both
  `tp_token_run` and `tp_token_match_memo` drop below the callgrind threshold and
  `tp_vm_consume_extras` inclusive falls from 26.71% to ~12.2%.

- *Invariant kept.* The per-position extras memo and the lead-byte bitset reject are
  untouched and still run alongside; the memo stays gated to the no-goal case, where its
  `memoable` flag is computed, and the baked branch's memo write is inert under a goal —
  matching the slow loop's behaviour exactly.

**Measured.** Total Ir on `ReactDOMFloat-test.js` (3 passes): 546,772,019 → 451,775,969
(**−17.4%**). Single-file wall-clock (500 reps): 15.3 → 17.8 MB/s (**~16%**). Whole-corpus
bench5 flat (C-compiled 9.71 MB/s, 0.59× tsc@6.0.3, 1.22× tree-sitter) — the corpus is
setup-dominated small files, so the per-byte lexer win amortises there and lands on real
files. Output-invariant: `--dump-tree` byte-identical across the React TSX sample plus
crafted regex/template/JSX trap files, both vm-ax and C-compiled arms (md5 unchanged);
every gate green with no golden re-baseline.

## Phase 18 — W18-PRATT: build a precedence-climbing primitive, prove it byte-identical, decline it

**Decision.** Do not replace the binary-operator ladder with a precedence-climbing
(Pratt) loop. The `(precedence-climb …)` grammar primitive was built end to end — a new
IR op, VM dispatch arm, generated-C emitter, and one shared driver
(`tp_vm_prec_climb`) both engines call — and proven **byte-identical** to the ladder.
It is declined because it raises the instruction count, not lowers it. The engine and
the `std.javascript` grammar are reverted to the ladder; the full study is in
`docs/peg-vs-glr.md` (W18-PRATT).

- *What it does.* The ladder is a stack of `(hidden)` rules; reaching any operand
  descends every rung as a real frame even with no operator present. The primitive
  collapses the pure-binary rungs into one loop run once per operand chain. It had to
  reproduce the ladder's exact CST, which `--dump-tree` showed is *not* the textbook
  nested `binary_expression`: each level is its own named node, a same-precedence run is
  flat (including `**`), and a no-operator rung vanishes. The driver climbs precedence
  but groups output to match — flat same-level run, right operand at strictly tighter
  precedence — so node names, field stamping, and spans are the rungs' own.

- *Byte-identical, proven.* A thesis slice (additive + multiplicative → one climb over
  the exponent operand) is byte-identical across a 131-file React `--dump-tree`
  reference; cc-diff 271/271 (the generated-C climb matches the VM) and metric 296/296,
  no golden re-baseline.

- *Why it loses.* On `ReactDOMFloat-test.js` (3 passes) the slice measured
  **451,776,574 → 481,704,930 Ir, +6.62%**. Collapsing two rung frames cut `tp_vm_enter`
  by 9.81M — real, and the only saving. But this engine already pays for a rung cheaply
  where a Pratt loop cannot: a rung's operator check is a one-probe lead-byte bitset
  reject (no node, no rollback), and each rung is already a flat `repeat`, not recursion.
  The driver trades N cheap rejections for one loop with O(levels) speculative operator
  matching per operand; `tp_vm_climb_match_operator` ran 84,834 times (vs ~5,200 per
  rung), adding 5.09M of `consume_extras` and 9.3M of new driver self cost. After
  W17-FLAT/RECLAIM thinned the frames and W18-EXTRAS thinned the lexer, the ladder is not
  the instruction sink frame-attribution suggested.

**Reusable finding.** On an engine with left-factored rungs and a single-bitset per-rung
operator reject, a precedence-climbing loop is the wrong tool: its per-operand bookkeeping
exceeds the frame descent it removes, and the gap widens with level count because the peek
scales with it. The profile's highest-value target stays the trivia/extras path, not the
operator ladder.

## Phase 18 — wrap-up: where tinyparse stands against tsc, and what it cost to know

Phase 18 set out to close the React-corpus gap to tsc. It did, on the question that
turned out to matter, and it learned more from what it declined than from what it shipped.
The phase landed one engine win, declined three measured probes, caught and fixed a silent
data-corruption bug on the shipping parse path, and re-validated the corpus standing on the
fixed engine.

### The calibration that framed the phase

Earlier rounds had calibrated tinyparse against the two production recursive-descent
TypeScript parsers and tree-sitter. The finding that set the phase's direction: the
*fastest* parser on this workload is recursive descent (tsgo, native Go, ~37 MB/s), not
tree-sitter's GLR table (~8 MB/s). A parse table is not the fast option here; tinyparse's
residual gap is **interpretation overhead** plus its per-byte lexer, not its PEG choice.
tsc (TypeScript on V8's JIT, ~14.6 MB/s) was the bar to reach; tsgo and meriyah (~37 MB/s)
were out of reach without a different lexer architecture. So the phase chased tsc and
treated the native-compiler arms as the horizon, not the target.

### What shipped: W18-EXTRAS

One change shipped, and it was the largest single-round instruction cut in the series.
Between every token the parser scans trivia; a baked straight-line C scanner existed but was
gated behind `active_goal == TP_NO_SYMBOL`, and in TSX a lexical goal is live almost
everywhere (`div`/`slash`, JSX text, template bodies), so the fast scanner barely fired and a
generic per-extra-def loop carried the cost. The investigation showed the guard was
conservative, not load-bearing: a goal cannot change which bytes are trivia — whitespace is
whitespace under every goal; the goal only decides the next *significant* token. Dropping the
guard was a one-condition change for **−17.4% instructions** on the 311 KB React file
(546,772,019 → 451,775,969 Ir) and a ~16% wall-clock speedup on real files, **byte-identical**:
cc-diff 271/271, metric 296/296, no golden re-baseline. The full study is in
`docs/peg-vs-glr.md` (W18-EXTRAS).

### Three measured declines, each with a reusable lesson

The phase declined three probes at the thesis gate. Each taught something:

- **W18-WORDCACHE (a scan-once cursor cache).** Declined: the cache targeted a fallback path
  that is never reached. `jsx_identifier` matches outright on every call, so the `identifier`
  fallback the cache meant to collapse fires 0 of 28,103 times on the profiled file. The whole
  word-class matcher is ~1.45% of instructions, of which <0.8% is reclaimable — against a cache
  cost the retired token memo (W16-GOAL) already showed is not worth paying. *Lesson: a cache
  has no value when its miss path is dead; measure the path before optimizing it.*

- **W18-PRATT (a precedence-climbing primitive).** Built end to end and proven byte-identical
  to the operator ladder, then declined because it raised the instruction count +6.62%.
  Collapsing rung frames cut `tp_vm_enter` by 9.81M, but the Pratt loop's O(levels) speculative
  operator peek (84,834 match calls vs ~5,200 per rung) and per-chain bookkeeping cost more than
  the frames it removed. *Lesson: the operator ladder is not the instruction sink frame-attribution
  suggested. After W17-FLAT/RECLAIM thinned the frames, the ladder rungs are flat `repeat`s with a
  one-probe lead-byte operator reject — cheap repeats a Pratt loop cannot beat.*

- The standing target across all three is the same: **the trivia/extras path**, the only mass
  W18-EXTRAS partly reclaimed and the one with headroom left.

### The silent corruption bug — caught, root-caused, fixed

The phase's most important finding was not a number but a bug the benchmark's byte-identical
assertion caught. Adding a persistent-engine bench arm — one engine reused across many parses —
surfaced **silent data corruption on the shipping default parse path** (the actor-hosted VM,
`engine_use_vm_actor`): reusing one engine across many parses of a large file collapsed a
~96k-node tree to a one-node root once the arena pool had grown, with no error surfaced.

Root cause (confirmed with ASan/UBSan and pool instrumentation): `tinyparse_push` on the
actor path marshalled each input chunk through `ax_post`, whose `event_make` copies the
payload into the **fixed system slice** `tinyparse_open` carves off the grant. The ax pool's
first-fit free list let a tiny allocation claim a freed file-sized chunk, so across reused
parses the slice fragmented until a chunk-sized allocation failed: `ax_post` returned
`AX_NOMEM`, `push` returned `TINYPARSE_INVALID` — and `finish`/`run` still ran, producing a
degenerate one-node tree with no fault. Two compounding faults: a copy through the fixed slice
that should not have existed (it contradicted the actor's own "adds no copy" contract), and a
failed push that passed as a successful degenerate parse.

Fixed in commit `86c4317`: `tp_parser_actor_push` now buffers each chunk straight into the
parser's own growable arena instead of posting it as an ax event, so nothing large flows
through the system slice; and a failed push records `push_failed`, which `finish` and the
generated `<prefix>_run` honour by refusing to build a tree and returning the error, so a
dropped chunk can never again pass as a degenerate parse. The standing oracle is
`ci/tinyparse-reuse-regression.sh`, which reuses one engine across 120 parses of a generated
pool-growing `.tsx` and asserts every tree is byte-identical to the fresh parse. *Lesson: a
byte-identical assertion in a benchmark is a correctness gate, not bench scaffolding — it found
a shipping-path corruption that every conformance case missed, because the corruption only
appears across reused parses past an arena grow.*

### W18-PARITY — the honest standing on the fixed engine

The persistent-engine corpus number reported before the fix (~15.7 MB/s, "0.99× tsc") was
measured on the buggy engine, possibly over silently-degraded one-node parses, and could not
be trusted. Re-validated on the fixed engine with the byte-identical assertion **passing for
all 1070 files**: the persistent-engine arm parses the corpus at **16.48 MB/s = 1.13× tsc,
2.03× tree-sitter**, and the fresh-per-file (RSS-bounded) arm at 11.04 MB/s = 0.76× tsc. The
gap between the two arms — not parse speed — is the entire corpus shortfall, and it is the
per-file engine-reopen / grammar-reload artifact behind the old "0.65× tsc" headline. Across
file sizes tinyparse parse-only is at or above tsc at every size measured (2.06× at 1 KB,
1.14× at 311 KB). Both arms are now reported by `ci/tinyparse-bench5.sh`, with the assertion
in the harness so a corrupt parse can never be timed silently again. The full study is in
`docs/peg-vs-glr.md` (W18-PARITY) and `docs/performance.md` (W18-PARITY).

### Honest conclusion, and the one lever left

On the fixed engine, driven the way a host actually parses a codebase, **tinyparse is no
longer behind tsc on the React corpus** — it is past it at 1.13×, and ahead of tsc on every
single-file size. It remains comfortably past tree-sitter (the phase floor) and well short of
the native-compiler arms tsgo and meriyah (~37 MB/s). Closing *that* distance, or pushing the
corpus well past tsc, would need a generated DFA lexer in place of the per-byte trivia scanner
— the token layer tsc's JIT and tree-sitter's table both have and tinyparse does not. That is
a separate project and is not committed: a maximal-munch DFA is unsafe to bake naively from the
grammar's terminals (the longest-match rule a hand-lexer encodes is not free to derive), and it
is recorded as future work. The deliverable of Phase 18 is the shipped W18-EXTRAS win, the three
declines with their lessons, the corruption fix with its regression gate, and the re-validated
standing at parity-or-better with tsc.
