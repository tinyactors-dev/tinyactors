# Actor topologies for running tinyparse on ax

This document is a design exploration, not a change. It asks a question the two
prior analyses left half-answered: *what is the right shape of actors and
messages for running tinyparse on `ax`?* The fine-grained statechart design
([`ax-execution.md`](ax-execution.md)) answered "the most literal one" and paid
for it — a regression to roughly 5–15× slower than today, low tens of times
tree-sitter. The owner rejected that and asked for the topology *space* to be
explored properly, learning from how `ax` is actually used in the examples and
the runtime rather than from how the design once imagined parsing would map.

So this is a survey. It enumerates six candidate topologies, sketches the actors
and messages of each, says what `ax` does versus what runs inside an action,
estimates performance honestly against today's 4.6× tree-sitter, and judges each
on two axes the owner cares about equally: *genuineness* (is `ax` doing real
structural work, or just wrapping a loop?) and *speed*. It ends with a sharp
recommendation — a composition — and the migration path that reaches it without
moving the public API or breaking the 291/291 conformance oracle.

The companion documents are [`bytecode-vm.md`](bytecode-vm.md), which designed
the VM instruction set and first named the "coarse actor / tight loop" pattern;
[`ax-execution.md`](ax-execution.md), the statechart design whose cost is the
baseline to beat; [`performance.md`](performance.md), which measured today's
residual; [`architecture.md`](architecture.md), which draws the layer
boundaries; and the contracts [`../tinyparse-design.md`](../tinyparse-design.md)
and [`../../../src/ax/ax-design.md`](../../../src/ax/ax-design.md). Untagged code
blocks below are illustrative; `mdc` extracts nothing from this file.

## Abstract

The right topology is a **composition**, not a single design. Grammar
compilation — reader, IR, id resolution, first-set computation, bytecode
emission — is a one-time **dataflow of actors**, where the per-message cost is
amortized over the whole compile and the staging is a genuine pipeline `ax`
schedules. Parsing itself is a **VM managed by one actor**: the parser actor
owns a flat bytecode program and runs its dispatch loop inside a single
transition action, allocating its backtrack and tree-builder stacks from
`ax_heap` with `ax_alloc`, while `ax` sees only coarse events —
`input.chunk`, `input.end`, and the occasional sub-parse. Genuinely separate
parses — quasiquote bodies, the linearized-tree query engine — are **child
actors** spawned with `ax_spawn`, reporting completion through `done.child` and
`ax_outcome`. Host I/O — tracing, diagnostics, module loading during compile —
stays on **plugs**. Journal/replay is deliberately **unused**: parsing is
ephemeral, there is nothing to recover, and the inner backtracking is exactly
the kind of speculative work `ax`'s replay suppresses. The honest estimate is
that this composition lands **at or under today's 4.6× tree-sitter** — the VM
dispatch loop is faster than the recursive-descent walk it replaces, so the
parse gets *faster*, not slower, while `ax` does real structural work at every
boundary it owns. This is the design's promise made real *and* fast, which the
statechart design could not be. The recommendation, in one line: **compile with
an actor pipeline, parse with a VM the parser actor manages, spawn child actors
for genuinely separate sub-parses, and never journal a parse.**

## What "genuinely on ax" has to mean here

The mandate is integrity *and* speed, and the two are not in tension once the
actor boundary is drawn at the right grain. The trap the statechart design fell
into was reading "genuinely on `ax`" as "let `ax` see every combinator step." A
design that fires one transition per token, or worse per IR node, *is* on `ax`
in the most literal sense and is also a betrayal of `ax`'s own warning against a
stringy hot path ([`../../../src/ax/ax-design.md`](../../../src/ax/ax-design.md),
*Events*: "compact event payloads, not dynamically formatted event names"). The
profile in [`performance.md`](performance.md) is the proof: an internal `ax`
transition walks a transition array, prefix-matches an event name, and updates
the active configuration, where a C `switch` arm does one indirect jump. Make
that the per-token unit and the parse is 5–15× slower; the statechart design's
own estimate confirms it.

The examples teach the opposite discipline, and they teach it consistently. In
**axsteroids** and **axforest** the host owns the real-time loop; `ax` is a
guest that is pumped once per frame, and the hot work — physics, the fire-spread
RNG — runs *inside actions*, never on the message bus:

```c
/* axforest/grid.c — the pump is coarse: one drain per frame, then render */
static void grid_pump(grid_state *grid) {
    while (ax_step(grid->sys) == AX_MORE) { }
}
```

axforest runs 102,400 actors, one per grid cell, and stays fast precisely
because idle cells send nothing: the `ax_step` ready queue tracks the fire
*front*, not the grid. The cell's spread decision is "a pure function of the
cell's own state and the event, so entropy lives in the cell" — local state in
the actor's data block, not a round-trip. The runtime ([`src/axr`](../../../src/axr))
makes the same choice at service scale: the embedder owns the loop, `axr_step`
is one bounded slice, and the world is reached only through ioprocessors whose
frames cross *coarse* — minislack's star topology ships 1280-byte message packs
and 4096-byte rendered fragments, not per-field events.

The dividing line, stated once and used throughout:

> An `ax` event crosses the actor boundary or suspends the actor. Everything
> inside one transition's action — the VM dispatch loop, the backtrack pushes
> and pops, the memo lookups, the tree construction — is plain C running to
> quiescence before the action returns. `ax` owns the *coarse structure*: which
> actor runs, when a sub-parse is its own actor, what crosses to the host
> through a plug. The grammar's *execution within a chunk* is C the action runs.

This is not a dodge around being on `ax`. It is what every fast VM-on-a-scheduler
does, and it is exactly how axsteroids puts a 60 Hz game on `ax` without the
scheduler ever seeing a physics tick. The question this document answers is not
"can we touch `ax` more" but "where does touching `ax` do *real work* that earns
the name, and where is it ceremony that only slows the parse?"

## The six topologies

The candidates run from "everything is an `ax` step" to "almost nothing is."
Each is judged on the same six questions: the actors and messages; what `ax`
does versus what runs in-action; which `ax` features it uses; an honest speed
estimate against 4.6×; genuineness; and complexity.

### 1. Fine-grained statecharts (the slow baseline)

This is the design fully specified in [`ax-execution.md`](ax-execution.md),
included here for contrast. Each grammar rule lowers to an `ax_spec`; `seq` is a
state chain, `choice` is a branch with a backtrack frame in `ax_data`, `repeat`
is a self-looping choice, `ref` is an inline compound substate. Tokens drive
transitions: a `match_*` action calls the lexer, then raises `token.matched` or
`fail` via `ax_raise` to drive the next transition.

```
input.chunk ──▶ [parser actor]
                   rule.List statechart:
                   open ──token.matched──▶ forms ──fail──▶ close ──▶ done
                     │                       │ form.matched (loop)
                     └fail──▶ abort          └ (backtrack frame in ax_data)
```

- **What `ax` does:** nearly everything structural — every combinator step is a
  transition `ax` matches and fires; the grammar's shape *is* the compiled state
  tree, inspectable through the inspection API.
- **What runs in-action:** only the leaf lexer call and the mark save/restore.
- **`ax` features:** `ax_spec`/`ax_state`/`ax_transition`, inline compound
  substates, `ax_raise` for the `fail`/`matched` cascade, `ax_data` for the
  backtrack stack, child actors for true sub-parses, and — tempting but wrong,
  see below — `ax_record`/`ax_replay`.
- **Speed:** **5–15× slower than today**, landing the corpus near 25–70×
  tree-sitter ([`ax-execution.md`](ax-execution.md), *An honest performance
  estimate*). An internal transition is ~5–15× the cost of the `switch`-arm-plus-
  call it replaces, and the transition count tracks today's node visits. This is
  back into the territory W45 and W46 climbed out of.
- **Genuineness:** maximal. The grammar is *visibly* an `ax` machine; `ax_snap`
  shows the parse position as a live configuration.
- **Complexity:** high — a full lowering from IR to nested specs, the
  backtracking-in-data mechanism, recovery snapshotted into frames.

The verdict the owner already reached: this is the most genuine topology and the
slowest, and the slowness is structural — it is *the cost of letting `ax` see
each step*. It is the baseline every faster design must beat.

### 2. VM managed by an actor (the owner's lead)

The grammar compiles to a flat bytecode program (the ISA of
[`bytecode-vm.md`](bytecode-vm.md): `MATCH`, `LITERAL`, `CHOICE`, `COMMIT`,
`PARTIAL_COMMIT`, `FAIL`, `CALL`, `RETURN`, `OPEN`, `CLOSE`, `FIELD`, `PREDICT`,
the `GOAL_*`/`EXTRAS_*`/`RESERVED`/`RECOVER_*` brackets). **One parser actor
owns it.** Its `ax_data` holds the VM registers — program counter, cursor — and
its `ax_heap` holds the program, the input buffer, the memos, and three
`ax_alloc`-grown stacks: backtrack, return, and the CST builder. The dispatch
loop runs *inside a transition action*; `ax` sees coarse events only.

```
tinyparse_push(chunk) ─ax_post─▶ [parser actor]
                                      on "input.chunk":
                                        ┌──────────────────────────────┐
                                        │  for (;;) switch(program[pc]) │  ← tight C loop
                                        │    MATCH/CHOICE/CALL/FAIL...  │     in ONE action,
                                        │    push/pop arena stacks      │     thousands of
                                        └──────────────────────────────┘     opcodes per event
                                      blocks on "need input" or chunk end
tinyparse_finish() ─ax_post─▶      on "input.end": finalize, raise done.state
   (sub-parse) ─ax_spawn─▶ [child VM actor] ─done.child──▶ splice subtree
   (trace/diag/module) ─────────────────────────────────▶ [plug → host]
```

- **What `ax` does:** owns the actor's lifetime and arena; routes the
  `input.chunk`/`input.end` events; spawns and reaps child actors for genuinely
  separate sub-parses; pumps to quiescence; carries plug traffic to the host.
  This is the **managing actor doing coarse control** — load the program, accept
  a chunk, run the VM, spawn a sub-parse, call a plug — exactly the role the
  game actor plays in axsteroids around the per-frame physics.
- **What runs in-action:** the entire dispatch loop, `ax_alloc`-grown stacks,
  both memos, CST construction. The hot path never touches the message bus.
- **`ax` features:** `ax_spawn`, `ax_data`, `ax_heap` + `ax_alloc` (used inside
  actions to grow the backtrack/CST stacks dynamically — explicitly endorsed),
  `ax_post` for the two external events, `ax_raise` for the internal
  finalize cascade, child actors + `done.child`/`ax_outcome` for sub-parses,
  ports for plugs. *Not* `ax_record`/`ax_replay`.
- **Speed:** **at or under 4.6×, and plausibly better.** This is the key claim.
  The VM dispatch loop is the design [`bytecode-vm.md`](bytecode-vm.md) estimated
  at **2–5× faster than today's recursive-descent walk** — a flat instruction
  stream the prefetcher likes, an explicit return stack instead of C recursion,
  one repeated dispatch site the branch predictor can learn, and the W45/W46
  memos called verbatim. If the VM is 2–3× faster than the IR walk and the per-
  chunk `ax` overhead is negligible (one `ax_post` per push, amortized over
  thousands of opcodes), the parse lands near **1.5–2.5× tree-sitter** — a *win*
  over today, not a regression. The actor wrapper costs essentially nothing
  because it is coarse.
- **Genuineness:** this is the live question, and the answer is "real, but at the
  coarse grain." `ax` is not wrapping a loop and walking away — it manages the VM
  (program load, input streaming, sub-parse spawning, plug calls), it owns the
  durable-shaped lifetime even though durability is off, and it does the
  *structural* work of spawning and reaping sub-parse children. What `ax` does
  *not* do is see each opcode, which is the whole point. Honesty requires saying
  this plainly: at this grain the grammar's fine structure is invisible to `ax`
  (it is a bytecode array, not a state tree), so `ax_snap` shows "running the VM"
  not "at rule List, alternative 3." Genuineness here is the managing actor's
  coarse control plus the child-actor topology, not the dispatch.
- **Complexity:** moderate. A lowering pass (IR → bytecode), a dispatch loop, the
  three arena stacks. Less than the statechart design's nested-spec lowering, and
  the loop is a single readable `switch`.

This is the owner's lead and it holds up: it is the only topology that is both
genuinely-on-`ax` (real coarse structure) and *faster* than today.

### 3. Compiler as an actor pipeline

Grammar compilation is a staged dataflow: read `.scm` bytes → Scheme forms →
validate and intern → IR → resolve ids and compute first-sets → emit bytecode.
Each stage is an actor; a stage's output is the next stage's input, posted as one
coarse message. This is the seven-step pipeline
[`architecture.md`](architecture.md) already names, run as actors instead of as
a straight-line C call chain.

```
[reader] ─forms─▶ [validate+intern] ─symbols─▶ [ir builder] ─ir─▶
   [resolver: ids + first-sets] ─resolved-ir─▶ [emitter] ─program─▶ done
                                                  (module load) ─plug─▶ host
```

- **What `ax` does:** schedules the stages, routes one message per stage
  transition, and — the one place a plug earns its keep at compile time —
  carries an `(import …)` module-resolution round-trip out to the host through a
  port and the answer back.
- **What runs in-action:** each stage's actual transform (parsing forms,
  building IR, computing first-sets, emitting opcodes) runs inside that stage's
  action over the whole grammar.
- **`ax` features:** a small chain of `ax_spawn`ed actors, coarse `ax_post`/
  `ax_raise` between stages, a plug for module loading, `done.state` to signal
  the finished program.
- **Speed:** **irrelevant to parse throughput, and that is the point.**
  Compilation is one-time and excluded from the timed loop
  ([`performance.md`](performance.md): "The one-time cost of loading the runtime
  and compiling the grammar is excluded"). The per-message cost — a handful of
  `ax_post`s across the whole compile — is negligible against the work each
  stage does. So this topology adds genuine `ax` structure at *zero* parse cost.
- **Genuineness:** high and *cheap*. Unlike the parse hot path, where every `ax`
  touch is a tax, a compile pipeline is a place where `ax`'s scheduling and
  staging are real work `ax` is good at, paid for once. The stages are a true
  dataflow; the dependency order (you cannot emit before you resolve, cannot
  resolve before you have IR) is exactly the dependency `ax`'s message ordering
  expresses. This is `ax` doing structural work that is *visible* and *honest*
  without slowing anything.
- **Complexity:** low-to-moderate. The transforms already exist as functions
  ([`grammar.c`](../src/grammar.c), [`pack.c`](../src/pack.c)); wrapping each in
  a stage actor is mechanical.

Is this worthwhile or just ceremony? It is the rare case where the answer is
"worthwhile," precisely because it is off the hot path. The owner's instinct is
right: compilation is where actor staging is natural and free, so it is where the
"compiler as actors" story should live — and it lets the design claim `ax` does
real, inspectable structural work without the parse paying for it.

### 4. Lexer / parser / tree-builder pipeline

A few streaming actors at parse time: a **lexer actor** scans bytes into tokens,
a **parser actor** consumes tokens and decides structure, a **tree-builder
actor** assembles the CST. Tokens flow lexer → parser as events; node-build
commands flow parser → builder.

```
input.chunk ─▶ [lexer] ─token─▶ [parser] ─open/close/field─▶ [tree-builder]
```

- **What `ax` does:** routes a `token` event per token and a node command per
  CST node — and that is the fatal flaw.
- **What runs in-action:** each actor's local step (one scan, one parse
  decision, one node append).
- **`ax` features:** three actors, `ax_post` or `ax_raise` per token and per
  node.
- **Speed:** **a regression, for the same reason as topology 1.** The per-token
  event boundary is the dominant new cost; a token event plus the transition it
  drives is 5–15× a function call. Even if the boundary is internal (`ax_raise`,
  not `ax_post`), each token still walks a transition array and prefix-matches a
  name. This lands in the low tens of times tree-sitter, no better than the
  statechart design.
- **Genuineness:** moderate, and *bought at the worst price* — the per-token
  message is exactly the stringy hot path the design forbids.
- **Complexity:** moderate, plus a fatal coupling problem: tinyparse's lexer is
  **parser-directed** ([`ax-execution.md`](ax-execution.md), *The lexer and
  token events*). Which token is produced at an offset depends on the parser's
  active lexical goal. A separate lexer actor would need the goal pushed to it
  before every token — a round-trip per token — or would have to read the
  parser's `ax_data`, crossing a boundary it has no business crossing.

Can batching or internal-raise rescue it? Batching tokens into a chunk before
crossing the boundary helps the message count, but then the lexer is just a
function the parser calls over a batch — which is topology 2, not a separate
actor. The parser-directed lexing kills the clean split outright: the lexer
cannot run ahead of the parser because it does not know the goal yet. The
conclusion the statechart design reached holds here too — **the lexer is a
function the parser actor calls, not its own actor** — and once the lexer folds
in, the tree-builder folding in is the same argument. This topology collapses
into topology 2.

### 5. Sub-parse topology (child actors vs inline VM calls)

This is not a whole-parse topology but a *decision rule* that overlays whichever
parse topology is chosen: when is a nested parse a VM `CALL` within one actor,
and when is it a spawned child actor? Three nested constructs force the question:
lexical-goal re-entry, a nested grammar (templates / quasiquote), and the query
engine ([`tp_query_run`](../src/internal.h)) over the linearized ENTER/LEAF/EXIT
stream.

```
[parser VM actor]
   ordinary (ref R), same grammar, same input ──▶ VM CALL/RETURN (no message)
   quasiquote ${...} body, different grammar ────ax_spawn──▶ [child VM actor]
   query over linearized tree, different input ──ax_spawn──▶ [query child]
                                                    │ done.child.<id> + ax_outcome
                                                    ▼ parent splices subtree
```

- **What `ax` does:** for a child sub-parse, it spawns (`ax_spawn`), gives the
  child "shutdown by geometry" (cancelled when the hosting state is left), and
  raises `done.child.<id>` carrying the child's `ax_outcome` payload so the
  parent splices the returned subtree. For an ordinary sub-rule, `ax` does
  *nothing* — it is a VM `CALL`.
- **What runs in-action:** the ordinary `CALL`/`RETURN` and everything the child
  does internally.
- **`ax` features:** `ax_spawn`, the self-naming child spec (so a recursive rule
  can fall back to a child at the recursion edge), `done.child`, `ax_outcome`,
  "shutdown by geometry."
- **Speed:** **negligible cost when the rule is followed; ruinous when it is
  not.** A spawn per *nested parse* (one per template literal, one per query) is
  coarse and amortized over the whole sub-parse — this is `ax`'s `<invoke>` model
  used as designed. A spawn per *rule reference* would put one `ax_spawn` and one
  `done.child` on every node, which is the per-message cost the profile forbids.
- **Genuineness:** this is where the child-actor part of the topology does
  *real, visible* structural work — a quasiquote body genuinely is a different
  parser over a different input span with independent completion, and modelling
  it as a child actor with `done.child` completion is `ax` expressing exactly
  that. The query engine is the same *kind* of need (it is a parser), so it is
  the same case, not a second one.
- **Complexity:** the discipline is the whole risk. **A sub-parse becomes a child
  actor only when it is conceptually a *different parser* — different program,
  different input span, independent completion. Everything else is a VM `CALL`.**
  Getting this boundary wrong in either direction is the main integration risk:
  spawn too eagerly and the message overhead returns; spawn too rarely and the
  quasiquote and query cases lose the clean child-completion semantics that
  recovery and tracing want.

This topology is not a standalone choice; it is the part of the recommendation
that makes the parse genuinely structured rather than one opaque loop.

### 6. Offload / worker topology

`ax` has **no thread or worker-offload primitive.** The closest facility is
`ax_evict` ([`../../../src/ax/ax-design.md`](../../../src/ax/ax-design.md)),
which pages a *settled* actor out of RAM and wakes it by replay — it is memory
management, not parallelism, and the core has no main loop and no threads ("the
host owns all scheduling and threading"). So an offload topology — heavy matchers
or independent sub-parses run on worker threads — is **not expressible on `ax`'s
core** without the host building its own thread pool and running a second `ax_sys`
per worker, which is a host concern outside this design.

- **Speed:** potentially large for *embarrassingly parallel* independent
  sub-parses (many template literals, a corpus of independent files), but only at
  the host level, and parsing one document is inherently sequential (the cursor
  is shared, ordered-choice is left-to-right). For the benchmark corpus —
  many small documents — parallelism is the host's loop over documents, not an
  `ax` topology.
- **Genuineness / fit:** out of scope. `ax` does not offer it; faking it would
  mean the host owning threads, which neither the core nor tinyparse's
  freestanding contract wants. Recorded here only to close the enumeration: the
  honest answer is "`ax` does not do this, and parsing does not need it."

## Comparison

| Topology | `ax` does | runs in-action | speed vs 4.6× | genuine? | complexity |
|---|---|---|---|---|---|
| 1. Fine-grained statecharts | every combinator step | leaf lexer call only | **5–15× slower** (25–70× ts) | maximal (grammar *is* the machine) | high |
| 2. VM managed by an actor | coarse control: load, stream, spawn, plug | whole dispatch loop + stacks + memos | **at or under 4.6×, likely 1.5–2.5× ts** | real, at coarse grain | moderate |
| 3. Compiler pipeline | stage scheduling + module plug | each stage's transform | **off the hot path (free)** | high *and* cheap | low–moderate |
| 4. Lexer/parser/builder | per-token, per-node routing | local per-token step | **regression** (low tens) | moderate, worst price | moderate + lexer coupling |
| 5. Sub-parse children | spawn/reap genuinely-separate parses | ordinary `CALL`/`RETURN` | negligible (if rule followed) | real, visible structure | discipline is the risk |
| 6. Offload / workers | — (not supported) | — | host-level only | out of scope | n/a |

The pattern in the table is the thesis: **`ax` touches that are coarse and
off-the-hot-path (3, the coarse control of 2, the spawns of 5) are genuine and
nearly free; `ax` touches that are per-token or per-node (1, 4) are genuine and
ruinous.** The right topology maximizes the former and forbids the latter.

## Recommendation

**Build the composition: a compiler-as-actor-pipeline at compile time (topology
3), a VM-managed-by-an-actor at parse time (topology 2), and child actors for
genuinely separate sub-parses (topology 5). Use plugs for host I/O. Do not
journal a parse.**

Concretely:

1. **Compile the grammar with an actor pipeline.** reader → validate/intern →
   IR → resolve+first-sets → emit bytecode, each a stage actor, one coarse
   message between stages, a module-resolution plug for `(import …)`. This is
   genuine `ax` structural work — a real dataflow whose dependency order is the
   message order — paid for once and excluded from the timed loop. It is where
   the "compiler as actors" story belongs because it is free there.

2. **Parse with a VM the parser actor manages.** One actor owns the bytecode
   program in `ax_heap`, the VM registers in `ax_data`, and three
   `ax_alloc`-grown stacks (backtrack, return, CST builder). `tinyparse_push`
   posts `input.chunk`; the handling transition runs the dispatch loop until it
   blocks on "need input" or exhausts the chunk; `tinyparse_finish` posts
   `input.end` to finalize and raise `done.state`. The message rate `ax` sees is
   one per chunk; the dispatch loop runs thousands of opcodes per message inside
   one action. The lexer is a function the action calls — never its own actor —
   because lexing is parser-directed and the active goal already lives in
   `ax_data`.

3. **Spawn child actors only for genuinely separate parses.** A quasiquote /
   template body, a nested grammar over its own input span, and the query engine
   are child actors via `ax_spawn`, reporting through `done.child` /
   `ax_outcome`, with shutdown by geometry. Every ordinary sub-rule of the same
   grammar over the same input is a VM `CALL`. This is the line that keeps the
   topology structured without paying per-rule message cost.

4. **Host I/O stays on plugs.** Tracing, diagnostics, and compile-time module
   loading go through ports, firing on cold/structural events — a chunk arriving,
   a child spawned or completed, a parse finishing — never per opcode. A `(class
   NAME)` predicate calls a host binding (a `tinyparse_class_fn`, a direct C
   function pointer) from inside the lexer function, a host boundary but not a
   message, "for hot predicates" as the design says.

### Why this is genuinely on ax

`ax` does *real structural work* at every boundary it owns, and the work is
the kind `ax` is good at rather than the kind that taxes it:

- **The compiler dataflow** is a true multi-stage pipeline `ax` schedules; the
  stages' dependency order is expressed as message order, and the module plug is
  a genuine host round-trip. This is `ax` doing what minislack's and axr's
  service topologies do — coarse staged work between long-lived actors.
- **The managing actor's coarse control** is the axsteroids pattern exactly: the
  actor loads the program, accepts input, runs the fast inner work in an action,
  spawns sub-work, and calls out through ports — the same shape as the game actor
  around per-frame physics, or a grid cell around its local RNG. The actor is
  doing real management; the VM loop is its "physics."
- **Sub-parse children** are `ax`'s `<invoke>`/`done.child` model used as
  designed — a quasiquote body or a query run *is* a separate parser with
  independent completion, and `ax` expresses that with a spawn and a `done.child`
  splice. This is structural work visible in the actor graph.
- **Plugs** are the host boundary drawn where the design already draws it.

What `ax` deliberately does *not* do is see each opcode — and that omission is
not a compromise of genuineness but the lesson the examples teach: axforest is
genuinely on `ax` with 100k actors precisely because the per-cell hot work stays
in actions and the bus carries only the fire front. The same discipline makes
tinyparse genuinely on `ax` while being fast.

### Honest performance estimate

The dispatch loop is **2–5× faster than today's recursive-descent IR walk**
([`bytecode-vm.md`](bytecode-vm.md)): a flat program the prefetcher sweeps, an
explicit arena return stack instead of C recursion, one repeated dispatch site
the predictor learns, and the W45 match memo and W46 first-set prediction called
verbatim rather than re-implemented. The per-chunk `ax` overhead is one `ax_post`
and one transition match per push — a few events for the whole parse, negligible
against thousands of opcodes per event. Net: the parse gets **faster**, landing
the corpus plausibly near **1.5–2.5× tree-sitter**, *under* today's 4.6×. It will
not reach tree-sitter's generated-table speed — tinyparse compiles grammars at
runtime, and that is the whole point of the project — but it is the rare
re-architecture that improves both integrity and speed instead of trading one for
the other. This is the decisive contrast with the statechart design, which bought
integrity at a 5–15× slowdown; here integrity and speed move together.

### Why journal/replay is not used, and why that is correct

`ax`'s `ax_record`/`ax_replay`/`ax_live` durability is deliberately **absent**
from this design, and the absence is principled, not an oversight:

- **Parsing is ephemeral.** A parse produces a CST and is thrown away; the
  benchmark even confirms tree deletion is a no-op because the arena owns it.
  There is no state worth surviving a crash. Durability solves "this actor must
  outlive a restart" — minislack journals a *channel's message history* and a
  *user's memberships* because those are the source of truth. A parse has no such
  source of truth; rerunning it from the input bytes is cheaper than replaying a
  journal of those same bytes.
- **The hot path must not pay write-ahead.** A durable actor never processes an
  event that was not journaled first ([`../../../src/ax/ax-design.md`](../../../src/ax/ax-design.md),
  *Durability*). Putting the parse under write-ahead would tax every
  `input.chunk` with a journal append for a result we discard.
- **Replay would corrupt the backtracking model anyway.** The inner backtracking
  is exactly the speculative work `ax`'s replay *suppresses* (sends, port emits,
  child spawns), and replay rebuilds from seq 0 rather than checkpointing — the
  mark-and-restore stack throws failed work away in O(1), where replay would
  re-derive it. Replay is the wrong cost shape for speculation, as
  [`ax-execution.md`](ax-execution.md) (*Why not record/replay speculation*)
  already established. The examples agree: axsteroids and axforest use no
  durability for the same reason — ephemeral real-time work has nothing to
  recover, and durability would violate the never-block-the-hot-loop contract.

The one capability journaling would buy — checkpoint/resume of a long-running
parse over a huge input — is real but not tinyparse's problem; the corpus is
many small documents, and a host that needs it can wrap the parser without the
core knowing. Leaving journaling off is the correct default here.

## Embedded languages: injection as child VM actors

The child-actor leg of the recommendation (topology 5) deserves its own section,
because it *is* the marquee capability it implies: **language injection**.
Parsing SQL inside a string literal, JavaScript inside a Markdown code fence, CSS
inside an HTML `<style>` — tree-sitter's "injections" — is not a new subsystem
here. It is a child VM actor whose program is a *different grammar*. "Parse this
byte span with grammar G, splice the result" is one operation; same-grammar
(a quasiquote body) and cross-grammar (SQL in a string) differ only in which
compiled program the child loads. This is the capability that moves tinyparse
past single-language toys.

### Why the actor model fits injection better than a bolt-on

tree-sitter implements injection as a separate layer: injection queries name a
node and a language, the host re-drives a second parser over the range, and the
ranges are stitched back. The actor topology gives the same result as structure,
not plumbing:

- **Isolation is free.** Each embedded language is its own actor with its own
  `ax_heap` arena, its own lexical goals, extras, and backtrack stack. There is
  no shared scanner state to corrupt — the classic failure of scannerless
  embedding. An embedded regex sublanguage, a template's `${…}`, and an embedded
  SQL each keep their own scanning context.
- **Nesting is just actor depth.** Markdown containing a ` ```js ` fence
  containing a template literal containing embedded SQL is a four-deep actor
  tree; the actor tree *is* the language-nesting tree. axforest runs 100k
  actors, so depth and breadth are not a concern.
- **Errors are contained.** A malformed SQL region produces an `ERROR` subtree
  delivered through `done.child` without derailing the host parse, because it is
  a separate actor with its own outcome.
- **Resolution already exists.** The embedded grammar is resolved by name
  through the embedded registry and grammar packages — `inject sql` resolves like
  any package, no new machinery.

### The two triggers

Injection mirrors the grammar/query split tinyparse already has:

- **Declarative, in the grammar** — a rule marks a region and names the
  sublanguage:

  ```scheme
  (rule sql_string
    (seq "\"" (inject grammar: std.sql content: (field body: StringChars)) "\""))
  ```

- **Injection query**, tree-sitter style, where the language is dynamic, chosen
  from another capture:

  ```scheme
  ((fenced_code_block (info_string) @lang (code) @content)
   (inject language: @lang content: @content))
  ```

  The Markdown ` ```js ` fence's info string names the grammar; the registry
  resolves `@lang` to a package.

### What the host plumbing must add

Three small pieces on top of the spawn / `done.child` / registry machinery the
topology already has:

- **Offset mapping** so the child tree's byte spans map back to absolute source
  positions.
- **Splicing** the child CST under the injection node.
- **A language tag per node** so tooling, highlighting, and the linearized-tree
  query engine know which language a subtree belongs to — one composite tree,
  many languages.

### Worked example

```
example.md
[markdown VM actor]
  └─ ```js fence ──ax_spawn(std.javascript)──▶ [javascript VM actor]
        const q = sql`SELECT * FROM t`;
        └─ sql`…` ──ax_spawn(std.sql)──▶ [sql VM actor]
                                            │ done.child → splice
                       done.child → splice  ▼
  one composite CST; each node tagged markdown | javascript | sql
```

Each actor parses its own span, in its own arena, with its own lexical state;
the parent splices each child's subtree at the injection point. A broken SQL
query is an `ERROR` under the `sql` node, nothing more. A multi-language file
becomes a tree of isolated VM actors — which is exactly what the actor model is
for, and what a flat parse-table design has to bolt on.

## Migration path

The composition can be reached incrementally, behind the frozen public API, with
the conformance suite as the differential oracle at every step. The public
surface — `tinyparse_parser_new`, `tinyparse_push`, `tinyparse_finish`,
the cursor API, the `tinyparse_class_fn` binding
([`../include/tinyparse.h`](../include/tinyparse.h)) — **does not move.**

1. **Keep `tp_execute` as the reference executor.** Today's recursive-descent
   walk ([`parse.c`](../src/parse.c)) stays as the fallback and the debugging
   oracle. `tinyparse_finish` chooses which executor runs — a flag or a
   build-time switch — so the public behavior is unchanged while the VM is
   incomplete.

2. **Land the compiler pipeline first, where it is safe.** Wrapping the existing
   compile stages ([`grammar.c`](../src/grammar.c), [`pack.c`](../src/pack.c)) in
   stage actors changes no parse output and is off the timed loop. It proves the
   actor staging and the module plug without risking the hot path, and it makes
   the "compiler as actors" claim concrete early.

3. **Lower a subset of opcodes; run the VM only for grammars that use that
   subset.** Fall back to `tp_execute` otherwise. Prove **byte-identical
   output** — the same CST, fields, and diagnostics — on every conformance case.
   The 291/291 metric ([`ci/tinyparse-metric.sh`](../../../ci/tinyparse-metric.sh))
   is the oracle: the VM is correct exactly when it produces the byte-identical
   tree `tp_execute` produces.

4. **Extend the opcode set until the VM covers the full IR**, the perf gate
   ([`ci/tinyparse-perf-gate.sh`](../../../ci/tinyparse-perf-gate.sh))
   confirming each step does not regress the median. Add the sub-parse child
   actors when the quasiquote/query cases land, with the same differential check.

5. **Make the VM the default once it dominates `tp_execute`** on the benchmark
   and matches it on every conformance case. Keep `tp_execute` as a reference and
   a divergence oracle indefinitely — a flat instruction stream is unreadable in
   a debugger, and the tree walk *is* the grammar.

## The hardest tensions

Three tensions decide whether the composition holds. Each is named with how the
recommended topology handles it.

**1. Genuineness versus speed — the central tension the owner posed.** The more
`ax` sees of the parse, the more genuine and the slower; the less it sees, the
faster and the more it looks like `ax` wrapping a loop. The statechart design sat
at one extreme and paid 5–15×. The resolution is not a compromise on the axis but
a *move off it*: put `ax`'s genuine structural work where it is coarse and nearly
free (the compile pipeline, the managing actor's control, the sub-parse spawns)
and keep the per-step work (opcode dispatch) inside an action where `ax` was never
meant to look. This is the axforest lesson — genuinely on `ax` at 100k actors
*because* the hot work stays in actions — applied to parsing. The composition is
genuine where genuineness is cheap and fast where speed is dear.

**2. The sub-parse boundary.** The line between a VM `CALL` and a spawned child
is load-bearing in both directions and easy to get wrong. Spawn per rule
reference and the per-message cost the profile forbids comes back, dragging the
parse into the low tens; spawn too rarely and the quasiquote and query cases lose
the clean `done.child` completion that recovery and tracing depend on. The
handling is a hard, stated rule — *a child actor only for a genuinely different
parser (different program, different input span, independent completion); a VM
`CALL` for everything else* — enforced by the differential oracle (a wrongly
inlined sub-parse produces a wrong tree) and the perf gate (a wrongly spawned
sub-rule shows up as a regression).

**3. Byte-identical behavior across a second executor.** Recovery insertion,
extras emission order, trivia placement, the whole-slice reserved-word check, and
field tagging must match `tp_execute` exactly, and the subtle cases are where a
divergence would hide — extras folded into `MATCH`/`CHOICE`, recovery disarmed
across a speculative `CHOICE` and re-armed on `FAIL`-restore, the `RESERVED`
guard on a matched slice. The handling is the migration discipline itself: keep
`tp_execute` as the reference forever, advance the VM one opcode subset at a time,
and treat the 291/291 suite as the gate that turns any divergence into a visibly
wrong tree rather than a silent behavioral drift. The VM is not "done" until it
is byte-identical on every case *and* faster on the benchmark.
