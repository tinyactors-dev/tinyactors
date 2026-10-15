# staticjs: rethinking the compiler; moves from liveness over bytecode

**Status:** Sketch (2026-10-15). A design for discussion; nothing here is
decided or scheduled.
**Touches:** `staticjs/src/compiler.c` (how it decides moves; the parser,
the events, the bytecode and the evaluator stay), and [exceptions and
async](staticjs-exceptions-and-async.md), whose move table it replaces.
**Not a plan:** ADRs and roadmaps win.

The question was whether staticjs needs a different compiler, now that
`try` and `async` strain the streaming one. The first draft of this
document recommended holding each function's events and deciding with
passes before emitting (option B below). A review against the code
refuted that draft: the pieces did not fit together, and a much cheaper
change answers the same problem. This version records both.

## Whom it serves, who pays, what is out

- **First, the maintainers.** Moves are where every new feature has had
  to add a deferral (parked reads, fences, cancels, the append and
  `store_slot` cases), and where `try` would add more ([the move
  table](staticjs-exceptions-and-async.md#the-move-problem)).
- **Then the application developer:** moves the streaming compiler gives
  up on today, or would give up inside a `try`, become provable, and code
  stays linear where it now copies.
- **Who pays:** one reverse walk over each function's bytecode when it
  ends, a live set per jump target, and a decoder of instruction lengths.
  Programs run the same evaluator; a few reads move that copy today.
  The public API does not change.
- **Out of scope:** the other decisions the compiler patches (path roots,
  mutating methods, rewrites). They are made by the end of the call they
  belong to, the same size before and after, and no feature in sight
  needs them later. Also out: a new bytecode, SSA, a tree interpreter,
  and loops.

## The frame: what the compiler is today

1. **One pass, an instruction per event**, jumps patched when their
   construct ends (`src/compiler.c:1`). A nested function's code is
   emitted inline in its parent's, behind a `JUMP` over it
   (`begin_function`, `src/compiler.c:1460`; `end_function`, `:1528`),
   with absolute jump targets and function offsets.
2. **Moves are decided as events arrive and taken back later.** The last
   `LOCAL` of each slot is remembered and flipped to `TAKE_LOCAL` when its
   block or function ends (`src/compiler.c:823`); a later read, a
   capture or a write cancels it (`forget_read`, `:896`); branches park
   their last reads behind fences (`:795` to `:960`); the append and a
   `let`'s reassignment from its own value flip a read of their own
   (`:2865`, `store_slot` at `:908`).
3. **Every jump goes forward** (`src/compiler.c:5`). The six jumps
   (`SJS_OP_JUMP` and its conditional forms, `src/internal.h:1158`) carry
   an absolute target greater than their own `pc`.
4. **A slot is touched by seven opcodes**: `LOCAL`, `TAKE_LOCAL`,
   `SET_LOCAL`, `PATTERN_COMPUTED`, `DEFAULT_LOCAL`, `RECEIVER` and
   `SORTED` (`src/vm.c`, each reads `f->base + operand`), and by
   `CLOSURE`, which copies captures. A `RECEIVER` pushes a reference; the
   slot is used where the `PATH`, `MUTATE` or `PATH_CALL` that consumes it
   runs, and the compiler already pairs them (`k->root_pc`).
5. **The other patches are local.** A path root, a mutating method and a
   rewrite are each decided by the end of their own call or assignment
   (`src/compiler.c:2248`, `:2283`, `:2434`, `:2474`, `:2806`), and some
   read the code just emitted (`assign_callee`, `:2328`; `path_callee`,
   `:1220`; `ends_in_return`, `:2603`).

## The problem, narrowed

Of the decisions in fact 2 and fact 5, only moves need facts from far
away: whether any later access, on any path, reads the slot. Everything
`try` and `async` break in the streaming compiler is a move (the
exceptions sketch's table: a `return` in a `try`, `store_slot`, the
append, the late take). The other patches are not in the way.

## The answer: liveness over the function's bytecode, at its end

Keep the streaming compiler as it is, minus fact 2's machinery. When a
function's code is complete (its `end_function`, or the program's end
for the main function), walk its bytecode **backwards**, from its last
instruction to its first, computing the slots live after each
instruction:

- Because every jump goes forward (fact 3), an instruction's successors
  (the next instruction, a jump's target) lie at higher `pc`s, so a
  reverse walk has seen them all: one walk, no fixed point. The live set
  after a jump is the union of the set at its target, recorded as the
  walk passed it, and the set falling through.
- `RETURN` has no successor: nothing is live after it.
- A read (`LOCAL`) of a slot not live after it becomes a `TAKE_LOCAL`, in
  place: the same size.
- A write (`SET_LOCAL`, `PATTERN_COMPUTED`'s store, `DEFAULT_LOCAL`'s)
  kills the slot; a read makes it live; `CLOSURE` makes its captured
  slots live; a `RECEIVER` makes its slot live where its consumer runs
  (the pairing of fact 4).
- A nested function's code is skipped: its `JUMP` over it lands after it,
  and its own walk ran when it ended.
- A module's slots are live at its end when exported, as exports are read
  then.

**With `try`**, the handler and the `finally` come after the region they
protect (exceptions sketch, part 1), so their live-in sets are known when
the walk enters the region from its end: every instruction in the region
that can throw has the handler's live-in as a second successor. That one
rule replaces the move table: a read inside the `try` whose slot the
handler or the code after it reads is live there, and stays a copy;
`return`, `store_slot` and the append need nothing of their own. **The
late take** (exceptions sketch, costs) becomes a question of the same
walk: whether anything between a read and its consumer can throw.

**With `async`**, an `AWAIT` is an ordinary instruction to this walk: the
frame's slots move with the activation and come back.

What it costs: one live set per jump target and one running set, a bit
per slot (a function has at most `max_locals`); a reverse walk needs the
instruction boundaries, so either a decoder of operand lengths run
forwards first, or a list of instruction starts the emitter records. Both
are linear in the function's code.

What it removes: the last-read machinery of fact 2, about 160 lines of
`compiler.c` (`:795` to `:960`) and the move parts of the append and
`store_slot`, with `mutation.md`'s "Last reads in branches" and the move
rules of "let" and "Appending to a let" reduced to one sentence.

## It is not the same bytecode, and that is the point

Liveness moves reads the streaming rules keep: a read followed by a write
of its slot (`let s = [a]; const t = [s]; s = [2];` copies `s` into `t`
today, as a write cancels the earlier read, `src/compiler.c:896`, and
moves it under liveness). Every difference is in one direction: a copy
becomes a move. A move gives the reader a value nothing else holds, so a
mutation that faulted "held elsewhere" may now succeed where nothing can
see the difference, as no later read exists; a fault becoming the value
JavaScript gives is the direction the rule allows. The test is therefore
not "identical bytecode" but:

- the spec corpus compiled both ways, every difference listed, and each
  one a `LOCAL` that became a `TAKE_LOCAL` (any other difference is a
  bug);
- `mise run test`, `diff-node`, and the fuel of every bench case equal or
  lower.

**Unverified until that spike runs:** that every slot effect is visible
in the bytecode (fact 4 lists them from the evaluator; `PATTERN_COMPUTED`,
`DEFAULT_LOCAL` and `SORTED` need checking against what the compiler's
rules assume), and how many reads change.

## Options considered

| | What | Verdict |
|---|---|---|
| A | Keep streaming, add a deferral per feature (the exceptions sketch as first written) | works, at the cost of a state machine per feature and their interactions |
| **E** | **Keep streaming; decide moves by liveness over each function's bytecode at its end** | **recommended**: answers every move problem with one rule, at the cost of a walk per function |
| B | Hold each function's events, decide with passes, then emit (this document's first draft) | refuted below |
| C | An IR with a control-flow graph and SSA ([tinyjs](../research/tinyjs.md)) | a new middle end and emitter for structured control flow that does not need one |
| D | Interpret the syntax tree ([tinyjs, the small way](../research/tinyjs-small.md)) | gives up the tuned bytecode evaluator, its fuel and its shapes for a problem in the compiler |

### Why B was dropped

The review found, each with a spike or a line:

- **Nested functions cannot be processed first.** Their code sits inline
  in the parent's, with absolute jumps and offsets (fact 1), their names
  and captures resolve against the parent's open scopes
  (`pending_captures`, `src/compiler.c:1473`), and function indices are
  public and in source order ("each arrow function in the order its
  source begins", `include/staticjs.h:930`). Holding only the outermost
  function works; for an expression program that is the whole program.
- **A module cannot be processed per statement.** Its moves span
  statements (`const b = [a]` moves `a` until a later statement reads it),
  and exports are read at its end.
- **Errors would come in another order.** `compile_fail` stops the parser
  at the event today; a names pass at a function's end would report a
  later syntax error first. The spec cases check which error wins.
- **Literals are code, not constants.** `[1, 2, 3, 4]` compiles to
  `ARRAY_NEW` and a push per element (`src/compiler.c:3110`), so a large
  literal would sit in the buffer whole.
- **Memory is worse than drafted.** About 3 to 10 bytes per source byte
  on real modules, but 32 bytes per byte on dense code (`[0,0,...]`,
  `a+a+...`), on top of today's peak of about 50 there; records need about
  40 bytes, not 32, and names' texts must be copied.
- **Several decisions read emitted code** (fact 5), so a pass before
  emission would reimplement them over events.
- **Event order is not slot-access order.** A path's root comes first but
  is used at the call's end: `s.push(s.length)` reads `s` last by events,
  and must not move. The walk would need a rule per construct, which is
  the machinery it meant to remove. Over bytecode the pairing of fact 4
  is one table.

## Lifetimes

| Object | Owner | Made | Read by | Ends |
|---|---|---|---|---|
| live set at each jump target | the compiler | when the reverse walk passes the target | the jumps to it, met later in the walk | at the function's end |
| running live set | the compiler | at the walk's start | each instruction | at the walk's end |
| instruction starts (if recorded) | the compiler | as the emitter emits | the reverse walk | at the function's end |
| `RECEIVER`-to-consumer pairs | the compiler, as now (`k->root_pc`) | at the call's end | the walk | at the function's end |

## Order of work

1. **Spike:** the reverse walk over the bytecode of every spec case,
   reporting where it disagrees with today's `TAKE_LOCAL`s, without
   changing them. Settles fact 4 and the size of the change.
2. **Switch:** let the walk decide moves; delete the last-read machinery.
   The tests above, `mutation.spec` first, and `bench-compare --quick`
   for `compile` and the cases whose fuel changes.
3. Then [exceptions and async](staticjs-exceptions-and-async.md), with
   the handler as a successor in the walk.

## Open questions

- Recorded instruction starts, or a forward decode before the walk.
- Whether a module's walk runs once at its end over all its top-level
  code, as its last reads are decided at its end today.
- Whether to revisit the other patches (fact 5) later, if a feature needs
  them decided from far away. None does today.
