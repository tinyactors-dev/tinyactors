# staticjs: a compiler and evaluator designed together

**Status:** Sketch (2026-10-15). A design for discussion; nothing here is
decided or scheduled.
**Touches:** `staticjs/src/compiler.c` and `vm.c` (replaced in the end),
`value.c` and `internal.h` (hot helpers), the loop frames in
`builtins.c`, `matcher.c` (retargeted to the IR), the inspection API,
and [exceptions and async](staticjs-exceptions-and-async.md), which this
design absorbs. The parser, its events and the values stay.
**Not a plan:** ADRs and roadmaps win.

## History of this document

- **First draft:** buffer each function's events before emitting.
  Refuted against the code.
- **Second draft:** keep the streaming compiler, and decide moves by
  liveness over its bytecode. That is the cheapest fix for moves.
- **Third draft:** with a rewrite allowed, start from what staticjs needs
  and where evaluation time goes. A review then measured prototypes
  against it. This version keeps what survived:
  - the time per bytecode is mostly the operations' own work, not
    dispatch;
  - the register evaluator's gain is real but smaller than the third
    draft implied: about 4 times on callbacks, about 2 on guards;
  - fuel per block is not worth its broken contracts;
  - inlined callbacks need loops in the IR and frames for inspection.

## Whom it serves, who pays, what is out

- **First, the application developer and the operator:** guards and
  callbacks that cost a fraction of what they cost now.
- **Then the maintainers:** one IR where moves, ownership, shapes, `try`,
  `finally` and `await` are decided by the same analyses, instead of a
  deferral per feature.
- **Who pays:**
  - the work in stages: helpers first, then the evaluator, then the
    compiler;
  - fuel costs change where the compiler fuses instructions;
  - compile memory for one function's tree at a time;
  - code size, where a callback is inlined and also kept as a function.
- **Out of scope:** the parser and the language; a JIT; a garbage
  collector; loops in the language. NaN-boxed values are a separate
  question.

## Requirements, from the repository

1. **Push, never pull** (`staticjs/AGENTS.md`). The compiler is a sink. It
   may hold what it received; it never asks for more.
2. **No recursion, no C library, at most three parameters.**
3. **Fuel is deterministic**, an evaluation pauses and resumes anywhere,
   and a run uses at most 2 units more than it was given
   (`include/staticjs.h:699`, `docs/guide/embedding.md`).
4. **Inspection:** a callback of `map` is a frame, stepped one unit at a
   time (`include/staticjs.h:697`, `embedding.md:639`, `api_test.c:3901`).
5. **The rule:** the same result as JavaScript, or a fault.
6. **Ownership:** values never change once shared, and the last read
   moves (`docs/internals/mutation.md`).
7. **Every evaluation ends.**
8. **Shapes:** `no_shapes` gives the same program but for the tags, and
   never less fuel (`include/staticjs.h:659`, `tests/spec_test.c:15`).
9. **Errors:** a syntax error only where JavaScript refuses, the first
   error found.
10. **Coming:** `try`, `throw`, `async`, `await`.

## Where the time goes today

Measured with callgrind. A driver compiled the program once and ran it
2,000 or 20,000 times; the counts are differences between runs, so they
exclude start-up. Built with clang 18 `-O2` on x86_64, and reproduced by
the review.

| Program | Fuel | Instructions | Dispatch and fuel | Start, reset, take |
|---|---|---|---|---|
| `_event.name === "notify.ack"` | 5 | 802 | about 154 (19%) | about 225 (28%) |
| `pending.has(_event.data.id) && _event.name === "notify.ack"` | 13 | 1,703 | 343 (20%) | about 225 |
| `xs.map(x => x * 2 + 1)`, per element | 7 | 663 | 180 (27%) | none |

What the rest is:

- **The operations' own work.**
  - `===` on two strings costs about 150. `sjs_str_equal`
    (`src/internal.h:21`) compares a byte at a time, and
    `sjs_is_primitive` and `sjs_boolean` are calls out of line.
  - `pending.has(...)` costs 331 inclusive: the call protocol about 150,
    `method()` 74, `sjs_builtin_call` 57, `sjs_set_contains` 106.
  - `sjs_undefined`, `sjs_boolean`, `sjs_is_primitive` and `sjs_truthy`
    are functions in `value.c`, not inline. About 40 instructions per
    guard.
- **Starting an evaluation.**
  - `sjs_vm_start` runs `sjs_vm_reset` every time. Reset releases the
    program, and start retains it again; reset also cuts the stack and
    releases the held globals.
  - Start then checks each global and initialises the frame.
- **The loop protocol of a callback**, per element: about 180 in
  `sjs_loop_step` and `sjs_vm_call_back`, and about 26 for the loop
  frame's own turn of the run loop.
- **Shapes' C paths go around the run loop once per element.** The
  reduce-sum shape costs 136 per element, about 64 of it that
  turnaround; the filter shape costs 232.

## What each change buys, measured or estimated

Per element of `xs.map(x => x * 2 + 1)` (663 today) and per evaluation
of the bench's guard (1,703 today):

| Change | map | guard | How known |
|---|---|---|---|
| Hot helpers inline, strings compared a word at a time, a fast start for an idle evaluator | small | about 150 to 250 less | estimated from the profile |
| Shapes step several elements in one turn of the run loop | shaped callbacks about half | none | estimated |
| Run-loop state in locals, today's stack bytecode | at most about 140 less | at most about 340 less | upper bound: all of dispatch's share |
| Callbacks inlined into a loop, today's bytecode | about 420 (1.6x) | none | estimated |
| Both of the above | about 300 (2.2x) | | estimated |
| **Register bytecode, state in locals, inlined loop** | **153 (4.3x)** | **about 790 to 950 (1.8x to 2.2x)** | **prototypes** (map: real `sjs_value`s and arrays; guard: reads borrowed, `set_contains` direct) |
| Fuel per block instead of per instruction | 8 less (5%) | | prototype |

So:

- The first three rows are cheap, independent of any new compiler, and
  worth doing first.
- The register evaluator with inlined loops is the only change that
  reaches 4 times on callbacks. On guards it does about what the cheap
  rows do together.
- Fuel per block is not worth its broken contracts.

## The architecture

```
bytes ─push─▶ parser ─events─▶ front end ─tree per function─▶ middle end (IR) ─▶ back end ─▶ program
                 └─events─▶ other sinks   (names, early errors)    (ownership, shapes,    (register
                                                                    inlining, liveness,    bytecode, tables)
                                                                    allocation)
```

### Front end: a tree per function

The compiler stays a sink.

- **It builds each function's syntax tree in an arena:** 16-byte nodes,
  children as indices, byte offsets and a line table.
- **Names and early errors stream:** a name must be declared before it is
  used, so names resolve as the events arrive, and the first syntax or
  name error still stops the parser where it shows (requirement 9).
- **Function indices are taken in source order**, when a function begins.
- **Each function is compiled to its own code when it ends**, with a
  lookup of positions per function. The public API exposes positions
  only (`sjs_program_position`), so this is internal.
- **A module's top level is compiled at its end**, where its exports and
  last reads are known, as now.
- **Refusals for ownership** are decided at a function's end, so a later
  syntax error in the same function now wins over them. That is the
  order JavaScript reports.
- **Literals of constants are not folded in general.** A folded literal
  is held by the program too, so every run-time uniqueness check fails:
  - `Object.freeze({a: 1})`;
  - `[1, 2].reduce((acc, x) => { acc.push(x); return acc; }, [])`;
  - `g([1])`, where `g` pushes onto its parameter;
  - a literal returned from an exported function.

  A literal folds only when every use in its function only reads it
  (`[1, 2, 3].includes(x)`).

### Middle end: ANF with join points, loops structured

- **The IR.** A small first-order IR in A-normal form, with join points
  (second-class continuations that compile to jumps; Kennedy, ICFP 2007).
- **Structured loops.** The language's control flow is a DAG, but inlined
  callbacks bring loops into the IR. Their accumulators, reduce's first,
  are carried from one iteration to the next. So the IR has structured
  loops with loop-carried values (φ at the loop's head), and liveness
  takes a fixed point over each loop: two walks of its body, as loops
  are structured and do not nest more deeply than the callbacks do.
- **`try`** gives every call and throw inside it a second edge, to the
  handler's join point. **`finally`** is a join point that takes a
  completion record. **`await`** ends a block.

The passes, in this order, because ownership must see the inlined code:

1. **Shapes and inlining** (the iburg matcher, on the IR's trees):
   - It tags a callback with its shape, as now.
   - It inlines a callback literal into the loop of `map`, `filter`,
     `reduce`, `some`, `every`, `find` and their kin. The loop runs when
     the receiver is an array at run time, arrays with extra properties
     included (`"ab".match(/a/).map(...)` works today); otherwise it
     falls back to the call.
   - The inlined body binds every parameter the callback declares, per
     element: the element, the index, the array, rest parameters.
   - Inlining is a rewrite, not a shape: `no_shapes` keeps it, so the two
     builds stay the same program but for tags (requirement 8).
2. **Ownership (Perceus;** Reinking et al., PLDI 2021, and Lean 4's IR,
   Ullrich and de Moura, IFL 2019**):**
   - Liveness, with exceptional edges and loop fixed points. A use after
     which a value is dead moves it.
   - Inlined parameters borrow from the array. A move of one takes a
     reference first, as `sjs_vm_call_back` does today (`src/vm.c:919`).
   - The rules of `mutation.md` become checks on the graph, and what it
     cannot prove stays a run-time check.
   - Every row of the exceptions sketch's move table is the one rule "a
     handler is a successor".
3. **Reuse:** a dropped value of the right size feeds a new one.
4. **Register allocation:** linear scan with loop-extended intervals.
   It records the live registers at each `await` (what an activation
   saves) and at each handler.

### Back end and evaluator

- **Register bytecode.**
  - A frame is a window of registers on the value stack, as in Lua 5.
  - Operands name registers, and a bit marks a move.
- **The run loop keeps `pc`, the frame's base, the code pointer and the
  fuel counter in locals.** It writes them back when it leaves: a call, a
  pause, a fault, a loop step.
- **Fuel per instruction, as now**, counted in a local; the overrun
  contract stays.
  - Fusing instructions changes fuel. The guide's documented costs change
    once.
  - A fused instruction may cost the units of the instructions it
    replaces, if those numbers should stay. This is an open question.
- **Inspection of inlined loops.** A table per function maps `pc` ranges
  of an inlined loop to a virtual frame: the built-in's name, the call's
  position, the registers that hold the index and the length, and the
  callback's function index. The inspection API reports a loop frame and
  the callback's frame from it, as today (requirement 4). `max_frames`
  counts them as today, so the limit faults where it does now.
- **Tables per function:** handlers, suspension points with their live
  registers, positions, and virtual frames.

### How each feature lands

| Feature | Where it is decided | Cost when unused |
|---|---|---|
| moves | ownership pass, an operand bit | none |
| owned mutation | ownership pass, or a run-time check | none |
| `try` / `throw` | an exceptional edge; a handler table | none |
| `finally` | a join point with a completion record | none |
| `await` | a block end; only live registers saved | none |
| callbacks over arrays | inlined into a loop; virtual frames for inspection | a kind check per call site |
| shapes | tags on the IR, C paths stepping several elements per turn | none |
| termination | forward jumps in user code; inlined loops over existing collections; no re-entry; the lineage rule for jobs | as now |

## Options compared

| | What | Verdict |
|---|---|---|
| Cheap fixes only (helpers, start, strings, shapes' stepping) | about 10 to 15% on guards, about half on shaped callbacks | do first, whatever else is chosen |
| E: streaming compiler, moves by liveness over stack bytecode | moves and `try`, nothing for speed | the minimum for exceptions if nothing else is built |
| Today's stack evaluator with state in locals and inlined callbacks | about 2.2 times on map (estimated) | half the gain, without try and async getting an IR |
| **This: a tree per function, ANF with join points and structured loops, register bytecode** | about 4.3 times on map, about 2 on guards (prototypes), and one place for moves, ownership, `try` and `await` | recommended |
| Full SSA on a general CFG | the same as this; structured loops suffice | no gain over this |
| CPS with heap continuations | suspension anywhere, which the evaluator already has | slower sequentially (Farvardin and Reppy, PLDI 2020) |
| Tree interpreter (CEK) | simplicity | more dispatch per node; the opposite of what the measurements ask for |
| B: buffered events, today's emitter | | refuted |

## Order of work

1. **Cheap fixes, now, on today's code**, each with `bench-compare
   --quick`:
   - hot helpers inline;
   - string equality a word at a time;
   - a fast start for an idle evaluator;
   - shapes' C paths stepping several elements per turn of the run loop,
     fuel checked per element.
2. **Spike: the front end.** Trees from the events for the spec corpus and
   the bench modules: memory, time, and today's error messages and order.
3. **The new compiler and evaluator, behind `sjs_compiler_config` and
   `sjs_vm_config` switches.**
   - Run the spec both ways, with `diff-node`.
   - They may differ in fuel and in which error wins where a later syntax
     error exists.
   - The inspection tests stay as they are.
4. **Switch the default**, delete the old compiler and evaluator, then
   `try`, `throw` and `async` on the new one.

## Open questions

- Whether fused instructions should keep the old fuel units, so that the
  guide's costs stay.
- NaN-boxed 8-byte values: they halve memory traffic, but change the
  public `sjs_value` and every binding.
- Inlining named callbacks (`const f = x => ...; xs.map(f)`), and calls of
  small functions generally.
- Interning strings that are compared often (event names), so `===` is a
  pointer comparison.
