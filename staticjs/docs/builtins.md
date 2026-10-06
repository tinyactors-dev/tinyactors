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
  `Math.max`, `Object.fromEntries`, the set methods. It gets its arguments
  borrowed in a `sjs_leaf_call` and returns an owned `result` and a `cost`.
  `call()` charges the cost, cuts the stack and pushes the result.
- **A loop** calls a callback: `map`, `filter`, `reduce`, `some`, `every`,
  `find`, `findIndex`, `findLast`, `findLastIndex`, `flatMap`, `toSorted`. It is a frame on the
  evaluator's stack (`FRAME_LOOP`). `sjs_loop_start` sets it up, and
  `sjs_loop_step` either calls the callback once (`LOOP_CALL`) or finishes
  (`LOOP_DONE`). A callback never runs on the C stack. That keeps an
  evaluation resumable on fuel and keeps the C call graph acyclic
  (`mise run check-recursion`).

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

**A method** (`xs.name(…)`, `s.name(…)`, `set.name(…)`):

1. Add an ID to `sjs_builtin_id` in `src/internal.h`, inside its receiver's
   group. `sjs_builtin_call` picks the group by ID range
   (`id <= SJS_METHOD_ARRAY_WITH`, `id <= SJS_METHOD_STRING_TRIM`, then
   sets). A new last member of a group must move that bound, or the call
   lands in the wrong group's switch.
2. Give its name's entry in `NAMES` (`src/builtins.c`) the ID in the
   column of the receiver kind: array, string or set. Every name Node 22 has
   on those prototypes is already listed, so that reading one faults instead
   of giving `undefined`. If the name is missing, add it in byte order;
   `names_test` checks the order and fails on a mistake. The flags say which
   prototypes have the name and whether the method changes its receiver
   (`SJS_NAME_MUTATING`, which faults).
3. If it calls back, add it to `sjs_builtin_loops`.

The compiler marks a key constant with its entry (`SJS_STRING_NAMED`), so
method lookup at run time is an index into `NAMES` plus a column. Never
compare method names as strings at run time: the old `strlen` and `strcmp`
lookup cost 17% of a guard.

**A function** (`Math.x`, `Object.x`, `Number.x`, or a bare global name):
add an ID before `SJS_BUILTIN_FUNCTION_COUNT`, an entry in
`FUNCTION_TABLE`, and an immortal value in `FUNCTIONS`. The compiler
resolves the name when it compiles (`sjs_builtin_lookup`), so the linear
table costs nothing at run time. A function value is immortal: its
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
  array and `sjs_value_unique` holds. Then call `take_receiver(call)`, which
  moves the value out of its stack slot.
- Set `call->cost` for the work actually done. Taking the receiver over
  copies nothing, so the copy's cost goes.
- Never reuse an argument this way. It is borrowed and may be the same
  value as the receiver. Never reuse a value you reached through another
  value, such as an element or a property: its own count says nothing about
  who holds its container.

If the built-in reads its receiver after taking it over, read it through
the taken pointer: `a` may move when the array grows.

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
  value.
- **Refuse mutating methods** (`push`, `sort`, `splice`, `add`, ...) by
  name (`SJS_NAME_MUTATING`). Do not implement them as copies: in
  JavaScript they change the receiver and return something else.

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
x.key` is SUM_KEY. A loop built-in may run such a callback in C, as `map`
does with PLUCK, `toSorted` with COMPARE_BY_KEY and COMPARE_NUMERIC,
`filter`, `find`, `findIndex`, `findLast`, `findLastIndex`, `some` and `every` with
KEY_COMPARE and ELEMENT_COMPARE, and `reduce` with SUM and SUM_KEY
(`pluck_step`, `sort_by_key`, `sort_numbers`, `predicate_step` and
`sum_step` in `src/builtins.c`). The tag only
says what the bytecode computes; the bytecode is still there, and the C
path must give exactly what it gives.

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

## Checklist

- [ ] A leaf unless JavaScript calls back; a loop calls back once per element.
- [ ] ID in its receiver's group, with the group's bound moved if it is last; `NAMES` entry; `sjs_builtin_loops` if it calls back.
- [ ] Arguments borrowed, `call->result` owned and kept current, internal inline helpers only.
- [ ] Output sized up front; a receiver nothing else holds is reused where the method returns a changed copy.
- [ ] Lookups through the index; no hand-written scans; ASCII fast path for text.
- [ ] `cost` charges the work done, in 64 bits; pinned in `api_test` if exact fuel matters.
- [ ] Spec cases written first; `diff-node` agrees.
- [ ] Bench case if it walks data; `bench-compare` rows and machine hash in the commit; `test --full` passes.
- [ ] A C path for a shaped callback: entered where the bytecode would be, every precondition checked each step, stack room, falls back to the bytecode, cases with `@shapes` at each edge, fuel pinned both ways.
