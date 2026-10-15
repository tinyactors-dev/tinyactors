# staticjs: a compiler that sees a whole function before it emits

**Status:** Sketch (2026-10-15). A design for discussion; nothing here is
decided or scheduled.
**Touches:** `staticjs/src/compiler.c` (its structure; the bytecode, the
evaluator and the parser stay as they are), `staticjs/src/matcher.c` (its
rewrites), and [exceptions and async](staticjs-exceptions-and-async.md),
which it makes simpler.
**Not a plan:** ADRs and roadmaps win.

## Whom it serves, who pays, what is out

- **First, the maintainers.** Each feature since owned mutation has
  added a way to take back a decision the compiler made too early. Try
  and async would add more (the move table in [exceptions and
  async](staticjs-exceptions-and-async.md#the-move-problem)).
- **Then the application developer, indirectly.** Moves the streaming
  compiler cannot prove today (a read after a `try`, `s = s + a + b` in a
  `try`) become provable, and stay linear.
- **Who pays:** compile memory, about 8 bytes per source byte of the
  largest function held at once (measured below), and a second and third
  pass over that function's events. Programs run the same bytecode, so
  evaluation pays nothing. The public API does not change: the parser
  still pushes events to a sink, and the compiler is still a sink.
- **Out of scope:** a new bytecode, a new evaluator, a tree-walking
  interpreter, SSA, and loops. Whether staticjs gets loops is a language
  decision ("every evaluation ends"); this design only removes the
  compiler as the reason it cannot.

## The frame: what the compiler is today

1. **One pass, an instruction per event.** "Events come in evaluation
   order, so most become one instruction; the branching constructs keep
   their jumps on a stack and patch them when their end arrives"
   (`src/compiler.c:1`).
2. **Decisions are made at an event and taken back later.** Ten places
   overwrite an instruction already emitted (`c->code.ptr[...] =`): a
   read becomes a move (`src/compiler.c:823`, `:2865`), a read becomes
   the root of a path (`:2248`, `:2434`, `:2806`), a member becomes a
   mutating method (`:2283`, `:2474`), and `ends_in_return` looks back at
   the last instruction (`:2603`).
3. **Facts the compiler needs arrive after the event they decide.**
   Whether a read is the last needs every later access; "never captured,
   before or after" needs every later closure
   (`docs/internals/mutation.md`, "The rule, at compile time"); whether a
   call is a mutation on a path needs its `CALL_END`. So the compiler
   keeps side machinery for each: last reads per slot, parked reads with
   fences for branches and returns (`src/compiler.c:795` to `:960`, about
   160 lines), the held path of a call, the append and `store_slot`
   moves.
4. **The matcher reconstructs trees from the same stream.** Its design
   note starts from this: "The compiler keeps reinventing small pattern
   matchers over the events it receives" (`docs/internals/matcher.md`).
   Its rewrites (section 12) then patch what the compiler emitted.
5. **The parser already holds events when order requires it:** a `const`'s
   pattern is read before its value and sent after it, held up to
   `max_held_events` (`docs/guide/language.md`).
6. **The events are a tree already.** They come in evaluation order,
   operands before operators, and each carries its node and where its
   node starts (`sjs_event.node`, `sjs_event.start`,
   `include/staticjs.h:551`). A function's events, kept in order, are its
   syntax tree in post-order.
7. **Every jump goes forward**, so instruction order is execution order on
   every path, and "no later access" means "last" (`src/compiler.c:55`).
   The move analysis rests on it.

Measured (this machine, a counting sink over `bench/modules`; the
compile benchmarks at the release flags):

| | power.js | campaign.js |
|---|---|---|
| source | 1,071 bytes | 577 bytes |
| events | 292 (0.27 per byte) | 138 (0.24 per byte) |
| compile peak memory today | 24,662 bytes (23 per byte) | 15,268 bytes |

Parsing alone runs at 8.1 MB/s and parsing with compiling at 9.2 MB/s on
power.js (noise of this machine: the compiler's own share is too small to
separate), so compile time is the parser's.

## The problem

Every decision in fact 2 is made when an event arrives, before the facts
of fact 3 do. Each new feature adds facts that arrive later still:

- `try` adds a reader of every slot (the handler, and the code after the
  `try`) that the compiler meets after the reads it must keep;
- `async` adds a function-wide property (it suspends) that changes what a
  call, a return and a callback compile to;
- the late take of the costs section needs to know, at a read, what
  consumes it.

The streaming design answers each with another deferral: park, fence,
cancel, patch. It works, but each one is its own state machine with its
own cases (`mutation.md`'s "Last reads in branches" is four rules for one
construct), and they interact.

## Options

| | What | Keeps | Costs | Verdict |
|---|---|---|---|---|
| A | Keep streaming; add the deferrals try and async need | everything | each feature adds a state machine and its interactions with the others | the default; what the exceptions sketch does |
| B | **Hold a function's events; decide with passes over them; emit with today's emitter** | the parser, the events, the bytecode, the evaluator, the emitter's per-event code | memory for the largest function's events; two passes more | recommended |
| C | An IR with a control-flow graph (SSA), as [tinyjs](../research/tinyjs.md) designs | the parser | a new middle end and a new emitter; register allocation; the bytecode's stack form fits it badly | far more than the problem needs: staticjs's control flow is structured |
| D | Interpret the syntax tree (a CEK machine, [tinyjs, the small way](../research/tinyjs-small.md)) | the parser | the bytecode evaluator, its fuel costs and its shapes' C paths, all benchmarked | gives up the tuned evaluator for a problem in the compiler |

## Option B

### The buffer

The compiler keeps the events of each open function in an array, in
order, as compact records (kind, flags, node, start, a payload index;
texts interned into the constants as today, about 32 bytes a record).
With 0.25 events per byte that is about 8 bytes per source byte, held
for the open functions only: a nested function's events are processed
when it ends, and the outer function keeps one record for it (its
compiled function index and its captures). A module's top level has no
control flow (imports, `const`, exports), so each top-level statement is
processed when it ends, and a module never holds more than its largest
statement.

A large literal (a module that exports a table of data) would make that
statement large. Literal arrays and objects of constants already become
constants; doing so as each literal closes keeps them out of the buffer,
as the compiler does now. **Unverified:** how big the largest function
in real plugins and agent programs is; the synthetic module and the spec
corpus give an upper bound to measure.

### The passes

Each pass walks the array forwards or backwards with an explicit stack,
so nothing recurses (`mise run check-recursion`), and each writes its
answers into a side array, one entry per event:

1. **Names** (forward): scopes, slots, captures, uses before declaration,
   as now. A closure's captures are known when its record is reached, as
   the nested function was processed first.
2. **Ownership** (forward): for each mutation, assignment and path call,
   whether its root was copied, passed or stored before, and whether it
   is captured anywhere in the function. "Before or after" becomes one
   lookup in the capture set, not a rule that waits.
3. **Liveness** (backward): the set of live slots after each read, with
   the structured merges the constructs need: at an `if`, the union of
   the branches; at a `return`, nothing (or a `finally`'s live set); at a
   `try`, every point inside it also has the handler's live set. A read
   whose slot is not live after it moves. This is the textbook analysis
   on a structured program, one walk, as there are no loops; it replaces
   last reads, parked reads, fences, cancels, the append's and
   `store_slot`'s moves and their special cases with one rule.
4. **Shapes and rewrites**: the matcher runs over the array as it runs
   over the stream now (it is a tee today), and a rewrite becomes an
   answer in the side array rather than a patch.
5. **Emit**: today's per-event emitter, reading the side array where it
   now guesses: a read emits `TAKE_LOCAL` or `LOCAL`, a path root
   `RECEIVER`, a mutating method `METHOD_MUTATE` or `PATH_METHOD`. Nothing
   is patched but jump targets.

### What it buys

- **The deferrals go.** Fact 2's patches of reads, roots and methods,
  and fact 3's side machinery, become answers computed before emission.
  Each answer is a function of the whole function, so the rules in
  `mutation.md` read as what they decide, not when.
- **Try is one rule.** The move table of the exceptions sketch becomes:
  the handler's live set flows into the `try`. The deferred moves of a
  `return`, of `store_slot` and of an append need nothing of their own.
- **The late take is a lookup**: at a read, the consumer is known.
- **Async is a flag known before the first event of the body is
  emitted**, rather than one that changes a call already compiled.
- **Better answers where streaming must guess.** `s = s + a + b` inside a
  `try` moves when the handler and what follows do not read `s`.
- **Loops become a language question.** Liveness over a loop needs a
  fixed point, which on a structured loop is two walks of its body; the
  compiler could support them. Termination would then rest on fuel.

### What it does not change

The parser, the events and the sink API; the bytecode and the evaluator;
the matcher's rules and generator; fuel; the order of compile errors
within what the streaming compiler can already know. The rule ("the same
result as JavaScript, or a fault") is not touched: this changes when the
compiler decides, not what.

## Lifetimes

| Object | Owner | Made | Read by | Ends |
|---|---|---|---|---|
| a function's event records | the compiler | as each event arrives | the passes | when the function has been emitted |
| a nested function's record in its parent | the parent's buffer | when the nested function ends | the parent's passes | with the parent's buffer |
| side arrays (names, ownership, liveness, rewrites) | the compiler | per pass | later passes, the emitter | with the function's buffer |
| a top-level statement's records | the compiler | as its events arrive | its passes | when the statement has been emitted |
| interned texts and constants | the program, as now | as now | as now | as now |

## Costs

- **Memory:** the open functions' event records, about 8 bytes per byte
  of source (32-byte records at 0.25 per byte; **unverified** record
  size), plus the side arrays, a few bytes per event. Today's peak is 23
  bytes per source byte for a whole module, which already holds the
  bytecode and constants. A guard or a small function adds little; a
  module's top level is bounded by its largest statement.
- **Time:** two or three walks over arrays the compiler just wrote, on
  top of a parse that dominates today. **Unverified** until step 1 below
  is built and `compile.*` compared.
- **Code:** the passes are new; fact 3's machinery and fact 2's patches
  go. **Unverified** which is larger.

## Order of work

Each step keeps the bytecode of every spec case identical, which is the
test: compile the whole spec corpus before and after and compare the
programs byte for byte, besides `mise run test` and `diff-node`.

1. **Hold and replay.** Buffer each function's events and feed them to
   today's compiler at the function's end. No decision changes; this
   measures the memory and time of option B alone (`compile.*`, the
   peak column).
2. **Liveness.** Compute moves with the backward pass; delete last reads,
   parked reads, fences and the append and `store_slot` special cases.
   The bytecode stays identical, and any difference is a bug in one of
   the two.
3. **Ownership and roots.** Compute mutations, path roots and methods in
   the forward pass; delete their patches.
4. **Rewrites as answers** from the matcher.
5. Then [exceptions and async](staticjs-exceptions-and-async.md) on top,
   with the handler's live set in step 2's pass.

## Open questions

- Is the bytecode identical after step 2 in every case, or does the
  streaming analysis keep a copy somewhere liveness would move? A
  difference there is a move today's compiler misses, worth seeing.
- The size of the largest function in real programs, and whether a limit
  (`max_function_events`) is needed beside `max_held_events`.
- Whether the parser's held events for a `const`'s pattern can go once
  the compiler holds the function anyway.
