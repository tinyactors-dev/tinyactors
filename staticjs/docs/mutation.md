# Owned mutation

staticjs values used never to change. This document describes the one way
they now can: a function may change, with one of its methods, by
assigning to one of its properties or elements or by deleting one of its
properties, a value that a variable of that function holds alone.
JavaScript cannot tell that from its own semantics, so the rule (`../AGENTS.md`) still holds. The invariant
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
3. **Done:** removal: `delete` ("delete", below), `splice`,
   `Set.prototype.delete` and `clear`, with removal from the hash index and
   shrinking ("Removal", below).

After the three steps, the methods above called on a path from such a
variable (`o.items.push(x)`, `s.byID[k].tags.add(t)`), which the shape
rules recognise and the compiler rewrites into the path machinery of step
2 ("Methods through a path", below); appending to a `let` (`s += x`),
whose value moves to the `+`, which extends a string nothing else holds
("Appending to a let", below); a map's `set`, `delete` and `clear`,
on a variable or a path, by the same rules (README.md, "Maps");
`Object.assign(x, ...sources)` of such a variable ("Object.assign",
below); an iterator's `next` and `return` on such a variable
("Iterators", below); a Date's `setTime` and UTC setters on such a
variable (docs/date.md, "Setters"); and `Object.freeze(x)` and
`Object.seal(x)` of such a variable, with every mutation above faulting on
a frozen or sealed value where JavaScript throws ("Frozen and sealed
values", below).

## The methods

| Receiver | Method | Returns |
|---|---|---|
| array | `push(...items)` | the new length |
| array | `pop()` | the last element, or `undefined` |
| array | `shift()` | the first element, or `undefined` |
| array | `unshift(...items)` | the new length |
| array | `sort(compare)`, `sort()` | the array |
| array | `reverse()` | the array |
| array | `fill(value, start, end)` | the array |
| array | `splice(start, deleteCount, ...items)` | the elements removed, a new array |
| set | `add(value)` | the set |
| set | `delete(value)` | whether the set had it |
| set | `clear()` | `undefined` |
| map | `set(key, value)` | the map |
| map | `delete(key)` | whether the map had it |
| map | `clear()` | `undefined` |
| iterator | `next()` | a new `{ value, done }` |
| iterator helper's result | `return()` | a new `{ value: undefined, done: true }` |
| Date | `setTime(t)`, `setUTCFullYear(y, m, d)`, `setUTCMonth(m, d)`, `setUTCDate(d)`, `setUTCHours(h, mi, s, ms)`, `setUTCMinutes(mi, s, ms)`, `setUTCSeconds(s, ms)`, `setUTCMilliseconds(ms)` | the new time value, a number |

Each has JavaScript's semantics and return value. `sort` sorts as
`toSorted` does, with the same merge sort, fuel and faults, and the same
C path for a comparator of a known shape (`(a, b) => a - b`,
`(a, b) => a.key - b.key`, docs/matcher.md); without one (or with
`undefined`) it compares the elements' strings, as `toSorted()` does
(`text_sort_start` in `src/builtins.c`), and faults before it changes
anything when two elements or more are not `undefined` and one is not a
primitive, whose `toString` JavaScript would call. `splice` takes its arguments as
`toSpliced` does, with the same code (`splice_arguments`): `start` and
`deleteCount` relative and clamped, everything from `start` removed when
`deleteCount` is left out, nothing when both are; it removes and inserts
in place, and returns what it removed. `Set.prototype.add` appends, as a
set keeps insertion order; it holds primitives only, as `new Set` does.
`delete` removes a member, found under SameValueZero as `has` finds it (NaN
is NaN, -0 is 0), from the set's hash index too ("Removal", below); an
object is never a member, so `delete` of one gives `false`. `clear`
removes them all. Arrays have no index: `pop` and `shift` remove from an
array without one. A map's `set` puts a key it has in its place with the
new value and a new key last, as `new Map` does; its key is any primitive
under SameValueZero (`-0` goes in as `0`), and an object faults, as
JavaScript keys one by its identity. Its `delete` and `clear` remove as a
set's do, the key's value with it; `delete` of an object gives `false`.
`delete` and `clear` name a set's method and a map's: the name's
`mutation` column holds the set's, which returns alike, and
`sjs_mutation_of` gives the map's for a map. An array's, a map's or a
set's iterator (`keys`, `values`, `entries`, and a helper's result from
one) holds the array, the map or the set, so a mutation of it faults
("held elsewhere") while one lives (`const it = xs.values(); xs.push(1)`);
after the iterator's last read, which moves it, the mutation runs. So does
`forEach`'s callback's, as the loop holds the array, the map or the set
(`xs.forEach((x, i, a) => a.push(x))`). A Date's setters write its time
value in place, as V8 computes it (docs/date.md, "Setters"); the mutation
is the Date type's alone of the Temporal kind (`sjs_mutation_of` takes the
receiver, and gives none for another Temporal value), and a setter called
as any method is faults (`SJS_METHOD_TEMPORAL_DATE_SETTER`).

## The rule, at compile time

A call `x.m(...)` of one of the methods above, by name, is a **mutation**.
The compiler accepts it only when the receiver `x` is a variable that the
function making the call owns. Otherwise it refuses the program as
`unsupported`, with a message that says why. Refusing is always allowed;
changing a shared value never is. A name that a method of another kind
shares, one that changes nothing (`add`, a Temporal value's), is the
exception: the compiler refuses none of its calls but a statement, and
leaves the rest to the run-time check ("Shared names", below).

1. **The receiver is a name, read directly**: `x.m(`, or a path from one,
   `x.a[k].m(` ("Methods through a path", below). A receiver that is
   anything else (`f().push(1)`, `[1].push(2)`, `(a || b).push(1)`,
   `x?.push(1)`, `x["push"](1)`, `f().a.push(1)`) is not a mutation: the
   call is compiled as any method call is, and faults when it runs, as it
   did before, on an array or a set.
2. **Declared in this function**: a parameter of the function, or a
   `const` or a `let` of it (in its body or one of its blocks), a name of a
   pattern included. A name of an enclosing function is a capture (rule 3); an
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
   - **copied**: bound by a `const` or a `let` (`const y = x`), assigned
     (`o.k = x`, `y = x`), or bound by a default value (`const {a = x} =
     o`), or passed on by `&&`, `||`, `??` or `?:`, whose result is the
     value itself;
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
- A receiver of another kind is called as any method call calls it: an
  object's own function found by the name (`{sort: f => 5}`), whose result
  is the call's, with the variable left as it was. `MUTATE` then skips the
  `SORTED` after it, which has no sorted array to put back: it once stored
  the property's result in the variable, which gave `o.sort(f)` the value
  `5` for `o` and leaked the object. So is a method of the name that
  changes nothing, a Temporal value's `add`, which makes a new value
  (docs/temporal.md): `MUTATE` puts the variable's value in place of the
  reference, as an ordinary call's receiver. Its result is a value, which
  a chain goes on with as an ordinary call's receiver (below). A path call
  faults on a sort of one ("Methods through a path"), where the
  container's place, not a slot, would take it back; a variable's
  ordinary call costs only the skip, so it follows JavaScript.
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
throws. A link that changed nothing (a Temporal value's `add`, an
object's own function whose property has such a name) gives a value, not
the reference: `METHOD_MUTATE` and `MUTATE` call the next link on it as an
ordinary method call, so `d.add(a).add(b)` adds twice, each to a new
value, and `o.add(1).add(2)` calls `add` on what `o.add(1)` returned, as
JavaScript does; a set or an array there faults ("changes its
receiver"), as a mutating method does on any value it is not called on
through a variable. A mutation's method ID with such a value as its
receiver is never compiled, and faults ("the program is damaged").

A statement may be a mutation, or a chain of them: `xs.push(x);`. Every
other expression statement is refused as before ("an expression
statement has no effect in staticjs"). The parser sends an expression
statement's events when its expression, as a whole, is a call; the
compiler accepts it only when that call is a mutation, and otherwise
refuses it after reading on, as the parser does, so that a syntax error
later in the source is still the program's error. A shared name's call
that compiled as an ordinary one because its receiver is not the
function's to change (`s.add(x);` on a capture, a global or an import:
"Shared names", below) is refused with that reason ("a function captured
this one: build the value with reduce instead"), not as one with no
effect.

## Shared names

`add` is a set's method, which changes the set, and a Temporal value's,
which makes a new value (docs/temporal.md). The compiler sees names, not
values, so the rules above would refuse common Temporal code
(`xs.map(n => start.add({ days: n }))`, a capture; `const e = d;
d.add(x)`, a copy; `[d.add(a), d.add(b)]`, a store). Those rules only
report early what the evaluator already guarantees: `MUTATE` changes a
value only when its slot is its one holder ("held elsewhere"), an
ordinary call of a set's `add` faults in `method()`, `MUTATE` calls a
method that changes nothing before it checks, and every jump goes
forward, so a closure made after a change sees the final value, and one
made before holds the value, which then fails the check. So for a name
whose method that changes its receiver has a namesake of another kind
that changes nothing (`sjs_mutation_shared`; today only `add`), the
compiler refuses nothing and the run decides:

- **A variable of the function** is the call's receiver (`RECEIVER`)
  whatever was done with it before (copied, passed, stored, captured), and
  the call does not count as changing it, so a capture after it is not
  refused. A Temporal value's `add` makes a new value; a set's checks that
  the slot is its one holder, and faults "held elsewhere" while a copy, an
  element, an argument or a closure still holds it.
- **A capture, a global or an import** is an ordinary method call's
  receiver: a Temporal value's `add` works, and a set's faults ("changes
  its receiver").
- **A path** (`o.d.add(x)`) whose root the path rules would refuse falls
  back to an ordinary call the same way. A chain through a path
  (`o.d.add(a).add(b)`) still faults ("on an array, a set or a map"), as a
  path call's chain needs the container back: write
  `const e = o.d.add(a); e.add(b)`.
- **As a statement**, a call whose receiver is not the function's to
  change is refused, as before, with the reason (above).

Neither compiled form can give a value other than JavaScript's, for
either kind: a set the compiler used to refuse faults when it runs
instead. Arrays, maps, iterators (whose `next` and `return` on a value
nothing else holds are the same method, not a namesake) and every other
name keep the rules above; so do a Date's setters, whose names no other
kind has. A kind per `const`, inferred from its
initializer (`new Set`, `Temporal.PlainDate.from`), could bring back the
early refusal for a set the compiler can see, but not replace the run's
check: a callback's parameter, a reduce's accumulator, a property and a
global have no kind the compiler knows.

## Assignment

An assignment is a statement of its own: `o.k = v;`, `o[k] = v;`,
`a.b[i].c = v;`, and the compound forms `+=`, `-=`, `*=`, `/=`, `%=`,
`**=`, `&&=`, `||=` and `??=`; and `++` and `--`, prefix or postfix, of such a
target, as a statement. Each has JavaScript's semantics and evaluation
order: the target's keys from left to right, a compound assignment's read
of the target, then the value, then the store. `&&=`, `||=` and `??=`
evaluate the value and store only when the target's value says so, and add
no key otherwise. `++` adds to a number only: `x - (-1)`, as `+` would join
a string where JavaScript converts it to a number, so `++` of anything but
a number faults.

**The target** is a path: a variable, then one or more members or
indices. Its root must be a variable the function owns, by the rules of a
mutation above (a parameter, a const or a let of the function; never captured,
before or after; never copied, passed or stored before), and is refused
when compiled otherwise, as `unsupported`, with the reason. So is a target
whose root is not a variable (`f().k = 1`, `[1][0] = 2`, `(a || b).k =
1`), a variable that is not a `let` of the function (`x = 1`, "let"
below), and a method that
changes the root's variable inside the target's keys or the value
(`xs[0] = xs.push(1)`): JavaScript holds the containers on the path from
before the value runs, and staticjs finds them again when it stores. An
assignment anywhere but as a statement (`f(o.k = 1)`, `o.a = o.b = 1`),
a pattern as the target (`[o.a, o.b] = xs`), and the bitwise
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
  An append's read of its `let` moves instead ("Appending to a let").
- **The value's last read of its own `let` moves** (`store_slot`): in
  `it = it.map(f)`, `xs = xs.concat(ys)` or `xs = f(xs, xs)`, the store
  follows the value on every path, with nothing between that reads the
  slot, so the last read of the old value in it moves, and a value the
  `let` held alone stays alone: `it` is an iterator nothing else holds,
  which a helper reads (JavaScript's would yield the values once). Only
  the last read moves (`f(xs, xs)` copies the first, and
  `xs = [xs, xs]; xs[0].push(1)` faults "held elsewhere", where
  JavaScript changes the one array both elements are); a read before the
  statement stays a copy, as a path that skips the store (an `if`
  without it) may read the slot again; and for `&&=`, `||=` and `??=`
  only a read in the value moves, as the operator may skip the value and
  the store. A `let` is never captured, so nothing else reads the slot
  meanwhile. The cases are in `mutation.spec`, "A let's reassignment from
  its own value".

The events are CONST_BEGIN's: count 1 for `let`, 2 for a declarator
without a value, which the parser tells from the token after its name
(`let x;`, `let x, y = 1`, `let x` and a line break):

```
let p = v      CONST_BEGIN(1) <v> <p> CONST_END
let x          CONST_BEGIN(2) BIND(x) CONST_END
x op= v;       <x> ASSIGN(op) <v> ASSIGN_END(op)
```

## Last reads in branches

The last read of a slot moves its value: a read that no read, capture or
write of the slot follows on any path. A read in one branch of an `if`
or of a conditional `c ? a : b` is followed by nothing in the other
branch, which does not run after it, so the compiler takes the last read
in each branch as the last of its path:

- **At the end of a branch** another branch follows (`ELSE`, the `:` of a
  conditional), the last read of each slot in it is parked
  (`end_branch`, `c->parked_reads`), fenced from the reads of the
  branches that follow: they do not cancel it.
- **When the whole `if` or conditional ends**, the fence goes: a read
  after it cancels the parked reads as it cancels any read before it, so
  each stays a copy; when nothing follows, the end of the slot's block or
  function moves each of them, as it moves any last read. One of them
  runs.
- **A branch that ends in a return** moves its last reads at its end:
  nothing after the return runs on its path. Reads before the branch,
  the test's included, are untouched.
- **`&&`, `||`, `??` and `?.`** need nothing of this: the left operand
  runs on every path, so a read there is followed by the right operand's,
  and a read in the right operand is the last only when nothing follows
  the expression, as before. A read the operator may skip is never moved
  before a read that follows.

So `cond ? it.drop(1) : it` reads an iterator nothing else holds in
either branch, and `if (b) ys = xs; else ys = xs.concat(zs)` gives `ys`
the array alone; a read of `xs` after the `if` keeps both copies, and a
mutation of `ys` then faults ("held elsewhere"), where JavaScript changes
the one array both names. A capture of the variable, after the construct
or in another branch, cancels the parked reads, as it cancels a read. A
module's exports are reads at its end, so a parked read of an exported
`const` never moves. The cases are in `mutation.spec`, "Last reads in
branches".

## Appending to a let

Building a string is common: `let s = ""; s += x;`, `s = s + x`,
`` s = `${s}...` ``. Each such statement used to copy the whole string, as
the read of `s` on the right is a copy (the slot still holds the value), so
`+` never saw a string nothing else holds. These forms, on a `let` the
function owns, now move the variable's value to the `+` instead:

- `s += e`;
- `s = s + e1 + e2 + ...`, `+` left-associated with `s` the leftmost
  operand;
- `` s = `${s}...` ``, a template whose head is empty and whose first
  substitution starts with `s`.

**The move.** The read of `s` that starts the value (for `+=`, the
target's own read) becomes a `TAKE_LOCAL`, which leaves the slot empty;
the store at the statement's end puts the result back. Between the two,
only the right-hand side runs, and the compiler moves the value only when
that side reads `s` no other time (`binding.reads`): `s += s`,
`s = s + "," + s` and `s += s.length` keep their copy, as such a read, or
a method of `s`'s own, would find the slot empty. A `let` captured by a
closure is refused, so nothing else can read the slot meanwhile. The store
follows the whole value on every path, so the move is never seen; a fault
ends the evaluation, as always.

**The append.** `+` and the template's join then find, when `s` holds a
string, a string with one holder, and extend it in place, with spare
room growing geometrically (`STRING_APPEND`, README.md, "`+` and a template
extend a string nothing else holds"): building is amortized linear. That
part is not mutation, and serves `reduce((acc, x) => acc + x, "")` as
well. A string another holder keeps (`const t = s`, an object's key, a
set's member, an array's element, a program's constant, an interned
string) is copied, as before, and the copy becomes the variable's,
extended in place from the next statement on.

**The semantics are `+`'s, operator by operator.** Nothing else changes:
each `+` of `s + e1 + e2` runs in turn, with its own types. On a string it
joins, as String converts the right operand; on two numbers it adds
(`s + 1 + 2` on a number adds 3, on a string appends "1" then "2"); a
string and a number join either way round; anything else faults, as
`+` faults where JavaScript would coerce (an object, a boolean onto a
number). A template's parts convert with String's rules, and an object in
one faults. So the append is only a fast path for a string on the left.

**The rules.** `s += e` and the spine of `+` are rewrites of the shape
rules (`src/shapes.rules`, `APPEND` and `APPEND_SUM`; docs/matcher.md,
section 12): the anchored lane reports APPEND at the end of an assignment
whose operator is `+=` and whose target is a name, and APPEND_SUM at each
`+` of a spine from a name. The compiler takes APPEND for its open
assignment, and APPEND_SUM when the spine's outermost `+` is the last
instruction of a plain assignment's value and its root is the first read
of that value, of the same `let`. The template is a bracket over no node
before it, which the lane does not anchor (docs/matcher.md, 12.7): the
compiler finds it in its own code, a `CONSTANT ""` and a `LOCAL s` at the
value's start and a `CONCAT` at its end. Then, in each case, it checks
`binding.reads` and changes the read's opcode, at `ASSIGN_END`, before the
operator and the store are emitted.

## delete

`delete o.k;`, `delete o[k];` and `delete a.b[i].c;` are statements of
their own that remove a property of an object. The target is a path, as an
assignment's is ("Assignment", above): its root a variable the function
owns, by the same rules, checked when compiled and again when it runs, and
each container on the path held by its place alone. When it runs, the keys
are evaluated left to right, and the last container, which must be an
object, loses the property: the keys after it move down, and a key added
again goes in as a new one ("Removal", below). A key the object does not
have is fine, as in JavaScript, where `delete` gives `true`; `__proto__`
and a name of `Object.prototype` the object does not have fault, as reading
them does.

- **An array:** `delete xs[i]` leaves a hole in JavaScript, which a
  staticjs array has none of, so it faults, whatever the key (`length`
  too, which JavaScript cannot delete); `splice` removes elements. So does
  a string, a set, a number or a function on the path or at its end, and
  `undefined` or `null`, on which JavaScript throws.
- **A variable:** `delete x` is a syntax error in strict code, a module,
  even in parentheses, as in JavaScript; in a script, which is sloppy, it
  is unsupported (JavaScript gives `false`).
- **As a value** (`const r = delete o.k`, `return delete o.k`, `delete o.k
  || 1;`, an arrow's body): unsupported, as an assignment is.
- **Any other operand** (`delete f().k`, `delete o.f()`, `delete o?.k`,
  `delete 1`, `delete [1][0]`): unsupported; JavaScript evaluates it and
  gives `true`.

The parser sends `delete` as a prefix operator of a statement, and DELETE
when the statement ends with its operand; the compiler finds the target as
an assignment's, and `PATH n, DELETE` removes the property:

```
delete t;      <t> DELETE
```

## Methods through a path

A method of the table above may be called on a path from a variable the
function owns: `o.items.push(x)`, `state.byID[k].tags.add(t)`,
`acc.byKey[x.k].push(x)` in reduce, `a.b.sort(f).reverse()`. The receiver
is a name, then one member or index or more, then the method, by name:
`ROOT(.k | [k])+ .m(...)`, and a chain of them. Each has JavaScript's
semantics and evaluation order, and returns what the method returns on a
variable:

- **Order:** the path's keys from left to right, then the arguments, then
  the call. JavaScript reads the method from the container before the
  arguments; staticjs finds the container when the call runs, after them,
  which gives the same result, as nothing in the arguments may change the
  root's variable (below). So a missing step faults after the arguments
  run, where JavaScript throws before: both are errors.
- **Return values:** as on a variable: `push` and `unshift` the length,
  `pop` and `shift` the element, `splice` the elements removed, `delete`
  whether the set or the map had it, `clear` `undefined`; `sort`,
  `reverse`, `fill`, `add` and a map's `set` the container, a copy of it,
  which then holds it too, so a later mutation of the container faults
  while the copy lives.
- **Chains:** a method that returns its receiver goes on with the same
  container: `o.a.sort(f).reverse()` sorts and reverses `o.a`, and
  `o.s.add(1).add(2)` adds to `o.s`. Any other method's result is a value:
  `o.xs.push(1).push(2)` calls `push` on a number, and faults as
  JavaScript throws.
- **Statements:** such a call, or a chain of them, may be a statement, as a
  mutation of a variable may.

**Refused when compiled**, as `unsupported`, with step 1's messages: a root
that is a global, an import or a capture, or a variable captured (before
or after), copied, passed or stored before the call (for a shared name,
an ordinary call instead, refused only as a statement: "Shared names").
What the call's own
arguments do with the root's value is the evaluator's to check, as for a
mutation of a variable: the value is then held twice when the call runs.
Also refused: a method that changes the root's variable in the path's keys
or in the arguments (`o.a.push(o.b.pop())`, `xs[0].push(xs.shift())`), as
the path is walked after them ("staticjs does not change a variable with
one of its methods while it calls a method on a path from it"); and, as
for an assignment, a path call in the value of an assignment to its root
(`o.n = o.a.push(1)`).

**Faults when it runs:** a container on the path, the root's value
included, or the receiver, held elsewhere ("held elsewhere": another
variable, a caller's argument, a value a native holds, an element of the
array a callback walks); a cycle (`o.items.push(o)`, `o.items.push(o.items)`),
as the argument is a second holder; a missing step (`o.missing.push(1)`,
"cannot read a property of undefined", where JavaScript throws a
TypeError); a step that is not an object or an array ("staticjs calls a
method on a path through objects and arrays only"); `__proto__` and a name
of `Object.prototype` the object does not have, as an assignment's path
faults on them. A receiver of a kind without the method is called as any
method is: a string's or a number's faults by name, and an object's own
function, found by the name, is called as an ordinary method call calls
it. `sort` and a chain on one fault ("on an array or a set only"), as they
need the container back. A Date's setters are not in `MUTATE_PATH`, nor are
an iterator's `next` and `return`: `o.d.setUTCDate(1)` is an ordinary
call, which faults ("setters change it"), and as a statement it is refused
("has no effect"); keep the Date in a variable of its own to set it.

**The rewrite.** The parser sends such a call as any method call on a path,
and the compiler compiles it so, holding the path from its CALL_BEGIN to
its CALL_END. The shape rules (`src/shapes.rules`, MUTATE_PATH) recognise
the call's form, and the matcher reports it during its CALL_END, with
shapes on and off (docs/matcher.md, section 12). The compiler then checks
the root's ownership and rewrites what it compiled, as it does an
assignment's target: the root's read becomes a RECEIVER, each member's
GET_MEMBER a CONSTANT of its key and each index's GET_INDEX a PATH_INDEX,
and the method's METHOD_MEMBER a PATH_METHOD, which pushes the method's
name and room for the container. After the arguments, `PATH_CALL n, argc,
flags` walks the `n` keys from the slot, each container held by its place
alone, and calls the method with the container's place as its receiver's
slot, changing it in place; for `sort`, toSorted's loop takes the array
over from its place, and `PATH_SORTED n` puts it back. With CHAIN (set
when the next call goes on with the container), the reference and the keys
stay on the stack for the next PATH_CALL. Each step costs a unit of fuel,
besides the instructions and the method's own cost.

```
o.a[k].push(x)    RECEIVER o, CONSTANT "a", <k> PATH_INDEX, PATH_METHOD "push", <x>, PATH_CALL 2, 1
o.a.sort(f)       RECEIVER o, CONSTANT "a", PATH_METHOD "sort", <f>, PATH_CALL 1, 1, PATH_SORTED 1
```

## Object.assign

`Object.assign(target, ...sources)` puts each source's own enumerable
properties into its target and returns the target itself. It is a function,
not a method, so its target is its first argument:

- **A variable the function owns** (`Object.assign(o, a, b)`, the whole
  first argument a name): a mutation of it, by the rules above. The
  compiler refuses a global, an import, a capture (before or after) and a
  variable copied, passed or stored before, with step 1's messages; the read
  of the variable becomes a RECEIVER, which holds nothing, and the call an
  `OBJECT_ASSIGN` after the sources, which puts them into the slot's object
  in place. Such a call may be a statement. It gives a copy of the
  variable's value, as a method that returns its receiver does, so
  `const r = Object.assign(o, x)` copies `o`, and a later mutation of `o` is
  refused. A reduce's accumulator is one: `(acc, x) => Object.assign(acc,
  x)` changes the accumulator the callback holds alone.
- **Any other target** (`Object.assign({}, a, b)`, `Object.assign(f(), a)`,
  `Object.assign(o.inner, a)`, or the function called through a value): an
  ordinary call, which needs a target that nothing else holds
  (`sjs_value_unique`), and gives a copy of it with the sources put. Nothing
  else can see that target, so no program can tell the copy from the
  target changed. That part is not mutation: it changes nothing a program
  can see. A target something else holds faults ("something else holds
  this one"). As a statement it is refused, as any call that changes no
  variable is.

**Semantics**, as Node's: the sources in order, a later one's key replacing
an earlier one's value in its place and a new key going in as a put does
(array indices first, ascending, then the others in insertion order, as
JavaScript orders an object's keys); `null` and `undefined` sources are
skipped, and a number, a boolean, a BigInt, a set, a map, an iterator, a
function and a regular expression put nothing (none has own enumerable
properties); an array puts its indices, not its length; a string its
characters by UTF-16 index. Values are shared, not copied: after the call
the target and the source hold the same nested values, which are then held
twice, so changing one in place faults ("held elsewhere"), while a new
value assigned to the target's key leaves the source as it was.

**Faults when it runs**, where Node gives a value: a primitive target
(JavaScript makes an object of it), even with nothing to put; with
something to put, a target that is not an object (an array, a map, a set, a
function: staticjs puts properties only into an object), a variable's value
held elsewhere too (`Object.assign(o, o)`, `Object.assign(o, {self: o})`,
a parameter whose argument the caller still holds), and so a nested call
whose target is the copy an inner call on a variable gives
(`Object.assign(Object.assign(o, a), b)`: `o` still holds the value); a match result as a
source, whose index, input and groups a put would need
(`sjs_array_extra_fault`); and a string source with a character outside the
BMP, whose halves a staticjs string cannot hold. With nothing to put
(no sources, or only ones that put nothing), the target is given as it is,
whatever holds it. No object holds the key `__proto__`, whose setter
JavaScript's assign would call (`object_spread` in `src/vm.c` says why).

Fuel: a unit per property put, and for an ordinary call a unit per key of
the target it copies.

## Iterators

An iterator (README.md, "Iterators") yields its values once. `next()`
takes the next one and advances the iterator; a spread, a helper or any
other reader of it afterwards gets the rest, as in JavaScript. So `next` is
a mutation of the iterator, by the rules above, of the kind `SJS_ITERATOR`:

- **On a variable the function owns** (`const it = m.values(); it.next()`),
  checked when compiled and again when it runs, it gives a new object
  `{ value, done }`, its keys in that order: the value the iterator yields
  next (an entry's a new `[key, value]` array, as a spread makes it, an
  array's `[index, element]`) and
  `false`, or `undefined` and `true` once the iterator has yielded
  everything, as often as it is called. Its argument is not read. The
  iterator keeps a position (`sjs_iterator.position`), the count of values
  taken: `sjs_iterator_count` and `iterator_range` (`src/builtins.c`), which
  every reader goes through, start there, and the writer writes the rest.
  An iterator still holds its array, its map or its set when it has yielded
  everything, so a mutation of that faults ("held elsewhere") while the
  iterator lives.
- **`return()`** is an iterator helper's (`take`, `drop`, `map`, `filter`,
  `flatMap`, marked `SJS_ITERATE_HELPER`): on such a variable it moves the
  position to the end and gives `{ value: undefined, done: true }`; its
  argument is not read, as JavaScript's helper gives `undefined`. An
  array's, a map's, a set's and `matchAll`'s iterators have no `return` in
  JavaScript, which
  throws a TypeError calling it; staticjs faults ("no return method"). Read
  as a value, either name faults ("does not read them"), where JavaScript
  gives a function or `undefined`.
- **On an iterator nothing else holds** (`m.keys().next()`, `new
  Set([1]).values().take(1).return()`, or a variable's last read, which
  moves its value): an ordinary method call (`SJS_METHOD_ITERATOR_NEXT`,
  `_RETURN`) gives the same result, without the move, which no program can
  see, as nothing reads that iterator again. That part is not mutation, but
  exists only with it.
- **Anything else faults when it runs**, where JavaScript gives a value: an
  iterator held elsewhere too (another variable, an array's element, a
  callback's element, a path from a variable that is read again:
  `o.it.next()`), as that holder would see it advance ("nothing else
  holds"). A method through a path (`o.it.next()`) is an ordinary call:
  `next` and `return` are not in the shape rules' `MUTATE_PATH`. So is a
  computed name (`it["next"]()`), which faults the same way on a variable
  read again. An iterator
  read before the call (`[...it]; it.next()`) faults as a second read does
  ("yields its values once").
- **Refused when compiled**, as for any mutation: an iterator variable
  captured (before or after), copied, passed or stored before `next`.
  `next` and `return` are names of mutations now, so an object's own
  method of either name follows the same rules: on a variable the function
  owns it is called as any method is (`const o = { next: f }; o.next()`),
  and on a captured one it is refused, as `o.add()` and `o.set()` are.

Fuel: a unit for the call, and what `iterator_range` charges for the value
it copies, a unit (an entry's new array included).

## Removal

Removing an item from a set or an object, and shrinking a container that
has become mostly empty, keep three properties: JavaScript's order, lookups
in O(1), and memory that comes back.

**Compaction, not deletion markers.** A removal moves the items after the
one removed down by one (`sjs_object_remove`, `sjs_set_remove`, value.c;
a map's entry, its key and its value, by `sjs_map_remove`, the set's
`keyed_remove` with a stride of two),
so a container's items stay dense and in insertion order. A key deleted and
added again goes in as a new key: at the end, or among the array indices in
order, as JavaScript puts it. The hash index loses the removed item's slot
by backward-shift deletion: linear probing finds an item by walking from
its home slot to the first empty one, so each later item of its cluster
that the hole would hide moves into it, and the hole moves on. No slot is
ever a marker, so a lookup probes exactly as it did before removal existed,
and a table never fills with markers that only a rebuild would clear. Then
every position after the removed one goes down by one.

The other choice, marking removed entries and compacting later, would make
removal O(1), but every reader of a container's items (every built-in,
spread, pattern, writer, the API's `sjs_object_at`) would have to skip the
marks, on the immutable paths too, or each would have to compact first.
Compaction keeps them all unchanged; it costs a removal O(n) in the
container's length, but for the last item, which costs O(1): a removal
moves the items after it, and renumbers the index once.

**Fuel** follows the lookup rule (`lookup_cost`, `dcca456`): finding the
item costs one unit through the index or in a container under
`SJS_INDEX_FROM` items, and its length when its index failed to allocate
(`SJS_CONTAINER_UNINDEXED`) and the lookup scans it. Removing any item but
the last costs the container's length more, for the items that move down
and the positions renumbered.

**Shrinking**, with hysteresis, so that adding and removing around an edge
does not reallocate each time:

- **Items:** a container grows by doubling when full; once its capacity
  is at least 16 (`SJS_SHRINK_FROM`) and its items fill under a quarter of
  it, it is reallocated with room for twice its items. This applies to an
  array after `pop`, `shift` and `splice`, and to a set and an object after
  a removal (`sjs_array_shrink`, `object_shrink`).
- **The index:** it is built a quarter full and rebuilt larger at half
  load; after a removal it is rebuilt a quarter full once it is at most an
  eighth full, and dropped when the container falls under half of
  `SJS_INDEX_FROM`, as a small container is scanned. Such a container gets
  an index again as it grows past `SJS_INDEX_FROM`, even if one failed to
  allocate before: `SJS_CONTAINER_UNINDEXED` is cleared with the index.
- **Out of memory:** a smaller block is allocated before the larger one is
  freed; when it cannot be, the container keeps the larger one, which is
  still correct. Shrinking never faults.

## Frozen and sealed values

`Object.freeze` and `Object.seal` have JavaScript's semantics, enforced.
Each sets a bit on an object or an array (`SJS_INTEGRITY_FROZEN`,
`SJS_INTEGRITY_SEALED` in its `h.flags`; freezing sets both). The bit is
the value's, so every copy of the value, which is the same value (a
variable, an element, a property, a closure's capture), is frozen with it,
as every reference to a JavaScript object is. staticjs code is module code,
strict, where a write JavaScript refuses throws a TypeError; there,
staticjs faults. (`tests/diff-node.mjs` evaluates an expression case
sloppily, with `new Function`, where an assignment to a frozen object is
ignored instead; it runs no fault case, so the cases' values were generated
in strict mode, and a fault is still the answer: the rule allows a fault
where JavaScript gives a value, never another value.)

**What faults**, checked where the change happens, after the checks of
ownership ("held elsewhere") and of the arguments, with JavaScript's
conditions, which V8 follows:

| Change | Frozen | Sealed |
|---|---|---|
| assign a key or an element it has (`o.a = v`, `xs[0] = v`, `o.a += 1`, `o.a++`, and `??=`, `\|\|=`, `&&=` when they store) | faults | works |
| assign a key it lacks, append an element (`xs[xs.length] = v`) | faults | faults |
| `delete` of a key it has; of one it lacks | faults; works | faults; works |
| `push(...)`, `unshift(...)` with arguments | faults | faults |
| `push()`, `unshift()` with none (they set the length) | faults | works |
| `pop()`, `shift()` of an array with elements; of an empty one | faults; faults | faults; works |
| `splice` that removes as many as it inserts (`splice()`, `splice(0, 1, x)`); any other | faults; faults | works; faults |
| `sort(f)`, `reverse()` of two elements or more; of fewer | faults; works | works |
| `fill(v, start, end)` of a range with elements; of an empty range | faults; works | works |
| `Object.assign(x, ...)` with something to put; with nothing | faults; works | works when every key it puts is one `x` has; works |

A method that works on a frozen or sealed array (`sort` of one element, a
sealed array's `sort`) keeps its bits: `sort`'s loop takes the array over
as it is (`toSorted`'s does not: below). Freezing is shallow: a path through
a frozen object to one that is not changes the inner one (`o.inner.x = 1`,
`o.items.push(x)`), and a set, a map or a Date held by a frozen object
changes as JavaScript's does, since a frozen object only holds them. An
`Object.assign` that is not a mutation (`Object.assign(Object.seal({a: 1}),
{a: 2})`) follows the same table, and its result keeps the target's bits.

**Freezing and sealing** change the value, so they follow the rules of a
mutation, with a value already frozen (or sealed, to seal it) left as it is,
whatever holds it, as JavaScript changes nothing then:

- **`Object.freeze(x)` of a variable the function owns** (the whole first
  argument a name) compiles as `Object.assign` of one does ("Object.assign"):
  the read becomes a RECEIVER, and `OBJECT_ASSIGN` tells freeze and seal
  from assign by its callee when it runs (`assign_variable`). As freezing a
  frozen value changes nothing, the compiler claims such a variable whatever
  was done with it and refuses nothing, as for a shared name ("Shared
  names"): the run faults ("something else holds this one") unless the
  slot holds the value alone or it is frozen already. It gives a copy of the
  variable's value, as Object.assign does, and may be a statement:
  `Object.freeze(o);`. On a capture, a global or an import it is an ordinary
  call, and as a statement refused with the reason.
- **Any other argument** (`Object.freeze({...})`, `Object.freeze(f())`) is
  an ordinary call (`object_integrity`, `sjs_integrity_set`), which sets the
  bits of a value nothing else holds and gives it back, and faults on one
  something else holds (an element of an array a callback walks, a module's
  export) unless it is already so.
- **A primitive** is given back, and is frozen and sealed. **A set, a map, a
  Date, a function, an iterator, a regular expression and a Temporal
  value** fault (deliberate): JavaScript freezes their properties, of which
  staticjs gives them none, while their internal state stays changeable
  (`Object.freeze(new Set([1])).add(2)` works in Node). Matching that would
  need a bit on each of those kinds for `isFrozen` alone; faulting is the
  smaller rule, and `isFrozen` and `isSealed` of each is false, as Node
  gives for one that was never frozen.

**`Object.isFrozen` and `Object.isSealed`** read the bits, and give Node's
answer: true for a primitive; for an object or an array, its bits, where a
sealed one with no key or element is frozen too (JavaScript's rule for an
object; for an array V8 says so as well, though its length stays writable,
so `push()` still works on it, and freezing it sets the bit); false for
every other value. A module's namespace is sealed, as JavaScript's is
(`sjs_vm_take_result` sets the bit on a module's value), so it is frozen
only when it has no export, and freezing one with exports faults, where
JavaScript throws.

**Copies are new values.** A copy JavaScript makes new, unfrozen and
extensible, must not keep the bits, and an optimization that takes over a
value nothing else holds would make it the same value. Each takeover
checks them, and copies a frozen or sealed value instead:

| Path | Guard |
|---|---|
| `concat`, `with`, `toSpliced` (`takes_receiver`) | `SJS_INTEGRITY` with `SJS_ARRAY_EXTRA` |
| `toSorted`'s loop, which `sort` shares | takes a frozen or sealed array over only for `sort` (the callee in the loop's slot 1) |
| `[...xs, ...]` (`SJS_OP_ARRAY_SPREAD`) | `SJS_INTEGRITY` with `SJS_ARRAY_EXTRA` |
| `{...o, ...}` (`object_spread`), and so reduce's accumulator `{...acc, k: v}` | `SJS_INTEGRITY` |
| `Array.from(xs)`, which shared `xs` (`from_list`) | `SJS_INTEGRITY` with `SJS_ARRAY_EXTRA` |
| `Object.assign`'s ordinary call, which copies its target | the copy takes the target's bits, as it is the target |

Every other way to make an array or an object builds a new one, whose bits
are clear (`builtins.spec`, "copies JavaScript makes new are not frozen").
The embedder's `sjs_array_append` and `sjs_object_put` refuse a frozen or
sealed value (`SJS_ERR_INVALID`).

**Fuel:** freezing, sealing and asking cost the call's unit; an
`Object.assign` into a sealed target a unit per key it looks up.

## What is left out

- **Assigning to a parameter**, and `var`: JavaScript allows both; a
  `let` does what they would. `var` stays refused ("staticjs declares with
  const").
- **`copyWithin`**, the one other method that changes an array: it faults
  by name, as before.
- **A method through any other receiver**: a path through an optional
  chain (`o?.a.push(1)`), a method named by a computed key
  (`o.a["push"](1)`), a path from a call or an operator (`f().a.push(1)`,
  `(a || b).k.push(1)`). Each is compiled as any method call and faults
  when it runs; as a statement it is refused as one with no effect.
- **A method that changes the root inside a path call's keys or
  arguments** (`o.a.push(o.b.pop())`): refused, even where the two paths
  part, as telling them apart needs the keys' values.
- **`Object.isFrozen(x)` and `Object.isSealed(x)` before a mutation of
  `x`**: they pass `x`, as any argument does, so the mutation is refused
  when compiled ("passed"); ask after it, or freeze a value of its own.
- **Appending, other forms**: `s = e + s` (a prepend), `s += e` where `e`
  reads `s`, `s = s + e` of a `const` or a parameter (not assignable), a
  property (`o.s += e`, whose value its object also holds), keep the
  copy. The value's last read of its `let` moves in any assignment
  ("let"), so `s = e + s` gives `+` the old string alone, but `+` appends
  only to its left operand; and a compound assignment's own read of its
  target moves only in `s += e` where `e` does not read `s`.

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
| `src/compiler.c`, the value's last read of its `let` and last reads in branches | the `slot_reads` entry of `c->last_reads` (restore a `uint32_t`, the pc), and the lines that say so in `reserve_reads`, `open_function_scope` and `close_function_scope`, `slot_entry` (restore `last_read` alone); `parked_read`, `scope.parked_base`, `c->parked_reads` and its free; `construct.branch_pc`, `.parked_base`, `.parked_mark`, set in `compile_if`, `compile_else` and the `CONDITIONAL` case of `event()`; `end_parked`, `fence_parked`, `store_slot` and `end_branch`, and their lines in `read_slot`, `forget_read`, `settle_reads`, `variable_store` (restore `write_slot`), `compile_else`, `compile_if_end` and the `CONDITIONAL_ELSE` and `CONDITIONAL_END` cases of `event()` |
| `bench/bench.c` | `eval.let.counter` |

Step 3, removal from the index and shrinking:

| File | What is tagged |
|---|---|
| `src/internal.h` | the index's comment on removal; `sjs_removal`, `sjs_object_remove`, `sjs_set_remove`, `sjs_set_clear`, `sjs_array_shrink` |
| `src/value.c` | the removal section at the end of the file |
| `src/builtins.c` | `array_remove`'s shrink (restore `array_remove(id, call)`, without it) |
| `tests/api_test.c` | the `test_mutation_shrink_*` functions (`live_of` and `allocations_of`, before them, stay: STRING_APPEND's tests use them) |

Step 3, `Set.prototype.delete` and `clear`:

| File | What is tagged |
|---|---|
| `src/internal.h` | `SJS_METHOD_SET_CLEAR`, `SJS_METHOD_SET_DELETE` |
| `src/builtins.c` | the names `clear` and `delete` (restore `NAME(text, flags, 0, 0, 0)`); `mutation_kind`, used by `sjs_mutation_of` (restore the test of `SJS_METHOD_SET_ADD`); `set_remove` and its case in `sjs_mutation_call` |
| `tests/api_test.c` | `test_mutation_set_removal`, `test_mutation_set_removal_out_of_memory` |
| `bench/bench.c` | `eval.set.delete-reduce`, `eval.set.delete-reduce.last` |

Step 3, `splice`:

| File | What is tagged |
|---|---|
| `src/internal.h` | `SJS_METHOD_ARRAY_SPLICE` |
| `src/builtins.c` | the name `splice` (restore `NAME(text, flags, 0, 0, 0)`); `array_splice` and its case in `sjs_mutation_call` |
| `tests/api_test.c` | `test_mutation_splice_fuel`, `test_mutation_splice_out_of_memory`, and the splice in `test_mutation_shrink_array` |
| `bench/bench.c` | `eval.array.splice`, `eval.array.splice-reduce` |

Step 3, `delete`:

| File | What is tagged |
|---|---|
| `include/staticjs.h` | `SJS_EV_DELETE`, and its line in the event grammar |
| `src/shapes.rules` | the `DELETE` event (`mise run shapes` rewrites `src/shapes_table.h`) |
| `src/internal.h` | `SJS_PATH_DELETE`, and its words in `SJS_OP_PATH`'s comment |
| `src/parser.c` | `ES_DELETE`; `OP_DELETE` and its case in `pop_op`, and `delete_end`'s declaration; `sjs_parser.last_name`, set in `send`; the section from `delete_value` to `delete_end`; `delete_word` in `unsupported_word` (restore `"delete"` to its starters) |
| `src/compiler.c` | `DELETE` in `paths_before`; `compile_delete`, and its case in `event()` |
| `src/vm.c` | `path_into.mode` and `path_walk.mode` (restore `bool store`), and the delete messages in `path_container`; `SJS_PATH_DELETE` in `path_op` and `path_delete` before it |
| `tests/api_test.c` | `DELETE` in `event_names`; the `test_mutation_delete_*` functions |
| `bench/bench.c` | `eval.object.delete-reduce`, `eval.object.delete-reduce.last` |

Appending to a let:

| File | What is tagged |
|---|---|
| `src/shapes.rules` | `op: operator` on the `ASSIGN` event (restore it without); `APPEND`, `APPEND_SUM` and `summed` (`mise run shapes` rewrites `src/shapes.h` and `src/shapes_table.h`) |
| `src/compiler.c` | the header's appends; `binding.reads`, counted in `read_name`; `construct.append` and `.reads`, and `k.node` and `k.reads` in `assign_begin`; `c->sum` and its start in `sjs_compiler_init`; `append_take` to `append_move`, and its line in `assign_end`; the two lines of `on_rewrite` |
| `tests/matcher_test.c` | the appends' reports; `APPEND_SUM` in the reports of `a + ...` (restore: none) |
| `tests/api_test.c` | `test_mutation_append_memory` |
| `bench/bench.c` | `eval.string.append-let` |

`STRING_APPEND`, which the appends rely on to be linear, is not mutation
and stays (below).

Methods through a path:

| File | What is tagged |
|---|---|
| `src/shapes.rules` | the rewrites' section: `MUTATE_PATH` and its nonterminals and rules (`mise run shapes` rewrites `src/shapes.h` and `src/shapes_table.h`) |
| `mise/shapes.rb` | rewrites, `_` and `...`, the anchored rules' checks and tables (docs/matcher.md, section 12); `mise/tasks/shapes-test` and `mise/shapes-test/small.*`, their checks and the small rewrite |
| `src/matcher.h` | `sjs_rewrite_match`, the reporter's `rewrite`, `sjs_matcher_config.rewrites_only`, and the lane's fields |
| `src/matcher.c` | `matcher_lane_reserve`, `matcher_free_lane`, the anchored lane's section, and their lines in `sjs_matcher_init`, `sjs_matcher_event` and `sjs_matcher_free` |
| `src/internal.h` | `SJS_OP_PATH_METHOD`, `SJS_OP_PATH_CALL`, `SJS_OP_PATH_SORTED`, `SJS_PATH_CALL` |
| `src/compiler.c` | the header's rewrites; `construct.node`, `.member`, `.held`, `.root_use`, `.mutate_path`; `mutation_site.keys`, `.flags_at`; `path.held`; `c->call_holds_path`; `path_callee` and its lines in `paths_before`; the chain's `keys` in `mutation_begin`; `refused_path_changing` to `path_mutation_end`, and their lines in `call_begin` and `call_end`; `on_rewrite`; the matcher made with `no_shapes` too (restore: none, and `matching`) |
| `src/vm.c` | the message in `method()` (restore it without ", or on a path from one"); `path_call_site`, the three cases in `execute_other`; the method's message in `path_container`; the section at the end of the file |
| `tests/api_test.c` | `test_mutation_path_*` |
| `tests/matcher_test.c` | the rewrites in the tee and `test_reports`, the lane's checks, deepest kinds and allocation |
| `bench/bench.c` | `eval.object.push-path-reduce` |

A map's `set`, `delete` and `clear`:

| File | What is tagged |
|---|---|
| `src/internal.h` | `SJS_METHOD_MAP_SET`, `SJS_METHOD_MAP_DELETE`, `SJS_METHOD_MAP_CLEAR`; `sjs_map_remove` |
| `src/value.c` | `sjs_map_remove` (`keyed_remove`, the set's removal with a stride, stays with the removal section) |
| `src/builtins.c` | the name `set` (restore `NAME("set", SJS_NAME_ON_MAP \| SJS_NAME_MUTATING, 0, 0, 0)`); `mutation_kind`'s first line, `sjs_mutation_of`'s two lines for a map and `SJS_METHOD_MAP_SET` in `sjs_mutation_returns_receiver`; `map_change`, its declaration and its case in `sjs_mutation_call`, the section at the end of the file |
| `src/vm.c` | ", a set or a map" in the message of paths (restore "or a set"), and in the chains' message that shared names removed (restore it with "or a set") |
| `src/shapes.rules` | `chaining() <- MEMBER{key: "set"}` (`mise run shapes` rewrites `src/shapes_table.h`) |
| `tests/api_test.c` | `test_mutation_map_removal`, `test_mutation_map_removal_out_of_memory` |
| `bench/bench.c` | `eval.map.set-reduce`, `CHECK_MAP_N` |

An iterator's `next` and `return`:

| File | What is tagged |
|---|---|
| `src/internal.h` | `sjs_iterator.position`; `SJS_ITERATE_HELPER`; `SJS_METHOD_ITERATOR_NEXT`, `SJS_METHOD_ITERATOR_RETURN`, `SJS_METHOD_ITERATOR_ADVANCE`, `SJS_METHOD_ITERATOR_CLOSE` |
| `src/value.c` | the position in `sjs_iterator_make` and `sjs_iterator_count` |
| `src/builtins.c` | the names `next` and `return` (restore `NAME(text, SJS_NAME_ON_ITERATOR, 0, 0, 0)`); `mutation_kind`'s first line; `iterator_step`'s declaration and its cases in `sjs_mutation_call` and `iterator_method`; the position in `iterator_range`; `SJS_ITERATE_HELPER` in `late_done` and `iterator_limit`; the section at the end of the file |
| `src/writer.c` | `written_from` and its uses, and the position in `iterated` |

Object.assign of a variable:

| File | What is tagged |
|---|---|
| `src/internal.h` | `SJS_OP_OBJECT_ASSIGN` |
| `src/compiler.c` | `construct.assigns` and `.target_pc`, and `AT_*`; `claim_receiver`, split out of `mutation_begin` (restore its body there); `assign_callee` to `object_assign_end`, and their lines in `call_begin`, `call_end` and `event()` |
| `src/vm.c` | `assign_variable`, and its case in `execute_other` |

Without them, `Object.assign` of a variable is an ordinary call, which
faults when it has something to put, as the variable holds the target too.
`Object.assign` itself (`assign_call` in `src/builtins.c`), the puts at the
end of `src/vm.c` (`sjs_vm_object_assign`, `sjs_assign_puts`) and
`sjs_object_copy` are not mutation, and stay; so do its cases in
`tests/spec/builtins.spec`.

A Date's `setTime` and UTC setters:

| File | What is tagged |
|---|---|
| `src/internal.h` | `SJS_METHOD_DATE_SET_TIME` to `SJS_METHOD_DATE_SET_UTC_MILLISECONDS`; `sjs_mutation_of`'s receiver (restore `uint8_t kind`) |
| `src/builtins.c` | `NAME_DATE_SETTER` and the eight names that use it (restore `NAME_TEMPORAL(text, SJS_METHOD_TEMPORAL_DATE_SETTER, 0)`); the local setters' `SJS_METHOD_TEMPORAL_DATE_LOCAL` (restore `SETTER`); `mutation_kind`'s first line; `sjs_mutation_of`'s receiver and its test of a Date; their case in `sjs_mutation_call` |
| `src/vm.c` | the receiver passed to `sjs_mutation_of` in `mutation_method` and `path_call` (restore `.kind`) |
| `src/vm.h` | `sjs_date_set` |
| `src/date.c` | `SETTER`'s and `LOCAL`'s messages (restore the ones they name); the section at the end of the file |

Shared names:

| File | What is tagged |
|---|---|
| `src/internal.h` | `sjs_mutation_shared` |
| `src/builtins.c` | `sjs_mutation_shared` |
| `src/compiler.c` | `construct.fallback`; `fallback_site` and `c->last_fallback`; `refused_copied`, `refused_passed`, `refused_stored`, `refused_global`, `refused_import`, named once (restore the strings in place), and `refused_path_changing`, moved before them (restore it before `path_mutation_target`), `FB_*`, `fallback_reason` and `fallback_of`; `claim_receiver`'s `fallback` (restore: refuse every receiver it refuses with `fallback` NULL, and set `USE_MUTATED`); its lines in `mutation_begin` and `path_mutation_target`; the lines in `call_end` and `compile_expression_statement` |
| `src/vm.c` | the value receiver in `mutation_method` and `mutate` (restore the chain's fault, "goes on only through an array, a set or a map") |

Frozen and sealed values:

| File | What is tagged |
|---|---|
| `src/compiler.c` | `AT_FREEZING`; `assign_callee`'s freeze and seal (restore a `bool` of Object.assign's constant); `assign_argument`'s `freezing` and its fallback |
| `src/vm.c` | `assign_variable`'s freeze and seal, and its `sjs_assign_allowed`; the sort guards in `mutate` and `path_call`; `sjs_integrity_allows` in `path_put_element`, `path_put` and `path_delete` |
| `src/vm.h` | nothing: the section's functions stay (below) |
| `src/builtins.c` | `sjs_integrity_allows` in `array_insert`, `array_remove`, `array_fill`, `array_splice` and `reverse`; `sort`'s callee in `toSorted`'s takeover (restore its test without the bits) |

Pushes into reduce's accumulator, tagged PUSH_KEY and PUSH (docs/matcher.md,
section 13):

| File | What is tagged |
|---|---|
| `src/shapes.rules` | `PUSH_KEY`, `PUSH`, `push_of` and their rules (`mise run shapes` rewrites `src/shapes.h` and `src/shapes_table.h`) |
| `tests/matcher_test.c` | their reports in `test_reports` |
| `src/builtins.c` | `PUSH_STACK`; the two rows of `shape_runners`; the two cases of `shape_start`; `push_step`, and its line in `sjs_loop_step` (restore `sum_step` alone) |
| `tests/api_test.c` | `test_mutation_push_paths` |

Not mutation, and kept on a rollback: rules on a call, which take its
callee as their first child (`matcher_open`, the generator's check, the
`SELF` shape of `mise/shapes-test/small.rules`), and shapes with no
arguments. `matcher_test`'s deepest pushes and its push in
`test_out_of_memory` test the first with PUSH's rules: without them,
give those checks a rule on another call.

Not mutation, and kept on a rollback: `Object.freeze`, `Object.isFrozen`,
`Object.seal` and `Object.isSealed` themselves (`object_integrity`,
`sjs_integrity_set` at the end of `src/builtins.c`), the bits
(`SJS_INTEGRITY_*` in `src/internal.h`), the guards of the takeovers
(`takes_receiver`, `from_list`, `SJS_OP_ARRAY_SPREAD`, `object_spread`),
the namespace's bit in `sjs_vm_take_result`, the API's refusal in
`src/value.c`, and the section "Frozen and sealed values" at the end of
`src/vm.c` (`sjs_integrity_allows`, `sjs_assign_allowed`), which
`Object.assign`'s ordinary call uses. Without mutation nothing changes a
frozen value but that call, and `Object.freeze(x)` of a variable is an
ordinary call, which faults while the variable holds the value too. Their
cases are in `tests/spec/builtins.spec`; the enforcement's in
`mutation.spec`, "Frozen and sealed values", which rolling back deletes.

The rest of maps (values, `new Map`, `get`, `has`, `size`, iteration,
`forEach`, `Map.groupBy`) is not mutation, and stays.

`splice_arguments` in `src/builtins.c` is `toSpliced`'s argument code,
moved into a function that both call, inlined; rolling back keeps it, or
puts its body back in `toSpliced`'s case.

Nothing on the immutable path removes an item or shrinks a container:
values never change there, and a value nothing else holds is only ever
appended to. Compaction leaves the index and every reader of a
container's items as they were (lookups probe the same slots, with no
deletion markers to skip), so rolling mutation back removes these with the
rest; keeping them would keep code nothing calls.

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
are now syntax errors, as in JavaScript (they were unsupported). Step 3
changed one case of `mutation.spec` that names a method staticjs lacks: it
called `splice`, and now calls `copyWithin`. Methods through a path changed
`mutation.spec`'s "a method on a container held by another variable", which
faulted "only on a variable" and now faults "held elsewhere"; the message a
mutating method of the wrong receiver faults with, which now ends ", or on
a path from one"; and `no_shapes`, which made no matcher and now makes one
that matches the rewrites alone. Before step 3, `delete` was
refused as a word staticjs does not have ("does not have this"); no case
tested it. `let` moved two cases from `tests/spec/statements.spec` to `mutation.spec`
("let" and "let with its name on the next line", which were refused,
"declares with const"). An iterator's `next` moved `map.spec`'s "next is
not staticjs's" and "next of a set's iterator is not staticjs's", which
faulted ("does not have this method"), to `mutation.spec`, where they give
a value ("next of an iterator nothing else holds"); rolling back restores
them. Shared names turned `mutation.spec`'s deliberate Temporal cases (a
chain, a copy, a capture) into values, and its chains through an object's
own function, which faulted ("goes on only through an array, a set or a
map"), into a value or the next receiver's own fault ("format digits",
"changes its receiver"); rolling back restores them. A Date's setters
replaced `date.spec`'s "setTime, deliberate" and the other UTC setters'
cases, which faulted ("setters change it"), with its section "The
setters", whose cases rolling back deletes, restoring those; and its local
setters' cases, which faulted "setters change it" too, now fault "reads
local time", as their names are `SJS_METHOD_TEMPORAL_DATE_LOCAL` now
(restore `SETTER` and the cases; `LOCAL`'s message gained "or the setUTC
methods"). The README, `AGENTS.md`, `docs/builtins.md` and
`docs/matcher.md` point here.

**Not mutation: `STRING_APPEND`.** `+`, a template and a string's `concat`
extend, in place, a string that nothing else holds (README.md, "`+` and a template extend a
string nothing else holds"). It is an optimization of immutable `+`, as
reusing an accumulator nothing else holds is, and no program can tell it
from a copy; it is tagged `STRING_APPEND` (`git grep -n STRING_APPEND --
staticjs`), its cases are in `tests/spec/strings.spec` and `api_test`'s
`test_string_append_*`, and its commits carry no `Mutation: yes`. Rolling
mutation back keeps it.

The new code sits at the end of `src/vm.c` and `src/builtins.c`, and the
evaluator reaches it from the default case of its switch, so that the code
before it compiles as it did: `call`, `method`, `sjs_loop_step`,
`sjs_loop_start` and `sjs_builtin_call` are the same instructions as before
mutation. Moving code earlier in those files moved the functions after it
in the binary, which made unchanged loops measure up to 18% slower.
