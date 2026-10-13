# Adding a built-in

How to add a built-in function or method to staticjs without making
evaluation slower. Read `../AGENTS.md` first: the rule comes before speed. A
built-in that returns a different value from JavaScript is a bug, however
fast it is.

The built-ins are in `src/builtins.c`. The evaluator calls them from
`call()` in `src/vm.c`. Each guideline below says why it holds: either a
measurement or a property of the language. If a change breaks one, the
commit must show with `bench-compare` that it costs nothing.

## 1. Pick the kind: a leaf or a loop

A built-in is one of two kinds, and the choice decides most of its cost.

- **A leaf** finishes in one C call: `includes`, `slice`, `join`,
  `Math.max`, `Object.fromEntries`, `Object.hasOwn`, a number's `toFixed`, `toExponential`, `toPrecision` and `toString`, `Object.assign` (of a target nothing else holds; of a variable it is a mutation, docs/mutation.md), `Object.freeze`, `Object.seal`, `Object.isFrozen` and `Object.isSealed` (docs/mutation.md, "Frozen and sealed values"), the set methods, a map's `get` and `has`, an array's, a map's or a set's `keys`, `values` and `entries`, an iterator's `take` and `drop` (and its `next` and `return`, on a variable a mutation: docs/mutation.md), `String.raw`,
  `encodeURI`, `encodeURIComponent`, `decodeURI`, `decodeURIComponent`, Temporal's
  functions and its values' methods (`src/temporal.c`), Date's (`src/date.c`), `JSON.parse` and
  `JSON.stringify` (in `src/json.c`, each over an explicit stack). It gets its arguments
  borrowed in a `sjs_leaf_call` and returns an owned `result` and a `cost`.
  `call()` charges the cost, cuts the stack and pushes the result.
- **A loop** calls a callback: `map`, `filter`, `reduce`, `reduceRight`, `some`, `every`,
  `find`, `findIndex`, `findLast`, `findLastIndex`, `flatMap`, `toSorted` (a loop without a compare function too, which calls nothing back and compares the elements' strings: `text_sort_step`, so that `sort()` runs as `sort(f)` does), `Object.groupBy`, `Array.from` (with a mapper), `Map.groupBy`, an array's, a map's
  or a set's `forEach`, an iterator's `map`, `filter`, `reduce`, `some`, `every`,
  `find` and `flatMap` (which run as the array's loops of their names over
  what the iterator yields: `late_start`; `flatMap` with its own step,
  `flatten_iterated`) and its `forEach` (a map's forEach's loop). It is a
  frame on the
  evaluator's stack (`FRAME_LOOP`). `sjs_loop_start` sets it up, and
  `sjs_loop_step` either calls the callback once (`LOOP_CALL`) or finishes
  (`LOOP_DONE`). A callback never runs on the C stack. That keeps an
  evaluation resumable on fuel and keeps the C call graph acyclic
  (`mise run check-recursion`).

- **A leaf that becomes a loop:** the regular expression methods (`test`,
  `exec`, and `search`, `match`, `matchAll`, `split`, `replace` and
  `replaceAll` with a regular expression; `src/regex_match.c`) are leaves
  by ID. A replacement function always makes the loop, which calls it back
  (`LOOP_CALL`) after swapping `LOOP_CALLBACK` and `LOOP_SECOND`. A short match
  runs whole in the leaf, in a buffer of the evaluator's; a long one makes
  the leaf return `SJS_LEAF_LOOP` with `call->loop` set, and `call()`
  enters that loop (`SJS_REGEX_LOOP_FIRST` on) on the same slots, so the
  match pauses like any loop. Both charge the same fuel: the leaf counts
  the steps the loop would take. `sjs_loop_start` tells these loops by one
  comparison; `sjs_loop_step` reaches them from the switch where a loop
  ends, as their index and length stay 0, because a test at its top made
  the predicate loops 5 to 13% slower (`findIndex` -13%, `filter` -12%).

Make it a leaf unless JavaScript calls a function. A closure callback runs
in one frame that the loop keeps from its first call to its end
(`sjs_vm_call_back` in `src/vm.c`): each element writes the arguments into
the parameter slots, runs the body and returns, and the loop clears the
slots before its next step. So an element costs the body and a return, not
a frame per call; that remains the cost in every loop benchmark. It is also
why fusing `filter` into `map` made the pipeline 17–23% slower: fusion
removed the arrays but kept every call.

For a loop built-in:

- **Call back at most once per element,** and only with the arguments
  JavaScript passes. `sjs_vm_call_back` already writes no more than the
  closure's parameters, so a callback written `x => …` costs one argument,
  not three. Keep it that way: build the full argument list, pass it to
  `call_back`, and let the evaluator trim it.
- **Give the callback its own value to update.** If the built-in threads a
  value through the calls, as `reduce` does, move it into the argument and
  take the result back (see `LOOP_OUTPUT` in `vm.h`). Never keep a second
  reference: a count of 1 is what lets `{...acc, k: v}` update in place
  (section 4).
- **Size the output when the loop starts,** as `map` does with `new_array(vm,
  a->length, …)`. `filter` cannot know its size, so it grows from 4.
- **The recursion check is already done once per loop**
  (`sjs_vm_enter_callback`, on the first call). Do not add a check per
  element.

## 2. Register it

**A method** (`xs.name(…)`, `s.name(…)`, `set.name(…)`, `x.toString()` of a
BigInt, `m.name(…)` of a map, `x.toFixed(…)` of a number, `d.with(…)` of a
Temporal value):

1. Add an ID to `sjs_builtin_id` in `src/internal.h`, inside its receiver's
   group. `sjs_builtin_call` picks the group by ID range
   (`id <= SJS_METHOD_ARRAY_WITH`, `id <= SJS_METHOD_STRING_TRIM_START`,
   `id <= SJS_METHOD_SET_UNION`, `SJS_METHOD_BIGINT_TO_STRING`,
   `id <= SJS_METHOD_NUMBER_TO_STRING`,
   `id <= SJS_METHOD_TEMPORAL_WITH_PLAIN_TIME` (`temporal.c`), then maps'
   `map_method`, at the end of the file). A new last member of a group must move that bound, or the call
   lands in the wrong group's switch.
2. Give its name's entry in `NAMES` (`src/builtins.c`) the ID in the
   column of the receiver kind: array, string, set, BigInt, map, iterator, regular expression, number or
   Temporal value (one ID per name for every Temporal type, and a Date's,
   which is a type of the kind: docs/date.md; `temporal.c`'s
   `METHOD_TYPES` says which types have it, and a getter is the entry's
   `field`: docs/temporal.md). Every name Node 22 has
   on those prototypes is already listed, so that reading one faults instead
   of giving `undefined`. If the name is missing, add it in byte order;
   `names_test` checks the order and fails on a mistake. The flags say which
   prototypes have the name and whether the method changes its receiver
   (`SJS_NAME_MUTATING`, which faults, unless the name's `mutation` column
   names a method that changes a variable the function owns:
   `docs/mutation.md`).
3. If it calls back, add it to `sjs_builtin_loops`.

The compiler marks a key constant with its entry (`SJS_STRING_NAMED`), so
method lookup at run time is an index into `NAMES` plus a column. Never
compare method names as strings at run time: the old `strlen` and `strcmp`
lookup cost 17% of a guard.

**A function** (`Math.x`, `Object.x`, `Number.x`, `String.x`, `BigInt.x`, `JSON.x`, `Date.x`, `Temporal.PlainDate.x`, or a bare global name):
add an ID before `SJS_BUILTIN_FUNCTION_COUNT` (below `SJS_METHOD_FIRST`,
128: an assert fails the build when they meet), an entry in
`FUNCTION_TABLE`, and an immortal value in `FUNCTIONS`. The compiler
resolves the name when it compiles (`sjs_builtin_lookup`), so the linear
table costs nothing at run time. A namespace is a row of `spaces` in
`read_name` and of `space_names` in `read_space_member` (`src/compiler.c`);
`String`, `Number` and `BigInt` are ones and functions too: `event()` reads each as
the function when no member follows. A function value is immortal: its
reference count is never written, which is why there is no global mutable
state.

## 3. Arguments, results and references

- **Arguments and the receiver are borrowed.** They live on the evaluator's
  stack. To keep one in the result, take a reference with `sjs_ref`.
- **The result is owned.** If the built-in fails, `call()` releases
  `call->result`. So keep `call->result` pointing at the container you are
  filling, and update it after anything that can move it (`array_add`,
  `set_add` and `sjs_object_put_key` reallocate). `from_entries` does this:
  `call->result.as.heap = &o->h` after every put.
- **Use the internal inline helpers on hot paths:** `sjs_ref`, `sjs_unref`,
  `sjs_num` and `sjs_vm_push`/`pop`. Do not use `sjs_retain`, `sjs_release`
  or `sjs_number`: those are the public API, out of line. Inlining these
  calls was most of why `map` got more than twice as fast in `86c3e3e`.
- **Fail through the evaluator:** `sjs_vm_fault(vm, "message")` for a
  program error, and `sjs_vm_fail(vm, r)` for running out of memory or a
  limit. Never leave a partly built value anywhere except in
  `call->result`.

## 4. Reuse a value nothing else holds

A built-in that returns a changed copy of its receiver (`concat`, `with`,
`toSpliced`, `toSorted`'s output) can change the receiver itself when nothing else holds
it. Values cannot form cycles, and every holder is counted, so a count of 1
means no program can see the difference.

- Check with `takes_receiver(call)`: the slot is known, the value is an
  array and `sjs_value_unique` holds, and it is neither a match result nor
  frozen or sealed (`SJS_INTEGRITY`: the copy JavaScript makes is
  extensible, so a taken-over value would keep bits the result must not
  have). Then call `take_receiver(call)`, which moves the value out of its
  stack slot. A new path that takes a value over without `takes_receiver`
  (a spread, `Array.from`, `toSorted`'s loop) checks the same bits.
- Set `call->cost` for the work actually done. Taking the receiver over
  copies nothing, so the copy's cost goes.
- Never reuse an argument this way. It is borrowed and may be the same
  value as the receiver. Never reuse a value you reached through another
  value, such as an element or a property: its own count says nothing about
  who holds its container.

If the built-in reads its receiver after taking it over, read it through
the taken pointer: `a` may move when the array grows.

A string's `concat` takes its receiver over the same way, when it is not
interned as well (the context's table finds an interned string by its
text), and extends it in place with `sjs_string_reserve` and
`sjs_string_extend`, as `+` does (`STRING_APPEND`).

## 5. No linear searches

- **Look up members** with `sjs_set_contains` and `sjs_object_find`. Past
  `SJS_INDEX_FROM` (8) members, sets and objects carry a hash index that
  keeps insertion order, so a lookup costs O(1). A hand-written loop over
  `items` or `entries` throws that away. At 10,000 members the set
  operations were up to 1,400× slower, and `fromEntries` 180× slower,
  before the index (`71dca6a`).
- **Build with the inserting helpers** (`set_add`, `sjs_object_put_key`,
  `array_add`). They keep the index up to date. Writing `items[...]`
  directly is only right for a fresh array whose size you fixed, as `map`
  does.
- **Compare keys by pointer first.** Most key strings are interned (one per
  text per context), so equal pointers settle most comparisons. Interning
  can be skipped when a bucket is full, so different pointers do not prove
  different keys: fall back to `sjs_str_equal` on the views.
- **For text, use the ASCII flag** (`SJS_STRING_ASCII`). A string's
  UTF-16 view is free when it is ASCII. Otherwise converting a position
  scans the string (`utf16_to_byte`), and doing that per element turns a
  linear method quadratic.

If a built-in's work is quadratic in JavaScript too, say so in a comment,
charge for it in fuel, and give it a Q-sized benchmark case.

## 6. Fuel

Fuel bounds how long an evaluation runs. Each call costs one unit; `cost`
is the work beyond that. A loop built-in pays per element through the
callback's own instructions.

- **Charge the work done, in 64 bits.** One unit per element copied,
  visited or looked up. A lookup through an index is one unit. Charge a
  lookup that scans (a set whose index failed to allocate) for its length:
  see `lookup_cost` in `set_method`. Never charge a product of two lengths
  for work the index makes linear. That made set operations 500× too
  expensive in fuel until `dcca456`.
- **Charge before or as you work,** never less than the work. An
  embedder's budget is only a bound if every built-in's cost is at least
  proportional to its time.
- **An optimization may only lower fuel,** never raise it. When a change
  moves fuel, `bench-compare` prints it (`fuel a->b`). When the exact fuel
  matters, pin it in `api_test`, as `test_fuel_pinned` and
  `test_set_fuel_linear` do.
- **A regular expression's fuel is its automaton's work**
  (`src/regex_match.c`, `rx_charge`): a unit per loop step (256
  instructions visited, finishing the position; the leaf counts the steps
  a loop would take), one per 8 instructions visited and per 16 slots
  copied, and what the result costs to build. Its speedups lower none of
  it: the skips of a pattern led by an ASCII character or by `^` pass
  positions without following them but add what following them counts,
  and stop where a step's budget would, so fuel and pauses do not depend
  on which skips a build has (docs/regex.md, section 15).

## 7. The rule

- **Write the spec cases first** in `tests/spec/builtins.spec` (or the
  file of its topic), including:
  - each argument type that should fault;
  - empty receivers;
  - -0, NaN and non-ASCII text where they apply.

  `mise run diff-node` runs them against Node 22.
- **Fault instead of coercing.** Where JavaScript would call `ToString`,
  `ToNumber` or a prototype method on an object, staticjs faults. A built-in
  that accepted an object and coerced it would be returning a different
  value. A conversion the program asks for by name may read a string:
  `Number(s)`, `parseInt` and `parseFloat` (and `String` of a primitive);
  one JavaScript does on the way to something else (`isNaN(s)`,
  `Math.abs(s)`) faults on a string, through `sjs_vm_to_number`.
  `sort()` and `toSorted()` without a compare function turn primitives
  into strings as `String` does, and fault on an object they would
  compare.
- **Mutating methods** (`push`, `sort`, `splice`, `add`, an iterator's `next`, a Date's `setTime`, ...) change the
  receiver and return something else in JavaScript, so never implement
  them as copies. One that changes a variable the function owns, or a
  container on a path from one, is a mutation (`docs/mutation.md`): called
  through `SJS_OP_MUTATE` with the variable's slot, or `SJS_OP_PATH_CALL`
  with the container's place, never through `sjs_builtin_call`, and tagged
  `MUTATION`. Every other one faults by name (`SJS_NAME_MUTATING`).

## 8. Measure it

1. **Add a bench case** in `bench/bench.c` when the built-in walks data:
   name `eval.<group>.<name>`, unit element, sizes L (linear) or Q
   (quadratic, capped), and the expected value at its smoke size, checked
   against Node (`../AGENTS.md`, Benchmarks).
2. **Run `mise run bench-compare <commit before it> <filter> --quick`**
   while iterating, then without `--quick` before committing. Quote the rows
   with the machine hash in the commit body.
3. **Check what it did to everything else:** `mise run bench-compare
   <commit> eval.guard --quick` at least. A new `case` in a hot switch can
   change code placement. Two builds with byte-identical code for a case
   have measured 15–20% apart (see `94094d0`), so a slowdown in code you did
   not touch is placement until shown otherwise. Show it by comparing the
   objects, or by padding the old build.
4. **Run `mise run test --full`** before the commit. It fails every
   allocation of every spec case, which is where a missed
   `call->result` update shows up as a leak.

## 9. Shapes: a C path for a callback

The compiler tags a function whose body has a known shape
(`docs/matcher.md`): `x => x.key` is PLUCK, `(a, b) => a.key - b.key` is
COMPARE_BY_KEY, `(a, b) => a - b` is COMPARE_NUMERIC, `x => x.key OP c` is
KEY_COMPARE, `x => x OP c` is ELEMENT_COMPARE (OP a strict equality or
an order, `c` a literal), `(a, x) => a + x` is SUM and `(a, x) => a +
x.key` is SUM_KEY, and `(a, x) => { a.push(x.key); return a; }` is
PUSH_KEY (`a.push(x)`: PUSH). A loop built-in may run such a callback in C, as `map`
does with PLUCK (so do `Object.groupBy`, `Array.from` and `Map.groupBy`, and an iterator's `map`, which runs as map), `toSorted` with COMPARE_BY_KEY and COMPARE_NUMERIC (and so `sort`, which is its loop: docs/mutation.md),
`filter`, `find`, `findIndex`, `findLast`, `findLastIndex`, `some` and `every` with
KEY_COMPARE and ELEMENT_COMPARE (and an iterator's `filter`, `find`,
`some` and `every`, which run as those), and `reduce` with SUM and SUM_KEY (an iterator's too, but not `reduceRight`,
which adds from the last element: another order, another floating-point sum),
and with PUSH_KEY and PUSH (an iterator's too)
(`pluck_step`, `sort_by_key`, `sort_numbers`, `predicate_step`,
`sum_step` and `push_step` in `src/builtins.c`). The tag only
says what the bytecode computes; the bytecode is still there, and the C
path must give exactly what it gives. A rule may look at a call, whose
callee is the node before its CALL_BEGIN (`docs/matcher.md`, section 13);
a callback that calls a captured name, such as `x => re.test(x)` or
`x => s.has(x.k)`, has no shape yet (11.9.3).

- **Set it up once per loop.** `shape_start` (from `sjs_loop_start`) notes
  the shape, its key and its flag in the loop frame when the callback is a
  closure tagged with a shape that `shape_runners` pairs with this
  built-in. A new consumer is a row there.
- **Enter the callback where the bytecode would.** At the step that would
  make the first call, call `sjs_vm_enter_callback` first, C path or not.
  A closure already running, a full frame stack or no room for its slots
  then fault at the same place on both paths.
- **Check every precondition, every step,** and call back (`call_element`,
  `call_back`) when one fails. The bytecode then gives the value, or
  faults with its own message, position and subject. Only what the C path
  can prove takes it: an object's own property (no prototype, no `length`
  of an array or a string), a number for arithmetic, a comparison the
  bytecode does not fault on.
- **Share the answer and the operation.** The bytecode's answer and the C
  path's go through one helper (`sort_take`, `predicate_answer`), and the
  C path computes with the evaluator's own operation where one exists
  (`sjs_vm_strict_equal`, `sjs_vm_order`), so the two cannot drift.
- **Room on the stack.** A C step requires as much room above the
  callback's slots as its bytecode pushes (`PLUCK_STACK`,
  `COMPARE_STACK`, `PREDICATE_STACK`, `SUM_STACK`), so it succeeds only where the bytecode would not run
  out of `max_stack`. More only falls back; less is wrong.
- **One unit of work per step,** returning `LOOP_AGAIN`, so a paused
  evaluation still pauses within the loop.
- **Charge the lookups** (one unit each) on top of the step's own unit.
  That is less than the instructions it replaces, which is the point; pin
  both figures in `api_test` (`test_shape_paths`).
- **Test both ways.** Spec cases with `@shapes` for every precondition's
  edge (a missing key, a key on Object.prototype, non-objects, non-numbers,
  NaN, -0, ties), and api_test's limit edges, where the C path and the
  bytecode must fault alike. `spec_test` runs every case with shapes and
  without and compares the outcomes and the fuel.

## 10. Mistakes already made

Each of these happened while adding the built-ins above (`2165a26` to
`0ec9cc9`), and each cost a fix commit, a second test run, or a spot check
to find. Check for them before committing.

- **An expected value written by hand.** Spec cases and bench smoke values
  were wrong more often than the code was: a key order guessed wrong, a
  `toSpliced` bench value that was right only at the smoke size, a
  `lastIndexOf` case. Never type an expected value: run the expression in
  Node and paste what `JSON.stringify` prints, at every size the case uses.
  `tests/spec/group_by.spec` and `array_methods.spec` were made that way.
- **A test that staticjs itself refuses.** `===` on two objects, `+`
  between a string and a number, a set of objects: each faults or is
  unsupported in staticjs, so the case tests nothing about the built-in.
  Know what staticjs refuses before writing a case (README.md, "Where it
  faults instead of following JavaScript"); `"a".k` is `undefined`, not a
  fault.
- **A fault message in shared code that names one built-in.** `call()` and
  `sjs_loop_start` run for every built-in, and both once named
  `Object.groupBy`; adding `Array.from` turned one of them into a ternary
  on the ID (fixed in `0ec9cc9`). A message that names a built-in belongs
  in code only that built-in runs: its own case, branch or function. A
  ternary on the ID in shared code still names one. Before committing, run
  `git diff -U0 | grep sjs_vm_fault` and check each new message.
- **A C path that faults instead of falling back.** `Object.groupBy`'s
  PLUCK path first grouped by any plucked value, so an object or
  `"__proto__"` faulted at the loop's position instead of the callback's,
  and shapes on and off disagreed. When the C path meets a value the
  bytecode would fault on, it must call back and let the bytecode fault
  (section 9). Give every fast-path case an `@shapes` line: without one,
  nothing shows that the C path ran at all, and the bug above passed the
  tests.
- **A needless fault.** `lastIndexOf` first faulted on `undefined`, `null`
  and `true` as its start index, which JavaScript converts without a
  coercion staticjs refuses (`to_integer` already handles them). The rule
  allows a fault, but check with Node what JavaScript does before refusing
  something, and refuse only what it coerces from an object or a string.
- **A deliberate fault without a case.** Where staticjs faults and Node
  gives a value (`Array.from({ length: -1 })` is `[]` in Node, an object as
  the search value of `lastIndexOf` is `-1`), write the case as a fault
  case, so the choice is visible and stays a fault.
- **A fast path claimed but not measured.** A perf commit quoted the new
  build's number alone, which says nothing about the fast path. Commit the
  plain built-in first, then measure the fast path against it (`mise run
  bench-compare HEAD <case> --quick` with the fast path uncommitted) and
  quote that old -> new row and its fuel. `findLastIndex`'s fast path,
  measured afterwards, was +169% at 1,000.
- **A bench case that measures nothing.** `Array.from(rows)` shares an
  array it does not need to copy, so it does no work per element. Make
  sure the case's source does the work the name says.
- **A slowdown blamed on the change, or the change blamed on placement,
  without evidence.** `0ec9cc9` read 9% slower on `findIndex`, from a
  check that runs once per loop. Show which it is: compile `src/` at both
  commits with the bench's flags and compare the per-element functions
  with the pinned `llvm-objdump -d`. Identical instructions mean placement.
- **A test on every step that tips the inliner.** Map's `forEach` first
  had its own step, tested for at the top of `sjs_loop_step`. That one
  branch, with its call, made clang stop inlining `pluck_step` and
  `call_element` into `sjs_loop_step`, and map's pluck and findIndex
  measured 10 to 14% slower; with the two forced inline, the test still
  cost every C path (every, reduce: 11 to 14%). A loop of a new kind takes
  its result through a case of the phase-1 switch and its call after the
  shapes' C paths, where only loops that call back pass; new helpers are
  `noinline` at the end of the file; and check with `llvm-nm` that the set
  of out-of-line helpers is the old one (`array_add`, `call_element` and
  `pluck_step` are now `always_inline` for this).
- **Lists that name only the old built-ins.** `README.md`'s method list,
  sections 1 and 9 of this guide, `docs/matcher.md`'s consumers, and
  comments such as "findLast walks from the end" each had to be updated by
  hand. `git grep` the name of the built-in the new one resembles and
  update what lists it.

## Checklist

- [ ] A leaf unless JavaScript calls back; a loop calls back once per element.
- [ ] ID in its receiver's group, with the group's bound moved if it is last; `NAMES` entry; `sjs_builtin_loops` if it calls back.
- [ ] Arguments borrowed, `call->result` owned and kept current, internal inline helpers only.
- [ ] Output sized up front; a receiver nothing else holds is reused where the method returns a changed copy.
- [ ] Lookups through the index; no hand-written scans; ASCII fast path for text.
- [ ] `cost` charges the work done, in 64 bits; pinned in `api_test` if exact fuel matters.
- [ ] Spec cases written first, every expected value generated by Node, none relying on something staticjs refuses; deliberate faults where Node gives a value are fault cases; `diff-node` agrees.
- [ ] No fault message in shared code names a built-in (`git diff -U0 | grep sjs_vm_fault`), not even through a ternary.
- [ ] Nothing faults that JavaScript converts without coercing an object or a string.
- [ ] Lists and comments that name similar built-ins (`README.md`, this guide, `docs/matcher.md`) name the new one too.
- [ ] Bench case if it walks data; `bench-compare` rows and machine hash in the commit; `test --full` passes.
- [ ] A C path for a shaped callback: entered where the bytecode would be, every precondition checked each step, stack room, falls back to the bytecode for any value the bytecode would fault on, cases with `@shapes` at each edge, fuel pinned both ways.
- [ ] The plain built-in committed first; the fast path's commit quotes `bench-compare` against it (old -> new and fuel), not the new build alone.
- [ ] A slowdown in code the change does not run per element is shown to be placement (identical per-element instructions) or fixed.
