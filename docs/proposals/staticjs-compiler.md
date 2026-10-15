# staticjs: register code from the streaming compiler

**Status:** Sketch (2026-10-15). A design for discussion; nothing here is
decided or scheduled.
**Touches:** `staticjs/src/compiler.c` (what it emits; its streaming
shape stays), `vm.c` (a register evaluator), `value.c` and `internal.h`
(hot helpers), and [exceptions and async](staticjs-exceptions-and-async.md),
whose move table this design answers.
**Not a plan:** ADRs and roadmaps win.

## Whom it serves, who pays, what is out

- **First, the application developer and the operator:** faster guards and
  callbacks.
- **Then the maintainers:** moves decided in one place, by one walk,
  instead of a deferral per feature. That place is ready for `try` and
  `await`.
- **Who pays:**
  - the evaluator's core is rewritten;
  - what the compiler emits changes;
  - the guide's documented fuel costs change once, as one register
    instruction does what several stack instructions did.
- **Out of scope:**
  - an intermediate representation;
  - a syntax tree;
  - optimization passes;
  - inlining callbacks;
  - a JIT;
  - changes to the parser, the events or the values.

## What the compiler must do

Turn events, which arrive in evaluation order (operands before their
operator, every jump forward), into code for a virtual machine, and move
each value at its last use. Nothing more.

## Where the time goes today

Measured with callgrind, as instructions per evaluation. Start-up is
excluded, and a review reproduced the figures.

| Program | Fuel | Instructions | Dispatch's share |
|---|---|---|---|
| `_event.name === "notify.ack"` | 5 | 802 | 19% |
| `pending.has(_event.data.id) && _event.name === "notify.ack"` | 13 | 1,703 | 20% |
| `xs.map(x => x * 2 + 1)`, per element | 7 | 663 | 27% |

The rest of the cost:

- **The operations' own work.** String `===` compares a byte at a time
  (`sjs_str_equal`, `src/internal.h:21`). `.has()` goes through a call
  protocol of about 150 instructions. Small helpers (`sjs_undefined`,
  `sjs_truthy`, ...) are calls out of line in `value.c`.
- **Starting an evaluation.** `sjs_vm_start` runs a full `sjs_vm_reset`
  each time: about 28% of the small guard.
- **The callback protocol:** about 200 instructions per element of `map`.
- **Every value passes through `vm->stack`.** The run loop rereads its
  state from memory before each instruction (`src/vm.c:1724`).

A prototype register loop, with its state in locals and the callback
inlined, ran the `map` at 153 instructions per element (4.3x fewer). The
inlining is not part of this design, so **what the register evaluator
gains with a real call per element is unmeasured**. That is the first
spike.

## The design

### 1. The compiler emits register code, still streaming

The compiler keeps its shape: one pass over the events, jumps patched
when their construct ends. What changes is what an event emits.

- The compiler keeps a stack of **operand descriptions**, where a stack
  compiler would have pushed code. Each says where an operand is: a
  constant, a local's register, or a temporary.
- **Reading a local emits nothing.** It pushes the local's register.
- **An operator pops its operands' descriptions** and emits one
  instruction that names them, writing its result into a temporary. It
  pushes that.
- So `x * 2 + 1` is two instructions (`MUL t, x, #2`, `ADD t, t, #1`)
  where it was five, and no value passes through a push and a pop.
- Temporaries are allocated and freed as a stack: the operand stack's
  depth is the register.

Lua 5 compiles this way, in one pass from its parser to register
bytecode (Ierusalimschy, de Figueiredo and Celes, "The Implementation of
Lua 5.0", 2005). staticjs's events are postfix, which makes it simpler
than Lua's recursive descent. Error order, compile memory and the sink
API are unchanged.

### 2. Moves: one backward walk per function

When a function's code is complete, walk it from its last instruction to
its first, keeping the set of locals still to be read:

- Every jump goes forward, so a reverse walk has seen everything that can
  run after each instruction. The set at a jump is its fall-through's
  set together with its target's, which the walk recorded when it passed
  there.
- An operand that reads a local not read again gets the instruction's
  **move bit** for that operand. A write removes the local from the set.
  A `RETURN` clears the set. A closure's captures count as reads.
- Temporaries are read once, so they always move.
- **`try`**: the handler comes after the code it protects, so its set is
  known when the walk enters that code. It is added at every instruction
  that can throw. That one rule answers the whole move table of the
  exceptions sketch.
- **`await`**: an ordinary instruction to the walk. The frame's registers
  go to the heap with it and come back.

This replaces the last-read machinery of today's compiler (parked reads,
fences, cancels, the special cases of the append and of a `let`
reassigned from itself, `src/compiler.c:795` to `:960`). The rest of the
compiler's patches (a path's root, a mutating method) stay as they are:
each is decided by the end of its own call. Liveness moves a few reads
that today's rules copy (a read followed by a write of its local). Each
such difference is a copy becoming a move, so the test is the spec corpus
compiled both ways, every difference of that kind.

### 3. A register evaluator with its state in locals

- **Frames:** a frame is a window of registers on the value stack:
  parameters, locals, then temporaries.
- **The run loop** keeps `pc`, the frame's base, the code pointer and the
  fuel counter in C locals. It writes them back only when it leaves: a
  call, a pause, a fault, a loop frame's step.
- **Fuel is one unit per instruction, as now**, so pausing anywhere and
  the "at most 2 units more" contract stay. Programs take fewer
  instructions, so their costs in the guide go down once.
- **Loop frames and callbacks** keep today's protocol, which is then the
  next thing to make cheaper.
- **Inspection** is unchanged: frames are still frames.

### Cheap fixes, first and independent

These pay on today's evaluator and on the new one:

- the hot helpers inline;
- strings compared a word at a time;
- a fast start for an idle evaluator;
- shapes' C paths stepping several elements per turn of the run loop.

## Options compared

| | Verdict |
|---|---|
| Today's compiler, a deferral per feature | grows per feature; nothing for speed |
| Today's compiler and stack bytecode, moves by a backward walk | the minimum if only `try` is wanted |
| **Streaming register code and a backward walk for moves, register evaluator** | **recommended: two changes to the compiler, one to the evaluator** |
| A tree per function, an IR, optimization passes, inlined callbacks | considered in an earlier draft. More parts than the problem needs: inlining brought loops into the IR and virtual frames into inspection |
| Buffering each function's events before emitting | refuted: nested code inline, order of errors, memory |
| A tree-walking interpreter | more dispatch, not less |

## Order of work

1. **The cheap fixes**, each with `bench-compare --quick`.
2. **Spike:** the register loop of the review's prototype with a real call
   per element, against 663 instructions per element, and the guard.
   Settles whether step 3 pays.
3. **The backward walk for moves on today's stack bytecode.** It pays
   whatever step 4 becomes, and is what `try` needs.
4. **Register code and the register evaluator**, behind a switch in
   `sjs_compiler_config` and `sjs_vm_config`. The spec runs both ways
   with `diff-node`, and the two may differ in fuel only.
5. Switch the default, delete the stack evaluator, then `try`, `throw` and
   `async`.

## Open questions

- Whether a cheaper call (the callback protocol) closes most of the gap
  before register code is needed.
- Interning strings that are compared often, such as event names.
