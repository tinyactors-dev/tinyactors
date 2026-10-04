# Parsing on the ax actor engine

This document is the design gate for re-architecting tinyparse so that
parsing genuinely runs on the `ax` actor engine, not merely on its arena. It
specifies how the grammar IR lowers to compiled `ax` state machines, how a
parser becomes a spawned actor, how tokens drive transitions, and — the part
that decides whether the whole idea is sound — how PEG ordered-choice
backtracking survives on a forward-only statechart. It ends with a verdict the
next work item (W58) can act on.

The companion documents are [`architecture.md`](../architecture.md), which already
claims this architecture as the intended one ("Grammar rules lower to `ax`
machines"); [`performance.md`](../performance.md), which measured the residual this
re-architecture must not wreck; [`bytecode-vm.md`](bytecode-vm.md), the prior
analysis of a flat bytecode VM, which this document deliberately contrasts; and
the two design contracts, [`../tinyparse-design.md`](../../tinyparse-design.md)
and ax's `src/ax/ax-design.md`. Untagged code
blocks below are illustrative; `mdc` extracts nothing from this file.

## Abstract

tinyparse parses today by walking an in-memory IR tree with a recursive C
function ([`tp_parse`/`tp_execute`](../../src/parse.c)) that touches `ax` only
through `ax_alloc`. The actor model the design promised never shipped. This
document specifies how to make it real: each grammar rule lowers to an `ax_spec`
whose states and transitions encode `seq`/`choice`/`repeat`/`optional`, the
parser runs as an actor spawned with `ax_spawn`, the lexer produces tokens that
arrive as events driving transitions, `ax_step` is pumped to quiescence, and a
genuinely separate sub-parse spawns a child actor whose `done.child` completion
feeds recovery and tracing. The public API in
[`../include/tinyparse.h`](../../include/tinyparse.h) does not move. The crux is
ordered-choice backtracking: `ax` statecharts only move forward on events, while
PEG must try an alternative and roll back the input cursor and the half-built
tree on failure. The mechanism specified here keeps an explicit backtrack stack
in the actor's `ax_data` and models `FAIL` as an internal event (`ax_raise`)
that restores the saved marks and advances to the next alternative. The W45 memo
and W46 first-set prediction carry through untouched. The honest cost is a
regression from today's 4.6× tree-sitter into the low tens, concentrated in
event dispatch at token granularity. The recommendation, in one line:
**proceed to W58 with named modifications**. The architecture is sound and
buildable, but the per-token event boundary must be coarsened to a per-chunk one
and backtracking must live in `ax_data` rather than in the statechart's
configuration, because a literal "one transition per token, roll back the
configuration" design fights `ax` exactly where `ax` is least willing to bend.

## What "genuinely on ax" has to mean

The mandate is integrity-first: parsing must execute *on* `ax`, and a
performance regression from today's 4.6× tree-sitter is acceptable so long as
the architecture is honest. Honesty cuts both ways. A design that posts one
event per byte and re-derives the parser's position from the statechart's active
configuration would be "genuinely on `ax`" in the most literal sense and also a
betrayal of `ax`'s own warning against a stringy hot path
(ax's `src/ax/ax-design.md`, *Events*:
"compact event payloads, not dynamically formatted event names"). So the
question this document answers is not "can we touch `ax` more" but "what is the
*coarsest* genuine use of `ax`'s machine model that still earns the name" — where
the grammar really is a tree of `ax` states, the parser really is an actor, and
tokens really drive transitions, without pretending the inner backtracking loop
is something the scheduler should see.

The dividing line, stated once and used throughout:

> An `ax` event crosses the actor boundary or suspends the actor. Everything
> inside one transition's action — the inner advance through a rule body, the
> backtrack-stack pushes and pops, the memo lookups — is plain C running to
> quiescence before the action returns. The grammar's shape lives in the `ax`
> state tree; the grammar's *execution within a token* lives in C.

This is the same resolution every fast VM-on-a-scheduler reaches, and the one
[`bytecode-vm.md`](bytecode-vm.md) reached for its bytecode loop. The difference
here is that the grammar's structure is not flattened into a bytecode array; it
stays a genuine `ax` statechart, and the C that runs inside an action walks that
statechart's compiled transitions rather than an instruction stream. That choice
is what makes this "parsing on `ax` machines" and not "a bytecode VM that
happens to be inside an actor."

## Lowering the IR to ax specs

Each rule becomes one `ax_spec`. The rule's body — a tree of `tp_ir` nodes
(`TP_OP_SEQ`, `TP_OP_CHOICE`, and the rest, defined in
[`../src/internal.h`](../../src/internal.h)) — lowers to a tree of `ax_state`s with
transitions between them. The mapping is structural, combinator by combinator.

**A sequence is a state chain.** `(seq A B C)` lowers to states `s0 → s1 → s2 →
done`, each transition firing on the `token.matched` event that the element
ahead of it accepted, its action advancing the cursor and emitting the child
node. A required element that fails raises `fail` (below), which in a bare `seq`
with no enclosing choice aborts the rule.

**An ordered choice is a branch with a backtrack frame.** `(choice A B C)` is the
hard case and has its own section ([the crux](#the-crux-peg-backtracking-on-a-statechart)).
In outline: entering the choice state pushes a backtrack frame; each alternative
is a substate; a `fail` event inside an alternative restores the frame's marks
and transitions to the next alternative's substate; the last alternative's `fail`
re-raises `fail` to the enclosing frame.

**A repeat is a self-looping choice.** `(repeat X)` is `(choice (seq X repeat) ε)`
unrolled into a state that transitions back to itself on a successful iteration
and to its `done` final on the iteration's `fail`. Because a repeat is iterated
choice, it reuses the same backtrack-frame machinery, with the refinement that a
successful iteration *rewrites* the frame's cursor mark in place rather than
pushing a new frame — the bounded-stack trick `tp_parse`'s `TP_OP_REPEAT` loop
gets for free today by reusing `saved_pos` each iteration.

**An optional is a one-alternative choice.** `(optional X)` enters a state that
tries `X`; on `fail` it restores marks and transitions to `done` anyway, so the
absence of `X` is never a failure.

**A reference is a submachine or a child.** `(ref R)` to another rule lowers, by
default, to entering a *compound substate* whose own states are `R`'s lowered
body — an inline submachine, sharing the parser actor, its cursor, and its
`ax_data` backtrack stack. A reference is promoted to a spawned child actor only
in the narrow cases that are genuinely a different parse (see
[sub-parses](#sub-parses-as-child-actors)). Recursion is handled the way `ax`
handles it: `ax` permits a spec to name itself as a child
(ax's `src/ax/ax-design.md`, *Children*:
"a spec may name itself"), so a left-recursive or mutually recursive rule that
cannot be an inline submachine without unbounded state nesting falls back to a
child actor at the recursion point.

**Field and alias are capture actions.** `(field name X)` and `(alias kind X)`
do not add states; they set a one-shot decoration on the transition action that
emits `X`'s node, exactly as `tp_parse` threads its `field`/`alias` arguments
down the recursion. The lowered transition's `then` action reads the decoration
and stamps the emitted node.

**The tinyparse-specific combinators are bracketing transitions.** `(goal NAME
X)` and `(rule … extras: none …)` (`TP_OP_NO_EXTRAS`) lower to an enter action
that pushes the active goal or the no-extras flag onto a small stack in
`ax_data` and a leave action that pops it — the save/restore `tp_parse` does
around `active_goal` and `no_extras` becomes a state's `enter`/`leave` pair, so
"shutdown by geometry" cleans them up even on a failing match.
`(reserved …)` (`TP_OP_NOT_KEYWORD`) is a guard (`ax_test`) on the transition
that would accept the matched slice. `(recover …)` is its own section
([recovery](#recovery-plugs-and-durability)).

### A worked lowering

Take the running example from [`bytecode-vm.md`](bytecode-vm.md) and the design:

```scheme
(rule List
  (seq
    (field open: "(")
    (repeat Form)
    (field close: ")")))
```

`List` lowers to one `ax_spec` whose `data` block carries the parser state (the
cursor, the backtrack stack, the goal/extras stacks). The body is a chain with
the `repeat` as a self-looping substate:

```c
const ax_spec list_spec = {
    .name = "rule.List",
    .data = sizeof(parse_state),          /* cursor, backtrack stack, ... */
    .states = (const ax_state[]){

        /* open: "(" — required; FAIL here aborts List (no choice above it) */
        { .id = "open", .enter = match_open_paren,
          .transitions = (const ax_transition[]){
              { .on = "token.matched", .to = "forms", .then = emit_open },
              { .on = "fail",          .to = "abort" },
              { 0 } } },

        /* (repeat Form) — iterated choice: loop on a match, leave on FAIL */
        { .id = "forms", .enter = push_backtrack_frame,
          .transitions = (const ax_transition[]){
              { .on = "form.matched", .then = rewrite_frame_cursor }, /* stay, loop */
              { .on = "fail",         .to = "close", .then = pop_restore_frame },
              { 0 } } },

        /* close: ")" — required */
        { .id = "close", .enter = match_close_paren,
          .transitions = (const ax_transition[]){
              { .on = "token.matched", .to = "done", .then = emit_close },
              { .on = "fail",          .to = "abort" },
              { 0 } } },

        { .id = "done",  .kind = AX_FINAL, .outcome = report_list_node },
        { .id = "abort", .kind = AX_FINAL, .outcome = report_fail },
        { 0 } },
};
```

Read against [`tp_parse`](../../src/parse.c): `enter = match_open_paren` is the
`tp_consume_extras` + `tp_token_match_memo` the `TP_OP_LITERAL` case runs;
`emit_open`/`emit_close` are the `tp_tree_builder_push` + field stamp;
`push_backtrack_frame`/`rewrite_frame_cursor`/`pop_restore_frame` are the
`saved_pos`/`saved_count`/`saved_sink` save-and-restore the `TP_OP_REPEAT` case
does by hand, lifted into named transition actions; the `done` final's `outcome`
is the `tp_link_children` + span fixup `tp_parse_rule_ref` performs on a matched
body. The whole rule is six states walked by `ax`'s transition engine, against a
recursive descent through a `seq` node, two `field` nodes, a `repeat` node, a
`ref`, and two literals today.

This is the design's promise made literal — and the cost it implies is the
subject of the [performance section](#an-honest-performance-estimate): every one
of those transitions is `ax` machinery where today there is a C function call.

## The lexer and token events

Token-driven parsing means the parser does not scan bytes itself; it asks for the
next token and receives it as an event that drives a transition. The question is
*who* produces that token.

**Decision: the lexer is a function the parser actor calls rather than its own
actor.** The reason is parser-directed scanning. tinyparse's lexer is not
free-running;
which token it produces at a given offset depends on the parser's active lexical
goal (`tp_goal_redirect` in [`../src/parse.c`](../../src/parse.c)) and on whether
extras are suppressed in the current scope. A separate lexer actor would have to
be told the current goal before every token — a message round-trip per token,
the stringy hot path `ax` warns against — or would have to read the parser's
`ax_data`, which crosses an actor boundary it has no business crossing. Keeping
the lexer a function called from inside a transition action means the goal it
needs is already in `ax_data(self)`, reachable with no message at all. This is
exactly `ax`'s rule that "the datamodel is C": the lexer is C the action calls,
not behavior the scheduler dispatches.

So a transition that wants the next token calls `tp_token_match_memo` directly,
gets back match/no-match and an end offset, and *itself* synthesizes the
`token.matched` or `fail` event via `ax_raise` to drive the next transition. The
"token event" is real — it is an `ax` internal event the state machine matches on
— but it is raised inside the action that produced the token, not posted from
outside. The token payload is the compact `tinyparse_token` the public header
already defines (kind, start, end, flags), never a formatted name.

**Lexical goals** ride in `ax_data` as a small stack, pushed and popped by the
`(goal …)` state's `enter`/`leave` (above). The lexer function reads the top of
that stack to resolve a redirect, exactly as `tp_goal_redirect` reads
`ctx->active_goal` today.

**Extras** are consumed by the same lexer function before each significant token,
folded into the `match_*` action rather than given a state of their own — the
W46 extras memo (`tp_extras_memo`) carries through and answers a revisit in O(1).
Giving extras their own state per element would double the transition count for
no gain, since extras consumption emits nodes as a side effect but never branches
the machine.

## The crux: PEG backtracking on a statechart

Here is the tension stated plainly. An `ax` statechart moves forward: an event
arrives, a transition fires, the active configuration changes, and `ax` has no
operation that says "put the configuration back where it was three transitions
ago." PEG ordered choice needs exactly that — try alternative A, and if it fails
after consuming input and emitting half a subtree, restore the input cursor and
discard the subtree before trying B. The naive mapping, "the statechart's
configuration *is* the parse stack, and backtracking rewinds the configuration,"
asks `ax` for the one thing it does not offer. That mapping does not fit, and
saying so is the honest core of this document.

The mapping that *does* fit keeps the backtrack state in data, not in the
statechart's configuration:

> The parser actor holds an explicit **backtrack stack** in its `ax_data`.
> Entering a choice (or a repeat iteration, or an optional) pushes a frame
> `{ cursor, builder_mark, sink_mark, goal_depth, extras_depth, recover_flag }`
> capturing everything a failed alternative must undo. A `fail` internal event,
> raised by `ax_raise`, restores the frame's marks and transitions to the next
> alternative's substate. A success pops or rewrites the frame.

The statechart's *configuration* never rewinds. What rewinds is the data the
frame snapshotted — the input cursor (today's `ctx->position`), the
tree-builder count (`builder.count`, which truncates a failed subtree wholesale
because nodes are linked into a parent only after the body matches), and the
sink count. These are precisely the `saved_count`/`saved_pos`/`saved_sink`
locals scattered through `tp_parse`'s `TP_OP_CHOICE`, `TP_OP_REPEAT`, and
`TP_OP_OPTIONAL` cases, gathered into one explicit stack so they survive across
transitions instead of living on the C call stack.

Mapped to concrete `ax` states and transitions, a three-way choice is:

```c
{ .id = "choice", .enter = push_backtrack_frame,
  .initial = "alt0",
  .states = (const ax_state[]){
      { .id = "alt0", .enter = try_alt0,
        .transitions = (const ax_transition[]){
            { .on = "matched", .to = "commit" },
            { .on = "fail",    .to = "alt1", .then = restore_frame_marks },
            { 0 } } },
      { .id = "alt1", .enter = try_alt1,
        .transitions = (const ax_transition[]){
            { .on = "matched", .to = "commit" },
            { .on = "fail",    .to = "alt2", .then = restore_frame_marks },
            { 0 } } },
      { .id = "alt2", .enter = try_alt2,
        .transitions = (const ax_transition[]){
            { .on = "matched", .to = "commit" },
            { .on = "fail",    .to = "exhausted", .then = restore_frame_marks },
            { 0 } } },
      { .id = "commit",    .kind = AX_FINAL, .enter = pop_frame },
      { .id = "exhausted", .kind = AX_FINAL, .enter = pop_frame_then_reraise_fail },
      { 0 } },
}
```

`restore_frame_marks` reads the top frame and writes the cursor and builder marks
back; `try_altN`'s enter action runs the alternative (an inline submachine call,
or a token match) and raises `matched` or `fail`. `commit` pops the frame and
the choice succeeds; `exhausted` pops and re-raises `fail` to whatever frame
encloses *this* choice, which is how failure propagates outward.

**Nested choices** compose because the frame is a stack: an inner choice pushes
its own frame, and its `exhausted` re-raises `fail`, which the outer choice's
current alternative catches and turns into *its* `restore_frame_marks` + advance.
The stack depth is the grammar's choice-nesting depth at the current position,
bounded by the grammar, arena-allocated, and never the C stack — which is a
robustness win over today's `tp_parse`, whose recursion depth is the grammar
depth on the C stack.

**Repeat is iterated choice** and reuses the frame, but a successful iteration
calls `rewrite_frame_cursor` (advance the saved cursor to the new position)
instead of pushing a fresh frame, so an `n`-element repeat costs one frame, not
`n`. This is the in-place rewrite the `TP_OP_REPEAT` loop achieves by reassigning
`saved_pos` each pass; `bytecode-vm.md` names it `PartialCommit` after LPeg.

### Why not record/replay speculation

`ax` offers a tempting native affordance: record/replay
(ax's `src/ax/ax-design.md`, *Durability*).
One could imagine speculating an alternative by recording the actor's events,
and on failure "replaying" to an earlier state. This does not fit, for three
concrete reasons. First, `ax`'s replay rebuilds an actor *from scratch* by
re-delivering its whole external journal from seq 0 — it is crash recovery, not a
checkpoint-and-rollback within a live run, and there is no "replay to seq N then
stop and continue live" primitive. Second, replay deliberately *suppresses*
sends, port emits, and child spawns
(ax's `src/ax/ax-design.md`, *Suppression
rules during replay*), so a sub-parse that spawned a child or emitted a
diagnostic could not be faithfully re-run as speculation. Third, the cost is
backwards: rolling back via replay re-derives all the work, where the
mark-and-restore stack throws the failed work away in O(1). The arena/builder
mark approach is not just a fit for `ax`; it is strictly cheaper than the native
durability machinery for this purpose. Record/replay earns its keep for
[checkpoint/resume of a whole parse](#recovery-plugs-and-durability), not for
inner backtracking.

### Carrying the W45 memo and W46 prediction through

Backtracking is where W45 and W46 paid off, and the statechart must not lose
them. Both survive because both are keyed on data the actor still holds.

The **W45 token-match memo** (`tp_match_memo`, keyed on `(pattern, offset,
goal)`) lives in `ax_data` and is consulted by the lexer function the `try_altN`
and `match_*` actions call. A choice that retries the same offset under the same
goal hits the cache, exactly as today — the retry is now a `fail`-driven
transition to the next alternative rather than a loop iteration in
`TP_OP_CHOICE`, but the call into `tp_token_match_memo` is identical, so the memo
is identical.

The **W46 first-set prediction** (`tp_choice_first_admits`) folds into the
choice's alternative-advance. Before `try_altN`'s enter action runs the
alternative, it tests the lookahead byte against the alternative's cached FIRST
set and, on a provable miss, immediately raises `fail` to skip to the next
alternative without pushing or matching anything. The FIRST set is already
computed once per node and cached on the IR (`choice_first_set`); lowering
carries the pointer onto the transition. This is the same eighteen-of-nineteen
pruning the profile credits, now expressed as an early `fail` instead of a
`continue` in the choice loop.

The **extras memo** carries through as described under
[the lexer](#the-lexer-and-token-events): the lexer function consults it, and a
revisit to a proven-empty offset answers in O(1).

## Streaming and the pump

The public streaming contract is unchanged: `tinyparse_push(bytes)` buffers a
chunk, `tinyparse_finish()` finalizes, and the tree comes out through the
cursor API. Under the new architecture these map to `ax` as follows, and the
mapping is deliberately *coarse*.

**`tinyparse_push(chunk)` posts one event to the parser actor.** It appends the
bytes to the actor's input buffer (in `ax_heap`) and posts an `input.chunk`
event via `ax_post`. The transition that handles `input.chunk` runs the parser
forward — many transitions, the whole inner advance through the grammar — until
the machine blocks on "need more input" (the cursor reached the buffered end mid
token) or the chunk is exhausted. Then the action returns and the actor settles.

**`ax_step` is pumped to quiescence per push.** After posting, the host loop runs
`while (ax_step(s) == AX_MORE) { }`, exactly the loop
ax's `src/ax/ax-design.md` (*The pump*)
prescribes. The internal `fail`/`matched` cascade and the automatic transitions
run to quiescence within the macrostep `ax` already guarantees runs eventless and
`done.*` chains to settle. The message rate the scheduler sees is one event per
chunk, not one per token and emphatically not one per transition — the
transitions are internal (`ax_raise`), which `ax` runs inside the step without
re-queuing on the external mailbox.

**`tinyparse_finish()` posts an `input.end` event.** The parser, blocked
waiting for more input or sitting at the start rule's `done`, takes the
`input.end` transition: it consumes trailing extras, checks for leftover input
(the `parse.trailing` diagnostic today), freezes the tree, and enters its
top-level final, which raises `done.state` carrying the result. The CST is then
read through the unchanged cursor API.

The one semantic subtlety: today `tp_execute` runs the entire parse at
`finish` time over the fully buffered input, so streaming is buffered, not
incremental. The actor model *permits* incremental parsing — a chunk could
advance the machine as far as its bytes allow — but matching the current public
behavior byte-for-byte means the safe first cut keeps buffering and runs the
machine at `finish`, posting `input.chunk` events that only buffer. Incremental
advance is a later refinement, not a W58 requirement, and it must not change the
observable CST or diagnostics.

## Sub-parses as child actors

A sub-parse fits the model two ways, and choosing per case is where the
architecture stays honest without paying for honesty it does not need.

**An ordinary sub-rule is an inline submachine — never a child actor.** A `(ref
R)` to another rule of the same grammar, over the same input, sharing the cursor
and the backtrack stack, lowers to a compound substate (above). It is the hot
path; it must not touch `ax`'s spawn machinery. Spawning a child per rule
reference would put one `ax_spawn` and one `done.child` on every node of the
parse, which is the per-message cost the design forbids.

**A genuinely separate parse is a child actor — via `ax_spawn` + `ax_outcome`.**
Three cases qualify, each conceptually a *different parser*:

- **A nested grammar.** The design's quasiquote / template-literal example, where
  a template string's content is parsed by a sub-grammar with `${…}` holes
  spliced back to the host grammar, is a different program over a different input
  span with independent completion. It spawns a child actor running the
  sub-grammar's spec over the slice. The child reports its CST through its final
  state's `ax_outcome` (the `done.child.<id>` payload), and the parent splices
  the returned subtree in.
- **A recursion point that cannot stay inline.** A directly or mutually recursive
  rule whose inline submachine nesting would be unbounded falls back to a child
  actor at the recursion edge, using `ax`'s self-naming child spec. This bounds
  the state nesting at the cost of one spawn per recursion descent — coarse
  enough to amortize.
- **The query engine.** [`tp_query_run`](../../src/internal.h) is itself a parser
  over the linearized ENTER/LEAF/EXIT stream. It is the same *kind* of need as a
  sub-parse and would spawn a child actor running a query-matcher spec; this is
  one case, not a second, exactly as [`bytecode-vm.md`](bytecode-vm.md) notes.

**Completion feeds recovery and tracing through `done.child`.** When a child
finishes, `ax` raises `done.child.<id>` in the parent carrying the child's
outcome. The parent's transition on `done.child` either splices the child's
subtree (success) or, if the child failed at end of input, runs the parent's
recovery insertion — so a failed sub-parse and a failed inline element reach the
same recovery path. A trace plug sees the spawn and the completion as the natural
rule-enter / rule-exit boundary events, at child granularity, never per token.

The discipline that keeps this from drifting: a sub-parse becomes a child actor
only when it is a *different parser* (different program, different input span,
independent completion). Everything else is an inline submachine. Getting this
boundary wrong in either direction is the main integration risk — spawn too
eagerly and the message overhead the profile warns about returns; spawn too
rarely and the quasiquote and query cases lose the clean child-completion
semantics recovery and tracing want.

## Recovery, plugs, and durability

**Recovery is an ordinary transition path.** `(recover BODY (on eof
…))` (`TP_OP_RECOVER`) lowers to a state that first tries `BODY` cleanly; on the
body's `fail` at end of input, an armed-recovery transition runs
`insert_missing` — the `tp_recover_insert` action that emits a zero-width
`TP_NODE_MISSING` node and the author's diagnostic — and continues into the next
element instead of propagating `fail`. The "recovery armed" state is the
`ctx->recover` flag of today, carried in `ax_data` and snapshotted into the
backtrack frame (the `recover_flag` field above) so a speculative alternative
correctly disarms recovery and a `fail`-restore re-arms it. This is the subtle
case `bytecode-vm.md` flags: a choice must disarm recovery for its speculative
alternatives, exactly as `TP_OP_CHOICE` saves and clears `ctx->recover` today.

**Plugs and bindings stay at the boundaries the design already drew.** A `(class
NAME)` token primitive calls a host *binding* (a `tinyparse_class_fn`, a direct
C function pointer) from inside the lexer function — a host boundary, not a
message, "for hot predicates" as the design says. Tracing, diagnostics, and the
module resolver are *plugs*, which the design backs with `ax_port`. They fire on
cold or structural events — a rule entered, a token consumed, a diagnostic
raised, a module loaded during compile — never per transition. The trace plug's
natural events are the actor-boundary ones: an `input.chunk` arriving, a child
spawned or completed, the parse finishing. The new architecture inherits this
split unchanged; it is the same split that keeps the hot path off the message
bus.

**Record/replay buys checkpoint and resume for a whole parse.** Now that the
parser is an actor, `ax_record` journals the external events it consumes — the
`input.chunk` and `input.end` events. A parser that crashed mid-parse, or was
evicted to page out RAM under `ax_evict`, wakes by the standard recipe: spawn,
replay the journaled chunks, `ax_live`. Because the inner backtracking is all
internal cascade (`ax_raise`, automatic transitions), it re-derives
deterministically during replay and is not journaled — which is precisely why
the determinism contract holds: the lexer and actions read only `ax_data`, the
arena, and the event, with no clock and no entropy. This is a genuine new
capability the recursive `tp_execute` cannot offer: a long-running parse over a
huge input becomes checkpointable and resumable, and an idle parser pages out for
the cost of its journal. It is the strongest concrete win the re-architecture
buys beyond "the design's promise made real."

## An honest performance estimate

This phase buys architecture, not speed, and the regression is real. Here is
where the cost goes and how large it is, in numbers of the right order.

Today's residual, after W44–W46, is the irreducible recursive-descent walk: a C
function call and a `switch` per IR node ([`performance.md`](../performance.md)
measures the corpus at 12 ms, 4.6× tree-sitter, 0.36 B instructions). The new
architecture replaces each combinator step — what is a `switch` arm and a
recursive call today — with an `ax` transition: an event match (prefix-compared
against the state's transition table), a guard evaluation, an action call, and a
configuration update. An `ax` transition is heavier than a `switch` arm by a
meaningful constant: even an internal `ax_raise`-driven transition walks the
transition array, prefix-matches the event name, and updates the active state,
where the C `switch` does a single indirect jump.

The cost concentrates at **token granularity and finer**. A `seq` of three
elements is three transitions where it was one function with a loop; a `choice`
over nineteen alternatives is up to nineteen `try_altN` enters and `fail`
re-raises where it was a single `for` loop (W46 prunes most, as it does today,
but each survivor is now a transition pair). The per-token overhead — the
`token.matched`/`fail` raise plus the transition it drives — is the dominant new
cost, and tokens are the unit the parse is counted in.

Order-of-magnitude estimate: an internal `ax` transition is plausibly **5–15×**
the cost of the `switch`-arm-plus-call it replaces, and the number of
transitions is within a small factor of today's node visits. That puts the parse
at roughly **5–15× slower than today**, landing the corpus near **25–70×
tree-sitter** — back into the territory W45 and W46 climbed out of, though not
all the way to the 45× baseline because the memo and prediction still hold. The
event-name prefix matching is a specific, attackable cost: compact integer event
codes rather than dotted-string names would recover a chunk of it, and the design
already insists on compact payloads, so this is a known lever, not a surprise.

This is a regression the mandate explicitly accepts. The trade is a genuine `ax`
actor architecture, bounded arena-allocated parse depth (no C-stack recursion),
and checkpoint/resume durability, against a constant-factor slowdown that further
work (integer event codes, optional incremental advance) can partly recover. It
will not approach tree-sitter; it was never going to, and runtime grammar
compilation is the reason, the same reason the bytecode VM stopped short.

## How this differs from the bytecode VM

[`bytecode-vm.md`](bytecode-vm.md) proposed flattening the grammar into a
contiguous bytecode array run by one tight `switch` dispatch loop *inside* an
actor transition — fast, cache-local, and explicitly *faster* than today (2–5×
over the tree walk). This document proposes the opposite trade on the same axis:
keep the grammar as a genuine `ax` statechart, walked by `ax`'s own transition
engine, accepting a regression to *be* genuinely on `ax` machines.

The two are not rivals so much as endpoints. The bytecode VM uses `ax` as a
coarse scheduler around an opaque fast loop; the grammar's structure is invisible
to `ax`. This design makes the grammar's structure *be* the `ax` machine, which
is what "rules are `ax` actors" was always supposed to mean, at the cost the
bytecode VM was designed to avoid. If the decision-maker's priority is the
design's integrity — the grammar visibly lowered to `ax` specs, inspectable
through `ax_snap` and the inspection API, durable through record/replay — this
design delivers it and the bytecode VM does not. If the priority is speed, the
bytecode VM wins and this design loses. The mandate names integrity, so this
design is the one on the table; the bytecode VM remains the answer to a different,
later question ("now make it fast again") and the two could even compose, a
statechart skeleton whose leaf actions dispatch a small bytecode run for a hot
token pattern.

## Decision-gate recommendation

**Verdict: proceed to W58 with named modifications.**

Genuine token-driven `ax`-statechart parsing with PEG backtracking is sound and
buildable — but only in the shape this document specifies, which differs from the
most literal reading of the mandate in two load-bearing ways. The literal reading
fights `ax`; the specified shape does not.

1. **Backtracking lives in `ax_data`, not in the statechart's configuration.**
   The one place this architecture fights `ax` is rollback: `ax` has no operation
   to rewind the active configuration, and any design that tries to make the
   configuration *be* the backtrack stack will not fit. The fix is the explicit
   backtrack-frame stack in `ax_data` with `fail` as an internal event — the
   configuration always moves forward; the snapshotted marks roll back. Build
   W58 this way; do not attempt configuration rewind.

2. **The event boundary is per chunk, not per token.** Posting an external event
   per token (or per byte) would be "on `ax`" in the most literal sense and would
   reintroduce exactly the stringy hot path the design warns against. Token events
   are real but *internal* (`ax_raise`), raised inside the action that produced
   the token and run to quiescence within one macrostep; only `input.chunk` /
   `input.end` cross the actor boundary. Build W58 with this coarse boundary.

With those two modifications the mechanism is genuinely on `ax`: grammars lower
to inspectable `ax` specs, the parser is a spawned actor, tokens drive
transitions, sub-parses are child actors with `done.child` completion, and
record/replay makes a parse checkpointable. The honest cost is a regression to
roughly the low tens of times tree-sitter, which the mandate accepts and which
integer event codes can later soften. The first build step should mirror the
incremental path the design suggests: lower a subset of combinators, run the
actor parser only for grammars that use that subset, fall back to `tp_execute`
otherwise, and use the 291/291 conformance suite as the differential oracle —
the actor parser is correct exactly when it produces the byte-identical CST,
fields, and diagnostics that `tp_parse` produces. Proceed.
