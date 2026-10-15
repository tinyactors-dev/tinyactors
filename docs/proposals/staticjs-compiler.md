# staticjs: the compiler and evaluator, from scratch

**Status:** Sketch (2026-10-15). A design for discussion; nothing here is
decided or scheduled.
**Touches:** everything between the lexer and the values: `parser.c`'s
output, `compiler.c`, `vm.c`, `matcher.c` and the shapes, the looping
built-ins in `builtins.c`, the value representation in `value.c`, and the
public API around them (the event sink, `sjs_value`). [Exceptions and
async](staticjs-exceptions-and-async.md) is designed on top of this.
**Not a plan:** ADRs and roadmaps win.

## Whom it serves, who pays, what is out

- **First, the application developer and the operator:** guards and
  callbacks that cost a fraction of what they do now (measured below).
- **Then the maintainers:** fewer mechanisms. Today the pipeline is a
  streaming compiler that patches its output, a matcher that rebuilds
  trees from events, hand-written resumable state machines for every
  built-in that calls back, and a stack evaluator. From scratch, it is a
  parser, a resolver, a code generator, one walk for moves, and a
  register evaluator.
- **Who pays:**
  - a rewrite;
  - every documented fuel cost changes once;
  - the public event sink and `sjs_value` change, and with them
    `staticjs-js`.
- **Out of scope:**
  - the language: what staticjs accepts, refuses and computes;
  - the built-ins that do not call back: strings, numbers, JSON,
    regular expressions, Temporal, the writer;
  - a JIT.

## What the design must give

1. **The rule:** the same result as JavaScript, or a fault.
2. **Pause anywhere on fuel**, deterministically: the same fuel on every
   machine, and no C recursion.
3. **Freestanding**: no C library, the embedder's allocator, wasm32 too.
4. **Bytes are pushed**, in chunks of any size.
5. **Owned mutation and moves:** a value changes only through a variable
   that holds it alone, and a last read moves.
6. **Every evaluation ends.**
7. **Inspection:** frames name a function or a built-in and a position,
   and an evaluation steps one unit at a time.
8. **Coming:** `try`/`throw`, `async`/`await` with suspension to the
   host.

## Where the time goes today

Measured with callgrind, as instructions per evaluation. Start-up is
excluded, and a review reproduced the figures:

- `_event.name === "notify.ack"`: 802, for 5 units of fuel;
- the bench's guard: 1,703, for 13;
- `xs.map(x => x * 2 + 1)`: 663 per element.

Where it goes:

- **The callback protocol** of a looping built-in: about 200 per element.
- **Dispatch, with its state in memory:** 19% to 27% of the total.
- **Starting an evaluation:** about 28% of the small guard.
- **A built-in method call:** about 150 instructions for `.has()`.
- **Strings compared a byte at a time**, and property reads that scan
  keys.

A prototype register loop ran the map at 153 instructions per element.

## The design

### 1. Parse to a tree

- The lexer is pushed bytes, as now.
- The parser builds the program's syntax tree in an arena:
  - nodes of 16 bytes, children by index;
  - built with an explicit stack;
  - complete when the last byte is pushed.
- Sources are small (`max_source_bytes`, 1 MiB), so the tree is small too:
  about 0.3 nodes per byte.
- Tools that took the event stream walk the tree instead. The event sink
  goes.

### 2. Resolve names on the tree

One walk with an explicit stack:

- scopes and slots;
- captures;
- use before declaration;
- the ownership rules of `docs/internals/mutation.md`, which on a tree
  are plain questions:
  - is this variable captured anywhere in its function?
  - was it copied, passed or stored before this mutation?

Errors come in source order, and every syntax error comes before any
refusal, as JavaScript reports them.

### 3. Generate register code

One walk over each function's tree.

- **The register frame.** Each function has a frame of registers:
  parameters, then locals, then temporaries. An expression is compiled
  into a target register, and temporaries are taken and released as a
  stack, so their number is the expression's depth.
- **Instructions:**
  - fixed-width 32-bit words, as Lua's: an 8-bit opcode and operands
    that name registers or constants;
  - fixed width makes decoding free, and walking the code backwards
    trivial;
  - `x * 2 + 1` is `MUL t, x, K2` then `ADD t, t, K1`.
- **Jumps go forward only in user code**, so every evaluation ends.
- **Each function gets its own code array** and its own table of
  positions.

### 4. Moves: one backward walk per function

Walk the function's code from its last instruction to its first, with
the set of registers still to be read:

- A read of a register not read again sets that operand's **move bit**.
  The instruction then takes the value's reference instead of adding one.
- Jumps go forward, so the walk has already seen every target. A jump's
  set is its target's set together with the fall-through's.
- A write removes the register from the set, and `RETURN` clears it.
- For `try`, the handler is reached from every instruction that can throw
  inside it.

About a page of code, because instructions are fixed-width.

### 5. Values: 8 bytes, objects with shared layouts

- **A value is 8 bytes, NaN-boxed.**
  - Numbers are themselves.
  - Everything else is a tagged pointer in the NaN space: 48 bits on 64-bit
    targets, 32 on wasm32.
  - It halves the traffic of every register, element and argument.
  - This replaces the public 16-byte `sjs_value`.
- **Objects are a layout and values.**
  - A layout is an immutable, shared list of keys.
  - Objects made by the same literal, or read from JSON with the same
    keys, share one layout (interned per context).
  - `GET_FIELD r, obj, K"name"` caches the last layout it saw and the
    index it found, so a guard's property read is a comparison and a
    load.
  - Today, staticjs scans keys, and remembers a position per instruction.
- **Strings used as keys and literals are interned**, so `===` on two
  interned strings is a pointer comparison. A string read from JSON
  compares its hash and length first.

### 6. A register evaluator with cheap calls

- **The run loop keeps its state in C locals:** `pc`, the frame's base,
  the code pointer, the fuel counter. It counts one unit of fuel per
  instruction, and writes them back only when it leaves.
- **A call, as in Lua.** The caller puts the callee and its arguments in
  consecutive registers. The callee's frame starts there, so nothing is
  copied, and the result is written into the caller's register. A frame
  record holds the return `pc`, the base and the function: no more.
- **Re-entry** ("a function cannot call itself") is a counter per
  function in the evaluation, not a scan of the frames.
- **Starting an evaluation** points the first frame at the globals, with
  nothing to reset.

### 7. Built-ins that call back are written in bytecode

`map`, `filter`, `reduce`, `toSorted`, `some`, `find`, `flatMap`,
`Array.from` with a function, `replace` with a function, the iterator
helpers, `forEach` on maps and sets, and `then`.

- **They are written once, in the same register bytecode,** using a few
  instructions that only built-in code may use:
  - read the length;
  - read an element;
  - append to an array the frame owns;
  - a backward jump bounded by a length.
- **A callback is an ordinary call** from that code, so:
  - pausing inside a callback needs nothing of its own;
  - a `throw` unwinds through it like any frame;
  - `await` in a callback is an ordinary suspension;
  - inspection sees a frame named `map`, as now.
- **This removes the hand-written resumable state machines**
  (`sjs_loop_step`, `sort_step` and their phases) and the callback
  protocol, which is today's largest cost per element.
- **What stays in C:** built-ins that do not call back. Long-running ones,
  a regular expression's match, stay resumable C loops, as they are
  today.
- **The shapes matcher is not needed to start with:** its purpose was a C
  path around the callback protocol. Whether a C comparator for `toSorted`
  by a key still pays is measured after.

### 8. Exceptions and async, on this

- **`throw`** unwinds frames using a table per function: a `pc` range, a
  handler, and the register to receive the value. Built-in frames are
  bytecode frames, so nothing special unwinds.
- **`finally`** is code that receives how the block ended (normal, return,
  throw) in two registers.
- **`await`** moves the frame's registers to the heap, and a job moves
  them back.
- The termination, promise and host rules are those of the exceptions
  sketch.

## What it replaces

| Today | From scratch |
|---|---|
| events in evaluation order, a streaming compiler that patches what it emitted | a tree, a resolver, a code generator |
| last-read machinery (parked reads, fences, cancels, special cases) | one backward walk, move bits on operands |
| a matcher rebuilding trees from events, the shapes' C paths | none to start with |
| loop frames: a resumable C state machine per built-in that calls back | those built-ins in bytecode; a callback is a call |
| stack bytecode of variable width; state in memory per instruction | fixed-width register code; state in locals |
| 16-byte values; objects searched by key | 8-byte values; layouts with a cache per instruction |
| re-entry by scanning frames | a counter per function |

## Costs and risks

- **Compile:** a tree held for the whole program, at about 16 bytes per
  node and 0.3 nodes per byte: about 5 bytes per source byte, against a
  peak of 23 to 50 today. Then three walks over it.
- **Built-ins in bytecode** run their loop as instructions where C ran it.
  Per element that is a few instructions plus a call, against about 200
  for today's protocol. A built-in with no callback stays in C.
  **Unverified:** sorting, whose comparator calls dominate, against
  today's merge sort in C.
- **8-byte values** change the public API and every binding. Pointers past
  48 bits (some 64-bit systems with tagged pointers) would need a
  different boxing. **Unverified** on the targets staticjs ships to.
- **The guide's fuel costs** all change once, as instructions do more.
- **Unverified overall:** the factor.

  | Program | Today | Expected | Basis |
  |---|---|---|---|
  | `map`, per element | 663 | about 100 to 150 | the 153 measured inlined, against a cheaper call and loop |
  | small guard | 802 | about 100 | a cached field read, an interned comparison, a start with nothing to reset |

## Order of work

1. **Spike the evaluator.** Hand-write the register code for the small
   guard and for `map` (its bytecode loop and the callback), with 8-byte
   values, a layout cache and the cheap call. Count with callgrind
   against 802 and 663. This settles whether everything after it pays,
   and which parts pay most.
2. **The tree and the resolver**, checked against today's errors on the
   spec corpus: same messages, and syntax before refusals.
3. **The code generator and the backward walk.**
4. **The evaluator, the values and the bytecode built-ins.** The spec
   runs with `diff-node`; fuel is the only difference allowed.
5. **The public API:** `sjs_value`, the tree for tools, `staticjs-js`.
6. **`try`, `throw`, `async`.**

## Open questions

- Whether tools need the event stream, or are served by walking the tree.
- Whether `toSorted` by a key wants a C comparator after measurement.
- The boxing on targets with pointers wider than 48 bits.
