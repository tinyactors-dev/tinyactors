# Owned mutation

staticjs values used never to change. This document describes the one way
they now can: a function may change, with one of its methods or by
assigning to one of its properties or elements, a value that a variable of
that function holds alone. JavaScript cannot tell that from
its own semantics, so the rule (`../AGENTS.md`) still holds. The invariant
every optimization relies on is unchanged:

> **A value is never changed once it is shared.**

A value nothing else holds is already updated in place today
(`sjs_value_unique`, README.md, "A value nothing else holds is updated in
place"). Mutation is the same idea made visible to the program: a call
such as `xs.push(x)` changes `xs` in place, and staticjs makes sure that no
one else holds `xs` when it does.

This is done in 3 steps:

1. **Done:** methods that change an array or a set, on a variable the
   function owns; such a call as a statement.
2. **Done:** assignment to properties and elements (`a.b.c = v`), with
   the ownership of the whole path checked ("Assignment", below); and
   `let`, a variable that statements assign to ("let", below).
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

## Assignment

An assignment is a statement of its own: `o.k = v;`, `o[k] = v;`,
`a.b[i].c = v;`, and the compound forms `+=`, `-=`, `*=`, `/=`, `%=`,
`&&=`, `||=` and `??=`; and `++` and `--`, prefix or postfix, of such a
target, as a statement. Each has JavaScript's semantics and evaluation
order: the target's keys from left to right, a compound assignment's read
of the target, then the value, then the store. `&&=`, `||=` and `??=`
evaluate the value and store only when the target's value says so, and add
no key otherwise. `++` adds to a number only: `x - (-1)`, as `+` would join
a string where JavaScript converts it to a number, so `++` of anything but
a number faults.

**The target** is a path: a variable, then one or more members or
indices. Its root must be a variable the function owns, by the rules of a
mutation above (a parameter or a const of the function; never captured,
before or after; never copied, passed or stored before), and is refused
when compiled otherwise, as `unsupported`, with the reason. So is a target
whose root is not a variable (`f().k = 1`, `[1][0] = 2`, `(a || b).k =
1`), a variable that is not a `let` of the function (`x = 1`, "let"
below), and a method that
changes the root's variable inside the target's keys or the value
(`xs[0] = xs.push(1)`): JavaScript holds the containers on the path from
before the value runs, and staticjs finds them again when it stores. An
assignment anywhere but as a statement (`f(o.k = 1)`, `o.a = o.b = 1`),
a pattern as the target (`[o.a, o.b] = xs`), and `**=` and the bitwise
forms are unsupported; a target that cannot be assigned to (`1++`,
`o?.a = 1`) is a syntax error, as in JavaScript.

**When it runs**, the root is a `RECEIVER`, a reference to its slot, and
each member or index pushes its key, so evaluating the target adds no
holder: a member's as a string, an index's as a number when it is one
(`PATH_INDEX`), which indexes an array without making a string, and a
string otherwise (an object as a key faults, as `TO_KEY` does). `PATH n, mode` then walks from the slot through the `n`
keys: `LOAD` pushes the value there (a compound assignment's, read as any
property is read), `STORE` stores the value above the keys, and `DROP`
drops them when `&&=`, `||=` or `??=` stores nothing. A store faults
("held elsewhere") unless each container on the way has one holder, the
place before it: the slot, then the property or element that holds it.
That catches what the compiler cannot see, as for a mutation (a caller's
argument, a global, a module's export, a value a native holds, an element
of the array a callback walks) and sharing inside the value: after
`const y = o.items`, `o.items[0] = 1` faults, as `y` holds the array too. It
makes no cycle: `o.self = o` and `o.a.b = [o.a]` fault, as the value is a
second holder of a container on the path.

What a store does:

- **An object:** a key it has keeps its place and takes the value, as in
  JavaScript; a new key goes in as a literal's would, an array index among
  the others in order and any other key last. The object's hash index stays
  current: a replaced key's entry does not move, and `sjs_object_put_key`
  indexes a key appended and rebuilds the index when an array index goes in
  before others.
- **An array:** an index inside the array replaces the element; the index
  at its length appends. Past the length JavaScript leaves holes, which a
  staticjs array has none of, so it faults; so does `length`, and any key
  that is not an index (`xs[-1]`, `xs.name`), which JavaScript would add as
  a property.
- **Anything else faults:** a string, a set, a number, a function (which
  JavaScript gives the property, or ignores, or throws on), and `undefined`
  and `null`, on which JavaScript throws. So do `__proto__` and a name of
  `Object.prototype` that the object does not have (`o.toString = f`), as
  reading them does; one it has is replaced.
- On the way, a key the container does not have faults as reading it
  would, or as setting a property of what reading it gives (`undefined`).

The compiler finds the target as it compiles any read, as the parser sends
it as one: a name read begins a path, and a member or an index whose node
starts at the path's root (`sjs_event.start`) goes on with it, its key's
code between them. Any other event whose node starts at the root holds the
path as an operand (`(o.a + 1).k`, `o.f().k`) and ends it. At `ASSIGN`,
the path that ends at the last instruction is the target: its read of the
root becomes the `RECEIVER`, each `GET_MEMBER` a `CONSTANT` of its key and
each `GET_INDEX` a `PATH_INDEX`. The events are

```
t op= v;       <t> ASSIGN(op) <v> ASSIGN_END(op)     (op 0 for =)
t++;  ++t;     <t> UPDATE(ADD)                       (SUBTRACT for --)
```

## let

`let` declares a variable of a function, in its body or one of its blocks
or branches' blocks, as `const` does, with a value or without one
(`let x;`, whose value is `undefined`), a pattern included (which needs a
value, as in JavaScript), several declarators to a statement. Statements
assign to it: `x = v;`, the compound forms, `x++;` and `x--;`, with the
semantics above. At a module's top level `let` is refused, as every
statement there is: a module holds imports, `const` and exports.

- **A `let` captured by any arrow function is refused** when compiled,
  whether or not it is assigned, before or after: a closure keeps a copy of
  what it captures when it is made, and JavaScript's would see what is
  assigned later. The message suggests `const`, or building the value with
  `reduce`.
- **Assigning to a `const`, a parameter**, a global, an import or a name
  bound nowhere is refused as `unsupported`. JavaScript reads it, and
  throws a `TypeError` for a `const` only when the assignment runs, so it is
  not a syntax error; a `const` declared twice, or a `let` and a `const` of
  one name, is.
- **Its value is owned as a `const`'s is**: a method that changes it, or an
  assignment to its properties, follows the rules above. Assigning to the
  variable gives it a new value, so what was done with the old one (copied,
  passed, stored) no longer refuses a mutation of it; the evaluator still
  faults if the new value has another holder (`xs = ys; xs.push(1)` while
  `ys` holds it).
- **The last read of a slot still moves its value** (`TAKE_LOCAL`,
  compiler.c): a write of the slot cancels the move of every read before
  it, so a read before a write stays a copy, and a read after it is a read
  of the new value, which moves when nothing reads or writes the slot
  later, on any path. A plain assignment compiles no read of its target;
  a compound one reads it (`LOCAL`), then `SET_LOCAL` stores the result.

The events are CONST_BEGIN's: count 1 for `let`, 2 for a declarator
without a value, which the parser tells from the token after its name
(`let x;`, `let x, y = 1`, `let x` and a line break):

```
let p = v      CONST_BEGIN(1) <v> <p> CONST_END
let x          CONST_BEGIN(2) BIND(x) CONST_END
x op= v;       <x> ASSIGN(op) <v> ASSIGN_END(op)
```

## What is left out

- **Assigning to a parameter**, and `var`: JavaScript allows both; a
  `let` does what they would. `var` stays refused ("staticjs declares with
  const").
- **The other mutating methods** (`splice`, `copyWithin`, `delete`,
  `clear`): step 3. They fault by name, as before.
- **A method that changes a property or an element** (`o.items.push(x)`):
  the receiver of a mutation is a variable, read directly, and an
  assignment's path is not one. It is compiled as any method call and
  faults when it runs; as a statement it is refused as one with no effect.
  `o.items[o.items.length] = x` appends.
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

Step 1, the methods:

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

Step 2, assignment:

| File | What is tagged |
|---|---|
| `include/staticjs.h` | `SJS_EV_ASSIGN`, `SJS_EV_ASSIGN_END`, `SJS_EV_UPDATE`, and their lines in the event grammar |
| `src/shapes.rules` | the `ASSIGN`, `ASSIGN_END` and `UPDATE` events (`mise run shapes` rewrites `src/shapes_table.h`) |
| `src/internal.h` | `SJS_OP_PATH_INDEX`, `SJS_OP_PATH`, `SJS_PATH_LOAD`, `SJS_PATH_STORE`, `SJS_PATH_DROP` |
| `src/parser.c` | the `ES_*` states of `F_EXPRESSION_STATEMENT`; `OP_STORE` and `OP_UPDATE`; `sjs_parser.statement_ends`, set in `terminator`; their cases in `pop_op`, and `prefix_update`'s declaration before it; `OP_STORE` in `takes_assignment`; the section from `update_value` to `postfix_update`, and the `++`/`--` cases of `operand` and `operator_token` (restore the refusal they name); `assignment_statement`, and its lines in `assignment` (restore the one refusal); the state in `end_expression_statement` |
| `src/compiler.c` | `binding.changed_at`, set in `mutation_begin`; `K_ASSIGN` and `construct.assign`, `.keys`, `.root_pc`; the `path` type, `c->paths` and `c->path_steps` and their free; the paths, from `top_path` to `path_step`; `path_begin` in `read_name` (restore `return SJS_OK`); `paths_before` in `event()`, and `path_step` in its `MEMBER` and `INDEX` cases; `ASSIGN_END` in `note_use`; the assignment section, from `TARGET_TAKEN` to `compile_update`, and its three cases in `event()` |
| `src/vm.c` | `SJS_OP_PATH_INDEX` and `SJS_OP_PATH` in `execute_other`, and `path_op`'s declaration before it; the assignment section at the end of the file |
| `tests/spec_test.c` | `ASSIGN` and `ASSIGN_END` in `role_of` |
| `tests/api_test.c` | `ASSIGN`, `ASSIGN_END` and `UPDATE` in `event_names`; the `test_mutation_assign_*` functions and their calls in `main` |
| `bench/bench.c` | `eval.object.assign-reduce`, `eval.array.assign-reduce`, `eval.array.assign-index` |

Step 2, `let`:

| File | What is tagged |
|---|---|
| `include/staticjs.h` | CONST_BEGIN's count, and its lines in the event grammar |
| `src/parser.c` | `frame.declares_let` and `.bare`; `let_declarator`, its declaration, `begin_let` and `end_bare_let`; the `bare` line of `const_token` and its message; the next declarator's count in `terminator` (restore `send_kind`); `begin_let` in `statement_token` (restore its refusal) |
| `src/compiler.c` | `binding.declared_let`, set in `declare`; `construct.declares_let`, set in `compile_const_begin` with the `UNDEFINED` of a declarator without a value; `refused_let_capture` in `note_capture`; `variable_target`, `variable_store` and `target_store` and their lines in `assign_target`, `assign_begin`, `assign_end` and `compile_update` |
| `bench/bench.c` | `eval.let.counter` |

Besides the tags, mutation changed what some existing cases expect, and
rolling it back restores them: a mutating method on a literal, or named
by a key built at run time, faults "only on a variable" in
`tests/spec/builtins.spec` (it was "do not change"); and `let` read as a
name in a script's expression statement is refused for the word itself,
"does not have this", in `tests/spec/statements.spec` and
`tests/spec/errors.spec` (it was "expression statement"), as the parser
now refuses an expression statement at its end, after the word. Step 2
changed two refusals' messages, which their comments restore: an
assignment that is not a statement is "staticjs has no assignment as a
value" (it was "staticjs has no assignment: values do not change"), and
`++` or `--` that is not one "staticjs has no ++ or -- as a value" (it was
"staticjs has no ++ or --: values do not change"); `1++` and `++(a + b)`
are now syntax errors, as in JavaScript (they were unsupported). `let`
moved two cases from `tests/spec/statements.spec` to `mutation.spec`
("let" and "let with its name on the next line", which were refused,
"declares with const"). The README, `AGENTS.md`, `docs/builtins.md` and `docs/matcher.md` point here.

The new code sits at the end of `src/vm.c` and `src/builtins.c`, and the
evaluator reaches it from the default case of its switch, so that the code
before it compiles as it did: `call`, `method`, `sjs_loop_step`,
`sjs_loop_start` and `sjs_builtin_call` are the same instructions as before
mutation. Moving code earlier in those files moved the functions after it
in the binary, which made unchanged loops measure up to 18% slower.
