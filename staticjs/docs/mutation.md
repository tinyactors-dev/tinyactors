# Owned mutation

staticjs values used never to change. This document describes the one way
they now can: a function may change, with one of its methods, a value that
a variable of that function holds alone. JavaScript cannot tell that from
its own semantics, so the rule (`../AGENTS.md`) still holds. The invariant
every optimization relies on is unchanged:

> **A value is never changed once it is shared.**

A value nothing else holds is already updated in place today
(`sjs_value_unique`, README.md, "A value nothing else holds is updated in
place"). Mutation is the same idea made visible to the program: a call
such as `xs.push(x)` changes `xs` in place, and staticjs makes sure that no
one else holds `xs` when it does.

This is step 1 of 3:

1. **This step:** methods that change an array or a set, on a variable the
   function owns; such a call as a statement.
2. Assignment to properties and elements (`a.b.c = v`), with the ownership
   of the whole path checked.
3. Removal: `delete`, `splice`, `Set.prototype.delete` and `clear`, with
   removal from the hash index and shrinking.

## The methods

| Receiver | Method | Returns |
|---|---|---|
| array | `push(...items)` | the new length |
| array | `pop()` | the last element, or `undefined` |
| array | `shift()` | the first element, or `undefined` |
| array | `unshift(...items)` | the new length |
| array | `sort(compare)` | the array |
| array | `reverse()` | the array |
| array | `fill(value, start, end)` | the array |
| set | `add(value)` | the set |

Each has JavaScript's semantics and return value. `sort` takes a compare
function and sorts as `toSorted` does, with the same merge sort, fuel and
faults, and the same C path for a comparator of a known shape
(`(a, b) => a - b`, `(a, b) => a.key - b.key`, docs/matcher.md); without one it faults, as `toSorted()` does, since JavaScript
would compare the elements as strings. `Set.prototype.add` appends, as a
set keeps insertion order; it holds primitives only, as `new Set` does.
None of these removes from a hash index: arrays have none, and `add` only
appends. `pop` and `shift` remove from an array, which has no index.

## The rule, at compile time

A call `x.m(...)` of one of the methods above, by name, is a **mutation**.
The compiler accepts it only when the receiver `x` is a variable that the
function making the call owns. Otherwise it refuses the program as
`unsupported`, with a message that says why. Refusing is always allowed;
changing a shared value never is.

1. **The receiver is a name, read directly**: `x.m(`. A receiver that is
   anything else (`f().push(1)`, `[1].push(2)`, `(a || b).push(1)`,
   `x?.push(1)`, `x["push"](1)`, `a.b.push(1)`) is not a mutation: the
   call is compiled as any method call is, and faults when it runs, as it
   did before, on an array or a set.
2. **Declared in this function**: a parameter of the function, or a
   `const` of it (in its body or one of its blocks), a name of a pattern
   included. A name of an enclosing function is a capture (rule 3); an
   import or a global is shared by definition. Both are refused.
3. **Never captured.** If any arrow function captures `x`, before or after
   the mutation, the mutation is refused. A closure keeps a copy of what
   it captures when it is made, so a capture of a value that later
   changes would show a different value from JavaScript's. There is no
   exception; the message suggests building the value with `reduce`, whose
   accumulator the callback holds alone.
4. **Never kept by something else before the mutation.** An earlier read of
   `x` (in source order, which is evaluation order on every path, as every
   jump goes forward) refuses the mutation when its value may be kept:
   - **copied**: bound by a `const` (`const y = x`) or by a default value
     (`const {a = x} = o`), or passed on by `&&`, `||`, `??` or `?:`,
     whose result is the value itself;
   - **passed**: an argument of a call or of `new Set` (`f(x)`);
   - **stored**: an element of an array literal or a property's value in
     an object literal (`[x]`, `{k: x}`).

   Reads that keep nothing are free: a member or a method (`x.length`,
   `x.map(f)`), an index (`x[i]`), a spread (`[...x]`, `f(...x)`),
   destructuring (`const [a] = x`), an operator, a test, and `return x`,
   which ends the function, so no mutation of it follows on that path.
   A read after the last mutation may keep the value: nothing changes it
   after that.

   The value a mutation returns its receiver as (`sort`, `reverse`, `fill`,
   `add`) counts as a read of the variable: `const y = x.sort(f)` copies
   `x`, so a later `x.push(1)` is refused.

The compiler streams, so it decides from the event after a read what that
read does: `MEMBER` is a member, `ARGUMENT` an argument, `BIND` a const,
and so on. Anything it does not list as keeping the value keeps nothing:
an operand that follows, as in `x[i]` or `x === y`, is consumed by an
operator, which keeps nothing either.

## The run-time check

The compiler cannot see every holder: a parameter whose argument the
caller still holds, a global the embedder lends, a module's export, a
value a native keeps, a reduce's initial value that a variable still
holds. So the evaluator checks every mutation when it runs:

- The receiver is not pushed. `RECEIVER slot` pushes a reference to the
  variable's slot (an internal value, which holds nothing), so evaluating
  the call adds no holder. `METHOD_MUTATE` finds the method from the
  value in the slot, and `MUTATE` calls it with the slot itself. `sort`
  is `toSorted`'s loop, which takes over the value moved out of the
  variable, and `SORTED slot` after it puts the sorted array back.
- `MUTATE` faults unless the slot's value has exactly one holder, the slot
  (`sjs_value_unique`). This catches the holders above, and a cycle:
  `xs.push(xs)` gives the argument a second reference, so it faults.
  Values still form no cycles.
- A mutation that changes nothing visible to another holder is the only
  kind there is, so no program can tell staticjs from JavaScript.

## Chains and statements

A method that returns its receiver (`sort`, `reverse`, `fill`, `add`)
passes it on as the same reference to its slot, so `s.add(1).add(2)` and
`xs.sort(f).reverse()` change the same owned variable. The compiler knows
from the name which methods return the receiver and which give something
else (`push` and `unshift` the length, `pop` and `shift` the element), so
`xs.push(1).push(2)` calls `push` on a number, and faults as JavaScript
throws. A chain on a receiver that turns out to be an object, whose
property happens to have such a name, faults when it runs: the property
need not return its object.

A statement may be a mutation, or a chain of them: `xs.push(x);`. Every
other expression statement is refused as before ("an expression
statement has no effect in staticjs"). The parser sends an expression
statement's events when its expression, as a whole, is a call; the
compiler accepts it only when that call is a mutation, and otherwise
refuses it after reading on, as the parser does, so that a syntax error
later in the source is still the program's error.

## What is left out

- **`let` with reassignment** (`let x = 0; x = x + 1;`, `+=`, `++`). The
  parser refuses `let` and every assignment where they begin and reads on
  without sending events; supporting them means new events for a `let`
  declaration and for assignments to a name (and their compound forms),
  automatic semicolon insertion around `let` and `++`, and refusing every
  other assignment target. That is not cheap here, and step 2 needs
  assignment events anyway, so `let` belongs with it. The ownership rules
  extend to it as they are: a `let` captured by any arrow function is
  refused.
- **The other mutating methods** (`splice`, `copyWithin`, `delete`,
  `clear`): step 3. They fault by name, as before.
- **`sort()` without a compare function**: faults, as `toSorted()` does.

## Rolling it back

Everything that exists only for mutation is tagged:

1. **Source:** every hunk of C carries a comment with the word `MUTATION`;
   `git grep -n MUTATION -- staticjs` lists them. A line that serves both
   says what to restore.
2. **Tests:** every spec case is in `tests/spec/mutation.spec`, and every
   `api_test` function is named `test_mutation_*`.
3. **Commits:** every commit body ends with `Mutation: yes`;
   `git log --grep '^Mutation: yes'` lists them.

To remove mutation, revert those commits (newest first), or delete what the
tags mark and restore what they say. The places tagged are listed below.

| File | What is tagged |
|---|---|
| `include/staticjs.h` | `SJS_EV_EXPRESSION_STATEMENT`, and its line in the event grammar |
| `src/shapes.rules` | the `EXPRESSION_STATEMENT` event; `mise run shapes` rewrites `src/shapes_table.h` from it |
| `src/internal.h` | `SJS_INTERNAL_SLOT`; the method IDs from `SJS_METHOD_MUTATION_FIRST`; `sjs_name.mutation`; `sjs_mutation_of`, `sjs_mutation_named`, `sjs_mutation_returns_receiver`; `SJS_OP_RECEIVER`, `SJS_OP_METHOD_MUTATE`, `SJS_OP_MUTATE`, `SJS_OP_SORTED`; `SJS_CALL_CHAIN` |
| `src/parser.c` | `F_EXPRESSION_STATEMENT` sends its events; `frame.refused_before`; `end_expression_statement`, which refuses at the statement's end what is not a call (restore: refuse where it begins) |
| `src/compiler.c` | `binding.use`; `construct.mutate`, `.returns`, `.receiver`; `name_read`, `mutation_site` and the compiler's fields after `unbound_at`; the block from `refuse_later` to `note_read`; the lines of `read_name` that note what it read; `mutation_begin`, `mutation_end` and their lines in `call_begin` and `call_end`; `compile_expression_statement`; `note_use` in `event()`; the refusal in `on_event` |
| `src/vm.h` | `sjs_mutation_call` |
| `src/vm.c` | the message in `method()` (restore the one it names); `execute()`'s default case, which calls `execute_other` (restore the fault alone), and its declaration; the section at the end of the file |
| `src/builtins.c` | `NAME_MUTATION`, the `mutation` column in `NAME`, and the eight names that use it (restore `NAME(text, flags, 0, 0, 0)`); the section at the end of the file |
| `tests/api_test.c` | `EXPRESSION_STATEMENT` in `event_names`; the `test_mutation_*` functions and their calls in `main` |
| `bench/bench.c` | `eval.array.push-reduce`, `eval.array.sort`, `eval.array.sort.rows`, `eval.set.add-reduce` |

Besides the tags, mutation changed what some existing cases expect, and
rolling it back restores them: a mutating method on a literal, or named
by a key built at run time, faults "only on a variable" in
`tests/spec/builtins.spec` (it was "do not change"); and `let` read as a
name in a script's expression statement is refused for the word itself,
"does not have this", in `tests/spec/statements.spec` and
`tests/spec/errors.spec` (it was "expression statement"), as the parser
now refuses an expression statement at its end, after the word. The
README, `AGENTS.md`, `docs/builtins.md` and `docs/matcher.md` point here.

The new code sits at the end of `src/vm.c` and `src/builtins.c`, and the
evaluator reaches it from the default case of its switch, so that the code
before it compiles as it did: `call`, `method`, `sjs_loop_step`,
`sjs_loop_start` and `sjs_builtin_call` are the same instructions as before
mutation. Moving code earlier in those files moved the functions after it
in the binary, which made unchanged loops measure up to 18% slower.
