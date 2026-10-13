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
value is evaluated in Node and must give the same value (a case with
`@seed` in a `node --random-seed` of its own, so `Math.random` draws the
same values); every case staticjs
refuses as a syntax error must be one JavaScript refuses too; every case it
refuses as unsupported must be valid JavaScript; every case it refuses for a
name (bound nowhere, not exported, no such module) must throw in JavaScript
too; and 100,000 of each of these must give Node's bits: doubles
formatted as `String(x)`; decimal strings (random digits, long ones, and
the exact midpoints between two doubles) read by `Number(s)`; strings with
white space, signs, prefixes and junk read by `Number(s)` and
`parseFloat(s)`, and by `parseInt(s, radix)` in every radix; doubles
through `Math.sqrt` and `Math.fround`; argument lists through `Math.hypot`;
and pairs through `Math.clz32` and `Math.imul`; and BigInts of 1 to 20
limbs through `+ - * / % **` and comparison, compared with doubles, and
written as `String(x)` and `toString(radix)`, read back, rounded as
`Number(x)` and wrapped by `BigInt.asIntN` and `asUintN`, and a fixed
list of 100 doubles (ties, values just under and over a half, subnormals,
1e21 and the largest double) through `toFixed`, `toExponential` and
`toPrecision` with 24 counts of digits each, 0 to 100, and 123 doubles
(integers to the largest double, fractions to the smallest subnormal,
negatives) with each radix's powers and their neighbours through
`toString(radix)` in every radix 2 to 36, and a fixed list of 7,909 pairs
(specials, subnormals, integers odd and even, halves, negative bases,
bases next to 1 to huge powers, results next to overflow and underflow and
in the subnormals) through `Math.pow(x, y)` and `x ** y`, and a fixed
list of 8,391 doubles (specials, subnormals, the thresholds of fdlibm's
ranges and their neighbours, integers, halves, powers of two and ten,
values next to 1 and -1, multiples of ln 2) through each of `Math.exp`,
`expm1`, `log`, `log1p`, `log2` and `log10`, a fixed list of 8,693
doubles (specials, subnormals, the thresholds of the argument reduction
and of each kernel, every power of two, integers, the doubles next to
multiples of pi/4 and pi/2, the doubles closest to a multiple of pi/2 of
many binary exponents, values in [-1, 1]) through each of `Math.sin`,
`cos`, `tan`, `asin`, `acos` and `atan`, and a fixed list of 2,179 pairs
(every special case, quotients next to 2^60 and 2^-60, x next to 1, small
integers) through `Math.atan2(y, x)`, and a fixed list of 6,965 doubles
(specials, subnormals, the thresholds of each function's ranges, among
them the overflow threshold of `sinh` and `cosh`, every power of two,
integers, halves, values next to 1 and -1, cubes) through each of
`Math.sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh` and `cbrt`
(`number_test --print`; these Math lists are compared on x86_64 only, as
Node on arm64 computes some last bits with fused multiply-adds); and random JSON documents read by `JSON.parse` and written back
by `JSON.stringify`, the same documents with a byte or two changed (a
SyntaxError where JavaScript throws one), and `JSON.stringify` calls with
replacers and spaces (`json_test --print`).

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
  copying it, `reduce` and `reduceRight` move their accumulator into the
  callback and back, a
  literal that begins with a spread (`{...acc, [k]: v}`, `[...xs, x]`)
  takes over a source nothing else holds, and `concat`, `with` and
  `toSorted` take over such a receiver. A closure, a global, a module's
  export or a value a native keeps is always another holder, so it is
  copied. No program can tell the difference, and fuel can only go down:
  `{...acc, [k]: v}` in a `reduce` no longer copies the accumulator at each
  step. Owned mutation (below) relies on the same count: `acc.push(x)` in a
  `reduce` changes the accumulator the callback holds alone.
- **BigInt arithmetic writes over an operand nothing else holds** (tagged
  `BIGINT_REUSE`): `+`, `-` and unary `-` of a BigInt the stack holds alone,
  a reduce's accumulator or a let a last read moved, put the result in its
  block when it has room, and `*` by a one-limb BigInt does too; a product
  of such an operand gets an eighth more room for the next. Adding small
  BigInts in a reduce then allocates nothing per element, and a factorial
  rarely. Fuel is unchanged; no program can tell, as nothing else sees the
  operand.
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
/* The current time, if the program reads it (Temporal.Now, Date.now()): pushed, never read by the library. */
sjs_vm_set_clock(vm, &(sjs_epoch){ seconds, nanoseconds });
/* Math.random's seed, if the program draws: pushed too, held by the evaluator. */
sjs_vm_set_seed(vm, seed);
while (sjs_vm_run(vm, 1000) == SJS_RUN_PAUSED) { /* do other work; sjs_vm_advance_clock(vm, ns) */ }

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
many digits a literal has, in any radix), BigInts (`10n`, `0x1Fn`, `0o7n`,
`0b1n`; see "BigInts" below), strings, template literals, `true`, `false`, `null`,
`undefined`, `NaN`, `Infinity`, arrays and objects (spread, computed and
shorthand keys), `new Set(...)`, `new Map(...)` (see "Maps" below), `new Temporal.PlainDate(...)`, `new Temporal.PlainTime(...)`, `new Temporal.PlainDateTime(...)`, `new Temporal.Duration(...)`, `new Temporal.Instant(...)` (see "Temporal" below), `new Date(...)` (see "Date" below); `+ - * / % **`, `< <= > >=`, `=== !==`,
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
An array pattern destructures an array, a set (in insertion order), a map
(its entries, as `[key, value]` arrays) or a string (by code point). The parser reads an arrow function's parameter list
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
| arrays | `at` `concat` `entries` `every` `filter` `find` `findIndex` `findLast` `findLastIndex` `flat` `flatMap` `forEach` `includes` `indexOf` `join` `keys` `lastIndexOf` `map` `reduce` `reduceRight` `slice` `some` `toReversed` `toSorted` `toSpliced` `values` `with`, `length` |
| strings | `at` `charAt` `charCodeAt` `codePointAt` `concat` `endsWith` `includes` `indexOf` `isWellFormed` `padEnd` `padStart` `repeat` `replace` `replaceAll` `slice` `split` `startsWith` `substr` `substring` `toLowerCase` `toUpperCase` `toWellFormed` `trim` `trimEnd` `trimLeft` `trimRight` `trimStart`, `length`, indexing |
| regular expressions | literals (docs/regex.md); `test` `exec`, `source` `flags` `global` `ignoreCase` `multiline` `dotAll` `unicode` `unicodeSets` `sticky` `hasIndices`, `lastIndex` (always 0); with a string: `match` `matchAll` `search` (with a regular expression only), `replace` `replaceAll` `split` (with one or a string, and a limit that is a number, a boolean, `null` or `undefined`). A match runs in time linear in its input, on a Pike automaton paused on fuel like any loop; where no thread is alive, a pattern that begins with an ASCII character, or with `^` past the start, skips the bytes that cannot begin a match, charging the fuel following them would. A match result is an array with `index`, `input`, `groups` and, with `d`, `indices`; a replacement is a string with `$&` `` $` `` `$'` `$$` `$n` `$<name>`, a primitive written as `String` writes it, or a function called once per match. A regular expression is written as `/source/flags`, `typeof` gives `"object"`, `JSON.stringify` `{}`, and a property it does not have `undefined` |
| sets | `has` `keys` `values` `entries` `forEach` `union` `intersection` `difference` `symmetricDifference` `isSubsetOf` `isSupersetOf` `isDisjointFrom`, `size` |
| maps | `get` `has` `keys` `values` `entries` `forEach`, `size` |
| iterators | `toArray` `map` `filter` `reduce` `some` `every` `find` `forEach` `flatMap` `take` `drop` |
| BigInts | `toString` (radix 2 to 36) |
| numbers | `toFixed` `toExponential` `toPrecision`: the exact decimal value of the double, rounded half up, as V8 gives it; digits 0 to 100 (precision 1 to 100), else a fault where JavaScript throws a RangeError; `toString` (radix 2 to 36): `String(x)` in radix 10, V8's digits in any other |
| functions | `String` `String.fromCharCode` `String.fromCodePoint` `String.raw` (called as a function: staticjs has no tagged templates) `Boolean` `BigInt` `BigInt.asIntN` `BigInt.asUintN` `Array.isArray` `Array.of` `Map.groupBy` `Object.keys` `Object.values` `Object.entries` `Object.fromEntries` `Object.hasOwn` `Object.assign` (see "Owned mutation" below) `Object.freeze` `Object.isFrozen` `Object.seal` `Object.isSealed` (see "Frozen and sealed values" below) `Number.isInteger` `Number.isSafeInteger` `Number.isFinite` `Number.isNaN`, `Number` `parseInt` `parseFloat` (also as `Number.parseInt` and `Number.parseFloat`) `isNaN` `isFinite` `encodeURI` `encodeURIComponent` `decodeURI` `decodeURIComponent` |
| `JSON` | `parse` `stringify` (see "JSON" below) |
| `Temporal` | `Temporal.PlainDate`: `new Temporal.PlainDate(y, m, d)`, `from` (a string, a property bag or a PlainDate; `overflow`), `compare`; `year` `month` `monthCode` `day` `dayOfWeek` `dayOfYear` `weekOfYear` `yearOfWeek` `daysInWeek` `daysInMonth` `daysInYear` `monthsInYear` `inLeapYear` `calendarId` `era` `eraYear`; `with` `equals` `add` `subtract` (`overflow`) `until` `since` (`largestUnit`, `smallestUnit`, `roundingIncrement`, `roundingMode`) `toString` (`calendarName`) `toJSON` `toPlainDateTime`. `Temporal.PlainTime`: `new Temporal.PlainTime(h, m, s, ms, us, ns)`, `from` (a string, a property bag, a PlainTime or a PlainDateTime; `overflow`), `compare`; `hour` `minute` `second` `millisecond` `microsecond` `nanosecond`; `with` `equals` `add` `subtract` `until` `since` `round` (`smallestUnit`, `roundingIncrement`, `roundingMode`) `toString` (`fractionalSecondDigits`, `smallestUnit`, `roundingMode`) `toJSON`. `Temporal.PlainDateTime`: `new Temporal.PlainDateTime(y, m, d, h, m, s, ms, us, ns)`, `from` (a string, a property bag, a PlainDateTime or a PlainDate), `compare`; a PlainDate's getters and a PlainTime's; `with` `withPlainTime` `equals` `add` `subtract` `until` `since` `round` `toString` (and `calendarName`) `toJSON` `toPlainDate` `toPlainTime`. `Temporal.Duration`: `new Temporal.Duration(y, mo, w, d, h, mi, s, ms, us, ns)`, `from` (a string, a property bag or a Duration), `compare` (`relativeTo`); `years` to `nanoseconds` `sign` `blank`; `with` `negated` `abs` `add` `subtract` `round` (`largestUnit`, `smallestUnit`, `roundingIncrement`, `roundingMode`, `relativeTo`) `total` (`unit`, `relativeTo`) `toString` (`fractionalSecondDigits`, `smallestUnit`, `roundingMode`) `toJSON`. `Temporal.Instant`: `new Temporal.Instant(epochNanoseconds)`, `from` (a string with `Z` or an offset, or an Instant), `fromEpochMilliseconds` `fromEpochNanoseconds` `compare`; `epochMilliseconds` `epochNanoseconds`; `equals` `add` `subtract` `until` `since` `round` `toString` (`fractionalSecondDigits`, `smallestUnit`, `roundingMode`, `timeZone`: UTC or a fixed offset) `toJSON`. `Temporal.Now` (the host clock): `instant` `plainDateISO` `plainDateTimeISO` `plainTimeISO` (a time zone: UTC or a fixed offset) `timeZoneId` (see "Temporal" below) |
| `Date` | `new Date()` (the host clock), `new Date(value)` (a Date, a string of the date time string format, or a number), `Date.now` `Date.parse` `Date.UTC`; `getTime` `valueOf` `getUTCFullYear` `getUTCMonth` `getUTCDate` `getUTCDay` `getUTCHours` `getUTCMinutes` `getUTCSeconds` `getUTCMilliseconds` `toISOString` `toJSON` `toUTCString` `toGMTString`; `setTime` and the UTC setters, on a variable (see "Owned mutation" and "Date" below) |
| `Math` | `abs` `min` `max` `floor` `ceil` `round` `trunc` `sign` `sqrt` `hypot` `fround` `clz32` `imul` `pow` `exp` `expm1` `log` `log1p` `log2` `log10` `sin` `cos` `tan` `asin` `acos` `atan` `atan2` `sinh` `cosh` `tanh` `asinh` `acosh` `atanh` `cbrt`, and its constants: each the same bits as V8 compiled without fused multiply-adds on every target, which is Node on x86_64 (Node on arm64 fuses them, and its last bit differs for some inputs: docs/extending.md), without a C library (`sqrt` is the hardware's correctly rounded instruction, `hypot` V8's own sum, `pow` V8's fdlibm, as `**` is, and so are `exp`, `expm1`, the logarithms, the trigonometric and hyperbolic functions and `cbrt`); `random`, V8's generator from the seed the host sets (`sjs_vm_set_seed`: the values `node --random-seed=<seed>` gives) |
| constants | `Number.EPSILON` `MAX_SAFE_INTEGER` `MIN_SAFE_INTEGER` `MAX_VALUE` `MIN_VALUE` `POSITIVE_INFINITY` `NEGATIVE_INFINITY` `NaN`; `Math.PI` `E` `LN2` `LN10` `LOG2E` `LOG10E` `SQRT2` `SQRT1_2`: number constants when compiled |
| changing a variable | arrays: `push` `pop` `shift` `unshift` `sort` `reverse` `fill` `splice`; sets: `add` `delete` `clear`; maps: `set` `delete` `clear`; iterators: `next` `return`; Dates: `setTime` `setUTCFullYear` `setUTCMonth` `setUTCDate` `setUTCHours` `setUTCMinutes` `setUTCSeconds` `setUTCMilliseconds`; objects: `Object.assign(x, ...sources)`; objects and arrays: `Object.freeze(x)` `Object.seal(x)` |

**Owned mutation.** A method that changes its receiver may be called on a
variable that the function owns: a parameter or a `const` of the function,
never captured by an arrow function, and never copied, passed or stored
before the call. `add`, a set's method and a Temporal value's (which
changes nothing), is checked when it runs instead: a Temporal value's
works on any receiver but a chain through a path, and a set's faults
unless the call is on a variable of the function that is the set's one
holder (docs/mutation.md, "Shared names"). Such a call, or a chain of them (`s.add(1).add(2)`), may
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
`Object.assign(x, ...)` changes such a variable's object, and may be a
statement; any other target must be a value nothing else holds
(`Object.assign({}, a, b)`), and faults otherwise (docs/mutation.md,
"Object.assign").

**Frozen and sealed values.** `Object.freeze(x)` and `Object.seal(x)` set
a bit on an object or an array, which every copy of the value shares, and
from then on each change strict JavaScript throws a TypeError at faults:
a frozen value changes no key or element (an assignment, `delete` of a key
it has, `push`, `sort` of two elements or more, `fill` of a range that is
not empty, `Object.assign` with something to put, ...), and a sealed one
takes no new key or element and loses none, while its keys' and elements'
values change (`o.a = 1` of a key it has, `sort`, `reverse`, `fill`,
`splice` that removes as many as it inserts). Freezing is shallow, as in
JavaScript. `Object.isFrozen` and `Object.isSealed` give what Node gives:
true for a primitive, the bits for an object or an array (a sealed one
with no key or element is frozen too, as V8 tells), and false for every
other value; a module's namespace is sealed, as in JavaScript. A copy
JavaScript makes new (`{...o}`, `[...xs]`, `Array.from`, `concat`,
`toSorted`, ...) is neither, so the copies that take over a value nothing
else holds copy a frozen or sealed one instead. Freezing or sealing
changes the value, so it needs a value nothing else holds, or a variable
of the function that holds it alone, as a mutation does, unless the value
is already frozen or sealed; it faults on a set, a map, a Date, a function,
an iterator, a regular expression and a Temporal value, whose internal
state JavaScript leaves changeable (docs/mutation.md, "Frozen and sealed
values").
The compiler
refuses any other receiver or target it can see is shared, and the
evaluator faults when the variable's value, or a container on the path,
has another holder: a caller, the embedder, a module's export, another
variable, or itself (`xs.push(xs)`, `o.self = o`). The rules, the check
and how to roll it back are in [docs/mutation.md](docs/mutation.md).

The embedder can add functions (`sjs_function_native`), such as SCXML's
`In()`. They are the one way into a program from outside.

## BigInts

A BigInt is a heap value like a string: a sign and a magnitude in 32-bit
limbs, immutable, reference counted, and at most `max_bigint_bits` bits
(262,144 by default, about 79,000 decimal digits; a value of 2^n bits or
more is a limit error, as V8 throws a RangeError past its own maximum).
`typeof` is `"bigint"`; `===` and `!==` compare the value (`1n === 1` is
false); `0n` is falsy; a set holds one BigInt once (SameValueZero, by
value, through its index); a BigInt as a key is its decimal digits
(`{[10n]: 1}` has the key `"10"`), as are `String(x)`, `"a" + x` and a
template's `${x}`. The writer writes `10n`, and `sjs_bigint_text` gives an
embedder the digits.

`+ - * / % **` and unary `-` take two BigInts: `/` truncates toward zero,
`%` takes the dividend's sign, `0n ** 0n` is `1n`. Division by `0n` and a
negative exponent fault (RangeErrors in JavaScript), as do unary `+` of a
BigInt and arithmetic that mixes a BigInt with any other value (TypeErrors).
`**` of two numbers is V8's: its fdlibm `pow` (`src/math.c`), the same bits
as `Math.pow`. `< <= > >=` compare BigInts with each other and with
numbers exactly (`1n < 1.5`, `9007199254740993n > 2 ** 53`'s double;
anything with NaN is false); with a string or a boolean they fault, where
JavaScript would convert. staticjs has no bitwise operators, for numbers or
BigInts. Multiplication is schoolbook, division Knuth's algorithm D, and
text repeated division by 10^9: at the default limit the largest product
or conversion takes a few tens of milliseconds, and fuel charges each
operation for its limb work (a unit per 16 limb products).

`BigInt(x)` converts an integer number exactly (a number with a fraction,
NaN or Infinity faults, a RangeError), a string as StringToBigInt reads it
(white space, a sign, `0x`/`0o`/`0b`; empty is `0n`; anything else faults,
a SyntaxError), and a boolean; `undefined`, `null` (TypeErrors) and an
object fault. `BigInt.asIntN` and `asUintN` take their bits as an index (0
to 2^53 - 1, else a fault) and a BigInt or a boolean. `x.toString(radix)`
writes 2 to 36. `Number(x)` rounds to the nearest double, ties to even, and
`parseInt(x, radix)` and `parseFloat(x)` read `String(x)`, as JavaScript
does; `isNaN(x)`, `Math` and every other place a number is needed fault on
a BigInt, as JavaScript throws there.

## Maps

A map is a value like a set: each key once, with its value, in insertion
order, reference counted, immutable once shared. Its storage is a set's: the
keys and their values alternate in one block, and the keys go through the
same insertion-ordered hash index past `SJS_INDEX_FROM` entries, so a lookup
costs one unit of fuel whatever the size. A key is any primitive, compared as
SameValueZero: `1` and `"1"` are two keys, NaN is one, `-0` is `0` (and is
kept as `0`), a BigInt is a key by its value (`10n` is not `10`). An object,
an array, a set, a map or a function as a key faults, as JavaScript keys
them by their identity, which staticjs does not have; `get`, `has` of one
find nothing, as in JavaScript, where a map could not hold that object.

`new Map()` (of `undefined` or `null` too) is empty; `new Map(entries)` takes
`[key, value]` arrays from an array, or from anything a spread takes, whose
missing items are `undefined`, a later key's value replacing an earlier
one's in its place; `new Map(otherMap)` copies it. An entry that is not an
array faults: a primitive throws in JavaScript, and an object (`{0: k, 1:
v}`), which JavaScript reads, is not taken. `m.get(k)` is the value or
`undefined`, `m.has(k)` and `m.size` as in JavaScript. A spread, `Array.from`,
an array pattern (`const [[k, v]] = m`) and `Object.fromEntries` take a
map's entries, each a new `[key, value]` array; `{...m}` and an object rest
element of a map are `{}`, as in JavaScript; `Object.keys`, `values` and
`entries` of one fault, as of a set, where JavaScript gives `[]`. `typeof m`
is `"object"`,
`Array.isArray(m)` is false. A property read follows the prototype rules: a
name of `Map.prototype` or `Object.prototype` read as a value faults, any
other is `undefined`. The writer writes `new Map([[key, value], ...])`, and
an embedder builds and reads maps with `sjs_map_new`, `sjs_map_set`,
`sjs_map_get`, `sjs_map_at` and `sjs_map_size`.

**Iterators.** `m.keys()`, `m.values()` and `m.entries()` of a map,
`s.keys()`, `s.values()` and `s.entries()` of a set (whose keys are its
values, and whose entries are `[value, value]` arrays), and `xs.keys()`,
`xs.values()` and `xs.entries()` of an array (whose keys are its indices,
and whose entries are `[index, element]` arrays; of a match result, its
elements only, as in JavaScript), give an iterator
(the value kind `SJS_ITERATOR`, `typeof` `"object"`), which a spread,
`Array.from`, `new Set`, `new Map`, `Object.fromEntries`, an array pattern
and its own `toArray()` take, and whose `map`, `filter`, `reduce`, `some`,
`every`, `find` and `flatMap` run as the array's loops of those names,
with their shapes, over what it yields (`forEach` as a map's): the
callback gets the value and a counter, and `map`, `filter` and `flatMap`
give iterators. `flatMap`'s callback returns an array, a set, a map or an
iterator. `take(n)` and `drop(n)` give an iterator of the first `n` values
or of the rest. A JavaScript iterator yields its values once and is then empty;
a staticjs value cannot change. So staticjs reads an iterator only where
nothing else holds it (`sjs_value_unique`, as for updating a value in
place): a temporary (`[...m.keys()]`), or a variable's last read, which
moves its value: the last on its path, in each branch of an `if` or a
conditional (`c ? it.drop(1) : it`), and in a `let`'s reassignment from
itself (`it = it.map(f)`; docs/mutation.md). Then nothing can read it again, and no program can tell
that it was not consumed. A second read, a closure's capture or any other
holder faults ("an iterator yields its values once"), where JavaScript
gives nothing the second time; so does a helper of an iterator something
else holds. An iterator also holds its array, map or set, so that cannot
change while the iterator lives (owned mutation faults: "held elsewhere"), as a
JavaScript iterator would yield what changed. The helpers are lazy in
JavaScript, but their callbacks change nothing, so running them at once
gives the same values; a callback of `map`, `filter` or `flatMap` runs on
every value, also those a later `take`, `some`, `every` or `find` never
reads, so it can fault where JavaScript gives a value. `some`, `every` and
`find` stop at the first value that decides them. `next()` on a variable
the function owns advances the iterator and gives a new `{ value, done }`,
and a later reader gets the rest; `return()` closes a helper's result, and
faults on an array's, a map's, a set's or `matchAll`'s iterator, which has
none; on an
iterator nothing else holds (`m.keys().next()`) both give the same, and on
one held elsewhere they fault (docs/mutation.md, "Iterators"). A property an
iterator lacks is `undefined`. The writer writes an iterator as
`[...].values()`, an iterator that yields the same values.

`m.set(key, value)`, `m.delete(key)` and `m.clear()` change a map a
variable of the function owns, or a container on a path from one
(`state.byID.set(k, v)`), as owned mutation changes a set
([docs/mutation.md](docs/mutation.md)): `set` gives the map, so it chains
(`m.set(1, "a").set(2, "b")`), and the reduce idiom
`(acc, x) => { acc.set(x.k, x); return acc; }` with `new Map()` changes the
accumulator in place. A key deleted and set again goes last; `delete` and
`clear` shrink the map as a set's removal does; a lookup costs one unit
through the index, and a `delete` but of the last entry its entries more.

`m.forEach((value, key, map) => ...)` is a loop that calls back for each
entry and gives `undefined`, and so are a set's `s.forEach((value, value,
set) => ...)`, which passes each member twice, as JavaScript does, and an
array's `xs.forEach((element, index, array) => ...)`; its callback changes
nothing outside, so it gives `undefined` or faults where the callback
faults, and the loop holds its receiver, which the callback cannot change.
As a statement (`xs.forEach(f);`) it can change nothing a program sees,
so it is refused, as any expression statement with no effect is. `Map.groupBy(items,
f)` groups an array's elements as `Object.groupBy` does, with its C path for
`x => x.key`, into a map whose keys are any primitive, in the order first
seen; an object as a key faults, and so do items that are not an array, as
for `Object.groupBy`.

## JSON

`JSON.parse(text)` reads JSON as ECMAScript's `JSON.parse` does: JSON's
white space only (space, tab, LF, CR), every escape, an escaped surrogate
pair as one character, numbers by JSON's grammar read exactly (to the
nearest double, ties to even: `1e400` is Infinity, `-0` is -0), objects in
JavaScript's key order (integer-like keys first, ascending), a duplicate key's
last value in its first place. A text that is not JSON faults, as JavaScript
throws a SyntaxError, naming the position in UTF-16 code units. A number, a
boolean, null or undefined as the text is read as its string, as JavaScript
reads it (`JSON.parse(1)` is 1). It faults where JavaScript would make what
staticjs cannot hold: a lone surrogate (`"\uD800"`), or an object with the
key `__proto__`, which JavaScript makes an own property; on a reviver,
which JavaScript calls back for every value; on a BigInt or an object as the
text, which JavaScript turns into a string; and on a text nested deeper than
`max_nesting`, a limit.

`JSON.stringify(value, replacer, space)` writes as JavaScript does: strings
with JSON's escapes, numbers as `String(x)` (NaN and the infinities as
`null`, -0 as `0`), objects in their order, `undefined` and functions left
out of objects and `null` in arrays, `undefined` (not a string) for them at
the top; a map, a set or an iterator is `{}`, as none has an own enumerable
property, and an iterator is not read. `space` indents by a number of spaces
(at most 10) or a string's first 10 code units, `[]` and `{}` staying empty;
an array replacer keeps its strings' and numbers' keys, each once, in its
order, and any other replacer that is not a function is ignored. It faults
on a BigInt (a TypeError in JavaScript); on a `toJSON` method and a replacer
function, which JavaScript calls back; on a key of an array replacer that
JavaScript would read from a prototype (`["toString"]`, a map's `size`); on
a string `space` whose 10th code unit splits a pair, which would write a
lone surrogate; and on a `space` between 0 and 1, which V8 indents with
empty lines where the specification does not indent. Its output, the
members it leaves out included, is at most `max_string_bytes`, so one call's
work is bounded. Both are leaves: one C call each, over an explicit stack of
the containers open, a unit of fuel per 16 bytes and per value.

## Temporal

A Temporal value is an object, immutable, in the ISO 8601 calendar
([docs/temporal.md](docs/temporal.md)): `Temporal.PlainDate`,
`Temporal.PlainTime`, `Temporal.PlainDateTime`, `Temporal.Duration` and
`Temporal.Instant` (UTC only) are built, the first three's limits
Temporal's (-271821-04-19 to +275760-09-13; a date and time a nanosecond
past -271821-04-19T00:00 to +275760-09-13T23:59:59.999999999). The writer
writes one as its type's `from` of its ISO string,
`Temporal.PlainDate.from("2024-01-31")`,
`Temporal.PlainTime.from("12:30:00")`; `typeof` gives `"object"`, and
`JSON.stringify` its ISO string. `from` reads an ISO 8601 string as both
the vendored temporal-polyfill 1.0.5 and `@js-temporal/polyfill` read it
(an offset and a time zone annotation are read and left out, and a date's
time; the calendar annotation must be `iso8601`; a time alone that reads
as a date, as `"2021-12"` does, needs its `T`), a property bag (`year`,
`month` or `monthCode`, `day`, `hour` to `nanosecond`, with `overflow`
`"constrain"` or `"reject"`) or a value of a type it reads. `round` and
`toString` round by any of the nine rounding modes. A Duration
(`Temporal.Duration.from("P1Y2M")`) is exact to its limit of 2^53
seconds; it reads ISO 8601 durations (`PT1.5H`), and its `round`, `total`
and `compare` count years, months and weeks from a `relativeTo` date, as
the specification rounds (a destination past the rounding window, from a
month's end into a shorter month, faults). `add` and `subtract` of a date
add years and months, the day constrained to the month (`2024-01-31`
plus `P1M` is `2024-02-29`) or, with `overflow: "reject"`, refused, then
weeks and days; `until` and `since` give a Duration in the type's units,
rounded the same way. An Instant (`Temporal.Instant.from("2024-01-31T10:00:00Z")`)
is exact nanoseconds within 8.64 x 10^21 of the epoch, made from a BigInt,
milliseconds or a string with `Z` or a UTC offset (applied to the
nanosecond); its `toString` writes `Z`, or the offset of the time zone
`UTC` or of a fixed offset (`+01:00`); it adds hours and smaller units
only, and rounds before 1970 as if positive. `Temporal.Now` reads the
host clock and nothing else: the host sets it with `sjs_vm_set_clock`
and advances it with `sjs_vm_advance_clock` (docs/temporal.md, "Host
clock"), and a program that reads it when it was never set faults. Its
time zone is UTC.

## Date

A Date is a time value, whole milliseconds since 1970-01-01T00:00Z within
8.64 x 10^15 either way or NaN, in UTC only ([docs/date.md](docs/date.md)).
It is an object (`typeof` gives `"object"`), held as a value of the
Temporal kind; the writer writes it as `new Date("2024-01-31T00:00:00.000Z")`
or `new Date(NaN)`, and `JSON.stringify` its `toJSON`: the ISO string, or
`null` when it is invalid. `Date.now()` and `new Date()` read the host
clock, as `Temporal.Now` does, in milliseconds toward negative infinity.
`new Date(value)` copies a Date, reads a string as `Date.parse` does, or
clips a number (truncated, NaN past the limits). `Date.parse` reads the
date time string format (`2024-01-31`, UTC; `2024-01-31T12:30:00.000Z` and
`+05:30` offsets; six-digit years) and gives NaN only past the limits.
`Date.UTC` gives V8's value bit for bit, its 1900 to 1999 mapping and its
balancing of months, days and times included. The UTC getters,
`toISOString` (six-digit years past 9999 and before 0), `toJSON` and
`toUTCString` read the time value. `setTime` and the UTC setters
(`setUTCFullYear` to `setUTCMilliseconds`) write it, as V8 computes it, on
a variable of the function that holds the Date alone (owned mutation,
docs/mutation.md), and give the new time value; on a temporary, a path or
an element they fault, and the local setters stay refused.

## Where it faults instead of following JavaScript

These follow from the rule: staticjs stops where JavaScript would coerce,
compare identities, or find something on a prototype.

- `===` on two objects (identity); `+` of a string and an object; arithmetic
  or `<` on values that are not both numbers (or, for `<`, both strings), or
  BigInts (and, for `<`, a BigInt and a number).
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
  JavaScript turns it into a string. A map's key that is not a primitive
  (`new Map([[{}, 1]])`), which JavaScript keys by its identity; an entry of
  `new Map` that is not an array (`new Map([{0: "k", 1: "v"}])`), which
  JavaScript reads as an object.
- `JSON.parse` with a reviver, of a lone surrogate (`"\uD800"`) or of the
  key `__proto__`, and of a BigInt or an object; `JSON.stringify` with a
  replacer function or of an object with a `toJSON` method, which JavaScript
  calls; with an array replacer naming a key JavaScript reads from a
  prototype; and with a `space` between 0 and 1, or a string cut inside a
  pair (the "JSON" section).
- `String.raw` with raw strings that are an object or a function, which
  JavaScript reads by their `length` and indexes (an array or a string is
  read; a number, a boolean or a BigInt is none, as in JavaScript), and
  where JavaScript throws: a template without `raw`. `decodeURI` and
  `decodeURIComponent` of a malformed escape or of escapes that are not
  UTF-8 (overlong, a surrogate, past U+10FFFF), where JavaScript throws a
  URIError. The five read text (a raw string, a substitution, what is
  encoded or decoded) as `String` gives it, so an object faults there, and
  a BigInt is its digits.
- An iterator read where something else holds it too (`[[...it], [...it]]`,
  a closure's capture), where JavaScript gives nothing the second time; an
  iterator's `next` or `return` where something else holds it too, or
  through a path (`o.it.next()`); a callback of an iterator's `map`, `filter` or
  `flatMap` that faults on a value a later `take`, `some`, `every` or
  `find` would not read, as staticjs runs them to the end; `take` and
  `drop` of a string or an object, which JavaScript converts to a number;
  `Map.groupBy` of anything but an array, as `Object.groupBy`.
- A property JavaScript finds on `Object.prototype` (`toString`,
  `__proto__`); reading a method without calling it, by name, through a
  pattern or with `in`: any method of an array, a string, a set or a map, those
  staticjs leaves out included (`const {at = 1} = "x"`). A namespace
  (`import * as ns`) is an object too: `ns.toString` and `"valueOf" in ns`
  fault where JavaScript finds nothing, and so does turning a namespace into
  a string.
- A method that changes its receiver on a value something else holds
  (a caller's argument, a global, a module's export, an element of an
  array, an argument that is the receiver), or on a receiver that is not a
  variable (`[1].push(2)`, `xs?.push(1)`).
- An assignment through a container something else holds, or one that
  would make a cycle (`o.self = o`); to an array's element past its end
  (JavaScript leaves holes), to its `length` or to a key that is not an
  index; to a property of a string, a set, a number or a function
  (JavaScript ignores it, adds it or throws); to `__proto__` or a name
  `Object.prototype` has, as reading it faults; `++` of anything but a
  number, which JavaScript converts.
- `sort()` and `toSorted()` without a compare function (or with
  `undefined`) of two elements or more that are not `undefined`, one of
  them an object, an array, a function, a regular expression, a set, a
  map, an iterator or a Temporal value, whose `toString` JavaScript calls
  to compare it; of primitives they sort as JavaScript does, by the
  elements' strings in UTF-16 code units, `undefined` last. `toLowerCase()` of a string
  that is not ASCII; cutting a string through the middle of a surrogate
  pair (`slice`, `substring`, `substr`), padding one (`padStart`,
  `padEnd`) with a pad string cut there,
  `at` or `charAt` of half of one (`charCodeAt` and `codePointAt` give its
  number), `replaceAll` with an empty pattern where it puts text between
  the halves of one, `String.fromCharCode` or `String.fromCodePoint`
  of a half that no other half follows or precedes, and `String.raw` of a
  string with a substitution between the halves of one: staticjs strings
  cannot hold a lone surrogate. So `encodeURI` and `encodeURIComponent`
  never meet one, where JavaScript throws a URIError.
  An object as a string method's argument that JavaScript turns into a
  string (a pad string, an argument of `concat`, the replacement of
  `replaceAll`), as `+` faults on one, and a BigInt there too: staticjs
  makes a BigInt's digits only for `String`, `toString`, `+` and a
  template; a string as `BigInt.asIntN`'s or `asUintN`'s value, or a
  string as any built-in's number (`toString("16")`), which JavaScript
  converts; a search argument that is not a
  string (`includes(1)`, `replaceAll(1, "x")`, `split(1)`); a function as the
  replacement of `replace` or `replaceAll` with a string pattern.
- Of regular expressions (docs/regex.md, 8.5): `exec` or `test` with `g`
  or `y`, and `match` or `replace` with `y` and without `g`, which change
  `lastIndex` in JavaScript, and staticjs values do not change; `matchAll`
  or `replaceAll` without `g`, whatever the other flags, where JavaScript
  throws a TypeError; `match`, `matchAll` or `search` with anything but a
  regular expression (`"a.b".match(".")`), of which JavaScript makes one;
  a match, a group or a piece of `split` that begins or ends between the
  halves of a surrogate pair, which JavaScript makes into a lone
  surrogate; a replacement that is an object or a BigInt, or a
  replacement function that returns one (an array included); a
  non-string argument (`re.test(1)`) or a `split` limit that is a string,
  an object or a BigInt (`s.split(re, "2")`) that JavaScript converts; `Object.keys`,
  `Object.values`, `Object.entries`, an object spread or rest, or `sort`
  of a match result, as JavaScript would show or keep `index`, `input` and
  `groups` among its keys. A regular expression is an object: `===` on
  two, `String(re)`, `re.toString()`, `"" + re` and `` `${re}` `` (which
  JavaScript writes as `/source/flags`), `Object.keys(re)`, `in` on one,
  `JSON.stringify` with a list of keys that names a property of
  `RegExp.prototype` (`source`, `flags`, ...) or `lastIndex`, an assignment to its properties, and one as a map key, a
  set member or a computed key fault; so does reading one of its methods
  (`re.exec`) or another name of `RegExp.prototype` (`re.compile`)
  without calling it. A pattern that would need more states than
  `max_regex_states` is a limit error when compiled.
- Of Temporal values (docs/temporal.md): `===` on two, a set member or a
  map key (identity: `equals` and `compare` are the way); `String(d)`, a
  template, `Object.keys` and `in`, which JavaScript turns into its string
  or its prototype's names; reading a method without calling it; a field,
  a `monthCode` or an option given as something JavaScript converts (a
  string, an array, an object); a calendar other than `iso8601`, in an
  argument, a bag or a string's annotation; a string the polyfill reads
  past the specification's grammar (`+24:00`, `[foo=-bar]`), or that the
  two polyfills read differently; `toLocaleString` (locale data) and
  `valueOf`, which throws in JavaScript; a Duration's `relativeTo` with a
  time zone (a ZonedDateTime, which staticjs does not have); an Instant's
  epoch nanoseconds or milliseconds given as a string (JavaScript converts
  them), `toString`'s `timeZone` other than `UTC` or a fixed offset (time
  zone data), `toZonedDateTimeISO`, and the roundings of an Instant before
  1970 that temporal-polyfill and the specification give differently
  (docs/temporal.md, "Instant"); `Temporal.Now` when the host never set
  the clock (JavaScript reads the machine's), with a time zone's name, or
  its `zonedDateTimeISO`.
- Of Dates (docs/date.md): what reads or writes local time (`new Date(y, m)`
  with two or more arguments, `Date()` called, a date and time string
  without `Z` or an offset, `getFullYear` and the other local getters,
  `getTimezoneOffset`, `toString`, `toDateString`, `toTimeString`) or the
  host's locale (`toLocaleString` and its kin); a string outside the date
  time string format, which Node reads by heuristics of its own (`"Jan 31
  2024"`, `"2024/01/31"`) or gives NaN for; `Date.now()` and `new Date()`
  when the host never set the clock; `===` on two, a set member or a map
  key; `-`, `<`, unary `+`, `+` with a string, a template, `String(d)` and
  `Number(d)` (JavaScript calls `valueOf` or `toString`); a string or an
  object where `Date.UTC` or a setter converts one, an object where `new
  Date` does; the local setters (`setFullYear` and its kin); `setTime` and
  the UTC setters but on a variable of the function that holds the Date
  alone (owned mutation: a Date copied, stored, passed or captured before,
  on a path, an element or a temporary).
- `Math.random()` when the host never set the seed (`sjs_vm_set_seed`;
  JavaScript draws from the machine's entropy).

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
function owns, and the bitwise forms: docs/mutation.md), and as a
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
unsupported too, but for `add` on a variable of the function, which is
checked when it runs: docs/mutation.md); `break` and `continue` are syntax
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
`switch`, `try` or another statement staticjs does not have, a `/` in a
method's body.

A regular expression literal is read as JavaScript reads it, and its
pattern is parsed when compiled: what Node refuses is a syntax error, and
what Node accepts but staticjs does not match is unsupported, and the
parser reads on. Unsupported are lookahead and lookbehind,
backreferences, `\p{...}` (of a name Node knows; another is a syntax
error, as in Node), the `v` flag, a non-ASCII letter with `i`, a group
name that is not ASCII letters, digits, `$` and `_` (and then `\k<...>`),
the forms only Annex B allows (a lone `]` or `{`, an identity escape such
as `\-`, `\1` with no group 1 as an octal escape, ...), and `RegExp` and
`new RegExp` (docs/regex.md, 8.3).

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
large objects and sets; a static cost estimate.

## Using a release

staticjs is released on its own, tagged `staticjs/v<version>`, its version
`SJS_VERSION_MAJOR`, `_MINOR` and `_PATCH` in `include/staticjs.h`
(root [ADR 0001](../docs/adr/0001-packaging-and-releases.md)). A release's
assets:

| Asset | Contents |
|---|---|
| `staticjs-<version>-src.tar.gz` | `staticjs.h`; `staticjs.c`, the amalgamation, every source as one file whose functions outside the API are static; `staticjs_wasm.c`, the wasm module's allocator; and `staticjs.api.json`, the API description |
| `staticjs-<version>-<target>.tar.gz`, for `aarch64-macos`, `x86_64-macos`, `x86_64-linux`, `aarch64-linux` | `include/staticjs.h`, `lib/libstaticjs.a` and the shared library (`libstaticjs.so.<major>.<minor>` with a `libstaticjs.so` link, or `libstaticjs.<major>.<minor>.dylib`) |
| `staticjs-<version>-wasm32.tar.gz` | `staticjs.wasm`, which imports nothing, and `include/staticjs.h` |
| `staticjs-<version>.json` | the manifest: commit, zig, flags, and every file's hash |

plus `SHA256SUMS`. Every tarball carries the repository's `LICENSE` and the
notices of the code `src/math.c` and `src/number.c` port: `LICENSE.fdlibm`
(fdlibm's, which FreeBSD's cbrt shares) and `LICENSE.v8`.

The libraries call no C library and export the `sjs_*` API alone; they leave
nothing undefined, so one Linux library serves glibc and musl. Link one, or
compile the amalgamation into a program:

```sh
cc -Iinclude app.c lib/libstaticjs.a                     # static
cc -Iinclude app.c -Llib -lstaticjs -Wl,-rpath,"$PWD/lib"  # shared
cc -I. app.c staticjs.c                                  # from the source tarball
```

A program gives a context its allocator (`sjs_context_config`), compiles a
source, and runs it on the fuel it pushes; `tests/release/staticjs_smoke.c`
is a whole program that does so, and the release runs it against the
libraries of the machine it runs on and, in containers, of each Linux
target.

`staticjs.wasm` is for browsers: load it with
`WebAssembly.instantiate(bytes, {})`. It has its own allocator: pass
`sjs_wasm_allocator()` as a context's allocator, place what a call reads (a
config, a source) with `sjs_wasm_alloc`, and give it back with
`sjs_wasm_free` and the size and alignment it asked for. The C ABI for
wasm32 passes a struct by value as a pointer to a copy and returns one
through a pointer given first; a `uint64_t` (fuel) is a BigInt. The struct
layouts are in `staticjs.api.json`, under `layout.wasm32`.
`tests/wasm/release_test.mjs` evaluates a program through the module so.

## Tasks

```sh
mise run build              # the library (freestanding) and the test programs
mise run test               # everything below, the way `mise run test` at the root runs it
mise run test --full        # also fail every allocation of every spec case
mise run shapes [--check]   # write src/shapes.h and src/shapes_table.h from src/shapes.rules, or check them
mise run shapes-test        # the shapes generator's own tests
mise run check-parameters   # at most three parameters per function
mise run check-recursion    # no cycle in the call graph
mise run check-freestanding # builds for every release target, wasm32 included, with no C library
mise run asan               # the tests under AddressSanitizer and UBSan
mise run diff-node          # the spec against JavaScript
mise run amalgamate [--check] # include/staticjs.c and staticjs_wasm.c, the single-file library, or check them
mise run api [--check]      # include/staticjs.api.json, the API description, or check it
mise run wasm-test          # staticjs.wasm, built as the release builds it, run in Node
mise run dist [debug]       # dist/: the header, amalgamation, API description and libstaticjs.a for this machine
mise run release -- <version> --build-only  # build, verify and pack every target: a look at a release
mise run release -- <version> [--skip-containers] [--skip-tests] [--publish]  # a dry run, or with --publish the release
mise run bench [filter] [--quick]                  # the benchmarks, optimized: throughput and memory
mise run bench-compare <commit> [--runs=3] [--store] # this tree against another commit, side by side
```

The tests are `tests/spec/*.spec` (read by `tests/spec_test.c`, which runs
each case with the source pushed whole, a byte at a time and in chunks, on
different amounts of fuel, and once more with shapes off, which must give
the same program but for its tags, the same outcome and no less fuel),
`tests/api_test.c`, `tests/matcher_test.c` (the shape matcher on every
spec source, the deepest and widest programs, unreadable streams and
running out of memory), `tests/number_test.c`, `tests/json_test.c`
(random JSON read and written back, and mutated JSON read or refused) and
`tests/temporal_test.c` (Temporal on fixed lists of strings, dates, bags
and options, which diff-node compares with the vendored polyfill, and
Date's strings, `Date.UTC` and getters, which it compares with Node's).
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

**Other engines.** `mise run bench-engines` runs the cases listed in
`bench/engines/cases.mjs` (bench.c's own, each at one size) in staticjs,
in Node with its JIT, in Node `--jitless`, and in quickjs-wasi (QuickJS-NG
compiled to WebAssembly, run by Node), and writes a report: time per op
in each, the ratios to staticjs and their geometric means, the cost of
starting an evaluation, and memory, with the method and its caveats.

- **What it needs:** Node (pinned in the root `mise.toml`), and npm the
  first time, to install the quickjs-wasi pinned in
  `bench/engines/package-lock.json` into `bench/engines/node_modules/`
  (ignored by git): the first run needs the network, later ones do not.
- **How long:** a full run takes 15 to 20 minutes on a 4-core machine,
  with nothing else running (`--quick` with a filter, under a minute).
- **Verified first.** The JavaScript engines build bench.c's fixtures with
  `bench/engines/guest.js`, a hand-written copy of its generators. Before
  timing anything, the task checks, at each case's smoke size and its
  measured size, that every fixture and every engine's result equals
  staticjs's (length and hash, through `bench/engines/dump.c`, which
  includes bench.c), and that each engine gives bench.c's expected value
  at the smoke size. A difference stops the run, naming the case: after a
  change to a fixture in bench.c, change guest.js to match.
- **Interleaved.** Each case runs 5 rounds (`--rounds <n>`); a round runs
  the four engines one after another, each in a process of its own, never
  two at once, and the next round in the reverse order. A cell is the
  median of the rounds.
- **Where the report goes.** A full run appends its records to
  `bench/results/<signature>/engines-<commit>.jsonl` and writes its report
  to `engines-<commit>.md` beside them and to `bench/engines/REPORT.md`,
  which always holds the latest; commit them. A run with a filter
  (`mise run bench-engines eval.array`) or `--quick` is a look, not a
  record: it writes `build/engines/report.md` only, as `--no-store` does.
- **Reading it.** A ratio is the other engine's time over staticjs's, so
  "3× faster" means staticjs took a third of the time. `±x%` follows a
  figure whose middle rounds spread past 10%. `≈` marks a ratio whose two
  ranges overlap: it is not resolved, the engines may be equal. The
  compile rows favour Node, which compiles inner functions lazily.

**Reporting.** Give throughput and memory together, name the machine
signature, and compare builds side by side with `bench-compare`, never
against numbers from another day or another machine.
