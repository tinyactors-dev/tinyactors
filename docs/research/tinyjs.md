# tinyjs: a freestanding, strict-mode ECMAScript engine with resumable execution

**Date:** 2026-09-30
**Used by:** nothing yet. This is design research for a possible sibling
library, not a decision. An ADR, or a proposal in `docs/proposals/`, would
decide whether and how to build it.

## Question

Can we build a small, correct, efficient and freestanding ECMAScript engine,
in the style of `tinyactors-core` and `tinyxml`, that is fully conformant for
strict-mode code and can suspend and resume a computation anywhere without
blocking the host's event loop? The target workload is many short, frequent
evaluations (chart guards, `<assign>`, short `<script>` blocks), with cheap
suspend and resume. The engine should be good enough that tinyactors could
ship an ECMAScript data model by default.

This note records the goals and constraints, the four architectures
considered, what Scheme compiler research says about resumable execution,
the resulting design of the pipeline, dynamic code and memory, the choice of
generator language, and a plan for a Racket experiment.

## Summary

- **tinyjs is a library in its own right.** It has no relation to
  tinyactors beyond the name. tinyactors is one embedder and keeps its
  adapter on its own side.
- **The big idea is "state machines all the way down", derived, not
  hand-written.** Converting code to continuation-passing style (CPS) and
  then defunctionalizing it produces an abstract machine whose control stack
  is plain data (Danvy et al. [1]). Any builtin or spec operation that can
  call back into user code is written in one small IR in direct style. A
  compiler turns it into a resumable state machine: a frame struct holding
  the variables live at each suspension point, a resume label, and a
  `switch`. User JS compiles to register bytecode with the same frame
  layout. Only work with a low constant bound stays in hand-written C.
- **Every suspension is one-shot**: fuel running out, `await`, `yield`, a
  pending host promise. That is the cheap case in Chez Scheme [4].
  Suspending returns to the host with the frames left in place. Resuming
  jumps to the top frame's label. Nothing is copied.
- **The pipeline is `tjs_parse` → CST, `tjs_compile` → program, and
  `tjs_run` over a program plus a state.** Parsing and compiling are not
  interruptible. They are bounded by limits and never recurse. The program
  is immutable and shareable. The state (heap, paused frames, job queue) is
  what gets resumed and can be written to bytes.
- **`eval` and `new Function` are an optional dependency.** The compiler is
  a separate single-file build. Without it, `HostEnsureCanCompileStrings`
  refuses, and the engine is still conformant. With it, dynamic code costs
  the compiler's size, a stall proportional to the source length, and a
  slowdown confined to functions that contain a direct `eval`.
- **Memory needs no general-purpose collector.** A read-only region, a
  permanent heap per VM, and an evaluation arena per run, plus a write
  barrier and a remembered set. At the end of a run, what survives is copied
  into the permanent heap by a Cheney scan, which needs no stack. This is a
  generational minor collection run only at evaluation boundaries. The
  permanent heap is compacted by writing the state and reading it back. One
  traversal serves promotion, compaction, state images and the object codec.
- **Racket as the build-time generator is likely high leverage**, if it only
  compiles the fixed set of builtins into checked-in C and the frame ABI is
  generated from one source. A one-week experiment with explicit criteria
  for dropping it decides the question.

## Design goals

1. **Correct.** Conform to ECMA-262 for strict-mode code, using test262 as
   the oracle.
2. **Small.** Suitable for embedded and WebAssembly (wasm) targets. There
   must be a version without the compiler, and a static version that drops
   builtins the program cannot reach.
3. **Efficient for short, frequent evaluations.** Entering an evaluation is
   cheap. The common operations (a data property read, adding two numbers)
   never push a frame. A run that allocates and keeps nothing costs a
   bump-pointer allocation and an O(1) reset.
4. **Suspendable anywhere, cheaply.** This includes inside callbacks from
   builtins (`map`, `sort`, getters, Proxy traps, `valueOf`) and inside
   generators and async functions.
5. **Never blocks the host.** There is no I/O, no event loop and no timers
   of its own. The host drives execution by calling `tjs_run` with a fuel
   budget.
6. **Deterministic.** Given the same program, the same state and the same
   inputs from its dependencies, a run produces the same result. Collection
   never depends on wall-clock time. This makes record and replay possible
   for any host.
7. **Resumable from bytes.** A paused VM's state can be written to bytes and
   resumed in another process.
8. **Cheap to have many of.** Programs are shared, and realms are lazy, so a
   host can keep thousands of small VMs.
9. **Easy to build tools for.** The CST is tightly packed and queried with a
   cursor, as in tinyxml, and source spans flow through to the generated
   code and the bytecode.

## Implementation constraints

These are the rules of `tinyactors-core/AGENTS.md` and `tinyxml/AGENTS.md`,
applied to tinyjs.

- **No C library.** Only `stdint.h`, `stddef.h` and `stdbool.h`. No
  `malloc`, `memcpy`, `strlen`, `printf`, `assert` or libm. A check proves
  that no object leaves an undefined symbol, including a `memset` the
  compiler emits on its own.
- **No I/O.** The library never opens, reads, fetches or resolves anything.
  Modules are loaded by the host.
- **All memory comes from the embedder** through a `tjs_allocator`: `alloc`
  and a sized `free`, with no `realloc`.
- **No recursion anywhere**: not in the parser, the compiler, the
  interpreter, the collector, the image codec or the RegExp engine. Walks
  use explicit stacks or queues held in memory from the allocator. A check
  builds the call graph and fails on any cycle.
- **Everything unbounded is bounded.** A `tjs_limits` struct caps heap
  bytes, frame depth, string length, source size for parsing and for
  dynamic code, nesting depth, and RegExp backtracking. Reaching a limit
  throws a catchable error or returns a result. It never exhausts the
  C stack.
- **JS exceptions are data, C errors are results**, as in tinyxml. A thrown
  value is `TJS_RUN_THREW` plus an exception the host can inspect.
  `tjs_result` is for API misuse, running out of memory, and limits.
- **No global mutable state.** Everything lives behind handles the caller
  owns.
- **At most three parameters per function.** Use `_params` or `_config`
  structs, often as compound literals.
- **One public header**, `include/tjs.h`, with `TJS_API` and annotation
  macros for the API extractor. The single-file builds and `tjs.api.json`
  are generated and checked in, and a check fails if one is stale.
- **Naming** follows tinyxml: the `tjs_` prefix, `offset`/`length`/`capacity`
  for byte ranges, and named sentinels, never `0`, for absent indices.
- **No VLAs, no `alloca`, no undefined behaviour.** No C pointer into the
  frame region or the heap survives a call that might suspend, grow the
  region or move objects. The heap uses 32-bit offsets tagged with their
  region.
- **Leaf C must be bounded.** Hand-written C may only do work with a low
  constant bound (formatting a number, one table lookup), or work that
  charges fuel in proportion to its size and runs in chunks within
  `tjs_limits`. Anything that can call user code is written in the IR.

## Standards scope

- **In scope:** ECMA-262, strict mode only. Every function is strict. Modules
  are always strict, so they fit naturally.
- **Out of scope:** sloppy mode, Annex B, and ECMA-402 (Intl). If the
  targeted edition includes Temporal, its time-zone data comes from the
  host.
- **Choosing test262 tests:** run tests flagged `onlyStrict`, and run default
  tests in strict mode. Skip `noStrict`, Annex B, and tests of sloppy dynamic
  code (below). Keep the skip list as data, with a reason for each entry.
- **Dynamic code is strict as well.** Indirect `eval` and `new Function`
  produce sloppy code unless the source begins with `"use strict"`. In tinyjs
  they produce strict code. The difference can be observed:
  `new Function("return this")()`, an old `globalThis` polyfill idiom,
  returns `undefined` instead of the global object. The stricter
  alternative is to throw for any dynamic source that is not strict; that
  breaks more real code.
- **Shared memory:** there is a single agent. `[[CanBlock]]` is false, so
  `Atomics.wait` throws.
- **Weak references:** the spec does not require collection. A WeakRef may
  keep its target alive forever, and FinalizationRegistry callbacks may
  never run. Cleanup runs only at points the host chooses.

## The system

### Public shape

```c
typedef struct {
    tjs_allocator allocator;
    tjs_limits limits;
    tjs_clock clock;            /* Date.now, and the local time zone offset */
    tjs_entropy entropy;        /* Math.random, crypto-style randomness */
    TJS_OPTIONAL const tjs_compiler *compiler;   /* absent: eval and Function refuse */
} tjs_vm_config;

typedef struct { uint32_t fuel; TJS_OUT tjs_run_status *status; } tjs_run_params;
tjs_result tjs_run(tjs_vm *vm, TJS_IN const tjs_run_params *params);
/* status: TJS_RUN_DONE, TJS_RUN_OUT_OF_FUEL, TJS_RUN_WAITING, TJS_RUN_THREW */
```

- **Host functions** are native callbacks. A callback can return a pending
  token instead of a value. The host later calls
  `tjs_settle(vm, token, value)`, and a script that awaited the token
  continues on the next `tjs_run`.
- **`TJS_RUN_WAITING`** means the job queue is empty and only host tokens
  are outstanding.
- **Timers, the scheduler and module loading** are not part of ECMA-262.
  The host defines them as globals or through a loader. The order of
  promise jobs is fixed by the spec. The order in which the host settles
  tokens is an input to the run, and a host that replays must record it.
- **Every dependency can be replaced** by a deterministic stand-in: a
  virtual clock, seeded entropy, a scripted scheduler.

### Two artifacts: program and state

| | Program | State |
|---|---|---|
| Made by | `tjs_compile` | `tjs_run`, `tjs_state_read` |
| Changes | never | on every run |
| Holds | bytecode, constants, source spans, the builtins it reaches | permanent heap, paused frames and their arena, job queue, pending host tokens, code compiled at run time |
| Shared | across VMs, content-addressed by hash | one per VM |

A state names its program by hash. Resuming needs both. "Keep the image to
resume" therefore means keeping the state; the program is usually shared
and cached.

## The pipeline

### `tjs_parse`: source to CST

- Tightly packed CST nodes in an arena, in pre-order. Each node records its
  kind, its source range as `offset`/`length`, and the index just past its
  subtree. This is tinyxml's node column.
- A `tjs_cursor` is a document plus an index: `first_child`,
  `next_sibling`, `parent`, and reading kind, span and text. Devtools
  (outline, highlighting, linters) are built on the cursor.
- The parser is not interruptible, but it is iterative: an explicit stack of
  operators and frames in the arena. Its time and memory are bounded by
  `tjs_limits` (source size, nesting depth).
- A syntax error is data: a document error with a position, as in tinyxml.

### `tjs_compile`: CST to program

- Static semantics: early errors, scope analysis, the TDZ, and whether a
  function contains a direct `eval`.
- Lowers user JS to register bytecode. The bytecode frame layout is the
  frame ABI shared with the generated builtins (below).
- It records the builtins the program can reach, so a static build can drop
  the rest.
- Options:
  - `no_dynamic_code` reports every `eval` call and every use of the
    `Function`, `GeneratorFunction` and `AsyncFunction` constructors as a
    diagnostic. This is host policy, not a language `SyntaxError`.
  - Optional ahead-of-time compilation of user code to C in the static
    version. Not a starting assumption; see the risks in the Racket section.
- Not interruptible. Iterative, and bounded by input size.

### `tjs_run`: executing a program over a state

One trampoline drives two kinds of frames: bytecode frames for user code,
and generated state machines for builtins. The trampoline loop:

- runs the top frame's step;
- when a frame returns, pops it and re-enters the caller at its label;
- when a frame throws, unwinds to the nearest frame with a handler label;
- charges fuel at loop back-edges and calls, and returns
  `TJS_RUN_OUT_OF_FUEL` when the fuel is spent.

This is Gambit's approach [5]: an explicit stack and a trampoline in
portable C, without native tail calls or `call/cc`.

## Execution model: four layers

### Layer 0: leaf C

Bounded work only. It never suspends and never calls into JS.

- Shortest round-trip number formatting and correctly rounded `parseFloat`.
- A math library (`sin`, `exp`, `pow`, and so on) with no libm.
- BigInt arithmetic within limits.
- Unicode tables and lookups: identifiers, case mapping, normalization,
  RegExp property escapes. These are expected to be the largest part of the
  binary.
- String concatenation as a rope node, which is O(1). Flattening a rope
  happens later, in chunks that are charged fuel.
- A RegExp engine with an explicit backtracking stack. It charges fuel and
  is bounded by `tjs_limits`, which also defeats ReDoS. It is written as
  resumable code if matching must be suspendable.

### Layer 1: the resumable IR

A small, first-order language in the style of Guile's CPS soup [2], with
Kennedy-style second-class continuations [3] (join points that compile to
jumps). Every function is marked with its effect:

- `call` may run user code, and is the only place a computation can
  suspend.
- `leaf` calls layer 0, and must not reach a `call`. The compiler rejects a
  `leaf` that does.

The spec's `?` ("rethrow an abrupt completion") needs no code, because every
`call` carries an exception continuation.

Spec operations are transcribed into the IR nearly line by line:
`ToPrimitive`, `OrdinaryGet`, `Array.prototype.sort`, promise jobs, Proxy
traps, iterator protocols. The syntax below is illustrative:

```scheme
;; ECMA-262 13.15.3 ApplyStringOrNumericBinaryOperator, opText = +
(define (add lval rval)
  (let* ((lprim (call to-primitive lval 'default))
         (rprim (call to-primitive rval 'default)))
    (if (or (string? lprim) (string? rprim))
        (leaf string-concat (leaf to-string lprim) (leaf to-string rprim))
        (leaf numeric-add lprim rprim))))

;; 7.1.1.1 OrdinaryToPrimitive
(define (ordinary-to-primitive o hint)
  (for-each (name (if (eq? hint 'string) '("toString" "valueOf") '("valueOf" "toString")))
    (let ((method (call get o name)))
      (when (callable? method)
        (let ((result (call call-function method o)))
          (when (not (object? result)) (return result))))))
  (throw TypeError))
```

### Layer 2: the compiler passes

The passes are nanopass-style [6]: each has a defined input and output
language.

1. **CPS conversion.** Split every function at each `call` into labelled
   blocks, a "soup" of labels.
2. **Liveness.** Compute which variables are live at each suspension point.
   Variables that are never live at the same time share a frame slot. In
   `add`, `rval` and `lprim` share a slot. In `ordinary-to-primitive`, only
   `o` and the loop index survive a call.
3. **Defunctionalization.** Return points become values of a resume label.
   The continuation is now data: a label plus the live slots.
4. **Frame layout.** Emit one frame struct per function: a header (function
   ID, resume label, the caller's offset) plus the slots.

### Layer 3: two back ends from one IR

- **Build time: builtins become C state machines.** These are generated and
  checked in, like `txml.c`. A step function returns after every call and
  never calls a callee itself:

  ```c
  typedef struct { tjs_frame header; tjs_value a; tjs_value b; } tjs_add_frame;
  enum { TJS_ADD_L0, TJS_ADD_L1, TJS_ADD_L2 };

  static tjs_step tjs_add_step(tjs_vm *vm, tjs_add_frame *f) {
      switch (f->header.label) {
      case TJS_ADD_L0:
          f->header.label = TJS_ADD_L1;
          return tjs_call_to_primitive(vm, f->a, TJS_HINT_DEFAULT);
      case TJS_ADD_L1:
          f->a = vm->result;
          f->header.label = TJS_ADD_L2;
          return tjs_call_to_primitive(vm, f->b, TJS_HINT_DEFAULT);
      case TJS_ADD_L2: {
          const tjs_value lprim = f->a;
          const tjs_value rprim = vm->result;
          if (tjs_is_string(lprim) || tjs_is_string(rprim)) {
              return tjs_return(vm, tjs_string_concat(vm, tjs_to_string_leaf(vm, lprim),
                                                          tjs_to_string_leaf(vm, rprim)));
          }
          return tjs_return_numeric_add(vm, lprim, rprim);
      }
      }
      return tjs_unreachable(vm);
  }
  ```

- **Run time: user JS becomes register bytecode.** Here the program counter
  is the resume label and the registers are the frame, so there is almost
  nothing to defunctionalize. The dispatch loop handles common cases inline
  and pushes an IR frame only for the rest:

  ```c
  case TJS_OP_ADD:
      if (tjs_both_numbers(l, r)) { R(dst) = tjs_number(tjs_num(l) + tjs_num(r)); break; }
      if (tjs_both_strings(l, r)) { R(dst) = tjs_string_concat(vm, l, r);         break; }
      frame->pc = pc; frame->pending = dst;
      return tjs_push_add(vm, l, r);
  ```

A fast path is also possible in generated code: the primitive case of
`ToPrimitive` can be inlined so that no frame is pushed.

**Only the build-time path needs the hard middle of the compiler.** CPS,
liveness and defunctionalization exist to make C resumable. The run-time
compiler is a conventional bytecode compiler. The two share a contract, not
passes: the frame ABI, the value representation, exception unwinding, and
fuel accounting. The ABI should be generated from one source into a header
(`tjs_abi.h`) that both sides use.

### A worked trace: suspending inside `valueOf`

```js
const unit = { valueOf() { while (slow()) {} return "kg"; } };
total + unit        // total = 3, result "3kg"
```

Fuel runs out on a back-edge of the `while` loop, and `tjs_run` returns
`TJS_RUN_OUT_OF_FUEL`. The frame region is left as it is:

```
┌ guard            (bytecode)  pc after ADD, pending → r2, r0 = 3, r1 = unit
├ add              (C)         label L2, a = 3     ← left operand was primitive: no frame
├ to-primitive     (C)         label after ordinary-to-primitive
├ ordinary-to-prim (C)         label after call-function, o = unit, i = 0
└ valueOf          (bytecode)  pc = loop back-edge, its registers
```

The host can call `tjs_run` again later, or write the state and resume it
elsewhere. On resume:

1. `valueOf` returns `"kg"`.
2. `ordinary-to-primitive` returns it, because it is not an object.
3. `to-primitive` returns it.
4. `add` resumes at `L2`. It formats `3` (leaf) and builds the rope `"3kg"`
   (leaf).
5. The guard's frame receives `r2` and continues.

`await`, `yield` and host tokens work the same way. Each is a `call` that
returns control to the host instead of to a callee.

### Frames and the stack discipline

- Frames live in a growable region per VM, addressed by offsets, with a
  stack discipline. This is the "resizable stack" that Farvardin and Reppy
  found the best all-round choice [7]. They advise against mutable linked
  heap frames.
- A frame that must outlive the stack discipline (a suspended generator or
  async function) moves out of the region when it yields. This is Chez's
  "seal the segment" for one-shot continuations [4].
- Short runs only bump a pointer to make frames and never involve the
  collector.

## Dynamic code: `eval` and `new Function`

### Semantics under strict mode

- **Direct eval can be found by reading the source.** In strict code a
  binding named `eval` cannot be declared or assigned. So only the functions
  whose text contains a call to `eval(...)` can do a direct eval; at run
  time, the callee must also be `%eval%`. Only those functions, and the
  scopes around them, keep their variables in named environment records.
  Strict direct eval gets its own variable environment, so its `var`
  declarations never leak into the caller.
- **Indirect eval and the `Function` constructors** see only the global
  scope. They need the compiler at run time but slow down no scope.

### Two versions from one library

- There are two single-file builds: `tjs.c` (runtime) and `tjs_compile.c`
  (parser and compiler). The runtime never references a compiler symbol.
  The host passes the compiler as the optional `tjs_vm_config.compiler`.
- **Without a compiler**, `HostEnsureCanCompileStrings`, which `PerformEval`
  and `CreateDynamicFunction` call, returns a throw completion (an
  `EvalError`, as browsers throw under Content Security Policy). This is
  conformant.
- **Optionally**, `tjs_compile` with `no_dynamic_code` rejects such code at
  compile time, so a program fails before it runs.

### What the dynamic version costs

- **Binary size.** The parser and compiler are linked into the runtime.
  After the builtins and the Unicode tables they are expected to be the
  largest component. This is the main cost.
- **Latency.** Compiling is not interruptible, so a stall is proportional to
  the source length. `tjs_limits` caps the size of dynamic source, and
  compiling charges fuel so the accounting stays correct.
- **Memory and state.** Code compiled at run time lives in the state, not
  in the shared program. Cache it by source hash, because templating code
  often builds the same function many times. A state can store dynamic code
  as source plus hash and recompile it on load, provided the compiler is
  deterministic.
- **Speed.** Only the functions that contain a direct `eval` are slower.

### What the static version lets you rely on

- All code is known when compiling. No named environments are needed, and
  variable names never exist at run time.
- Builtins the program cannot reach can be dropped from it.
- User code can be compiled to C ahead of time through the builtins' back
  end.
- There is no code injection, which matters for charts built from untrusted
  input.

## Memory: regions instead of a general collector

A general-purpose collector is not needed. What is needed is a minor
collection at each evaluation boundary.

### Three regions

- **ROM.** Shared and read-only: programs and the frozen built-in objects.
  A write into a ROM object first copies it into the permanent heap. That
  is how realms stay lazy: a VM pays only for the built-in objects it
  changes.
- **Permanent heap.** One per VM.
- **Evaluation arena.** One per run. Allocation bumps a pointer.

### Write barrier and promotion

- **The barrier.** Storing a value into an object checks whether the target
  is in the permanent heap and the value is in the evaluation arena. With
  offsets tagged by region this is one comparison. If both hold, the slot
  goes into a remembered set.
- **Promotion.** When a run completes (`DONE` or `THREW`), everything
  reachable from the remembered set, the result, the job queue and host
  handles is copied into the permanent heap by a Cheney scan. The scan
  queue is the destination region itself, so there is no stack and no
  recursion. The arena is then reset in O(1).
- **Cost.** Proportional to what survives. A guard that keeps nothing costs
  a bump pointer and a reset.

### Compaction and the codec

- **The permanent heap only grows between compactions.** `this.state = {…}`
  on every run leaves the old objects behind.
- **Compaction is a copy from the roots.** Writing a state is a copying
  traversal from the roots, and reading it back produces a compact heap. A
  full collection is therefore "write the state and read it again", which
  tinyactors already does whenever it offloads an actor. Hosts that never
  offload call `tjs_compact`.
- **A limit makes growth visible.** A cap on permanent heap size turns
  unbounded growth into an error the host can see, never a crash.
- **One traversal, three uses.** Promotion (arena to permanent heap),
  compaction (permanent heap to a new one) and the object codec (heap to
  bytes or to host values) are the same Cheney traversal with a different
  destination.

### Details

- **A paused run keeps its arena**, because its frames live there. A reset
  waits until the run completes, and a written state includes the arena of
  a paused run.
- **Long async runs can outgrow their arena.** Because frames are data,
  every root is precise and known, so a promotion in the middle of a run
  can be added later. Start with a limit that throws a catchable error.
- **Objects move, so hosts hold handles**, never pointers. The handle table
  is a root, and promotion updates it.
- **WeakMap entries** whose key did not survive need ephemeron handling
  during promotion: repeat until nothing more is copied. Dead keys in the
  permanent heap disappear at the next compaction.
- **Collection points are deterministic**: run completion, writing a state,
  and explicit compaction.

## Using tinyjs from tinyactors

The adapter lives in tinyactors, not in tinyjs. It maps:

- `ta_datamodel_vtable` calls to `tjs_*` calls;
- `prepare_definition` to one compile per definition, shared by all actors;
- one VM per actor;
- `ta_dm_host` to tinyjs's clock and entropy dependencies;
- `value_write` and `value_read` to writing and reading the state.

The layer rules in the root `README.md` place this adapter outside core,
which ships only the null data model (core ADR 0057). The host kernel and
the bindings would register it by default.

What changes in tinyactors itself:

- **The data-model vtable must be able to pause.** It is synchronous today:
  `ta_op_status` is OK, ERROR or NOMEM. It needs a paused status and a way
  to resume. `ta_continuation` must record that an action is part-way
  through evaluation. This is a core ADR, and it must keep the guarantees
  of ADR 0061 (pausing inside executable content), ADR 0070 (saving a pause
  cannot fail) and ADR 0047 (no repeated effects).
- **Replay must see host inputs.** Values passed to `tjs_settle`, the clock
  and the entropy seed must enter the input log, so replay stays
  deterministic.

## Architectures considered

All four share leaf C and the stackless requirement. They differ in how
code that re-enters user code is made resumable.

| | A: AST interpreter | B: bytecode, builtins self-hosted in JS | C: derived from the spec text | D: resumable IR compiled to state machines |
|---|---|---|---|---|
| How builtins become resumable | hand-written C state machines | written in strict JS, compiled to bytecode | ecmarkup compiled to IR (ESMeta-style) | written in IR; the compiler derives the state machines |
| Size | smallest engine | medium | largest | medium (bytecode for builtins) to larger (C for builtins) |
| Short expressions | good | good | poor | good (fast paths) |
| Loops and scripts | fair | best | poor | best |
| Cost of compliance | high and ongoing | medium | high up front, low per feature | medium, close to transcribing the spec |
| Main risk | hand-written state machines are error-prone | self-hosted code must be unobservable to user code | slow without fast paths, which bring hand-written code back | the build-time and run-time compilers must agree on the ABI |

- **A: stackless AST interpreter.** It is closest to core's
  `ta_exec_frame` pattern, but it amounts to doing Danvy's derivation by
  hand for every builtin.
- **B: stackless bytecode VM with self-hosted builtins.** This is the design
  of SpiderMonkey's and V8's self-hosted libraries, with QuickJS showing
  what a compact engine looks like. Its self-hosted code must avoid
  mutable built-in prototypes through intrinsics.
- **C: spec-derived.** Gives the highest fidelity and makes editions easy to
  follow, but the spec's algorithms are written for clarity, not speed.
- **D (preferred): one IR, one derivation.** It combines B's VM with C's
  closeness to the spec, and it removes hand-written resumability.

## What the research says

- **CPS plus defunctionalization gives an abstract machine.** Ager,
  Biernacki, Danvy and Midtgaard show that converting an evaluator to CPS
  and defunctionalizing it produces an abstract machine. The machine's
  control stack is the defunctionalized continuations [1]. This is the
  basis of design D.
- **CPS as an IR names every control point and every intermediate value.**
  Guile's IR is a map from continuation labels to their bodies, with no
  nested scopes, and was inspired by Kennedy [2]. Kennedy makes
  continuations second-class, so they compile to jumps and act as join
  points [3].
- **One-shot continuations are cheap.** Chez Scheme observed that most
  continuations, including those used for threads, are invoked at most
  once. Restricted to one use, capturing a continuation seals the current
  stack segment and starts a new one, with no copying [4].
- **Scheme compiles to portable C with a trampoline.** Gambit compiles its
  virtual machine (GVM) to portable C. Tail calls and continuations use a
  trampoline and an explicit stack array with overflow checks, at a small
  cost compared with native code [5].
- **Many small passes can be as fast as a traditional design.** The Chez
  Scheme compiler was rewritten as many single-task passes, each with a
  formally defined intermediate language, and performs on par with its
  older structure [6].
- **The stack strategies have been compared with measurements.** Farvardin
  and Reppy compare six strategies in one compiler:
  - CPS with heap-allocated continuations is the simplest to implement and
    well suited to concurrency, but its sequential performance is poorer.
  - A resizable stack is the better choice among the stack-based
    strategies, because it is space efficient.
  - Linked mutable frames "should always be avoided".

  If sequential speed matters and advanced control does not, a contiguous
  stack is best [7].
- **JavaScript with first-class continuations has been done.** Stopify
  compiles JS to JS with first-class continuations. This gives stopping,
  stepping and long-running programs without freezing the browser, and
  needed no or trivial compiler changes for nine of the ten languages it
  was applied to [8]. In JS, implicit calls (getters, `valueOf`, Proxy
  traps) are what make every operation a potential call. We have not
  checked its overhead figures.
- **There is a tested semantics for strict-mode ES5.** S5 is a semantics
  for ES5.1 strict mode. It reduces the language to under 35 syntactic
  forms and was tested against conformance suites [9]. It is the closest
  existing model of the IR the builtins would be written in.
- **Nanopass is available for Racket.** It is a Racket package; its README
  says it was tested on Racket 6.0 to 6.2 [10]. Whether it runs unchanged
  on current Racket (mise offers 9.1 to 9.3, the minimal distribution
  through aqua) is part of the experiment below.

## Generator language: Racket, Ruby or C

The build-time generator compiles the IR builtins to C and generates the
frame ABI header. It never runs in an ordinary build: its output is checked
in, and `mise run generated` fails when the output is stale.

| Generator | Assessment |
|---|---|
| **Racket** | The best tools for this job. Nanopass gives checked intermediate languages. A `#lang` with `syntax-parse` gives a builtin language with good error messages and source locations, which become `#line` directives back to the IR and the spec step. A reference interpreter, or a PLT Redex model, makes random differential testing of the generated C possible. The cost is a second language beside the Ruby tasks. |
| **Ruby** (the repo's task language) | Workable, since pattern matching handles trees. But we would build IR checking, source locations and a reference interpreter ourselves: the parts that give the leverage, without the leverage. |
| **C** (one compiler with two back ends) | No duplication. But every pass must follow the house style (freestanding, no recursion, at most three parameters), which is the most expensive way to write a compiler, and the build-time path doesn't need those constraints. |

**Where Racket goes wrong, and the mitigations:**

1. **The two compilers drift apart.** Racket generates `tjs_abi.h` (frame
   structs, label enums, value tags), and the C compiler includes it. A test
   has builtins and user code call each other in both directions.
2. **Only one person can change a builtin.** Keep the IR close to spec
   pseudo-code, and add builtins from a template. Contributors who don't
   change builtins never need Racket.
3. **The build splits across languages.** Racket is pinned in the root
   `mise.toml` and runs only behind a Ruby task (`mise run generate`).
4. **Scope creep.** Compiling user JS to C ahead of time in Racket, reading
   `tjs_parse`'s CST, would put Racket into every user build. It needs its
   own ADR.

## Outline of the Racket experiment

**Time box:** one week.

**Scope:**

1. Pin Racket (minimal) with mise, and check that the nanopass package
   installs and runs on it.
2. Define a small IR `#lang`: `define`, `let*`, `if`, `for-each`, `call`,
   `leaf`, `return`, `throw`, and spec-step annotations.
3. Write four builtins in it: `add` (13.15.3), `to-primitive` (7.1.1),
   `ordinary-to-primitive` (7.1.1.1) and `Array.prototype.map`.
4. Write passes: parse to IR, check effects (no `leaf` reaches a `call`),
   CPS conversion, liveness and slot sharing, defunctionalization, frame
   layout, then emit C and `tjs_abi.h` with `#line` directives.
5. Write a reference interpreter for the IR in Racket.
6. Write a minimal C harness: a value representation, an object with a
   `valueOf`, a trampoline with fuel, and a small bytecode loop that
   executes `ADD` and a call into a bytecode `valueOf`.

**Success criteria:**

1. The generated C meets the house rules without hand edits. It is
   freestanding, with no undefined symbols at -O0 and -O2, passes a
   recursion check and a check of at most three parameters, and compiles
   with the pinned clang and `zig cc`.
2. The builtins read like the spec. Someone who doesn't know Racket can
   review `ordinary-to-primitive` against section 7.1.1.1.
3. A random differential test (IR interpreter against generated C) finds a
   bug deliberately planted in the liveness pass.
4. The trace above works end to end. The C bytecode loop calls `add`,
   `add` calls a bytecode `valueOf`, fuel runs out inside the loop, the
   frames are written to bytes and read back, and the run resumes to
   `"3kg"`.
5. Frame sizes match the hand analysis: `add` has one value slot plus its
   reused argument slot, and `ordinary-to-primitive` keeps `o` and the
   index.

**Decision:**

- If all five criteria pass, Racket is the generator.
- If 1 or 2 fail, keep the IR and the ABI design and write the generator in
  Ruby. The IR and the ABI are the real design, so little is lost.
- If 4 fails, revisit design D itself before choosing a language.

## Open questions

- **The IR's exact form.** How many forms, how exceptions and `finally` are
  expressed, and whether iterators and generators are primitives or
  library code.
- **Builtins as bytecode or as C.** When to compile builtins to C (faster,
  larger) rather than to bytecode (smaller), and whether the static version
  should decide per builtin.
- **Paused frames and closures.** How the frames of a paused run and the
  closures created during it interact with promotion at the end of the run.
- **RegExp.** Whether matching is suspendable (written in the IR) or bounded
  leaf C with fuel and chunking.
- **Budgets.** Real figures for size, bytes per VM and cost per evaluation,
  to be measured, not estimated, and recorded per the benchmark rules in
  `AGENTS.md`.
- **The tinyactors side.** The core ADR for a data-model vtable that can
  pause, and how host tokens enter the input log.

## Sources

1. Ager, Biernacki, Danvy, Midtgaard. *A Functional Correspondence between
   Evaluators and Abstract Machines.* BRICS RS-03-13, 2003.
   <https://www.brics.dk/RS/03/13/BRICS-RS-03-13.pdf>. See also Danvy,
   *Defunctionalized Interpreters for Programming Languages*, ICFP 2008,
   <http://jfla.inria.fr/2014/danvy-ICFP08.pdf>.
2. Wingo. *CPS soup*, 2015,
   <https://wingolog.org/archives/2015/07/27/cps-soup>; Guile manual,
   *CPS Soup*,
   <https://www.gnu.org/software/guile/manual/html_node/CPS-Soup.html>;
   *approaching cps soup*, 2023,
   <https://wingolog.org/archives/2023/05/20/approaching-cps-soup>.
3. Kennedy. *Compiling with Continuations, Continued.* ICFP 2007.
   <https://www.microsoft.com/en-us/research/wp-content/uploads/2007/10/compilingwithcontinuationscontinued.pdf>
4. Bruggeman, Waddell, Dybvig. *Representing Control in the Presence of
   One-Shot Continuations.* PLDI 1996. Listed at
   <https://www.scheme.com/pubs/index.html>. See also Hieb, Dybvig,
   Bruggeman, *Representing Control in the Presence of First-Class
   Continuations*, PLDI 1990,
   <https://www.cs.tufts.edu/~nr/cs257/archive/kent-dybvig/stack.pdf>.
5. Gambit Scheme. <https://gambitscheme.org/research/>;
   <https://en.wikipedia.org/wiki/Gambit_(Scheme_implementation)>.
6. Keep, Dybvig. *A Nanopass Framework for Commercial Compiler
   Development.* ICFP 2013.
   <https://www.cs.tufts.edu/comp/150FP/archive/icfp13.pdf>
7. Farvardin, Reppy. *From Folklore to Fact: Comparing Implementations of
   Stacks and Continuations.* PLDI 2020.
   <https://www.cs.tufts.edu/comp/150FP/archive/john-reppy/pldi20-stacks-n-conts.pdf>
8. Baxter, Nigam, Politz, Krishnamurthi, Guha. *Putting in All the Stops:
   Execution Control for JavaScript.* PLDI 2018.
   <http://static.cs.brown.edu/~sk/Publications/Papers/Published/bnpkg-stopify/paper.pdf>;
   <https://arxiv.org/abs/1802.02974>
9. Politz, Carroll, Lerner, Pombrio, Krishnamurthi. *A Tested Semantics for
   Getters, Setters, and Eval in JavaScript.* DLS 2012.
   <https://cs.brown.edu/~sk/Publications/Papers/Published/pclpk-s5-semantics/paper.pdf>
10. nanopass-framework-racket.
    <https://github.com/nanopass/nanopass-framework-racket>
