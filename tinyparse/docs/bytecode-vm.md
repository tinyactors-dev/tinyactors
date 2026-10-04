# A bytecode VM for pocketparse parsing

This document is a design analysis, not a change. It asks whether replacing
pocketparse's recursive-descent IR walk with a bytecode virtual machine would
cut the residual parse cost, sketches the instruction set and VM state that
would do it, works one grammar fragment through the lowering, and then spends
most of its length on the question that decides where the code should live: is a
bytecode VM something `ax` should offer in general, or is it specific to
parsing? The hard constraint throughout is that the VM runs **on** the `ax`
actor engine — it does not bypass or replace it.

The companion documents are [`performance.md`](performance.md), which measured
the residual this proposal targets, [`architecture.md`](architecture.md), which
draws the layer boundaries, and the two design contracts,
[`../pocketparse-design.md`](../pocketparse-design.md) and
[`../../../src/ax/ax-design.md`](../../../src/ax/ax-design.md). Untagged code
blocks below are illustrative; `mdc` extracts nothing from this file.

## Abstract

pocketparse parses by walking an in-memory IR tree with a recursive C executor
(`pp_parse` in [`../src/parse.c`](../src/parse.c), `pp_match_ir` in
[`../src/token.c`](../src/token.c)). After the W44–W46 optimizations took the
gap to tree-sitter from ~45× to ~4.6×, the profile attributes what remains to
the *shape* of that executor: a function call and a pointer dereference per IR
node, a `switch` on an opcode reached through two levels of indirection, and a
branch history the predictor cannot learn because every node visit dispatches
from a different call site. A bytecode VM replaces the tree with a flat
instruction stream and the recursion with one dispatch loop. The realistic
payoff is interpreter-VM territory — **2–5× over a tree walk**, plausibly
landing pocketparse near 1.5–2.5× tree-sitter — and it will not reach
tree-sitter's generated-table speed, because pocketparse keeps runtime grammar
compilation and that is the whole point of the project. The recommendation, in
one line: **build the VM as a pocketparse layer, design it as a clean reusable
pattern, and do not add a generic instruction-actor primitive to `ax` until
several independent interpreter use cases actually exist.** The rest argues each
clause.

## The problem the IR walk leaves behind

The residual cost is not redundant work — W45's match memo and W46's first-set
prediction removed the speculative re-scans that once dominated. What is left is
the *interpreter overhead of the structure itself*. Three costs compound, none
of them algorithmic:

- **A call frame per node.** `pp_parse` recurses through `PP_OP_SEQ`,
  `PP_OP_CHOICE`, `PP_OP_REPEAT`, `PP_OP_FIELD`, and the rest. A JavaScript
  `statement` is a choice over ~19 alternatives, each a `seq` of refs and
  literals; matching one walks dozens of nodes, and each interior node is a C
  call with its own prologue, its save/restore of the backtrack marks
  (`saved_count`, `saved_pos`, `saved_sink`), and its return.
- **Pointer chasing with no locality.** An IR node holds `children` as a
  separate arena allocation of `pp_ir *` pointers (`pp_ir` in
  [`../src/internal.h`](../src/internal.h)); each child is itself a scattered
  arena node. Walking a rule body is a tour of the heap, and the working set is
  the whole reachable IR, not a window over it.
- **Unpredictable dispatch.** The `switch (ir->op)` is reached from a single
  recursive function, so the indirect branch the compiler emits for it sees the
  full mix of opcodes from one site. The predictor never specializes; every
  node pays a likely misprediction.

A bytecode VM attacks all three at once. The grammar compiles to a contiguous
array of fixed-width instructions. Dispatch is one loop — `for (;;) switch
(program[pc].op)` or a computed-goto threaded dispatch where the compiler allows
it — so the program counter sweeps memory the prefetcher likes, the backtrack
state lives in explicit arena stacks rather than C call frames, and rule entry
is a `Call` instruction that pushes a return address instead of a C recursion.
The opcode stream is cache-local and the dispatch site, repeated, gives the
predictor something to learn.

What it does not buy is a different algorithm. The VM still runs an ordered-
choice PEG with backtracking; it still consults the same memo. It runs the same
work with a far smaller constant factor. That ceiling is why the honest estimate
is a multiple, not an order of magnitude, and why it stops short of
tree-sitter: tree-sitter spends its parse in a generated LR/GLR table indexed by
state and lookahead, a fundamentally lower-overhead machine that pocketparse
gave up the moment it chose to compile grammars at runtime. The VM is the middle
ground — a flat, fast interpreter that keeps the runtime-compilation promise the
table-generator design cannot.

## The instruction set

The prior art is Ierusalimschy's LPeg parsing machine: a PEG compiles to
instructions `Char`, `Set`, `Any` for terminals; `Choice`, `Commit`,
`PartialCommit`, `BackCommit`, `FailTwice` for ordered choice and repetition;
`Call`/`Return` for rules; `Fail` and a backtrack stack; `Span` and `Capture`
for the extraction LPeg does instead of building a tree. We adapt it rather than
copy it, because pocketparse does more than accept-or-reject: it builds a
concrete syntax tree with fields, runs parser-directed lexing, handles extras,
guards reserved words, and recovers. Those become instructions too.

### Matching terminals

pocketparse's terminals are not single bytes; they are compiled token patterns
(`pp_token_def.pattern`). The VM keeps the existing token IR matcher as a
*subroutine* the dispatch loop calls, rather than lowering every character class
to `Set`. One instruction covers it:

```
MATCH  tok          ; run token tok's pattern at the cursor via the memo;
                    ; on match advance the cursor, on no-match FAIL
LITERAL lit         ; the same for a bare literal (its pattern is the bytes)
EOF                 ; succeed iff the cursor is at end of input, else FAIL
```

`MATCH` is exactly `pp_token_match_memo` — first-byte dispatch, the
`(pattern, offset, goal)` memo, the interpreter fallback — invoked from one
call site instead of from the leaves of a tree walk. This is the single most
important design decision in the ISA: **the hot lexer stays as it is.** W45's
work is not re-implemented; it is called.

### Ordered choice and repetition

Choice is two instructions bracketing the alternatives, with a backtrack stack
underneath:

```
CHOICE  L_next      ; push a backtrack entry {pc=L_next, cursor, marks}
... alternative A ...
COMMIT  L_done      ; pop the backtrack entry, jump to L_done
L_next:
... alternative B ...
COMMIT  L_done
L_next2:
FAIL                ; no alternative left: pop and resume the next backtrack
L_done:
```

`FAIL` is the engine's spine: it pops the top backtrack entry, restores the
cursor and the tree-builder marks it saved, and jumps to its `pc`. A `MATCH`
that does not match executes `FAIL` implicitly. Repetition is choice without the
commit — `PartialCommit` rewrites the top backtrack entry's cursor in place and
loops, the LPeg trick that makes `repeat` iterate without growing the stack.
`optional` is a `CHOICE`/`COMMIT` pair around one alternative.

The W46 first-set predictor folds directly into `CHOICE`. Either the compiler
threads a per-alternative first-set table that `CHOICE` consults to skip a
provably-impossible branch before pushing its backtrack entry, or it emits an
explicit `PREDICT set, L_skip` ahead of each alternative. The table is the same
`choice_first_set` already computed in [`../src/token.c`](../src/token.c),
attached to the program instead of to the IR node.

### Rules

```
CALL    R           ; push return address; open a CST node for rule R; jump to R
RETURN              ; close the node, extend its span, link children; pop, resume
```

`CALL`/`RETURN` replace `pp_parse_rule_ref`'s C recursion with an explicit
return stack. The CST bookkeeping that function does by hand — push a node,
parse the body into a fresh child sink, link children, set the end byte — splits
across `CALL` (open) and `RETURN` (close), with the child sink living on the
VM's capture stack (below).

### CST construction, fields, aliases

The tree is built by instructions, not as a side effect of recursion:

```
OPEN    kind        ; begin a node of kind; remember the child-sink mark
CLOSE               ; finish it: link the children gathered since OPEN, set span
FIELD   name        ; tag the next produced child with field name
ALIAS   kind        ; report the next produced node under an alias kind
```

`OPEN`/`CLOSE` are what `CALL`/`RETURN` use internally; exposing them lets a
`seq` that should not become its own rule still group children. `FIELD` and
`ALIAS` set a one-shot decoration consumed by the next `OPEN` or `MATCH`,
mirroring how `pp_parse` threads `field` and `alias` arguments down the
recursion today.

### pocketparse specifics

The features that distinguish pocketparse from a plain PEG each get an
instruction, and each is deliberately *coarse* — a single opcode that brackets a
region, so the hot loop stays tight and the rare feature costs one instruction
at its boundary, not a per-node check:

```
GOAL_PUSH g / GOAL_POP        ; (goal NAME X): set/restore the active lexical
                              ; goal around a region; MATCH reads it for redirects
EXTRAS_PUSH mode / EXTRAS_POP ; (rule ... extras: none/all ...): toggle implicit
                              ; extras consumption around a region
EXTRAS                        ; consume trivia/skip/error here (emit nodes), via
                              ; the W46 extras memo; usually folded into MATCH/CHOICE
RESERVED set                  ; reject the just-matched slice if it spells a
                              ; reserved word (the (reserved ...) whole-slice check)
RECOVER_ARM L / RECOVER_DISARM; mark a region recoverable; on FAIL at EOF inside
                              ; it, run INSERT instead of unwinding
INSERT  kind                  ; emit a zero-width MISSING node of kind + diagnostic
```

`GOAL_PUSH`/`GOAL_POP` and `EXTRAS_PUSH`/`EXTRAS_POP` are the bracket form of
`PP_OP_GOAL` and `PP_OP_NO_EXTRAS`: the executor today saves and restores
`active_goal` and `no_extras` around a child; the VM does it with a paired
instruction and a small mode stack. `RESERVED` is `PP_OP_NOT_KEYWORD` after its
inner match. The recovery pair models `PP_OP_RECOVER`: arming sets a flag the
`FAIL` handler checks, so a required element missing at end of input runs
`INSERT` (synthesizing the MISSING node `pp_recover_insert` produces) and
continues rather than unwinding the whole parse.

Two subtleties worth flagging. First, extras consumption happens between every
element today; in the VM it is cheaper to fold the "consume leading extras" step
into `MATCH` and `CHOICE` (which already consult the extras memo) than to emit a
standalone `EXTRAS` before each element — the bytecode stays dense and the memo
does the work. Second, `CHOICE` must disarm recovery for its speculative
alternatives exactly as `PP_OP_CHOICE` saves and clears `ctx->recover` today;
the backtrack entry carries the recovery flag so `FAIL` restores it.

### A worked lowering

Take the design's running example:

```scheme
(rule List
  (seq
    (field open: "(")
    (repeat Form)
    (field close: ")")))
```

It lowers to a contiguous block (labels are byte offsets the assembler resolves;
`Form` and `List` are program addresses):

```
List:
  OPEN    List
  FIELD   open
  LITERAL "("            ; FAIL here aborts List (no CHOICE above it)
L_rep:
  CHOICE  L_endrep       ; speculatively try another Form
  CALL    Form
  PARTIAL_COMMIT L_rep   ; matched: rewrite backtrack cursor, loop
L_endrep:
  FIELD   close
  LITERAL ")"
  CLOSE
  RETURN
```

Reading it against `pp_parse`: `OPEN`/`CLOSE`/`RETURN` are the node push, child
link, and span fixup `pp_parse_rule_ref` performs; the `CHOICE`/`CALL`/
`PARTIAL_COMMIT` loop is `PP_OP_REPEAT` with its per-iteration mark save turned
into one backtrack entry rewritten in place; `FIELD` carries the field symbol
`PP_OP_FIELD` threads down. The whole rule is nine instructions in one cache
line or two, walked by one loop, versus a recursive descent through a seq node,
two field nodes, a repeat node, a ref node, and two literal nodes — each a call,
each a separate arena allocation.

A choice-heavy rule shows the prediction win:

```scheme
(rule Statement
  (choice IfStatement WhileStatement ReturnStatement ... Block))
```

```
Statement:
  OPEN    Statement
  CHOICE  L1
  PREDICT {i}            ; first-set of IfStatement: skip unless lookahead is 'i'
  CALL    IfStatement
  COMMIT  L_done
L1:
  CHOICE  L2
  PREDICT {w}
  CALL    WhileStatement
  COMMIT  L_done
L2: ...
```

`PREDICT` is W46 made explicit: nineteen alternatives, eighteen skipped in one
byte-test each before any backtrack entry is pushed. Today that test lives in
`pp_choice_first_admits`; in the VM it is an instruction the dispatch loop runs
without a function call.

## VM state and how memoization maps

The VM's entire mutable state is a handful of cursors and three arena-allocated
stacks, all freestanding, all carved from the parser arena exactly as the memo
and builder are today:

- **`pc`** — index into the program (the compiled instruction array).
- **`cursor`** — the input offset, today's `ctx->position`.
- **backtrack stack** — entries of `{pc, cursor, builder_mark, sink_mark,
  goal, extras_mode, recover_flag}`. This is the explicit form of the
  `saved_*` locals scattered through `pp_parse`'s `CHOICE`/`REPEAT`/`OPTIONAL`
  cases; `FAIL` restores from it.
- **return stack** — `{pc, open_node, sink_mark}` for `CALL`/`RETURN`, the
  explicit form of the C call stack `pp_parse_rule_ref` uses.
- **capture / CST-builder stack** — the child sinks (`pp_sink`) being filled,
  one frame per open node. `OPEN` pushes a sink mark, `CLOSE` links the
  children gathered since.

The two memos carry over unchanged because they are keyed on data the VM still
has. The **W45 token-match memo** is keyed on `(pattern, offset, goal)`; `MATCH`
passes the token's pattern pointer, the cursor, and the active goal — the same
triple — so `pp_token_match_memo` works verbatim. The **W46 first-set
prediction** is consulted at `CHOICE`/`PREDICT`; the set moves from the IR node
to the program, computed once at lowering time. The **extras memo** is consulted
inside the folded extras step. Nothing about memoization is re-invented; the VM
changes how instructions are *dispatched*, not what the lexer and the caches
compute.

A note on the packrat memo that [`performance.md`](performance.md) lists as
unspent: a whole-rule memo keyed on `(rule, offset)` maps cleanly onto
`CALL` — consult it on entry, and on a hit replay the cached node span instead
of running the body. The VM does not require it, but it leaves a natural seam
for it, which the tree walk does not (the tree walk would have to thread the
cache through every recursive frame).

## Running on the ax actor engine

This is the crux, and it has a wrinkle the profile names plainly: **today's
`pp_execute` is not running on `ax` at all.** The profile in
[`performance.md`](performance.md) reports "ax actor dispatch: absent — no
`ax_*` function except `ax_alloc` appears anywhere on the hot path." The design
([`../pocketparse-design.md`](../pocketparse-design.md), *Actor model*) intends
rules to lower to `ax` machines, but the shipped executor is a plain recursive
C function over the IR. So "the VM must run on `ax`" is a constraint on the
*architecture the design commits to*, not a description of the current hot path.
The VM must honor the actor shape the design promises without reintroducing the
per-message cost the design itself warns against ("the implementation should
avoid making the hot path stringy ... compact event payloads, not dynamically
formatted event names").

The resolution is the same one every fast VM-on-a-scheduler uses: **the actor
boundary is coarse; the dispatch loop is fine.** Concretely, drawing on
`ax`'s actual model ([`../../../src/ax/ax-design.md`](../../../src/ax/ax-design.md)):

- **The parser is an `ax` actor.** Its `ax_data` (the per-instance state block
  the spec reserves) holds the VM state — `pc`, `cursor`, the three stacks —
  and its `ax_heap` arena holds the program, the builder, and the memos. This
  is precisely `ax`'s "actor data holds parse state, captures, CST builder
  cursors, and rule-local data."
- **`push`/`finish` are messages; instructions are not.** A
  `pocketparse_push(chunk)` posts one event to the parser actor. The transition
  that handles it runs the dispatch loop — *many thousands of instructions* —
  until the loop blocks on "need more input" or reaches the end of the chunk,
  then the transition returns and the actor settles. `ax_step` is pumped to
  quiescence per push, exactly as the design's *Operational model* says. The
  message rate is one per chunk, not one per byte and emphatically not one per
  instruction. This is what keeps the hot path tight: `ax`'s scheduler,
  mailbox, and event-matching machinery run at chunk granularity, where their
  cost is negligible, while the instruction loop runs as a tight C `switch`
  inside a single transition action.
- **Plugs and bindings stay at the boundaries the design already names.** A
  `MATCH` whose token is a `(class NAME)` calls a host *binding* directly (a C
  function pointer), as it does today — bindings are explicitly "for hot
  predicates," not message-style. Tracing, diagnostics, and module loading go
  through *plugs* (backed by `ax` ports), which fire on cold/structural events,
  never per instruction. The VM changes none of this; it inherits the existing
  plug/binding split, which the design drew precisely to keep the hot path off
  the message bus.

What, then, is genuinely a message versus an in-loop instruction? The dividing
line is **does it cross the actor boundary or suspend the loop:**

| In the dispatch loop (no message) | An `ax` message / boundary crossing |
|---|---|
| `MATCH`, `LITERAL`, `EOF`, `CHOICE`, `COMMIT`, `FAIL`, `CALL`, `RETURN`, `OPEN`, `CLOSE`, `FIELD`, `ALIAS`, `PREDICT`, `GOAL_*`, `EXTRAS_*`, `RESERVED` | `pocketparse_push(chunk)` / `pocketparse_finish` arriving as events |
| Reading the active goal, consulting either memo | A `(class NAME)` binding call (direct, not a message, but a host boundary) |
| Synthesizing a MISSING node (`INSERT`) | A trace / diagnostic event out through a plug |
| Restoring backtrack state | Module load for an `(import …)` during compile (a plug round-trip) |

### Sub-parses: a child actor or a VM call

A nested grammar fits the model two ways, and the choice is a real design
tension rather than a settled answer:

- **As a VM `CALL`** (the default). A nested rule, the body of a `repeat`, an
  ordinary sub-rule — these are `CALL`/`RETURN` within one program, on one
  actor, with no message. This is right for everything that shares the parser's
  input cursor and grammar. It is the hot path and it must not touch `ax`.
- **As a child `ax` actor** (the exception). A genuinely *separate* parse —
  the design's quasiquote example, where a template literal's content is parsed
  by the JavaScript grammar as a nested parse with `${…}` holes spliced in, or
  a lexical construct that wants its own program and its own input slice — maps
  to spawning a child actor running its own VM over its own program. The
  parent's state hosting the child gives "shutdown by geometry": the child is
  cancelled when the state is left, and `done.child.<id>` reports completion.
  This is `ax`'s `<invoke>` model used exactly as designed, and it is *coarse*
  — one spawn per nested parse, not per rule — so its cost is amortized over
  the whole sub-parse.

The discipline that keeps the actor shape honest without paying for it: a
sub-parse becomes a child actor only when it is conceptually a *different
parser* (different program, different input span, independent completion); a
sub-rule of the same grammar over the same input is always a VM `CALL`. Getting
this boundary wrong in either direction is the main risk of the actor
integration — spawn too eagerly and the message overhead the profile warns
about returns; spawn too rarely and the quasiquote/lexical-goal cases lose the
clean child-completion semantics the recovery and trace machinery want.

## The generalization question

Should `ax` grow a general "bytecode/instruction-stream interpreter actor"
facility, or does this belong only in pocketparse? This is the document's
center, and the two sides both follow from `ax`'s stated design rules.

### The case for generalizing into `ax`

`ax` actors are compiled state machines whose datamodel is C and whose behavior
is a tree of states and transitions. A bytecode VM is *also* a way to give an
actor behavior — an instruction stream instead of a transition table. Several
plausible `ax` use cases are compute/interpreter-shaped rather than
event-driven:

- a regex engine (an NFA/DFA or a Thompson VM),
- a binary/protocol parser (the design's PNG example),
- an expression evaluator or a small query engine,
- the linearized-tree query matcher pocketparse already builds (*Queries as
  parsers over linearized trees* — itself a parser, hence itself a VM
  candidate),
- a general VM a host wants to embed and make durable.

If `ax` offered a blessed "this actor's behavior is this instruction stream,
pumped N steps per `ax_step`" primitive, all of these would share one tested,
durable, offload-aware engine. The durability story is genuinely attractive:
`ax`'s record/replay (`ax_record`/`ax_replay`/`ax_live`) would make a long-
running VM checkpointable and resumable for free, and `ax_evict` would page out
a paused VM — capabilities no ad-hoc interpreter loop gets.

### The case against — and it is stronger

`ax`'s design rules cut hard the other way:

- **Small core.** The header is one document, deliberately tiny ("Think
  SQLite, but for actors"). A bytecode facility means an instruction format, a
  dispatch contract, a per-VM-state convention — a second behavior model beside
  the state-machine one. That is a large surface for a core whose
  *Deliberate Non-Features* list already refuses macros, an expression
  language, and serialization formats. A general instruction set is exactly the
  kind of thing that list exists to keep out.
- **Actor semantics over opaque bytes.** `ax`'s rule is "payloads are opaque
  bytes; the core never reads them." A bytecode VM in the core would invert
  this: the core *would* interpret bytes, becoming an evaluator. That is a
  different kind of system. `ax` routes and schedules; it does not compute on
  payloads, by design.
- **No leaking `ax` into the public API.** pocketparse's own contract says
  `ax` powers the runtime but never appears in the public surface. A generic
  `ax` instruction-actor would tempt every consumer to reach for it, widening
  `ax`'s public commitments for a feature only one consumer has so far needed.
- **The use cases are speculative.** Today there is exactly *one* concrete
  interpreter-shaped need: this parsing VM. The query matcher is the same need
  (it is a parser), so it is the same use case, not a second one. The regex,
  binary-parser, and evaluator cases are real but unbuilt. Designing a core
  primitive for one realized use case and a list of plausible ones is how small
  cores stop being small.

There is also a clean architectural fact: a VM does not *need* to be in `ax` to
*use* `ax`. The pattern above — actor holds VM state in `ax_data`, messages at
chunk boundaries, tight loop inside a transition action — is implementable
entirely on the existing `ax` API. `ax` already gives the VM everything it
needs (per-actor arena, scheduler, durability, offload, child actors for
sub-parses) without `ax` knowing what an instruction is.

### Pattern versus primitive

The distinction that resolves the tension: a **pattern** is a reusable *shape*
— "run an instruction loop inside a transition action, message only at coarse
boundaries, keep VM state in `ax_data`" — that any `ax` consumer can copy. A
**core primitive** is something added to `ax.h` itself, with the maintenance,
documentation, and API-stability weight that implies. The parsing VM wants the
pattern. It does not want, and cannot yet justify, the primitive.

### Recommendation

1. **Build the bytecode VM as a pocketparse layer.** It lives under
   `tools/pocketparse/src/`, compiles the existing IR to bytecode, and runs on
   `ax` through the public `ax` API. `ax` does not change.
2. **Design the VM-actor as a clean, documented pattern**, so a future regex
   engine or binary parser can copy the shape rather than reinvent it. The
   pattern is the deliverable beyond the parser itself: the coarse-message /
   tight-loop / state-in-`ax_data` discipline, written down.
3. **Adopt a generic instruction-actor in `ax` core only if and when N
   independent interpreter use cases appear** — concretely, when at least two
   or three *unrelated* consumers (not "parser and the query engine," which are
   one) have each built the pattern and the duplication is real and painful.
   Until then, the pattern documented once is cheaper than a primitive
   maintained forever. The criterion is independence and realization, not
   plausibility: a list of things that *could* be VMs is not evidence; two
   shipped ones that *are* is.

The bet mirrors `ax`'s own: keep the core narrow, let capability accrete in the
layer that needs it, and promote to the core only on demonstrated, repeated
demand.

## Migration, risk, and whether it is worth doing

### Coexistence and incremental path

The VM can live beside the IR walk behind the same public API. Both consume the
same compiled grammar; the VM adds a lowering pass (IR → bytecode) and an
execution mode. `pocketparse_finish` chooses which executor to run — a flag, or
a build-time switch — and the public surface does not move. The conformance
suite (`ci/pocketparse-metric.sh`, 291/291) is the differential oracle: the VM
is correct exactly when it produces the byte-identical CST the IR walk produces
on every corpus case. The incremental path:

1. Lower a subset of opcodes; run the VM only for grammars that use that subset,
   falling back to the IR walk otherwise. Prove byte-identical output.
2. Extend the opcode set until the VM covers the full IR, the perf gate
   (`ci/pocketparse-perf-gate.sh`) confirming each step does not regress.
3. Make the VM the default once it dominates the IR walk on the benchmark and
   matches it on every conformance case; keep the IR walk as a reference
   executor and a debugging oracle.

### Risks

- **Debuggability.** A tree walk is readable in a debugger — the recursion *is*
  the grammar. A flat instruction stream is not; a fault at `pc=4093` means
  nothing without tooling. Mitigation: keep the IR walk as the reference, emit
  source-mapped disassembly, and lean on the differential suite so divergence
  is caught as a wrong tree, not a wrong instruction.
- **Complexity.** This is a second executor, a lowering pass, and a new state
  representation, against a system that is *already good enough at ~4.6×*. The
  complexity is permanent; the speedup is a constant factor.
- **The freestanding constraint.** Everything — program, stacks, memos — must
  be arena-allocated from the host grant, no libc, no recursion-as-C-stack
  (the explicit return stack is partly *why* the VM fits freestanding: it
  bounds stack depth in arena memory the host budgeted, where deep recursion in
  `pp_parse` today risks the C stack on a pathological grammar). Computed-goto
  threaded dispatch is a GCC/Clang extension and the project is strict C99 for
  `ax`; the VM would use a plain `switch` to stay portable, giving up the last
  increment of dispatch speed.
- **Byte-identical behavior.** Recovery, extras emission order, trivia
  placement, and field tagging must match the IR walk exactly. The subtle cases
  — extras folded into `MATCH`, recovery disarmed across `CHOICE`, the
  whole-slice `RESERVED` check — are where a divergence would hide.

### Is it worth doing?

Honestly: **not urgently.** pocketparse at ~4.6× tree-sitter while staying
freestanding and runtime-compiled is a defensible place to stop, and
[`performance.md`](performance.md) lists cheaper unspent wins (the packrat memo,
`parser_reset`) that do not require a second executor. The VM is the right move
*if and when* raw parse throughput becomes the binding constraint — a host
parsing large corpora in a tight loop, where a 2–3× constant-factor win pays for
the complexity. Until then, the strongest reasons to prototype it are the two
that survive even if the speed never ships: it bounds parse stack depth in arena
memory (a robustness win on adversarial grammars), and it forces the design to
finally make the "rules are `ax` actors" promise concrete on the hot path,
turning the actor-shaped story from an aspiration the profile cannot find into a
running architecture. Build it as a layer, prove it byte-identical, and let the
benchmark decide whether it becomes the default.
