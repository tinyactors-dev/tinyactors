---
title: The language
description: "What a staticjs program can say, and what every built-in gives."
---

# The language

What a staticjs program can say, and what each construct gives. Everything
here gives the value Node gives, or faults: where staticjs stops instead of
following JavaScript is in [Faults and refusals](./faults.md), with what it refuses
when compiled. How a program reaches the library is in
[Embedding](./embedding.md).

A program is either one **expression** (`SJS_GOAL_EXPRESSION`, read as
JavaScript reads `(` source `)`), or a **module** (`SJS_GOAL_MODULE`):
imports, `const` declarations and exports.

```js
// An expression: a guard, an assignment's value, a send's target.
orders.filter((o) => o.total > limit).map((o) => o.id)

// A module: imports, consts and exports. "money" is a module the host provides.
import { round2 } from "money";
export const net = (lines) => round2(lines.reduce((sum, l) => sum + l.price * l.qty, 0));
export default net;
```

## Expressions and statements

Expressions: numbers (read to the nearest double, ties to even, however
many digits a literal has, in any radix), BigInts (`10n`, `0x1Fn`, `0o7n`,
`0b1n`; see [BigInts](#bigints)), strings, template literals, `true`, `false`, `null`,
`undefined`, `NaN`, `Infinity`, arrays and objects (spread, computed and
shorthand keys), `new Set(...)`, `new Map(...)` (see [Maps](#maps)), `new Temporal.PlainDate(...)`, `new Temporal.PlainTime(...)`, `new Temporal.PlainDateTime(...)`, `new Temporal.Duration(...)`, `new Temporal.Instant(...)` (see [Temporal](#temporal)), `new Date(...)` (see [Date](#date)); `+ - * / % **`, `< <= > >=`, `=== !==`,
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

Try it: change the order, or the discount.

```js live
((order) => {
  const { items = [], discount = 0 } = order;
  if (items.length === 0) return "nothing to pay";
  const subtotal = items.reduce((sum, item) => sum + item.price * item.qty, 0);
  return `${subtotal * (1 - discount)} after ${discount * 100}% off`;
})({ items: [{ price: 12, qty: 2 }, { price: 5, qty: 1 }], discount: 0.1 })
```

## Modules

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

A module's value is the object of its exports:

```js live module
const rate = 0.2;
const net = (gross) => Math.round((gross / (1 + rate)) * 100) / 100;

export const prices = [120, 60, 9.99].map(net);
export default prices.reduce((a, b) => a + b, 0);
```

## Built-ins

Each built-in below gives what Node gives. Some take only the arguments
JavaScript would not convert: the cases are in [Faults and refusals](./faults.md).

| | |
|---|---|
| arrays | `at` `concat` `entries` `every` `filter` `find` `findIndex` `findLast` `findLastIndex` `flat` `flatMap` `forEach` `includes` `indexOf` `join` `keys` `lastIndexOf` `map` `reduce` `reduceRight` `slice` `some` `toReversed` `toSorted` `toSpliced` `values` `with`, `length` |
| strings | `at` `charAt` `charCodeAt` `codePointAt` `concat` `endsWith` `includes` `indexOf` `isWellFormed` `padEnd` `padStart` `repeat` `replace` `replaceAll` `slice` `split` `startsWith` `substr` `substring` `toLowerCase` `toUpperCase` `toWellFormed` `trim` `trimEnd` `trimLeft` `trimRight` `trimStart`, `length`, indexing |
| regular expressions | literals (see [Regular expressions](#regular-expressions)); `test` `exec`, `source` `flags` `global` `ignoreCase` `multiline` `dotAll` `unicode` `unicodeSets` `sticky` `hasIndices`, `lastIndex` (always 0); with a string: `match` `matchAll` `search` (with a regular expression only), `replace` `replaceAll` `split` (with one or a string, and a limit that is a number, a boolean, `null` or `undefined`). A match runs in time linear in its input, with no backtracking, and pauses on fuel like any loop; where no thread is alive, a pattern that begins with an ASCII character, or with `^` past the start, skips the bytes that cannot begin a match, charging the fuel following them would. A match result is an array with `index`, `input`, `groups` and, with `d`, `indices`; a replacement is a string with `$&` `` $` `` `$'` `$$` `$n` `$<name>`, a primitive written as `String` writes it, or a function called once per match. A regular expression is written as `/source/flags`, `typeof` gives `"object"`, `JSON.stringify` `{}`, and a property it does not have `undefined` |
| sets | `has` `keys` `values` `entries` `forEach` `union` `intersection` `difference` `symmetricDifference` `isSubsetOf` `isSupersetOf` `isDisjointFrom`, `size` |
| maps | `get` `has` `keys` `values` `entries` `forEach`, `size` |
| iterators | `toArray` `map` `filter` `reduce` `some` `every` `find` `forEach` `flatMap` `take` `drop` |
| BigInts | `toString` (radix 2 to 36) |
| numbers | `toFixed` `toExponential` `toPrecision`: the exact decimal value of the double, rounded half up, as V8 gives it; digits 0 to 100 (precision 1 to 100), else a fault where JavaScript throws a RangeError; `toString` (radix 2 to 36): `String(x)` in radix 10, V8's digits in any other |
| functions | `String` `String.fromCharCode` `String.fromCodePoint` `String.raw` (called as a function: staticjs has no tagged templates) `Boolean` `BigInt` `BigInt.asIntN` `BigInt.asUintN` `Array.isArray` `Array.of` `Map.groupBy` `Object.keys` `Object.values` `Object.entries` `Object.fromEntries` `Object.hasOwn` `Object.assign` (see [Changing a value](#changing-a-value)) `Object.freeze` `Object.isFrozen` `Object.seal` `Object.isSealed` (see [Changing a value](#changing-a-value)) `Number.isInteger` `Number.isSafeInteger` `Number.isFinite` `Number.isNaN`, `Number` `parseInt` `parseFloat` (also as `Number.parseInt` and `Number.parseFloat`) `isNaN` `isFinite` `encodeURI` `encodeURIComponent` `decodeURI` `decodeURIComponent` |
| `JSON` | `parse` `stringify` (see [JSON](#json)) |
| `Temporal` | `Temporal.PlainDate`: `new Temporal.PlainDate(y, m, d)`, `from` (a string, a property bag or a PlainDate; `overflow`), `compare`; `year` `month` `monthCode` `day` `dayOfWeek` `dayOfYear` `weekOfYear` `yearOfWeek` `daysInWeek` `daysInMonth` `daysInYear` `monthsInYear` `inLeapYear` `calendarId` `era` `eraYear`; `with` `equals` `add` `subtract` (`overflow`) `until` `since` (`largestUnit`, `smallestUnit`, `roundingIncrement`, `roundingMode`) `toString` (`calendarName`) `toJSON` `toPlainDateTime`. `Temporal.PlainTime`: `new Temporal.PlainTime(h, m, s, ms, us, ns)`, `from` (a string, a property bag, a PlainTime or a PlainDateTime; `overflow`), `compare`; `hour` `minute` `second` `millisecond` `microsecond` `nanosecond`; `with` `equals` `add` `subtract` `until` `since` `round` (`smallestUnit`, `roundingIncrement`, `roundingMode`) `toString` (`fractionalSecondDigits`, `smallestUnit`, `roundingMode`) `toJSON`. `Temporal.PlainDateTime`: `new Temporal.PlainDateTime(y, m, d, h, m, s, ms, us, ns)`, `from` (a string, a property bag, a PlainDateTime or a PlainDate), `compare`; a PlainDate's getters and a PlainTime's; `with` `withPlainTime` `equals` `add` `subtract` `until` `since` `round` `toString` (and `calendarName`) `toJSON` `toPlainDate` `toPlainTime`. `Temporal.Duration`: `new Temporal.Duration(y, mo, w, d, h, mi, s, ms, us, ns)`, `from` (a string, a property bag or a Duration), `compare` (`relativeTo`); `years` to `nanoseconds` `sign` `blank`; `with` `negated` `abs` `add` `subtract` `round` (`largestUnit`, `smallestUnit`, `roundingIncrement`, `roundingMode`, `relativeTo`) `total` (`unit`, `relativeTo`) `toString` (`fractionalSecondDigits`, `smallestUnit`, `roundingMode`) `toJSON`. `Temporal.Instant`: `new Temporal.Instant(epochNanoseconds)`, `from` (a string with `Z` or an offset, or an Instant), `fromEpochMilliseconds` `fromEpochNanoseconds` `compare`; `epochMilliseconds` `epochNanoseconds`; `equals` `add` `subtract` `until` `since` `round` `toString` (`fractionalSecondDigits`, `smallestUnit`, `roundingMode`, `timeZone`: UTC or a fixed offset) `toJSON`. `Temporal.Now` (the host clock): `instant` `plainDateISO` `plainDateTimeISO` `plainTimeISO` (a time zone: UTC or a fixed offset) `timeZoneId` (see [Temporal](#temporal)) |
| `Date` | `new Date()` (the host clock), `new Date(value)` (a Date, a string of the date time string format, or a number), `Date.now` `Date.parse` `Date.UTC`; `getTime` `valueOf` `getUTCFullYear` `getUTCMonth` `getUTCDate` `getUTCDay` `getUTCHours` `getUTCMinutes` `getUTCSeconds` `getUTCMilliseconds` `toISOString` `toJSON` `toUTCString` `toGMTString`; `setTime` and the UTC setters, on a variable (see [Changing a value](#changing-a-value) and [Date](#date)) |
| `Math` | `abs` `min` `max` `floor` `ceil` `round` `trunc` `sign` `sqrt` `hypot` `fround` `clz32` `imul` `pow` `exp` `expm1` `log` `log1p` `log2` `log10` `sin` `cos` `tan` `asin` `acos` `atan` `atan2` `sinh` `cosh` `tanh` `asinh` `acosh` `atanh` `cbrt`, and its constants: each the same bits as V8 compiled without fused multiply-adds on every target, which is Node on x86_64 (Node on arm64 fuses them, and its last bit differs for some inputs), without a C library (`sqrt` is the hardware's correctly rounded instruction, `hypot` V8's own sum, `pow` V8's fdlibm, as `**` is, and so are `exp`, `expm1`, the logarithms, the trigonometric and hyperbolic functions and `cbrt`); `random`, V8's generator from the seed the host sets (`sjs_vm_set_seed`: the values `node --random-seed=<seed>` gives) |
| constants | `Number.EPSILON` `MAX_SAFE_INTEGER` `MIN_SAFE_INTEGER` `MAX_VALUE` `MIN_VALUE` `POSITIVE_INFINITY` `NEGATIVE_INFINITY` `NaN`; `Math.PI` `E` `LN2` `LN10` `LOG2E` `LOG10E` `SQRT2` `SQRT1_2`: number constants when compiled |
| changing a variable | arrays: `push` `pop` `shift` `unshift` `sort` `reverse` `fill` `splice`; sets: `add` `delete` `clear`; maps: `set` `delete` `clear`; iterators: `next` `return`; Dates: `setTime` `setUTCFullYear` `setUTCMonth` `setUTCDate` `setUTCHours` `setUTCMinutes` `setUTCSeconds` `setUTCMilliseconds`; objects: `Object.assign(x, ...sources)`; objects and arrays: `Object.freeze(x)` `Object.seal(x)` |

```js live
Object.groupBy(
  ["apple", "avocado", "banana", "blueberry", "cherry"],
  (word) => word[0],
)
```

## Changing a value

Values are never changed once shared. A function may still change a value
that one of its own variables holds alone, so the usual idioms work:

```js live
// The accumulator is the callback's alone:
// an assignment changes it in place.
[
  { region: "north", kwh: 2 },
  { region: "south", kwh: 1 },
  { region: "north", kwh: 3 },
].reduce((acc, r) => {
  acc[r.region] = (acc[r.region] ?? 0) + r.kwh;
  return acc;
}, {})
```

```js live
// A function's own const, changed by its methods.
((xs) => {
  const out = [];
  out.push(...xs.filter((x) => x > 0));
  out.sort((a, b) => a - b);
  return out;
})([3, -1, 2])
```

A frozen value refuses a change, where strict JavaScript would throw:

```js live fault
((xs) => { xs.push(4); return xs; })(Object.freeze([1, 2, 3]))
```

**Owned mutation.** A method that changes its receiver may be called on a
variable that the function owns: a parameter or a `const` of the function,
never captured by an arrow function, and never copied, passed or stored
before the call. `add`, a set's method and a Temporal value's (which
changes nothing), is checked when it runs instead: a Temporal value's
works on any receiver but a chain through a path, and a set's faults
unless the call is on a variable of the function that is the set's one
holder (design notes: [shared names](https://github.com/dhamidi/tinyactors/blob/main/staticjs/docs/internals/mutation.md#shared-names)). Such a call, or a chain of them (`s.add(1).add(2)`), may
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
(design notes: [appending to a let](https://github.com/dhamidi/tinyactors/blob/main/staticjs/docs/internals/mutation.md#appending-to-a-let)).
`Object.assign(x, ...)` changes such a variable's object, and may be a
statement; any other target must be a value nothing else holds
(`Object.assign({}, a, b)`), and faults otherwise (design notes:
[Object.assign](https://github.com/dhamidi/tinyactors/blob/main/staticjs/docs/internals/mutation.md#objectassign)).

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
state JavaScript leaves changeable (design notes: [frozen and sealed
values](https://github.com/dhamidi/tinyactors/blob/main/staticjs/docs/internals/mutation.md#frozen-and-sealed-values)).
The compiler
refuses any other receiver or target it can see is shared, and the
evaluator faults when the variable's value, or a container on the path,
has another holder: a caller, the embedder, a module's export, another
variable, or itself (`xs.push(xs)`, `o.self = o`). The rules and the check are in the design notes on [owned mutation](https://github.com/dhamidi/tinyactors/blob/main/staticjs/docs/internals/mutation.md).

## Functions from the host

The embedder can add functions (`sjs_function_native`), such as SCXML's
`In()`. They are the one way into a program from outside.

## Regular expressions

A regular expression is a literal, `/source/flags`; `RegExp` and
`new RegExp` are refused. A match runs on an automaton in time linear in
its input: a pattern such as `/^(a+)+$/` cannot backtrack catastrophically.
It pauses on fuel like any other loop. The table above lists the methods.

```js live
[
  "2024-01-31".match(/(?<y>\d{4})-(?<m>\d\d)-(?<d>\d\d)/).groups,
  [..."a1b22c333".matchAll(/\d+/g)].map((m) => [m[0], m.index]),
  "a,b;c".split(/[,;]/),
  /^(a+)+$/.test("a".repeat(40) + "b"),
]
```

Characters and escapes, `.`, `\d \w \s` and their negations, classes and
ranges, `^ $ \b \B`, every quantifier and its lazy form, groups,
non-capturing and named groups, alternation, and the flags `d g i m s u y`
match as in JavaScript. Without `u`, a match steps over UTF-16 code units;
with it, over code points; every index a method reports is in UTF-16 code
units.

Refused as unsupported: lookahead and lookbehind, backreferences, `\p{...}`,
the `v` flag, and `i` with a letter that is not ASCII. A pattern whose
automaton would need more than `max_regex_states` is a limit error when
compiled. Methods that would change `lastIndex` (`test` and `exec` with `g`
or `y`) fault, as a staticjs value cannot change; `lastIndex` is always 0.
[Faults and refusals](./faults.md) has the full list.

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
`**` of two numbers is V8's: its fdlibm `pow`, the same bits
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


```js live
[2n ** 64n, 20n * 19n * 18n * 17n * 16n, (10n ** 30n) / 7n, BigInt.asIntN(8, 255n)]
```

## Maps

A map is a value like a set: each key once, with its value, in insertion
order, reference counted, immutable once shared. Its storage is a set's: the
keys and their values alternate in one block, and the keys go through the
same insertion-ordered hash index once it is large, so a lookup
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
nothing else holds it (as for [updating a value in
place](performance.md#a-value-nothing-else-holds-is-updated-in-place)): a temporary (`[...m.keys()]`), or a variable's last read, which
moves its value: the last on its path, in each branch of an `if` or a
conditional (`c ? it.drop(1) : it`), and in a `let`'s reassignment from
itself (`it = it.map(f)`). Then nothing can read it again, and no program can tell
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
one held elsewhere they fault (design notes: [iterators](https://github.com/dhamidi/tinyactors/blob/main/staticjs/docs/internals/mutation.md#iterators)). A property an
iterator lacks is `undefined`. The writer writes an iterator as
`[...].values()`, an iterator that yields the same values.

`m.set(key, value)`, `m.delete(key)` and `m.clear()` change a map a
variable of the function owns, or a container on a path from one
(`state.byID.set(k, v)`), as owned mutation changes a set
([Changing a value](#changing-a-value)): `set` gives the map, so it chains
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


```js live
["apple", "avocado", "banana"].reduce((counts, word) => {
  counts.set(word[0], (counts.get(word[0]) ?? 0) + 1);
  return counts;
}, new Map())
```

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


```js live
JSON.stringify(
  JSON.parse('{"b": 1, "a": [true, null, 1e400], "10": "x"}'),
  null,
  2,
)
```

## Temporal

A Temporal value is an object, immutable, in the ISO 8601 calendar
(design notes: [Temporal](https://github.com/dhamidi/tinyactors/blob/main/staticjs/docs/internals/temporal.md)): `Temporal.PlainDate`,
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
and advances it with `sjs_vm_advance_clock` ([Embedding](./embedding.md#the-clock-and-the-random-seed)), and a program that reads it when it was never set faults. Its
time zone is UTC.


```js live
[
  Temporal.PlainDate.from("2024-01-31").add({ months: 1 }),
  Temporal.PlainDate.from("2024-01-01").until("2024-03-01").days,
  Temporal.Duration.from("PT90M").round({ largestUnit: "hour" }).toString(),
  Temporal.Now.plainDateISO().dayOfWeek,
]
```

## Date

A Date is a time value, whole milliseconds since 1970-01-01T00:00Z within
8.64 x 10^15 either way or NaN, in UTC only (design notes: [Date](https://github.com/dhamidi/tinyactors/blob/main/staticjs/docs/internals/date.md)).
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
a variable of the function that holds the Date alone ([changing a
value](#changing-a-value)), and give the new time value; on a temporary, a path or
an element they fault, and the local setters stay refused.

```js live
[new Date("2024-01-31T12:30:00Z").toISOString(), Date.UTC(2024, 1, 29), Date.now()]
```

The page sets the clock to 2024-01-31T10:00Z for its samples: a host always sets it.
