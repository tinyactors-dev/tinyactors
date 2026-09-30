# tinyjs, the small way: a CEK machine over the syntax tree

**Date:** 2026-09-30
**Used by:** nothing yet. This is a competing design to
[tinyjs](tinyjs.md), written to be compared with it. Neither is a decision.
An ADR, or a proposal in `docs/proposals/`, would choose between them.

## Question

[tinyjs](tinyjs.md) meets its goals with a resumable IR, a build-time
compiler written in Racket, two back ends that share a generated ABI, three
memory regions with a write barrier, and a verifier for bytecode. Can the
same goals be met, under the same constraints, with far fewer parts?

The approach is PicoLisp's: pick the simplest algorithm that is known to
work, use one mechanism for each job, and put the effort into the
implementation of that one mechanism instead of into more mechanisms [1].

## Summary

- **The goals, the constraints and the standards scope are unchanged.**
  This note takes the [design goals](tinyjs.md#design-goals),
  [implementation constraints](tinyjs.md#implementation-constraints) and
  [standards scope](tinyjs.md#standards-scope) of tinyjs as they are. So
  are the public shape (`tjs_run` with fuel, `tjs_settle`, `tjs_abort`), the
  split into program and state, and the way tinyactors uses the engine.
  Only the inside is different.
- **The machine is a CEK machine** [2]: the node to evaluate (C), the
  environment (E), the continuation (K), and a value register. Continuations
  and environments are ordinary heap objects. The whole state of a running
  computation is those registers and the heap.
- **Every suspension is "store K and return".** Fuel running out,
  `yield`, `await`, a pending host token and a paused RegExp match are one
  case, not five. There is nothing to detach, reattach, copy or relocate.
- **The code is the syntax tree.** The parser's packed, pre-order CST, with
  a side column of resolved variable slots, is what the machine runs. There
  is no bytecode, no bytecode compiler, and no bytecode verifier.
- **Builtins are strict JavaScript.** Every builtin that can call user code
  is written in a prelude, in JavaScript extended with `%`-intrinsics that
  only the prelude can name. The prelude runs on the same machine, so it
  suspends everywhere user code does. The generator language is tinyjs
  itself: no Racket, no second compiler, no ABI to keep in step.
- **One heap per VM and a Cheney collector** [3]. The collector copies what
  is reachable from the registers. It needs no stack and no write barrier,
  and it can run between any two steps of the machine. The same traversal
  writes a state to bytes.
- **Built-in objects live in ROM**, shared by every VM, and a VM copies one
  into its heap the first time it writes to it, through a table of aliases.
- **What it costs**: loops run slower than on a register bytecode VM, and
  every collection is a full one. The target workload is many short
  evaluations, where entering an evaluation matters more than loop speed.
  Each cost has a known remedy that stays inside one module, to be applied
  only when a measurement asks for it (see Risks).
- **A one-week experiment in C** (see the end) tests the same end-to-end
  trace as the Racket experiment, so the two designs can be compared on
  size, effort and speed.

## What "the PicoLisp approach" means here

PicoLisp is an interpreter only. It has one data type underneath
everything, the cell; code is data and is evaluated directly; its collector
is a plain mark and sweep. Its author argues that a small number of
uniform mechanisms, implemented carefully, beats a large number of
specialised ones for the programs it targets [1]. We do not copy PicoLisp's
data structures, which are designed for Lisp, not for ECMAScript. We copy
its rules:

1. **One way to do each thing.** One kind of frame, one form of code, one
   heap, one traversal, one language for the builtins.
2. **Code is data.** The program that runs is the tree the parser built.
3. **The state is a few registers and a heap.** Suspension, collection and
   writing a state follow from that without further design.
4. **Textbook algorithms with known bounds.** CEK machine, Cheney copying,
   property lists with a hash index for large objects, a backtracking
   RegExp matcher, merge sort. The effort goes into the representation and
   the inner loops.
5. **A fast path needs a measurement and stays local.** A faster
   representation is added inside one module when a benchmark asks for it,
   never as a second mechanism beside the first.

## The parts

| Part | What it is | Replaces, in tinyjs |
|---|---|---|
| `tjs_parse` | source to packed CST, iterative, as in tinyxml | same |
| `tjs_resolve` | early errors, scope analysis, a slot column on the CST | the run-time compiler and its bytecode |
| the machine | one loop, two `switch` statements | the trampoline, the dispatch loop, the frame ABI, detach and reattach |
| the prelude | builtins in strict JS with `%`-intrinsics | the IR `#lang`, its nanopass passes, both back ends, `tjs_abi.h` |
| leaf C | bounded work: numbers, math, BigInt, Unicode, string bytes, one RegExp step | same, minus the RegExp frame kind |
| the heap | bump allocation, Cheney collection, an alias table for ROM | three regions, barrier, remembered set, promotion, ROM copy-on-write |
| the codec | the Cheney traversal, writing to bytes | same idea, one destination fewer |

Everything that remains is C in the house style, except the prelude, which
is JavaScript and is checked by the engine that runs it.

## The machine

### Registers

```c
typedef struct {
    tjs_ref code;      /* program and node: what to evaluate next */
    tjs_ref env;       /* the environment it is evaluated in */
    tjs_ref kont;      /* the continuation: a heap object, or the halt frame */
    tjs_value value;   /* the value being returned to kont */
    uint8_t mode;      /* EVAL, or RETURN, THROW, BREAK, CONTINUE with value */
} tjs_regs;
```

The loop has two halves. In `EVAL` mode it switches on the kind of the
node in `code`. In the other modes it switches on the kind of the frame in
`kont` and hands it `value`. That is the whole interpreter:

```c
while (vm->fuel > 0) {
    if (r->mode == TJS_EVAL) tjs_eval_node(vm, r);   /* one switch on node kind */
    else                     tjs_apply_kont(vm, r);  /* one switch on frame kind */
}
```

Neither function calls the other, or itself. An evaluation that needs a
sub-result pushes a frame (allocates it, points it at the old `kont`, makes
it the new `kont`) and sets `code` to the child. A result pops the frame.
This is the defunctionalized evaluator of Danvy's derivation [4], written by
hand once for the grammar instead of derived for every builtin.

### Continuation frames are heap objects

A frame is a heap object: its kind, the node it resumes at, the
environment, the parent frame, and the few values it holds (the left
operand of `+`, the callee of a call being evaluated, the loop's label).
Farvardin and Reppy call this the simplest strategy to implement, well
suited to concurrency, and slower than a contiguous stack for sequential
code [5]. Two things narrow the gap:

- **Allocation is a pointer bump**, and a copying collector spends nothing
  on dead frames. Appel argued that under these conditions heap frames can
  cost no more than stack frames [6].
- **Popping the newest object returns its memory.** When the frame being
  popped is the last object allocated and has never been captured, the
  bump pointer moves back over it. For expressions that nest without
  allocating anything else, the heap then behaves as a stack and stays in
  the cache. A frame is captured when a generator, an async function or a
  host token stores a reference to it; capturing sets a bit in its header,
  and a captured frame is never reclaimed this way.

### Completions travel up the continuation

`throw`, `return`, `break` and `continue` are one mechanism. The machine
sets `mode` and `value` and pops frames until one accepts the completion:

- a `try` frame accepts `THROW` and evaluates its `catch` block;
- a function's call frame accepts `RETURN`;
- a loop or labelled statement frame accepts its own `BREAK` and
  `CONTINUE`;
- a `finally` frame accepts every mode. It saves the completion (its mode
  and value) in two of its slots, runs the block, and then resumes the
  saved completion, unless the block completed abruptly and replaced it.
  This is the spec's completion record, as in tinyjs, with nothing added.

`tjs_abort` sets `kont` to the halt frame. No `catch` or `finally` runs.

### Fuel and suspension

Fuel is charged per step, with an extra charge for leaf work proportional
to its size. When it runs out, `tjs_run` returns `TJS_RUN_OUT_OF_FUEL`. No
C frame holds anything: the registers and the heap are the computation.
The next `tjs_run` continues the loop.

- **`yield`** stores `kont` (up to the generator's call frame) in the
  generator object and returns to the frame that resumed the generator.
  Resuming stores that frame and loads the saved one. Generators are
  shallow, so the saved chain is the generator body's own frames.
- **`await`** stores `kont` in a promise reaction. Settling the promise
  queues a job that loads it back.
- **A host token** is a promise that only `tjs_settle` can settle.
- **A RegExp match** is a loop in the prelude around a leaf step (below),
  so it pauses at that loop's back edge like any other loop.

### Common operations never push a frame

Goal 3 of tinyjs requires that a data property read, or adding two
numbers, never pushes a frame. The machine meets it in two ways:

- **Leaf operands are evaluated in place.** A literal, a variable, `this`,
  and a read of a data property of an ordinary object need no frame. `a +
  b`, `x.count > 3` and `f(x, 1)` evaluate their operands without one.
- **Pure subtrees are tried in one go.** The resolver marks a subtree as
  *simple* when it has a bounded depth and contains no call, assignment or
  `new`. The machine evaluates a simple subtree in a C loop, with a small
  fixed array as its operand stack. If that loop meets anything that could
  run user code or allocate (a getter, a Proxy, an object operand of `+`,
  a string concatenation), it drops what it computed and evaluates the
  subtree step by step from its root. Everything it did before stopping was
  a read of data or arithmetic on primitives, which cannot be observed, so
  doing it twice is safe. A guard like `x.count > 3 && y.state === "on"`
  therefore runs without allocating.

The second is an optimization for after the machine works. The first is
part of the machine from the start.

## Code is the syntax tree

The CST is tinyxml's layout: nodes in pre-order in an array, each with its
kind, its source span, and the index just past its subtree. `tjs_resolve`
walks it once, with an explicit stack of scopes, and adds columns:

- for each identifier reference, its slot: how many environments out, and
  which slot in that environment, or "global";
- for each function and block, the size of its environment, and whether
  anything captures it (a closure, a direct `eval`, a generator's body);
- for each expression, whether it is simple (above);
- early errors, as document errors with positions.

Nothing is lowered. The children of a node are found from the node and its
subtree end, so the machine moves through the tree with integer arithmetic.
Variable names do not exist at run time, except in functions that contain
a direct `eval`, which keep a name column for their scopes.

**Why no bytecode.** A register bytecode VM runs loops faster. For the
target workload (a guard, an `<assign>`, a short `<script>`) the work is
dominated by entering the evaluation, and a tree the parser already built
is entered with no compile step. The tree is also what tools read: the
cursor, the source spans and the running code are one structure, so a
debugger's "current position" is a CST node with nothing to map back.
If loops turn out to matter, the remedy is to lay expression subtrees out
in post-order in the resolver's output, which makes them read like stack
code, and keep the machine; see Risks.

### Environments

A function call allocates one environment object for its parameters and
locals, sized by the resolver. A block gets an environment of its own only
when something captures its `let` or `const` bindings, including the
per-iteration bindings of a `for (let ...)` loop. An environment that
nothing can capture is released at return by the same "newest object"
rule as frames.

## Builtins are a prelude in strict JavaScript

This is the design of SpiderMonkey's and V8's self-hosted libraries, and
tinyjs's architecture B [7]. It was set aside there because self-hosted
code must not be observable to user code. Here that rule is enforced
mechanically.

### The prelude dialect

The prelude is strict JavaScript parsed in a *prelude mode*:

- **Identifiers starting with `%` name intrinsics**: `%Call`, `%IsObject`,
  `%ArrayPrototype`, `%TypeError`, `%StringIndexOf`. User code cannot write
  such a name; the parser rejects it outside prelude mode.
- **A free identifier must be an intrinsic.** The resolver refuses a
  prelude that names a global, so the prelude never reads a mutable
  `globalThis.Array` or `Math`.
- **A small set of syntax is refused** because it goes through protocols
  that user code can change: method calls (`a.push(x)`: write
  `%Call(%ArrayPush, a, x)` or a leaf), `for-of`, spread, destructuring,
  template literals, and `instanceof`. Property reads and writes on user
  objects remain allowed, because the spec's algorithms perform exactly
  those `Get` and `Set` operations.
- **Prelude functions are native to user code**: their `name` and `length`
  are set as the spec lists them, and `Function.prototype.toString` prints
  `[native code]`.

The refusals are a table of CST node kinds that the resolver checks in
prelude mode. There is no separate lint tool.

### What it reads like

```js
// 13.15.3 ApplyStringOrNumericBinaryOperator, opText = +
function %Add(lval, rval) {
  const lprim = %ToPrimitive(lval, "default");
  const rprim = %ToPrimitive(rval, "default");
  if (%IsString(lprim) || %IsString(rprim))
    return %Concat(%ToString(lprim), %ToString(rprim));
  return %NumericAdd(%ToNumeric(lprim), %ToNumeric(rprim));
}

// 7.1.1.1 OrdinaryToPrimitive
function %OrdinaryToPrimitive(o, hint) {
  const first  = hint === "string" ? "toString" : "valueOf";
  const second = hint === "string" ? "valueOf"  : "toString";
  for (let i = 0; i < 2; i++) {
    const method = o[i === 0 ? first : second];    // Get(O, name)
    if (%IsCallable(method)) {
      const result = %Call(method, o);
      if (!%IsObject(result)) return result;
    }
  }
  throw %TypeError("cannot convert object to primitive value");
}
```

`%ToString` and `%ToNumeric` here receive primitives, so they are leaf C;
they can still throw (a Symbol), which a leaf reports as a completion.

### The split between prelude and C

**The prelude handles protocol, C handles bytes.** A builtin's prelude
function performs every step that can call user code (coercions, `Get`,
iterator steps, callbacks) and then calls a leaf for the bounded work:
`String.prototype.indexOf` coerces in JS and searches in C.
`Array.prototype.sort` is a merge sort in the prelude, because every
comparison can call user code.

The rules of tinyjs's layer 0 apply unchanged to leaf C: bounded work, or
work charged fuel in proportion to its size within `tjs_limits`.

### Operators call the prelude only when they must

The machine handles the common cases of every operator in C: `+` on two
numbers or two strings, `===`, `<` on numbers, a data property read or
write on an ordinary object. For anything else it calls the prelude
function for the spec operation (`%Add`, `%ToPrimitive`, `%OrdinaryGet`),
as tinyjs's dispatch loop pushes a builtin frame. Exotic objects (Proxy,
typed arrays, module namespaces, an array's `length`) have their internal
methods in the prelude, with C fast paths only for the ordinary cases.

### RegExp without a frame kind

The matcher is a backtracking engine whose whole state (pattern position,
input position, backtrack stack) is a heap object. `%RegExpStep(state)`
runs at most a fixed number of steps and reports "matched", "failed" or
"not yet". The prelude's `RegExpBuiltinExec` calls it in a loop:

```js
let r;
while ((r = %RegExpStep(state)) === %NotYet) {}
```

Fuel is charged at the loop's back edge and inside the step, so a long
match pauses like any loop. The backtrack stack is bounded by
`tjs_limits`. The matcher is the one leaf that can be called many times
for one piece of work, and that is its only special property.

### Recursion in the prelude is allowed

The rule "no recursion anywhere" is about the C stack. Recursion in the
prelude runs on heap frames, bounded by the frame depth limit, which
throws a `RangeError`. So the spec's recursive algorithms, such as
`InnerModuleLinking`, `InnerModuleEvaluation`, and `IsLooselyEqual`,
which calls itself after each conversion, are transcribed as the spec
writes them. tinyjs has to rewrite them
with explicit stacks in its IR.

### How the prelude is built

The prelude is parsed and resolved by tinyjs's own parser, at build time,
by a host program compiled from the same sources. It writes two C arrays
that are checked in, as `txml.c` is:

- the prelude's CST and columns, which are ROM code;
- the **realm image**: the built-in objects after the prelude's
  initialisation code has run, written by the state codec.

`mise run generated` fails when either is stale. There is one compiler in
the repo, and the builtins are its first user.

## A worked trace: suspending inside `valueOf`

The trace of tinyjs, on this machine:

```js
const unit = { valueOf() { while (slow()) {} return "kg"; } };
total + unit        // total = 3, result "3kg"
```

The `+` node sees a number and an object, so it calls `%Add`. `%Add`
converts `3` (it returns at once), then calls `%ToPrimitive(unit)`, which
calls `%OrdinaryToPrimitive`, which calls `valueOf`. Fuel runs out at the
back edge of the `while` loop:

```
code  → the `while` node in valueOf           env → valueOf's environment
kont  → loop       valueOf                    test pending
        call       %OrdinaryToPrimitive       at %Call(method, o); env: o = unit, i = 0
        call       %ToPrimitive               at %OrdinaryToPrimitive(...)
        call       %Add                       binding rprim; env: lval = 3, lprim = 3
        call       the guard                  at the `+` node
        halt
```

On resume, `valueOf` returns `"kg"`, each frame is popped in turn, `%Add`
concatenates `"3"` and `"kg"`, and the guard's `+` receives `"3kg"`.

Writing the state is the Cheney traversal from the registers, the handle
table, the job queue and the alias table into bytes. Reading it back and
calling `tjs_run` gives the same result. There is no second picture for
"a builtin that was moved to C", because there is no such thing.

## Values, objects and strings

Simple representations, each chosen to make one inner loop short.

- **Values are 64 bits, NaN-boxed.** Doubles are stored as themselves.
  Every other value is a quiet NaN with a tag and a 32-bit payload:
  `undefined`, `null`, booleans, a heap reference, a ROM reference, a
  Symbol, a string. NaNs are canonical, so no double collides with a tag.
  There is no separate integer type; array indices are doubles checked
  with one conversion.
- **Heap references are 32-bit offsets** in units of 8 bytes, with one bit
  that says heap or ROM. No C pointer into the heap is kept across a
  reservation (below).
- **Objects are a prototype, flags, and a property list**: an array of
  entries (key, value, attributes) in insertion order, which gives the
  spec's enumeration order for free. Up to a threshold (about eight entries,
  to be measured) lookup is a linear scan. Above it, the object gets a hash
  index over the same entries. Keys compare by cached hash and length
  first, then by content, so there is no intern table to keep per VM.
  Symbols compare by identity. Deleted entries leave a tombstone, and the
  collector drops tombstones when it copies the object.
- **Arrays** have a dense element part beside the property list, as Lua
  tables have an array part and a hash part [8]. A write that would make
  the array sparse, or an element with an accessor or non-default
  attributes, moves the elements into the property list.
- **There are no shapes and no inline caches.** They make property access
  fast in long-running code and cost a transition tree per VM. Short
  evaluations over small objects do not repay them.
- **Strings are flat**, UTF-16 code units, with a cached hash. A
  concatenation copies, charged fuel per unit and bounded by the string
  length limit. Ropes are left out until a benchmark shows a need; a
  one-byte representation for Latin-1 text is the first thing to try if
  memory matters.
- **The hash seed** comes from the entropy dependency when the VM is
  created and is part of the state, so hashing is deterministic across a
  replay and not predictable to an attacker.

## Memory: one heap and a Cheney collector

### Reserve, then allocate

The rule that replaces tinyjs's "no C pointer survives a call that might
move objects":

- A machine step that allocates first calls `tjs_reserve(vm, bytes)` for
  everything it will allocate, before it takes any C pointer into the
  heap. A step's allocation is either a constant or computed up front (a
  concatenation knows the sum of its lengths).
- `tjs_reserve` is the only function that collects. After it returns, the
  step's allocations cannot fail and nothing moves until the step ends.
- Between steps, every live value is in a register, in the heap, or in a
  root table. So a collection at a reservation sees every root.

A debug build has a stress mode that collects at every reservation and
fills the old space with a poison pattern. Running test262 in that mode
finds any C pointer kept past a reservation.

### The collector

- **Cheney's algorithm** [3]: copy the roots into a fresh space, then scan
  that space from start to end, copying what each object refers to. The
  scan pointer and the allocation pointer are the queue. It needs no stack
  and no recursion, and it compacts.
- **Roots**: the registers, the handle table, the job queue, the table of
  programs in use, and the alias table. Nothing else.
- **When it runs**: when a reservation would pass the collection threshold,
  which is the live size after the last collection times a factor, with a
  floor. The threshold depends only on byte counts, so collection points
  are deterministic, and the work per allocated byte is bounded by a
  constant (amortized).
- **Memory**: the new space is taken from the allocator for the duration
  of a collection and the old one is returned after it. An idle VM holds
  only its live data. The permanent heap limit of `tjs_limits` caps the
  live size and throws a catchable error when it is reached.
- **Weak collections**: WeakMap entries are ephemerons, handled by
  rescanning the pending entries until nothing more is copied. WeakRef and
  FinalizationRegistry follow tinyjs: targets may be kept, and cleanup
  runs only at points the host chooses.

What goes away compared with tinyjs: the arena, the permanent heap as a
separate region, the notion of a quiet VM, the write barrier, the
remembered set and promotion. What is lost: tinyjs's collection at quiet
points copies only what survived the evaluation, while here every
collection copies everything live. For a VM with a large live heap and a
steady stream of garbage, each collection costs more. The threshold keeps
the cost per allocated byte constant, and a nursery can be added later
inside the heap module without changing the machine, because the roots
stay the same.

### ROM and aliases

The prelude's code and the realm image are read-only and shared by every
VM. A ROM object is named by a ROM reference, which is its index. A VM
has an **alias table**, one heap offset per ROM object, allocated in its
heap on first use:

- reading a ROM object checks its alias entry: empty means read ROM,
  otherwise read the heap copy;
- the first write to a ROM object copies it into the heap and fills its
  alias entry;
- references stay ROM references, so identity (`===`) is unchanged, and
  nothing that pointed at the ROM object needs updating.

Only ROM references pay the extra check. A new VM starts with an empty
heap and no alias table; the global object itself is a ROM object until a
script defines its first global. Moddable's XS engine keeps built-in and
preloaded objects in ROM and gives each machine aliases for the ones it
changes [9]; we have not read its source in detail.

## Program and state

The table of tinyjs applies, with one change: a program is the CST and its
columns, not bytecode. Its identity is the hash of its source and the
engine version.

**Programs cross a trust boundary as source.** A host that stores or sends
a program sends its source and parses it again on the other side. The
parser is deterministic, so node indices, and so every `code` reference in
a state, mean the same thing. There is no program format to verify.

A runtime built without the parser (below) can still take a program as a
CST from bytes. Verifying it is structural: each node's subtree ends
inside its parent's, each node kind has the children the grammar allows
(one table, the one the parser is written from), and every column entry is
in range. No `%`-intrinsic reference may appear outside ROM code.

**Reading a state verifies:**

- every heap reference is in range and points at the start of an object of
  a kind allowed there;
- every `code` reference names a program in the state's program table and
  a node of a kind the frame kind can resume at;
- every program named by hash is available;
- module records are consistent with their programs.

Reading a state produces a compacted heap, as in tinyjs.

## Dynamic code

The semantics of [dynamic code in tinyjs](tinyjs.md#dynamic-code-eval-and-new-function)
hold unchanged: dynamic code is strict, only functions that contain a call
to `eval(...)` keep names at run time, and without a compiler
`HostEnsureCanCompileStrings` throws an `EvalError`.

What changes is the size of the optional part. In tinyjs the optional part
is a parser and a bytecode compiler. Here it is the parser and the
resolver, which every host that loads source links anyway. The split is
still two single-file builds, `tjs.c` (the machine, the heap, the leaves,
the prelude) and `tjs_parse.c` (the parser and the resolver), so a static
build that runs only programs compiled into the binary leaves the parser
out.

## Modules

Unchanged from [tinyjs's modules](tinyjs.md#modules), with one
simplification: the spec's linking and evaluation algorithms, which are
recursive, are written in the prelude as the spec writes them, because the
prelude's recursion runs on heap frames. A module instance is heap data,
its program is a CST, and top-level `await` is ordinary `await`.

## Using it from tinyactors

Unchanged from [Using tinyjs from tinyactors](tinyjs.md#using-tinyjs-from-tinyactors):
a synchronous data-model vtable, fuel running out as a fault, `tjs_abort`,
and long or asynchronous scripts in an invoked child. The adapter cannot
tell which design it is using.

## The two designs side by side

| | tinyjs (resumable IR) | this design (CEK over CST) |
|---|---|---|
| Languages in the build | C, Ruby, Racket | C, Ruby, and JS for the prelude |
| Compilers | two (Racket IR to bytecode and C; JS to bytecode in C), sharing a generated ABI | none: a resolver adds columns to the CST |
| Frame kinds | bytecode, generated C, RegExp | one |
| Suspension | return with frames in place; detach and reattach for generators | store `kont` |
| Builtins that call user code | IR, derived into state machines | strict JS with intrinsics |
| Memory | ROM, permanent heap, arena; barrier, remembered set, promotion at quiet points, compaction by codec | ROM with aliases, one heap, Cheney |
| Verification on read | bytecode, heap, module map | heap, CST references, module map |
| Loops | fast: register bytecode | slower: tree walking, heap frames |
| Short evaluations | fast paths in the dispatch loop | leaf operands and pure subtrees in C; no compile step |
| Spec algorithms that recurse | rewritten with explicit stacks | transcribed |
| Hot builtins | can move to generated C | stay in the prelude; their byte work is leaf C |
| Main risk | the two compilers and the ABI disagree | loop speed and allocation rate |

## Risks, and where each remedy lives

| Risk | Sign | Remedy, confined to |
|---|---|---|
| Loops too slow | a benchmark of a loop-heavy script against QuickJS or napi's current data model | post-order layout of expressions in the resolver's output; then threaded dispatch. The machine |
| Heap frames dominate | allocation rate and collections per evaluation in profiles | a contiguous frame stack, with the generator's frames copied out at `yield` (a few frames, since generators are shallow). The machine |
| Full collections too costly | collection time on VMs with a large live heap | a nursery with a barrier. The heap module |
| Prelude builtins too slow | `map`, `sort`, promises in profiles | more leaf C for the byte work; a C fast path for the ordinary case, beside the prelude function it shortcuts, tested against it. One builtin at a time |
| Quadratic concatenation | `s += x` loops in real charts | ropes. The string module |
| The prelude leaks | a test262 failure that observes a prelude step | a new refusal in the prelude-mode table |

Each remedy is a change to one module, made one at a time, with a
benchmark reported as `AGENTS.md` requires: throughput and memory
together, the machine named, builds compared side by side.

## Outline of an experiment

**Time box:** one week, in C, with no new language in the build. It
reproduces the end-to-end trace of the [Racket experiment](tinyjs.md#outline-of-the-racket-experiment),
so the two can be compared.

**Scope:**

1. A parser and resolver for a subset: function declarations and
   expressions, `let`/`const`, `if`, `while`, `for`, `return`,
   `try`/`catch`/`finally`, object literals, property reads, calls, `+`,
   comparisons, string and number literals, and prelude mode with its
   refusal table.
2. The machine: registers, the two switches, heap frames with "newest
   object" reclaim, completions, fuel, leaf operands.
3. The heap: reservation, Cheney collection, the stress mode, and writing
   and reading a state.
4. The prelude: `%Add`, `%ToPrimitive`, `%OrdinaryToPrimitive` and
   `Array.prototype.map`, built into checked-in C arrays by the same
   parser.

**Success criteria:**

1. The C meets the house rules: freestanding, no undefined symbols at -O0
   and -O2, no cycle in the call graph, at most three parameters, and it
   compiles with the pinned clang and `zig cc`.
2. The trace works end to end: fuel runs out inside `valueOf`, the state is
   written to bytes and read back, and the run resumes to `"3kg"`. It also
   works in stress mode, collecting at every reservation.
3. The prelude reads like the spec: a reviewer checks
   `%OrdinaryToPrimitive` against section 7.1.1.1 step by step.
4. The guard `x.count > 3 && y.state === "on"` allocates nothing after its
   first evaluation. Its time per evaluation is reported beside napi's
   current JavaScript data model, measured on the same machine.
5. The size of each part is recorded, in lines and in object bytes for the
   host and for wasm, to compare with the Racket slice.

**Decision:**

- If 1 to 3 pass, this design is carried to the same thin vertical slice as
  tinyjs (parse, run a guard, collect, write and read a state) and the two
  are compared on the figures of criterion 4 and 5, and on how long each
  took to build.
- If 4 fails on time, try the post-order layout before anything else.
- If 2 fails in stress mode only, the reservation rule is wrong somewhere
  and must be fixed; it is the one invariant the heap depends on.

## Sources

1. Burger. *PicoLisp: A Radical Approach to Application Development.*
   2006. <https://software-lab.de/radical.pdf>; PicoLisp,
   <https://picolisp.com/>.
2. Felleisen, Friedman. *Control Operators, the SECD-Machine, and the
   λ-Calculus.* Formal Description of Programming Concepts III, 1986.
   The CEK machine.
3. Cheney. *A Nonrecursive List Compacting Algorithm.* Communications of
   the ACM 13(11), 1970.
4. Ager, Biernacki, Danvy, Midtgaard. *A Functional Correspondence between
   Evaluators and Abstract Machines.* BRICS RS-03-13, 2003.
   <https://www.brics.dk/RS/03/13/BRICS-RS-03-13.pdf>
5. Farvardin, Reppy. *From Folklore to Fact: Comparing Implementations of
   Stacks and Continuations.* PLDI 2020.
   <https://www.cs.tufts.edu/comp/150FP/archive/john-reppy/pldi20-stacks-n-conts.pdf>
6. Appel. *Garbage Collection Can Be Faster Than Stack Allocation.*
   Information Processing Letters 25(4), 1987.
7. See tinyjs, [Architectures considered](tinyjs.md#architectures-considered),
   architecture B, and its sources.
8. Ierusalimschy, de Figueiredo, Celes. *The Implementation of Lua 5.0.*
   Journal of Universal Computer Science 11(7), 2005.
   <https://www.lua.org/doc/jucs05.pdf>
9. Moddable. *XS* documentation, on preloading modules into ROM.
   <https://github.com/Moddable-OpenSource/moddable/tree/public/documentation/xs>
