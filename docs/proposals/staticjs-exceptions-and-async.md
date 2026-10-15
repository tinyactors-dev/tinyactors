# staticjs: exceptions, and async functions over host promises

**Status:** Sketch (2026-10-15). A design for discussion; nothing here is
decided or scheduled.
**Touches:** `staticjs/` (lexer, parser and its public events, compiler,
evaluator, values, built-ins, writer, matcher, `include/staticjs.h`),
`staticjs-js` (it maps the new run status onto JavaScript promises), and
later the data model `tinyactors-staticjs`, which refuses the async half.
**Not a plan:** ADRs and roadmaps win. [The staticjs
proposal](staticjs.md) still lists `try`, `throw`, `async` and `await` as
left out; this document says what it would take to bring them in without
breaking [the rule](staticjs.md#the-rule).

## Whom it serves, who pays, what is out

- **First, the application developer who writes plugins and code-mode
  programs** (`staticjs/docs/guide/language.md`'s module example; programs an
  agent writes against the host's tools). They need `try`/`catch` around a
  step that may fail, and `await Promise.all(xs.map(async (x) => tool(x)))`
  to call the host's tools concurrently and get control back to the host
  while those run.
- **Then the C embedder**: a tool call that takes time cannot be a
  synchronous native (`include/staticjs.h:351`: "must finish quickly").
  Today such a host must split a program by hand around every call.
- **The binding author** (`staticjs-js`) maps a deferred native onto a
  JavaScript promise: one new run status and one call.
- **Who pays.** A program with neither feature pays nothing per
  instruction: handlers are found from a table only when something is
  thrown, and an async call is one flag test on the closure call path that
  already reads `fn->flags` (`src/vm.c`, `enter_closure`). Each evaluator
  gains a job queue and a promise table, allocated on first use. The
  public API gains one run status, one error code, a native's way to defer
  or throw, and a settle call. The streaming events gain about eight kinds,
  which every sink sees.
- **Out of scope:** generators (the same suspended activation would carry
  them later), `for await` (there are no loops), top-level `await` in a
  module (module evaluation order under it is a separate design),
  thenables other than staticjs's own promises (they fault), and making
  faults catchable (below: they stay faults).
- **The data model** (stage 2 of [staticjs.md](staticjs.md#stages)) runs
  expressions inside a macrostep, which cannot wait for the host. It would
  compile with async refused (`unsupported`), and keep `try`.

## The frame: what is true today

Each fact below is in the repository; every later claim is checked
against it.

1. **One evaluation per evaluator, driven by fuel, on an explicit stack.**
   `sjs_vm_run(vm, fuel)` returns `SJS_RUN_DONE`, `SJS_RUN_PAUSED` or
   `SJS_RUN_FAULT` (`include/staticjs.h:738`); frames and slots are arrays
   in `sjs_vm` (`src/vm.h`, `struct sjs_vm`), never the C stack (AGENTS.md,
   "No recursion"). An evaluation pauses anywhere, a callback of `map`
   included, because a looping built-in is a frame too (`FRAME_LOOP`).
2. **The only abnormal exit is a fault, and a fault ends the evaluation.**
   Nothing catches it; `cut` releases the stack (`src/vm.c:73`).
3. **Values are never changed once shared**, and a function changes a
   value only through a variable of its own that holds it alone
   (`docs/internals/mutation.md`). Shared values are immutable trees with
   reference counts and no cycles (staticjs.md, "Values").
4. **Moves rely on fact 2.** The last read of a slot is a `TAKE_LOCAL`
   that leaves it undefined (`src/compiler.c:52`). An append to a `let`
   takes the value before the right-hand side runs and stores it after:
   "The store follows the whole value on every path, so the move is never
   seen; a fault ends the evaluation, as always"
   (`docs/internals/mutation.md`, "Appending to a let"). `sort` on a
   variable or a path leaves the place undefined while toSorted's loop,
   comparator included, runs (`src/vm.c:2067`, `src/vm.c:2593`).
5. **Every jump goes forward, and a running function cannot be entered
   again** (`closure_running`, `src/vm.c:723`, which scans the frames).
   Together they make every evaluation end (staticjs.md, "Every evaluation
   ends").
6. **Natives are synchronous** and receive only the context
   (`sjs_native_call`, `include/staticjs.h:357`).
7. **Programs and closures outlive an evaluation**; a closure called in a
   later evaluation reads its realm's globals (`sjs_run_params.function`).
8. **The parser already refuses** `try`, `throw` (`src/parser.c:1168`),
   `async` (`src/parser.c:1086`) and `await` (`src/parser.c:1178`), and
   reads arrow parameter lists ahead (`max_lookahead`).

Three behaviours of JavaScript the design depends on, checked with Node
22.22 (spike in [Evidence](#evidence)):

- a comparator that throws leaves the array unsorted:
  `[3,1,2,5,4]` stays `[3,1,2,5,4]`;
- `s = s + f()` with `f` throwing leaves `s` as it was;
- a callee that pushes onto its argument and then throws: the caller's
  `catch` sees the push (`[1,2]`).

## Part 1: `throw`, `try`, `catch`, `finally`

### What is caught, and what stays a fault

`catch` catches a value a program threw (`throw v`), a rejection an
`await` resumes with, and a value a native throws (below). **A fault is
never caught**, and `finally` does not run on one. Faults stand where
JavaScript coerces, compares identity or reaches a prototype; there
JavaScript does not throw at all, so a catchable fault would let the
program go on with a value JavaScript never computes. Where JavaScript
does throw (`null.x`, `JSON.parse("{")`), it throws an error whose message
is the engine's own, and making it catchable means giving that message.
So the rule holds as it is: an uncaught fault is still a fault, and a
program that relied on catching a built-in's error gets a fault, never a
different value.

A built-in may later throw where JavaScript throws, one at a time, when its
error's `name` and `message` equal V8's and `diff-node` checks them.
`JSON.parse` is the first candidate: it is the call people wrap in `try`.
That is a follow-up per built-in, not part of this design.

### Error values

`new Error(message, { cause })`, and `TypeError`, `RangeError`,
`SyntaxError` the same way, make a new value kind. Its own properties are
`message` and `cause`, which JavaScript makes non-enumerable: `Object.keys`
gives `[]`, `JSON.stringify` gives `"{}"`, `{...e}` gives `{}`, and
`String(e)` gives `"Error: x"` (spike). `name` comes from the kind. `stack`
is the engine's text, so reading it faults. `instanceof` stays out; a
program tests `e.name`. Anything can be thrown, as in JavaScript: `throw
{ code: 404 }` needs no error kind.

### The move problem

Fact 4 is the heart of it: every move that leaves a slot empty while code
runs is safe today only because nothing but a fault can leave that code
early. A `catch` in the same function is a second exit that goes on, and
reads the slot. Callers cannot see a callee's slots, so only a handler in
**the same function** can observe an empty slot. That bounds the work:

| Window | Today | With a handler in the function | Fix |
|---|---|---|---|
| Last read moves (`TAKE_LOCAL`) into a callee, which changes it in place and throws | never seen | the `catch` reads `undefined`; JavaScript gives `[1,2]` | compiler: a read inside a `try` is not last while a `catch`, a `finally`, or code after the `try` reads the slot. Then the callee's change faults ("held elsewhere"), which the rule allows |
| A `return` inside a `try` ends its path, so its reads move (`mutation.md`, "Last reads in branches") | never seen | `try { return use(x) } catch { return x }` | compiler: a `return` inside a `try` with a handler does not end the path for the slots the handler reads |
| Append to a `let` takes the value before the right-hand side | never seen | `s` is `undefined` after `s = s + f()` throws; JavaScript keeps `"ab"` | compiler: evaluate the right-hand side first, then take, then append (`APPEND slot`). Nothing in the right-hand side can read or change `s` (a `let` is never captured, and a right-hand side that reads `s` already keeps its copy), and `+` on what staticjs accepts calls nothing, so the order cannot be seen |
| `sort` takes the array from its variable or path while its comparator runs | never seen | the variable is `undefined` after a comparator throws; JavaScript leaves the array unsorted | evaluator: unwinding a sort's loop frame puts `LOOP_RECEIVER` back in its place. The sort works on scratch arrays and orders the receiver only at its end (`src/builtins.c`, `sort_step`), so the receiver is the unsorted original |
| Others that take a value out while code may run | | | **untraced**: an audit of every opcode that leaves a slot or a place empty (iterator helpers' `next` with a callback first). Each gets one of the two fixes, or a compile-time refusal inside a `try` |

The rule of thumb for each window: close it in the compiler when the order
cannot be seen, otherwise give the unwinder a restore. Every case gets a
spec case in `mutation.spec` that throws inside it and reads the variable
in the `catch` and after the `try`.

What needs no fix: a change made in place before a throw is seen by the
`catch` exactly as in JavaScript (the value was held alone, so nobody else
can tell); and the thrown value moves into the `catch` parameter, a new
variable of the function that holds it alone when nothing else does, so
`catch (e) { e.push(1) }` works by the existing rules.

### Through the pipeline

- **Lexer:** nothing; `try`, `catch`, `finally` and `throw` are keywords
  already (`src/lexer.c:88`).
- **Parser:** statements `throw <expr>` (no line break after `throw`, a
  syntax error as in JavaScript), and `try` block with `catch`
  (`catch (pattern)` or `catch` alone) and/or `finally`. New events,
  public because sinks read them (`sjs_event`,
  `include/staticjs.h:540`): `TRY_BEGIN`, `CATCH_BEGIN` (its pattern
  follows as `const` patterns do), `FINALLY_BEGIN`, `TRY_END`, and `THROW`
  after its operand.
- **Compiler:**
  - each function gets a handler table: `(start, end, handler, height)`,
    innermost first, emitted as regions close. No instruction runs on
    entering a `try`, so a program pays only when it throws;
  - `catch` binds its parameter in a block scope of its own;
  - `finally` keeps a completion in two hidden slots (kind: normal,
    return, throw; and its value). A `return` inside a `try` with a
    `finally` stores the completion and jumps forward to it;
    `END_FINALLY` then goes on, returns or rethrows, to the next
    `finally` out when there is one. Every jump stays forward (fact 5);
  - the move rules of the table above;
  - the matcher (`src/matcher.c`) passes the new events through, and a
    shape never matches a body with a `try`.
- **Evaluator:** `THROW` sets the thrown value and unwinds: from the top
  frame down, a code frame whose table covers its `pc` cuts the stack to
  the handler's height, pushes the value and jumps; any other frame is cut
  (a loop frame through `sjs_loop_free`, a sort restoring its receiver
  first), a unit of fuel per frame. Past frame 0 the evaluation ends with
  the value thrown. A native may throw: `sjs_native_call` gains a flag
  that makes its `result` the thrown value.
- **Values and built-ins:** the error kinds above, in `builtins.md`'s
  way (`docs/internals/builtins.md`), and in the writer: an error writes as
  JavaScript's `JSON.stringify` writes it, `{}`.
- **API:** an uncaught throw is `SJS_RUN_FAULT` with a new error code,
  `SJS_ERROR_THROWN`, and `sjs_vm_take_thrown(vm, &value)` gives the
  value. A host tells "the program threw" from "staticjs declined".

## Part 2: `async`, `await`, and promises

### What a program can observe

Nothing staticjs shares can change (fact 3), so two interleaved async
functions cannot see each other's work except through what they return.
Order is observable in two places only: which promise settles first
(`Promise.race`, `Promise.any`), and the order in which natives are
called. Both must match JavaScript's job order exactly; everything else
about interleaving is invisible. That keeps the surface small, and says
what the tests must pin down.

### Promises as values

A promise is a record in the evaluator: its state, its value, its
reactions, and whether something handled its rejection. The value a
program holds is a handle (an index and a generation), which takes no
reference: so the reference counts still form no cycles, although
reactions can (`a` waits on `b`, whose callback returns `a`). The records
are freed when the evaluation ends, all at once; a settled record also
drops its reactions at once, so a long evaluation does not keep what it
finished with. A handle read after its evaluation ended faults ("a promise
belongs to the evaluation that made it"), as a closure can carry one out
(fact 7). The writer faults on a promise.

**Unverified:** whether records should also be reference counted by their
handles, so that one nothing holds is freed before the end. The cost is a
count on every copy of a handle; the gain shows only in long evaluations
that make many promises. A measurement decides.

The built-ins: `new Promise(executor)`, `Promise.resolve`, `reject`,
`all`, `allSettled`, `race`, `any`, `withResolvers`, and `then`,
`catch`, `finally`. The resolving functions and `all`'s element functions
are built-in function values that hold a handle and an index. Resolving
with a staticjs promise follows the specification's
`NewPromiseResolveThenableJob` (two ticks more than a value: the spike's
`ret-promise` lands after `p3`). Resolving with any other object that has
a `then` key faults: following a user's `then` is a thenable job staticjs
need not have at first.

### Suspension: an activation moves to the heap

An `await` is in the body of the async function itself, so when it
suspends exactly one frame suspends: the top one. `AWAIT` moves the frame
record and its slots `[base, sp)` into an activation record, without
touching a reference count (a move), removes the frame, and pushes the
function's promise where its return value would go, as `RETURN` does. The
caller goes on, as JavaScript's does. The awaited promise's reaction
owns the activation, alone. A job that resumes it copies the slots back
to the top of the stack, pushes the frame, and either pushes the value or
throws it at the `AWAIT`'s `pc`, where the handler table of part 1 finds a
`catch`. `RETURN` from an async frame resolves its promise, and pushes the
promise only when the frame was entered by a call, not by a job.

An async function called as a loop's callback (`xs.map(async (x) =>
...)`) cannot keep its frame for the whole loop, as a synchronous closure
does (`sjs_vm_call_back`, `src/vm.h`): it may leave mid-element. It is
called as a frame of its own each time, and its result, a promise, is the
element.

### Jobs, host promises and the run

The evaluator gets two queues: jobs (the specification's microtasks:
reactions, resumptions, thenable jobs) and settlements from the host (a
macrotask each). A native defers:

```c
/* In a native: give the program a promise the host settles later. */
uint32_t ticket;
sjs_native_defer(call, &ticket);   /* call->result is now a pending promise */
/* the native keeps (ticket, its arguments) and starts the work */
```

and the host settles, once per ticket, whenever it likes:

```c
sjs_vm_settle(vm, &(sjs_settlement){ .ticket = t, .value = v, .rejected = false });
```

`sjs_vm_run` then works in this order, on fuel throughout:

1. run the frames until the stack is empty;
2. take the next job and run it (back to 1);
3. with no job left: if a rejected promise has no handler, fault
   ("a promise was rejected and nothing handled it"), as Node exits with
   the error (spike: `p.catch` in a later timeout is too late);
4. take the next settlement, settle its promise, and go to 2;
5. with nothing queued: if tickets are pending, return the new status
   **`SJS_RUN_WAITING`**: control is the host's until it settles one; else
   the evaluation is done. Its result is the program's value, or, when
   that is a promise, what it settled to (a rejection is
   `SJS_ERROR_THROWN`). A result promise that can no longer settle faults.

Settlements apply one at a time with a full drain of jobs after each, as
Node runs an I/O callback and then its microtasks. A settlement made while
jobs are queued waits for them. Tickets are numbered in the order natives
deferred, so the same program, given the same settlements in the same
order, does the same thing: a host that records `(ticket, value,
rejected)` in order can replay an evaluation. **Unverified:** whether an
evaluation waiting on the host should also be serializable (activations
and records hold programs by pointer); it would let a tinyactors host
hibernate one, and is out of scope here.

`sjs_vm_reset` abandons a waiting evaluation as it does a paused one; its
pending tickets are the host's to cancel.

### Every evaluation still ends

Fact 5 no longer suffices: a job runs with the function that scheduled it
gone from the stack, so a function can schedule itself through a promise.

```js
const step = (k) => Promise.resolve(k).then((g) => g(g));
step(step)
```

`step` is never running when `g(g)` enters it, so `closure_running`
allows it, and the jobs never end; fuel stops it, but the structural
guarantee is gone. The proposed rule: **a job carries its lineage**, the
functions that were running when its reaction was registered (an `await`,
a `then`), and a function in the lineage cannot be entered in that job.
A lineage is a chain of immutable nodes, shared by the jobs that extend
it; a function appears in a chain at most once, so a chain is no longer
than the program has functions, and a chain of jobs that schedule each
other is as short. Resuming an activation is not entering its function.
Programs that keep working: `Promise.all(xs.map(async (x) => ...))`,
`await f(); await f();` (each `f` finished before the next; the lineage is
the scheduler's, not the resolver's), and `p.then(a).then(b).then(a)`
(all three registered by the same function). **Plausible**: the argument
above needs writing out as a proof in the internals note, and spec cases
that try to break it. The fallback is to say that an async program ends
only by fuel.

### Through the pipeline

- **Lexer:** nothing new.
- **Parser:** `async` before an arrow's parameters (`async x =>`,
  `async (x) =>`, with no line break after `async`, read with the
  lookahead arrows already use; `async(x)` without `=>` stays a call of a
  name staticjs does not bind); `await <operand>` inside an async arrow,
  a syntax error in a module's other functions as now, and still
  unsupported at a module's top level. Events: a flag on the arrow's
  begin event, and `AWAIT` after its operand.
- **Compiler:** `SJS_FUNCTION_ASYNC` in the function's flags; `AWAIT` and
  `RETURN` in an async function; no shape for an async function (its
  result is a promise, which a shape's C path does not make). A read
  before an `await` may still move: the slots move with the activation.
- **Evaluator:** the queues and records above; the activation; the
  lineage check beside `closure_running`; the new status. A code frame's
  promise handle fits in a loop-only field (`index`), so `vm_frame` stays
  112 bytes (`src/vm.h`, its `_Static_assert`).
- **Limits:** `max_promises`, `max_jobs` and `max_tickets` in the
  context's limits, faulting as `SJS_ERROR_LIMIT` as the others do.
- **Writer, inspection:** a promise faults in the writer; inspecting an
  evaluation lists its frames, and an activation waiting is not one
  (a follow-up may list them).
- **staticjs-js:** a JavaScript function given as a native that returns a
  promise is deferred: the package settles the ticket when that promise
  settles and runs slices again. `evaluate` returns a promise already, so
  its users see `await` in staticjs as `await` in JavaScript.

## Lifetimes

| Object | Owner | Made | Read by, when | Ends |
|---|---|---|---|---|
| thrown value | the evaluator, while unwinding | `THROW`, a native's throw, a rejected `AWAIT` | the handler's pattern, at once | moved into the `catch` binding, or into the result past frame 0 |
| completion of a `finally` | two hidden slots of the frame | entering `finally` | `END_FINALLY` | released by it, or by the frame's cut |
| promise record | the evaluator's table | a promise-making call or `async` call | `then`, `await`, the jobs, the host's settle | the evaluation's end; its reactions at settlement |
| promise handle | any value, without a reference | as the record | anyone | never freed; dead after the evaluation |
| activation | the reaction of the promise awaited, alone | `AWAIT` suspending | the job resuming it | moved back to the stack by that job, or freed with the records |
| job | the job queue | a settlement or a resolution | `sjs_vm_run`, once | when it runs, or at reset |
| settlement value | the host until `sjs_vm_settle`, then the evaluator | the host | the job that settles | moved into the record |
| ticket | the host's bookkeeping, a number | `sjs_native_defer` | the host's settle | its settlement, or the evaluation's end |

## Costs

- **Per instruction:** nothing new on the dispatch path. Handlers are
  found only on a throw; `AWAIT` and `THROW` are opcodes of their own in
  `execute_other`.
- **Per call:** one more flag in `fn->flags`, read where `REST` is read.
- **Per frame:** no growth (above).
- **Per evaluator:** the queues and the table, empty until a program first
  makes a promise.
- **Per async step:** a copy of the frame's slots out and back per
  `await`, and a job; a unit of fuel each, plus a unit per frame unwound
  on a throw.
- **Benchmarks:** `vm` and `eval.guard` with `bench-compare` for the
  closure-call change; a new `async.*` group (a chain of `then`s, an
  `await` per element over `Promise.all`) checked against Node.

## Order of work

Each step lands with its spec cases, `diff-node`, and its guide pages.

1. `throw` with any value, uncaught: the parser, `THROW`, the new error
   code and `sjs_vm_take_thrown`. No handlers yet.
2. `try`/`catch`: the handler table and unwinding through code and loop
   frames, a sort's restore, the compiler's move rules, the error kinds.
3. `finally`.
4. A native that throws.
5. Promises without `async`: the records, the job queue, `then` and the
   statics, deferred natives, `sjs_vm_settle` and `SJS_RUN_WAITING`,
   unhandled rejections, the lineage rule.
6. `async` and `await`: activations, loops' callbacks, the job order tests
   against Node (`Promise.race` cases in the spec, a logging native in
   `api_test`).
7. staticjs-js's deferred natives.

## Open questions

- Whether `catch` should ever see a built-in's error, and which built-ins
  first, given that each then owes V8's message.
- Lineage, or fuel only, for async termination.
- Reference-counted promise records, or freed at the evaluation's end
  only.
- Whether an evaluation should be done when its result settles even with
  tickets pending (the host then cancels them), rather than when nothing
  is pending, as Node does.

## Evidence

Node 22.22.0, `node s1.mjs` (JavaScript semantics the design copies):

```
sort after throw: [3,1,2,5,4]
append after throw: ab
callee mutation visible: [1,2]
keys [] {} {} Error: x Error [ 'stack', 'message', 'cause' ]
thenable awaited
order: a1 a2 p1 ret-value a3 p2 p3 ret-promise
```

and a rejection handled only in a later timeout ends the process with the
error, exit status 1.

The source of `s1.mjs` (the `order` line logs two `await`s, a chain of
three `then`s, and an async function that returns a value and one that
returns a promise):

```js
const xs = [3,1,2,5,4];
try { xs.sort((a,b) => { if (a===5||b===5) throw 0; return a-b; }); } catch {}
let s = "ab"; const f = () => { throw 1 };
try { s = s + f(); } catch {}
const xs2 = [1]; const g = (a) => { a.push(2); throw 0; };
const r = (() => { try { return g(xs2); } catch { return xs2; } })();
const e = new Error("x", { cause: 1 });
const log = [];
(async () => { log.push("a1"); await null; log.push("a2"); await null; log.push("a3"); })();
Promise.resolve().then(() => log.push("p1")).then(() => log.push("p2")).then(() => log.push("p3"));
(async () => Promise.resolve(0))().then(() => log.push("ret-promise"));
(async () => 0)().then(() => log.push("ret-value"));
```
