# staticjs

staticjs is a pure, total subset of JavaScript, as a freestanding C library.
It is the language runtime of the staticjs data model; see the
[proposal](../docs/proposals/staticjs.md) for why it exists and how a
statechart uses it. The data model itself will be a separate project,
`tinyactors-staticjs`, that depends on this one, as a tree-sitter grammar
depends on the tree-sitter runtime.

## The rule

> A program staticjs accepts is a valid JavaScript program, and evaluating
> it gives the same result as JavaScript does, or a fault. Never a
> different value.

`mise run diff-node` checks it: every case in `tests/spec/` that expects a
value is evaluated in Node and must give the same value; every case staticjs
refuses as a syntax error must be one JavaScript refuses too; every case it
refuses as unsupported must be valid JavaScript; every case it refuses for a
name (bound nowhere, not exported, no such module) must throw in JavaScript
too; and 100,000 doubles must format as `String(x)` does, and 100,000
decimal strings (random digits, long ones, and the exact midpoints between
two doubles) must read as `Number(s)` does.

## What it guarantees

- **No side effects.** There are no statements but `const` and `let`,
  `if`, blocks, `return`, a call that changes a variable and an assignment,
  and no way to reach anything that is not passed in. Values are never changed once shared: a function changes only a value
  that a variable of its own holds alone, with one of its methods
  (`xs.push(x)`, `s.add(x)`) or by assigning into it (`o.k = v`,
  `xs[i] += 1`), which no one else can see
  ([docs/mutation.md](docs/mutation.md)).
- **Every evaluation ends.** Loops are built-ins over collections that
  already exist; code jumps only forward. A function cannot refer to itself
  or to a later `const`
  (refused when compiled), and a function that is already running cannot
  be entered again (refused when called, as Starlark does). Evaluation runs
  on fuel the caller pushes, and stops where the fuel ends.
- **Nothing recurses.** The parser, the compiler, the evaluator, the writer
  and the release of nested values all keep explicit stacks. A callback of
  `map` or `reduce` is a frame on the evaluator's own stack, not a C call.
- **No C library, no I/O.** Memory comes from the embedder's allocator.
  Every unbounded construct has a limit in `sjs_limits`.
- **A value nothing else holds is updated in place.** Values are counted
  references without cycles, so a value with one reference is visible only
  to what holds it. The last read of a local moves its value instead of
  copying it, `reduce` moves its accumulator into the callback and back, a
  literal that begins with a spread (`{...acc, [k]: v}`, `[...xs, x]`)
  takes over a source nothing else holds, and `concat`, `with` and
  `toSorted` take over such a receiver. A closure, a global, a module's
  export or a value a native keeps is always another holder, so it is
  copied. No program can tell the difference, and fuel can only go down:
  `{...acc, [k]: v}` in a `reduce` no longer copies the accumulator at each
  step. Owned mutation (below) relies on the same count: `acc.push(x)` in a
  `reduce` changes the accumulator the callback holds alone.
- **`+` and a template extend a string nothing else holds** (tagged
  `STRING_APPEND`): in `reduce((acc, x) => acc + x, "")` or
  `` (acc, x) => `${acc}${x},` ``, or wherever the left operand (a
  template's first substitution, after an empty head) is a value a last
  read moved, the string's block grows to twice its length when full, and
  the right-hand values are appended. Building a string of n bytes then
  copies O(n) bytes in O(log n) allocations, where it copied the whole
  string at each step. Its byte and UTF-16 lengths, its ASCII flag and
  whether it is an array index stay true; an interned string (a program's
  constants, keys put through the API) is copied, as the context's table
  finds it by its text. The spare room is at most the string's length, and
  stays with it when it is shared or returned: a string holds at most twice
  its bytes. Fuel is a unit per 64 bytes appended, where a copy charges for
  the whole string. Numbers still add first: `add` tests for two numbers
  before anything else. A string's `concat` extends its receiver the same
  way when nothing else holds it (`acc.concat(x)` in `reduce`), charging a
  unit per 16 bytes appended, as the string methods do.
- **Shapes change speed and fuel, never a value or a fault.** `toSorted`
  (and `sort`, which runs its loop) with a comparator `(a, b) => a.key - b.key` (or `b.key - a.key`) and
  `map` with `x => x.key` run the callback in C while each step's
  preconditions hold: objects that hold the key, numbers for a comparator.
  Any other step calls the callback's bytecode, which gives its value or
  faults where it faults. A comparison costs 3 units of fuel instead of 7,
  an element of a pluck 2 instead of 4. The spec runs whole with shapes and
  without, and both must agree (`docs/matcher.md`, section 8).

## Push and stream

```c
/* Bytes in, events out: the compiler is a sink of the parser's events. */
sjs_compiler *compiler;
sjs_compiler_config cc = { .context = context, .goal = SJS_GOAL_EXPRESSION, .globals = globals,
                           .global_count = global_count, .modules = modules };
sjs_compiler_init(&compiler, &cc);
sjs_parser *parser;
sjs_parser_config pc = { allocator, SJS_LIMITS_DEFAULT, SJS_GOAL_EXPRESSION, sjs_compiler_sink(compiler) };
sjs_parser_init(&parser, &pc);
while (have_bytes) sjs_parser_push(parser, next_chunk());   /* any size, down to one byte */
sjs_parser_finish(parser);
sjs_program *program;
sjs_compiler_finish(compiler, &program);

/* Fuel in, a value out. */
sjs_vm *vm;
sjs_vm_init(&vm, &(sjs_vm_config){ context });
sjs_vm_start(vm, &(sjs_run_params){ program, values, global_count });
while (sjs_vm_run(vm, 1000) == SJS_RUN_PAUSED) { /* do other work */ }

/* A value out as text, into a buffer of any size. */
sjs_value result;
sjs_vm_take_result(vm, &result);
sjs_writer *writer;
sjs_writer_init(&writer, &(sjs_writer_config){ context, result });
while (sjs_writer_write(writer, &buffer) == SJS_NEED_SPACE) drain(&buffer);
```

`sjs_compile()` does the first half for a source held in memory. A sink
other than the compiler (a formatter, a linter) can take the same events.

Each event names the syntax node it belongs to (`sjs_event.node`) and where
that node's source starts (`sjs_event.start`). Every event of a bracketed
node, from its BEGIN through its separators to its END, and an operator's
markers and its last event, share one id; every other event is a node of
its own. So a sink can tell which subtree an event closes, and where that
subtree began, without counting events. The start of an operator, a
member, an index or a call is its first operand's. Any sink may use them;
the compiler's shape matcher does.

**Shapes.** The compiler passes each event through a matcher first
(`docs/matcher.md`): a bottom-up tree automaton, built from the rules in
`src/shapes.rules`, that recognises functions of a known shape, such as
`x => x.key` or `(a, b) => a.key - b.key`, and tags them. A tag never
changes the code a function compiles to; `map` and `toSorted` use it to
run such a callback in C, giving the same values and faults on less fuel.
`sjs_compiler_config.no_shapes` turns the tags off. The same rules also
declare rewrites, matched with `no_shapes` too, as they change what a
program means: a mutating method called on a path (`o.items.push(x)`),
which the compiler compiles as a path to the container it changes
(docs/matcher.md, section 12).

## The language

Expressions: numbers (read to the nearest double, ties to even, however
many digits a literal has, in any radix), strings, template literals, `true`, `false`, `null`,
`undefined`, `NaN`, `Infinity`, arrays and objects (spread, computed and
shorthand keys), `new Set(...)`; `+ - * / %`, `< <= > >=`, `=== !==`,
`&& || ??`, `?:`, `!`, unary `-` and `+`, `typeof`, `in`; members, calls
and spread arguments, with `?.`; arrow functions with an expression body,
or a block body of statements: `const`, `if`/`else` (with `else if`), nested
blocks `{ }` and `return` anywhere in them; array and object patterns in
parameters and `const`.

A `const` belongs to the block it is in, as in JavaScript: a block may
shadow a name from around it, and its names are gone after it. An `if`
tests its condition for truthiness, as JavaScript does; a dangling `else`
binds to the nearest `if`; falling off the end of a body returns
`undefined`; code after a `return` is accepted, and never runs. A line
break after `return` ends it, as JavaScript inserts a semicolon there.

Parameters and the targets of patterns take default values (`(a, b = a *
2) => ...`, `const {limit = 10} = o`), taken when the value is `undefined`
and evaluated only then, after the targets before them are bound. A
function can take a rest parameter (`(...args) => ...`), and an array or
object pattern a rest element (`[head, ...tail]`, `{a, ...others}`), last
and without a default. Object patterns take names, strings, numbers
(`{0: first}`) and computed keys (`{[key]: value}`) as keys; an object rest
element copies the properties the pattern does not name, in their order.
An array pattern destructures an array, a set (in insertion order) or a
string (by code point). The parser reads an arrow function's parameter list
ahead to tell it from a parenthesized expression, so the list, defaults
included, must fit in `max_lookahead` tokens (256 by default), and its
patterns nest at most `max_nesting` deep, as everything else does. A
`const`'s pattern comes before its value in the source and after it in
evaluation order, so the parser holds the pattern's events, its default
values whole, until the value has been read: `max_held_events` (65,536 by
default) bounds them, and with them the parser's memory.

Modules: `const` declarations, `import x from "m"`, `import { a, b as c }
from "m"`, `import * as ns from "m"`, `import x, { y }` and `import x, *
as ns`, `import "m"`; `export const`, `export default` of an expression,
`export { a, b as c }` (also of names declared later, and `a as default`),
and the re-exports `export { a, b as c } from "m"`, `export * from "m"`
and `export * as ns from "m"`. A line break ends a statement where
JavaScript inserts a semicolon. Imports may come anywhere at the top level,
but a name is bound only from its import on. A module's value is its
namespace: an object of every export, with keys sorted by UTF-16 code units
as JavaScript sorts a namespace's. A later program imports it by name
through an `sjs_modules` adapter, and only a module already evaluated can
be imported, so imports cannot form a cycle.

Built-ins:

| | |
|---|---|
| arrays | `at` `concat` `every` `filter` `find` `findIndex` `findLast` `findLastIndex` `flat` `flatMap` `includes` `indexOf` `join` `lastIndexOf` `map` `reduce` `slice` `some` `toReversed` `toSorted` `toSpliced` `with`, `length` |
| strings | `at` `charAt` `charCodeAt` `codePointAt` `concat` `endsWith` `includes` `indexOf` `padEnd` `padStart` `repeat` `replaceAll` `slice` `split` `startsWith` `toLowerCase` `toUpperCase` `trim` `trimEnd` `trimStart`, `length`, indexing |
| sets | `has` `union` `intersection` `difference` `symmetricDifference` `isSubsetOf`, `size` |
| functions | `String` `String.fromCharCode` `String.fromCodePoint` `Boolean` `Array.isArray` `Array.of` `Object.keys` `Object.values` `Object.entries` `Object.fromEntries` `Number.isInteger` `Number.isSafeInteger` `Number.isFinite` `Number.isNaN`, `Number` `parseInt` `parseFloat` (also as `Number.parseInt` and `Number.parseFloat`) `isNaN` `isFinite` |
| `Math` | `abs` `min` `max` `floor` `ceil` `round` `trunc` `sign`, and its constants |
| constants | `Number.EPSILON` `MAX_SAFE_INTEGER` `MIN_SAFE_INTEGER` `MAX_VALUE` `MIN_VALUE` `POSITIVE_INFINITY` `NEGATIVE_INFINITY` `NaN`; `Math.PI` `E` `LN2` `LN10` `LOG2E` `LOG10E` `SQRT2` `SQRT1_2`: number constants when compiled |
| changing a variable | arrays: `push` `pop` `shift` `unshift` `sort` `reverse` `fill` `splice`; sets: `add` `delete` `clear` |

**Owned mutation.** A method that changes its receiver may be called on a
variable that the function owns: a parameter or a `const` of the function,
never captured by an arrow function, and never copied, passed or stored
before the call. Such a call, or a chain of them (`s.add(1).add(2)`), may
also be a statement. So may an assignment to a property or an element of
such a variable, as deep as it goes (`acc[r.id] = r.kwh`, `o.a[i].n += 1`,
`o.k ??= []`, `o.n++`), with JavaScript's evaluation order, and to a `let`
of the function (`let n = 0; n += x;`), which no arrow function may capture;
and `delete` of a property on such a path (`delete acc[k]`), which faults
on an array, as it would leave a hole; and a method that changes its
receiver called on such a path (`acc.byKey[x.k].push(x)`,
`o.a.sort(f).reverse()`). Appending to a `let` (`s += x`, `s = s + x + y`,
`` s = `${s}${x}` ``) moves its value to the `+`, which extends a string
nothing else holds, so building a string with them is linear
(docs/mutation.md, "Appending to a let").
The compiler
refuses any other receiver or target it can see is shared, and the
evaluator faults when the variable's value, or a container on the path,
has another holder: a caller, the embedder, a module's export, another
variable, or itself (`xs.push(xs)`, `o.self = o`). The rules, the check
and how to roll it back are in [docs/mutation.md](docs/mutation.md).

The embedder can add functions (`sjs_function_native`), such as SCXML's
`In()`. They are the one way into a program from outside.

## Where it faults instead of following JavaScript

These follow from the rule: staticjs stops where JavaScript would coerce,
compare identities, or find something on a prototype.

- `===` on two objects (identity); `+` of a string and an object; arithmetic
  or `<` on values that are not both numbers (or, for `<`, both strings).
- Converting an object to a number or a string where JavaScript would call
  its methods: `Number([])`, `parseInt({})`, `parseFloat([1])`. A string
  converted to a number on the way to something else: `isNaN("x")`,
  `isFinite("1")`, `Math.abs("1")`, `parseInt("10", "16")`'s radix; `Number(s)`,
  `parseInt(s)` and `parseFloat(s)` are the explicit conversions, and read
  any string as JavaScript does (`parseInt` in a radix that is not 10 or a
  power of two as V8 does, as the specification lets it approximate).
- An object rest element of a string (`const {...r} = "ab"`), as with
  spreading a string into an object: JavaScript copies UTF-16 code units,
  which can split a character. An object pattern's key that indexes half of
  a character outside the BMP (`const {0: c} = "😀"`), as indexing does.
- An object pattern over a number or a boolean (`[1].map(({a = 2}) => a)`),
  whose properties JavaScript finds on its prototype; an object rest
  element of one gives `{}`, as in JavaScript.
- A computed key, in a literal or a pattern, that is an object or an array:
  JavaScript turns it into a string.
- A property JavaScript finds on `Object.prototype` (`toString`,
  `__proto__`); reading a method without calling it, by name, through a
  pattern or with `in`: any method of an array, a string or a set, those
  staticjs leaves out included (`const {at = 1} = "x"`). A namespace
  (`import * as ns`) is an object too: `ns.toString` and `"valueOf" in ns`
  fault where JavaScript finds nothing, and so does turning a namespace into
  a string.
- A method that changes its receiver on a value something else holds
  (a caller's argument, a global, a module's export, an element of an
  array, an argument that is the receiver), or on a receiver that is not a
  variable (`[1].push(2)`, `xs?.push(1)`). `sort()` without a compare
  function, as `toSorted()`.
- An assignment through a container something else holds, or one that
  would make a cycle (`o.self = o`); to an array's element past its end
  (JavaScript leaves holes), to its `length` or to a key that is not an
  index; to a property of a string, a set, a number or a function
  (JavaScript ignores it, adds it or throws); to `__proto__` or a name
  `Object.prototype` has, as reading it faults; `++` of anything but a
  number, which JavaScript converts.
- `toSorted()` without a compare function; `toLowerCase()` of a string
  that is not ASCII; slicing a string through the middle of a surrogate
  pair, padding one (`padStart`, `padEnd`) with a pad string cut there,
  `at` or `charAt` of half of one (`charCodeAt` and `codePointAt` give its
  number), `replaceAll` with an empty pattern where it puts text between
  the halves of one, and `String.fromCharCode` or `String.fromCodePoint`
  of a half that no other half follows or precedes: staticjs strings
  cannot hold a lone surrogate.
  An object as a string method's argument that JavaScript turns into a
  string (a pad string, an argument of `concat`, the replacement of
  `replaceAll`), as `+` faults on one; a search argument that is not a
  string (`includes(1)`, `replaceAll(1, "x")`, `split(1)`); a function as
  `replaceAll`'s replacement.

## Refused when compiled

JavaScript staticjs leaves out is refused as `unsupported`, never given a
different meaning. Among the module forms: `export let`, `export function`,
`export class` and `export default function`; a string as an exported or
imported name (`export { a as "x-y" }`), as an object cannot keep the order
a namespace gives it; `import()`, `import.meta`, top-level `await` and
import attributes (`with { type: "json" }`); two `export *` that give one
name, unless an explicit export settles it, as staticjs cannot tell whether
they are one binding, so even two that lead to the same binding are
refused; and a name used before its import or declaration, even in an
earlier function. A default value that reads a later parameter, its own
parameter, or a name its function's body declares is refused too:
JavaScript would throw, or would find another binding, as parameters have a
scope of their own. An assignment, `++` and `--` are refused as
unsupported where JavaScript can assign but staticjs does not (anywhere
but as a statement, `[a] = xs`, a target whose root is not a variable the
function owns, `**=` and the bitwise forms: docs/mutation.md), and as a
syntax error where JavaScript cannot (`1 = 2`, `a + b = 1`, `1++`).
`delete` is refused as unsupported anywhere but as a statement, and of
anything but a property on such a path; of a name it is a syntax error in a
module, as in strict JavaScript. In a script, which is sloppy, `eval`, `arguments` and the
words only strict code reserves (`yield`, `let`, ...) are names, which
staticjs does not bind; in a module they are syntax errors.

Among the statements: loops, `switch`, `throw`, `try`, `var`, a `let`
captured by an arrow function or at a module's top level, assigning to a
`const` or a parameter,
labels, function and class declarations, and expression statements, which
can have no effect, are unsupported, but for a call that changes a variable
the function owns and an assignment (and the mutation of a variable the function does not
own, or that was captured, copied, passed or stored before it, is
unsupported too: docs/mutation.md); `break` and `continue` are syntax
errors, as staticjs has no loop for them to leave, but for `break name`
inside the label `name`, which is unsupported with it. A label inside one
of the same name is a syntax error, as in JavaScript. A declaration
as the statement of an `if`, an `else` or a label is a syntax error, as in
JavaScript (`if (x) const y = 1`). An `if` or a block at a module's top
level is unsupported, as a module holds imports, `const` and exports; the
parser reads on through it, so a `return` in it is the syntax error
JavaScript reports. A name read in a block, or in a function in it, and
declared later in that block or in one around it, is refused as a use
before its declaration, as at a function's top level. A name bound nowhere
is refused even in a branch that never runs or after a `return`, where
JavaScript would throw only if it ran.

After JavaScript staticjs leaves out, the parser reads on where it can,
sending nothing more: past an assignment, the comma operator, an operator
staticjs does not have, `this`, `new.target`, a method in an object
literal, an expression statement that is not a call, a label, and an `if`
or a block at a module's top level. An expression statement that is a call
is sent, and the compiler, refusing it unless it changes a variable, reads
on the same way. So a program that is not JavaScript further on, such
as `([a.b = 1]) => 0`, `a = 1 = 2` or `x => { a b; }`, is a syntax error,
not unsupported. It stops at the first refusal where it cannot read on: a loop,
`switch`, `try` or another statement staticjs does not have, a
regular expression (where it reads ahead without parsing, a `/` that cannot
divide is one, as after an `if`'s condition), a `/` in a method's body.

Errors come in the order JavaScript finds them only so far as a stage that
streams can know it. The compiler stops at the first error it meets; what it
can only decide at the end of a module (an `export { a }` the module never
declares, a name bound nowhere, an ambiguous `export *`) comes after
everything it met before, in the order syntax, then name, then unsupported.
So a module that uses what staticjs leaves out and later exports a name it
never declares is refused as unsupported, where JavaScript reports the
export.

## Status

The first stage of the proposal: the language. Not done yet, in the
proposal's order: the data model (`tinyactors-staticjs`); a hash index for
large objects and sets; a static cost estimate; `Map`.

## Tasks

```sh
mise run build              # the library (freestanding) and the test programs
mise run test               # everything below, the way `mise run test` at the root runs it
mise run test --full        # also fail every allocation of every spec case
mise run shapes [--check]   # write src/shapes.h and src/shapes_table.h from src/shapes.rules, or check them
mise run shapes-test        # the shapes generator's own tests
mise run check-parameters   # at most three parameters per function
mise run check-recursion    # no cycle in the call graph
mise run check-freestanding # builds for wasm32-freestanding with no C library
mise run asan               # the tests under AddressSanitizer and UBSan
mise run diff-node          # the spec against JavaScript
mise run bench [filter] [--quick]                  # the benchmarks, optimized: throughput and memory
mise run bench-compare <commit> [--runs=3] [--store] # this tree against another commit, side by side
```

The tests are `tests/spec/*.spec` (read by `tests/spec_test.c`, which runs
each case with the source pushed whole, a byte at a time and in chunks, on
different amounts of fuel, and once more with shapes off, which must give
the same program but for its tags, the same outcome and no less fuel),
`tests/api_test.c`, `tests/matcher_test.c` (the shape matcher on every
spec source, the deepest and widest programs, unreadable streams and
running out of memory) and `tests/number_test.c`.
`mise run test` also runs `bench --smoke` (below), which times nothing.

## Benchmarks

`bench/bench.c` measures each stage on the inputs a statechart gives it:

- **compile**: a chart's modules (`bench/modules/power.js` and
  `campaign.js`, from the spec), a module of 32 KiB, and guards, pushed
  whole, 64 bytes or one byte at a time, and the parser alone; in MB/s.
- **eval**: guards, an assign, a send's target, loading a module and
  calling a function from it, in evaluations per second; and the built-ins
  over arrays, objects and sets of growing size, in elements per second,
  so a row shows whether the time per element is flat or grows with n.
- **write**: rows, numbers and strings as text, with a large buffer and a
  64-byte one, in MB/s.
- **vm** and **values**: a fresh evaluator, a kept one, abandoning one,
  running on little fuel at a time, and building values through the API.

Each case gives its throughput (median of 15 samples of at least 8 ops,
with the spread between the quartiles) and its memory: the bytes its
inputs take, the op's peak, what its result holds, allocations per op, and
bytes left behind, which must be none. `mise run bench` builds with the
pinned clang at `-O2`, prints a table, and stores the records under
`bench/results/<machine signature>/` (see its README). `mise run
bench-compare <commit>` builds that commit's library beside this tree's,
with the same bench program (or, when today's does not compile against
the older `staticjs.h`, that commit's own, with a warning: those rows then
compare two programs as well), and runs both alternately, 4 processes a
side in ABBA order. It flags a throughput more than 5% slower in every run
and any memory increase; a case the older build cannot run is reported as
such.

**Noise.** Identical code does not measure the same twice: on machine
`92727593a59b` (a 4-core cloud VM), `mise run bench-compare HEAD` with
nothing changed moved half the rows by more than 1%, one in ten by more
than 3.5%, and three of 78 by more than 5% (one by 31%), while each side's
four runs spanned 4.5% for the median row. So a row gives each side's
range over its runs (`runs o/n`), a change past 5% inside that range is
reported as within the noise, not as slower, and `?` marks a row whose
runs or samples vary past 5%, which cannot resolve a 5% change; rerun
those cases with `--runs=8` before reading anything into them.

**Reporting.** Give throughput and memory together, name the machine
signature, and compare builds side by side with `bench-compare`, never
against numbers from another day or another machine.
