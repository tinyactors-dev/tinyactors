---
title: Faults and refusals
description: "Where staticjs stops instead of following JavaScript, and what it refuses when compiled."
---

# Faults and refusals

staticjs gives JavaScript's value or stops. It stops in two places:

- **When compiled**, a program is refused with an `sjs_error` whose code is
  `SJS_ERROR_SYNTAX` (JavaScript refuses it too), `SJS_ERROR_UNSUPPORTED`
  (valid JavaScript that staticjs leaves out), `SJS_ERROR_NAME` (a name bound
  nowhere, bound twice, not exported, or a module the host does not have) or
  `SJS_ERROR_LIMIT` (an `sjs_limits` value reached).
- **When it runs**, an evaluation ends with `SJS_RUN_FAULT` and an
  `sjs_error` whose code is `SJS_ERROR_FAULT`, where JavaScript would throw
  or where it would coerce, compare identities or reach a prototype;
  `SJS_ERROR_LIMIT` or `SJS_ERROR_MEMORY` when a limit or the allocator
  stops it.

Each error has a position (`at`: byte offset, line and column) and a fixed
English `message`; `subject` names the token or name concerned, when there
is one. [Embedding](./embedding.md) shows how to read them.

A fault is never a different value. Most programs that work in JavaScript
and avoid the cases below give the same value here: these lists are what
to look for when one does not.

Try them: each sample below stops where JavaScript would go on.

```js live fault
[1, 2] === [1, 2]
```

```js live fault
"5" * 2
```

```js live fault
[{ total: 1 }, null].map((order) => order.total)
```

```js live fault
JSON.parse('{"a": 1}', (key, value) => value)
```

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
  pair ([JSON](language.md#json)).
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
- Of regular expressions ([The language](./language.md#regular-expressions)): `exec` or `test` with `g`
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
- Of Temporal values ([The language](./language.md#temporal)): `===` on two, a set member or a
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
  (design notes: [Instant](https://github.com/dhamidi/tinyactors/blob/main/staticjs/docs/internals/temporal.md#instant)); `Temporal.Now` when the host never set
  the clock (JavaScript reads the machine's), with a time zone's name, or
  its `zonedDateTimeISO`.
- Of Dates ([The language](./language.md#date)): what reads or writes local time (`new Date(y, m)`
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

A loop is refused when compiled, even one that would end:

```js live fault
((xs) => {
  let sum = 0;
  for (const x of xs) sum += x;
  return sum;
})([1, 2, 3])
```

The same sum, with a built-in that walks the array:

```js live
((xs) => xs.reduce((sum, x) => sum + x, 0))([1, 2, 3])
```

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
function owns, and the bitwise forms: see
[Changing a value](language.md#changing-a-value)), and as a
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
checked when it runs: see [Changing a value](language.md#changing-a-value)); `break` and `continue` are syntax
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
`new RegExp`.

Errors come in the order JavaScript finds them only so far as a stage that
streams can know it. The compiler stops at the first error it meets; what it
can only decide at the end of a module (an `export { a }` the module never
declares, a name bound nowhere, an ambiguous `export *`) comes after
everything it met before, in the order syntax, then name, then unsupported.
So a module that uses what staticjs leaves out and later exports a name it
never declares is refused as unsupported, where JavaScript reports the
export.
