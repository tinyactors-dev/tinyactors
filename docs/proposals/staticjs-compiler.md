# staticjs: a compiler and evaluator designed together

**Status:** Sketch (2026-10-15). A design for discussion; nothing here is
decided or scheduled.
**Touches:** `staticjs/src/compiler.c` and `vm.c` (replaced),
`matcher.c` (retargeted from events to the IR), the loop frames in
`builtins.c`, the documented fuel costs, and [exceptions and
async](staticjs-exceptions-and-async.md), which this design absorbs. The
parser, its events and the values stay.
**Not a plan:** ADRs and roadmaps win.

## History of this document

A first draft proposed buffering each function's events before emitting.
A review refuted it against the code. A second draft kept the streaming
compiler and decided moves by liveness over its bytecode, which is the
cheapest fix for moves. This third draft starts from what staticjs needs
and from where evaluation time goes, with a rewrite allowed. Measured,
the cost is in the evaluator's execution model, not in the compiler's
decisions. So the two are designed as one.

## Whom it serves, who pays, what is out

- **First, the application developer and the operator:** guards and
  callbacks that cost a fraction of what they cost now. The measurements
  below locate the cost.
- **Then the maintainers:** one place, an IR, where moves, ownership,
  shapes, `try`, `finally` and `await` are decided by the same analyses,
  instead of a deferral per feature.
- **Who pays:**
  - a rewrite of the compiler's back half and of the evaluator;
  - every documented fuel cost changes, as fuel would count blocks of new
    instructions (determinism stays);
  - compile memory for one function's tree at a time.
- **Out of scope:** the parser and the language (what staticjs accepts and
  refuses); a JIT; a garbage collector; loops in the language (the design
  could compile them, the language does not have them). NaN-boxing values
  is listed as a separate question: it changes the public `sjs_value`.

## Requirements, from the repository

1. **Push, never pull** (`staticjs/AGENTS.md`). The parser is pushed bytes
   and pushes events to a sink. The compiler is a sink. It may hold what it
   has received; it never asks for more.
2. **No recursion, no C library, at most three parameters**
   (`staticjs/AGENTS.md`). Every tree walk uses an explicit stack.
3. **Fuel is deterministic, and an evaluation pauses and resumes anywhere**
   (`docs/guide/embedding.md`, "Evaluating, on fuel"). The same program on
   the same inputs uses the same fuel on every machine.
4. **The rule:** the same result as JavaScript, or a fault. `diff-node`
   checks it.
5. **Ownership:** values are never changed once shared. A function changes
   a value only through a variable of its own that holds it alone. A last
   read moves (`docs/internals/mutation.md`).
6. **Every evaluation ends:** forward jumps, no re-entry. With async, also
   the lineage rule of the exceptions sketch.
7. **Errors:** a syntax error only where JavaScript refuses, "unsupported"
   otherwise, the first error found.
8. **Public surface:** the event sink, function indices in source order
   (`include/staticjs.h:930`), frames for inspection, `sjs_value`.
9. **Coming:** `try`/`finally`, `throw`, `async`/`await` with suspension to
   the host ([exceptions and async](staticjs-exceptions-and-async.md)).

## Where the time goes today

Machine instructions per evaluation, counted with callgrind. The driver
compiled once and ran each program 2,000 or 20,000 times; the counts are
differences between runs, so start-up is excluded. Built with clang 18
`-O2` on x86_64.

| Program | Fuel | Instructions | Per fuel unit |
|---|---|---|---|
| `_event.name === "notify.ack"` | 5 per evaluation | 802 per evaluation | 160 |
| `pending.has(_event.data.id) && _event.name === "notify.ack"` (the bench's guard) | 13 per evaluation | 1,703 per evaluation | 131 |
| `xs.map(x => x * 2 + 1)` | 7 per element | 663 per element | 95 |

Where it goes:

- **The small guard**, per evaluation:
  - `sjs_vm_run` takes 596 instructions for five bytecodes;
  - `sjs_vm_start` takes 166;
  - `sjs_vm_reset` and `sjs_vm_take_result` take about 60.
- **`map`**, per element:
  - the loop protocol takes about 245: `sjs_loop_step` 177 inclusive and
    `sjs_vm_call_back` 68;
  - the callback's six bytecodes (`LOCAL`, `NUMBER`, `MULTIPLY`, `NUMBER`,
    `ADD`, `RETURN`) and their frame take about 420, about 70 per
    bytecode.

**Why each bytecode costs 70 to 100 instructions.** The dispatch loop
keeps its state in memory, not in registers (`src/vm.c:1724` to `:1741`,
`:1144` to `:1149`). Every instruction:

- reads `vm->status`, `vm->fuel_used`, `vm->frame_count` and
  `vm->frames`;
- tests the frame's kind;
- reloads `f->program->code` and `f->pc`;
- stores `vm->op_pc` and `f->pc`;
- pushes and pops 16-byte values through `vm->stack` with a capacity
  check.

The hottest lines of the `map` run are the frame fetch (8.5 M of 134 M
instructions), the `switch` (7.3 M), the status, fuel and kind checks,
and `cut`'s release loop. Every value passes through memory between
instructions.

**What this says about the compiler.** No compiler can fix this while it
targets this evaluator: a perfect move analysis saves reference counts,
which are a small part of these numbers. The compiler's real job is to
produce code that an evaluator with its state in registers runs in fewer,
larger instructions, and to remove the per-element call protocol where it
can.

## The architecture

```
bytes ─push─▶ parser ─events─▶ front end ─tree per function─▶ middle end (IR passes) ─▶ back end ─▶ program
                 │                (names, early errors,          (ownership, moves,        (register
                 └─events─▶ other sinks   constants)              shapes, liveness,         bytecode,
                                                                   allocation)              tables)
```

### Front end: a tree per function, built from the events

The compiler stays a sink. It builds each function's syntax tree in an
arena: nodes of 16 bytes, children as indices, positions as byte
offsets, with a line table for errors. It also does what must stay in
event order:

- **Names and early errors, as the events arrive.** Every name is
  declared before it is used, or the program is refused, so names
  resolve in one forward pass. The first syntax or name error still stops
  the parser at the event that shows it (requirement 7).
- **Function indices** are taken when a function begins, in source order
  (requirement 8).
- **Literals of constants fold as they close.** `[1, 2, 3]` becomes one
  constant, rather than `ARRAY_NEW` and a push per element as now
  (`src/compiler.c:3110`). It costs less at run time and in the tree. A
  literal bound to a variable that a mutation reaches must be a new value
  nothing else holds. The ownership pass marks it, and the back end
  emits a copy of the constant for it, made once per evaluation.

The tree, unlike the events, gives each node its children. That removes
what made the first draft fail:

- a path's root is found from the node, not by event order;
- a call knows its callee;
- `ends_in_return` and `assign_callee` become questions about a node
  rather than about code already emitted.

Each function is compiled to its own code array when it ends, so a nested
function's code no longer sits inline in its parent's code behind a
`JUMP`. The parent's tree keeps one node for the nested function, with
its index and its captures.

A module's top level holds declarations only. It is compiled once, at the
module's end. That is the point where its exports and its last reads are
known; today's compiler decides them there too.

### Middle end: an IR with join points, ownership made explicit

Each function lowers to a small first-order IR in A-normal form:

- Every intermediate value has a name.
- Control is blocks joined by **join points**: second-class continuations,
  which compile to jumps, as in Kennedy, "Compiling with continuations,
  continued" (ICFP 2007), and Guile's CPS soup.
- Every jump goes forward, so the graph is a DAG, and SSA form comes for
  free: a `let` assigned in two branches gets a φ at the join (Braun et
  al., "Simple and efficient construction of static single assignment
  form", CC 2013, with every block sealed at once).
- `try` gives every call and throw inside it a second edge, to the
  handler's join point. `finally` is a join point that receives a
  completion record (`tinyjs.md`, layer 1, designs the same).
- `await` is an instruction that ends its block. Its successor block is
  the resumption.

Passes, each a walk with an explicit stack, in order:

1. **Ownership (Perceus).** Reinking, Xie, de Moura and Leijen, PLDI
   2021, as `docs/proposals/staticjs.md`, stage 3, already cites.
   - Liveness over the DAG, exception edges included, in one backward
     walk. A use where the value is dead after it consumes the
     reference: a move. Every other use borrows or duplicates.
   - The ownership rules of `mutation.md` become checks on this graph:
     - "Captured before or after" means the value flows into a closure's
       captures anywhere in the function.
     - "Copied, passed, stored before" means another live reference to
       it exists at the mutation.
     - What the compiler cannot prove stays a run-time uniqueness check,
       as now.
   - Every row of the exceptions sketch's move table is this one rule: a
     handler is a successor.
   - Lean 4's IR does the same for its compiled code (Ullrich and de
     Moura, "Counting immutable beans", IFL 2019): explicit reference
     counting, borrow inference for parameters, and reuse of a cell
     nothing else holds.
2. **Reuse.** A dropped value of the right size feeds a new one: `{...acc,
   [k]: v}` in a reduce, a `concat` onto a moved array. Today's take-overs
   in the built-ins become one analysis.
3. **Shapes, as tree rewriting on the IR.**
   - The iburg matcher is a tree matcher; it was adapted to the event
     stream (`docs/internals/matcher.md`). On the IR it matches trees
     directly.
   - It does two things:
     - tags a callback with its shape, for the C paths, as now;
     - **inlines a callback literal into its loop** (below).
4. **Liveness and register allocation.** A DAG has no back-edges, so
   linear scan over the blocks in order allocates registers directly.
   The same liveness records:
   - the live registers at each `await`, which is what an activation
     saves;
   - the live registers at each handler.

### Back end: register bytecode, and loops the compiler emits

- **Registers, not a stack.**
  - A frame is a window of registers on the value stack, as in Lua 5:
    parameters, `let` and `const` slots, and temporaries.
  - An instruction names its operands. A bit on an operand marks a move:
    the last use, from pass 1.
  - So `x * 2 + 1` is one or two instructions, not five, and nothing
    passes through a push and a pop.
  - Shi, Gregg, Beatty and Ertl, "Virtual machine showdown: stack versus
    registers" (VEE 2005; Shi, Casey, Ertl and Gregg, TACO 2008),
    translating Java bytecode to a register form, report close to half
    the instructions dispatched and about a third less run time, with
    about a quarter more code (figures quoted from memory, to check
    against the papers). **Unverified here:** the ratio for staticjs's
    instruction mix.
- **An internal loop instruction for callbacks inlined by the compiler.**
  - `xs.map(x => x * 2 + 1)` becomes, when `xs` is an array at run time
    (a guard on the kind, falling back to the call): a loop over the
    elements that runs the callback's code in the caller's frame, its
    parameter in a register, with no frame pushed and no call made.
  - The loop is over a collection that already exists, so every
    evaluation still ends.
  - This removes the 245 instructions of loop protocol per element for
    the common case. Shapes stay for the cases the C paths do better
    (sorting by a key).
- **Fuel per block.**
  - Each block's instruction count is charged when the block is entered,
    and an evaluation pauses at a block boundary.
  - The total per evaluation stays deterministic and depends only on the
    program and its inputs. The check is per block, not per instruction.
  - A block that does not fit in what is left of the fuel pauses before
    it, unless nothing has run yet in this run; then it runs whole, and
    the overrun is at most one block.
  - Built-ins charge per element as now.
- **Evaluator state in registers.**
  - `pc`, the frame's base and the code pointer live in locals of the
    run loop.
  - They are written back only when the loop leaves: a call, a pause, a
    fault, or a step of a loop frame.
- **Tables per function:**
  - handlers (`pc` ranges to a handler and its live registers);
  - suspension points (a resume `pc` and its live registers);
  - lines.

### How each feature lands

| Feature | Where it is decided | Cost when unused |
|---|---|---|
| moves | pass 1, an operand bit | none |
| owned mutation | pass 1 proves or leaves a run-time check | none |
| `try` / `throw` | an exceptional edge in the IR; a handler table in the back end | none: tables are read only on a throw |
| `finally` | a join point taking a completion record (two registers) | none |
| `await` | a block end; an activation saves only the live registers | none |
| shapes | pass 3 on the IR | none |
| callbacks over arrays | pass 3 inlines, the back end emits the loop | a kind guard per call site |
| termination | forward-only jumps, no re-entry, the lineage rule for jobs | as now |

## Costs

- **Compile memory:** the tree of each open function, 16-byte nodes. The
  events run at 0.25 to 1 per source byte (refutation spike), so about 4
  to 16 bytes per source byte for the largest function open. Folding
  literals keeps data out of it. A nested function's tree is freed when
  it is compiled. The module top level is held until its end, as its
  decisions are made there. **Unverified:** whether today's peak (23 to
  50 bytes per source byte) goes down, as code is no longer kept inline
  and literals become constants.
- **Compile time:** several passes over trees the compiler just built.
  **Unverified:** today's compile time is about the parse's, and a
  guard's compile cost matters where charts are loaded often.
- **Run time:** the purpose. **Unverified:** the factor, until the spike.
  The numbers above are the baseline.
- **Fuel:** every documented cost changes once (the guide's "5 units" for
  a filter, `docs/guide/performance.md`). Determinism holds.
- **Code:** a new compiler back half and a new evaluator core. The
  built-ins' leaf calls, values, regular expressions, JSON, Temporal and
  the writer keep their interfaces.

## Options compared

| | Representation | What it solves | Verdict |
|---|---|---|---|
| A | streaming, a deferral per feature | features, one at a time | the cost grows per feature, and the evaluator is untouched |
| E | streaming, liveness over stack bytecode | moves, `try` | the best fix within today's evaluator; leaves the 70 to 100 instructions per bytecode |
| B | events buffered per function, today's emitter | moves | refuted (nested code inline, order of errors, memory) |
| **This** | tree per function, ANF with join points, register bytecode | moves, ownership, `try`, `await`, shapes, inlined callbacks, dispatch cost | recommended, subject to the spike |
| Full SSA with a general CFG | same as this, plus loops' fixed points | nothing more without loops; ANF on a DAG already is SSA |
| CPS with heap continuations | suspension anywhere | the evaluator already pauses anywhere; heap continuations are slower sequentially (Farvardin and Reppy, PLDI 2020, in `docs/research/tinyjs.md`) |
| Tree interpreter (CEK) | the syntax tree | suspension, simplicity | more dispatch per node than bytecode; the opposite of what the measurements ask for |

## Order of work, with spikes first

1. **Spike: the evaluator.**
   - Hand-write the register code for `xs.map(x => x * 2 + 1)` (an
     inlined loop) and for the small guard.
   - Run them in a prototype run loop that keeps its state in locals and
     charges fuel per block.
   - Count with callgrind against the table above. This settles the
     factor, and whether the rewrite pays.
2. **Spike: the front end.** Build trees from the events for the spec
   corpus and the bench modules. Measure the memory and time, and check
   that names and early errors keep today's messages and order.
3. If both pay, build the new compiler and evaluator behind
   `sjs_compiler_config` and `sjs_vm_config` switches. Run the spec, both
   ways, with `diff-node`. The two may differ in fuel only, and in which
   unsupported error wins where a later syntax error exists: there the
   new order is closer to JavaScript's, which reports the syntax error.
4. Switch the default. Delete the old compiler and evaluator. Then
   `try`, `throw` and `async` on the new one.

## Open questions

- **NaN-boxed 8-byte values** instead of 16-byte `sjs_value`s. They halve
  the memory traffic of every register and element. They change the
  public value type and every binding's handling of it. This is a
  separate decision, measured after step 1.
- **Inlining callbacks other than literals.** A `const f = x => ...`
  passed by name could be inlined, and so could a module's exported
  function.
- **Superinstructions** chosen from the shape rules (a member read and a
  comparison, as guards do).
- **Whether fuel per block** should keep a per-instruction count for
  inspection (`sjs_vm` frames, the debugger's stepping).
