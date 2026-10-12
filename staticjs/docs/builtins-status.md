# Built-in status

Every JavaScript built-in in scope, with its status and the reason for it. The
registered-or-not column comes from the name table (`NAMES`) and the function
table (`FUNCTION_TABLE`) in `src/builtins.c`; `mise run builtin-table dump`
prints both. The plan behind the "to build" and "defer" rows is the agreed
list in this file's history, not a commitment to a date.

Statuses:

- `built`: registered and implemented.
- `named, not built`: registered in the name table with no method behind it.
- `not registered`: not in either table.
- `to build`: agreed for the next work.
- `build later`: agreed, after the `to build` rows.
- `defer`: exact in principle, but costly; decided later.
- `refused`: refused when compiled or called, with the reason given.

Rules behind the refusals are in `README.md` ("Refused when compiled") and
`docs/extending.md`. The clock is host-provided: a read-only value the host
sets and advances with a call. `Date.now()`, `new Date()` with no arguments
and `Temporal.Now` read that value and nothing else. `Math.random` reads a seed
the host injects in the same way. UTC is the default everywhere.

## Object

| Name | Status | Note |
|---|---|---|
| `keys`, `values`, `entries`, `fromEntries`, `groupBy`, `hasOwn` | built | `hasOwn` of `String`, `Boolean`, `Number` or `BigInt` faults on a key other than `length` and `name`. |
| `assign` | built | Mutates its target, under the owned-mutation rules (`docs/mutation.md`, "Object.assign"): a variable the function owns, or a target nothing else holds; faults on a shared target, an array, map or function target with something to put, and a primitive target. |
| `freeze`, `isFrozen`, `seal`, `isSealed` | build later | Pending a decision on writes to a frozen object. |
| `is` | refused | Listed in `docs/extending.md` as a case not to write. |
| `create`, `getPrototypeOf`, `setPrototypeOf`, `defineProperty`, `defineProperties`, `getOwnPropertyDescriptor`, `getOwnPropertyDescriptors`, `getOwnPropertyNames`, `getOwnPropertySymbols`, `preventExtensions`, `isExtensible` | not registered | Refused: prototype machinery. staticjs has no prototype chains and no property descriptors. |

## Map

| Name | Status | Note |
|---|---|---|
| constructor, `size`, `get`, `set`, `has`, `delete`, `clear`, `keys`, `values`, `entries`, `forEach`, `groupBy` | built | |

## Set

| Name | Status | Note |
|---|---|---|
| constructor, `size`, `add`, `has`, `delete`, `clear`, `union`, `intersection`, `difference`, `symmetricDifference`, `isSubsetOf`, `isSupersetOf`, `isDisjointFrom` | built | The set methods take only a set as their argument: a map, which JavaScript reads as set-like (its keys), faults (`builtins.spec`). |
| `keys`, `values`, `entries` on a set | named, not built | Each faults ("staticjs does not have this method"), so the iterator helpers cannot start from a set; spread the set into an array instead. |
| `forEach` on a set | named, not built | The README lists `forEach` for maps only; check with `builtin-table`. |

## String

| Name | Status | Note |
|---|---|---|
| `at`, `charAt`, `charCodeAt`, `codePointAt`, `concat`, `endsWith`, `includes`, `indexOf`, `lastIndexOf`, `padEnd`, `padStart`, `repeat`, `replace`, `replaceAll`, `slice`, `split`, `startsWith`, `toLowerCase`, `toUpperCase`, `trim`, `trimStart`, `trimEnd`, `match`, `matchAll`, `search`, `length`, indexing | built | Regex forms as in `docs/regex.md`. |
| `substring`, `substr` | built | By UTF-16 code unit, arguments converted as `slice` converts them; a cut through a surrogate pair faults. |
| `isWellFormed`, `toWellFormed` | built | Always true and the string itself: staticjs cannot hold a lone surrogate. |
| `trimLeft`, `trimRight` | named, not built | Legacy aliases of `trimStart` and `trimEnd`; not in the agreed plan. |
| `localeCompare`, `toLocaleLowerCase`, `toLocaleUpperCase` | refused | Depend on locale data. Registered, so refused explicitly. |
| `normalize` | refused | Needs Unicode normalisation tables. |
| `anchor`, `big`, `blink`, `bold`, `fixed`, `fontcolor`, `fontsize`, `italics`, `link`, `small`, `strike`, `sub`, `sup` | refused | Deprecated HTML methods: pure, but not worth the code. Registered, so refused explicitly. |
| `fromCharCode`, `fromCodePoint`, `raw` | built | Called as functions; `String.raw` is not a tag (no tagged templates). |
| template literals, plain and with `${}` | built | Tagged templates refused. |

## Number

| Name | Status | Note |
|---|---|---|
| `isInteger`, `isSafeInteger`, `isFinite`, `isNaN`, `parseInt`, `parseFloat`, `Number()` | built | |
| `toString(radix)` on a number | named, not built | The name is registered for BigInts only: `(255).toString(16)` faults (`numbers.spec`, "toString of a number, which staticjs does not have"). |
| constants (`EPSILON`, `MAX_SAFE_INTEGER`, …) | built | As listed in the README. |
| `toFixed`, `toPrecision`, `toExponential` | built | The exact decimal value of the double, rounded half up, without libm (`sjs_number_format_digits`); swept against Node on a fixed list of doubles and digit counts (`number_test --print`, tag P). Out of range faults where JavaScript throws a RangeError; a string or object argument faults. |
| `toLocaleString` | refused | Locale data. |

## Math

| Name | Status | Note |
|---|---|---|
| `abs`, `min`, `max`, `floor`, `ceil`, `round`, `trunc`, `sign`, `sqrt`, `hypot`, `fround`, `clz32`, `imul`, and constants | built | |
| `random` | to build | Reads a seed the host injects, like the clock. |
| `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `asinh`, `acosh`, `atanh`, `exp`, `expm1`, `log`, `log1p`, `log2`, `log10`, `cbrt`, `pow` | defer | Not registered. Must match V8 bit for bit without libm; each needs its own algorithm and a sweep. |
| `f16round`, `sumPrecise` | not registered | Recent additions, not in the agreed plan; decide when the trig group is decided. |

## Array

| Name | Status | Note |
|---|---|---|
| `from`, `isArray`, `of` | built | |
| `at`, `concat`, `every`, `filter`, `find`, `findIndex`, `findLast`, `findLastIndex`, `flat`, `flatMap`, `includes`, `indexOf`, `join`, `lastIndexOf`, `map`, `reduce`, `slice`, `some`, `toReversed`, `toSorted`, `toSpliced`, `with`, `keys`, `values`, `entries`, `forEach`, `length` | built | `keys`, `values`, `entries` as listed in the README. |
| `push`, `pop`, `shift`, `unshift`, `sort`, `reverse`, `fill`, `splice` | built | Changing a variable, under the owned-mutation rules. |
| `copyWithin` | build later | Registered, no method; mutates its receiver. |
| `reduceRight` | named, not built | Not in the agreed plan. |
| `new Array(...)` | refused | Array holes cannot be represented. |

## JSON

| Name | Status | Note |
|---|---|---|
| `parse`, `stringify` | built | |
| `parse` reviver, `stringify` replacer function | build later | Calls back from inside the serializer or parser. |
| `toJSON` on a value | build later | Agreed for `JSON.stringify`; a `toJSON` method is a user hook, so it is also covered by the protocols row. |

## BigInt

| Name | Status | Note |
|---|---|---|
| `BigInt()`, `asIntN`, `asUintN`, `toString` | built | |
| `toLocaleString` | refused | Locale data. |

## Iterators

| Name | Status | Note |
|---|---|---|
| `toArray`, `map`, `filter`, `reduce`, `take`, `drop`, `flatMap`, `some`, `every`, `find`, `forEach` | built | Run when called, not lazily: a callback of `map`, `filter` or `flatMap` can fault on a value a later `take`, `some`, `every` or `find` would not read. `take` and `drop` fault on a string or an object limit. |
| `next`, `return` | named, not built | Not in the agreed plan. Calling `next` by hand advances a held iterator, which is a mutation of a held value. |

## RegExp

| Name | Status | Note |
|---|---|---|
| literals, `test`, `exec`, `source`, `flags`, `global`, `ignoreCase`, `multiline`, `dotAll`, `unicode`, `unicodeSets`, `sticky`, `hasIndices`, `lastIndex` | built | As in `docs/regex.md`. |
| `match`, `matchAll`, `search`, `replace`, `replaceAll`, `split` | built | |
| `compile` | refused | Legacy: changes a regex in place. Regex values are immutable here. |
| `new RegExp(...)` | refused | Builds a pattern at run time; a name refusal. |
| lookahead, lookbehind, backreferences, `\p{}`, `v` flag, non-ASCII `i` | refused | As in `docs/regex.md`. |

## Date and Temporal

| Name | Status | Note |
|---|---|---|
| `Temporal.PlainDate`: constructor, `from`, `compare`, `year`, `month`, `monthCode`, `day`, `dayOfWeek`, `dayOfYear`, `weekOfYear`, `yearOfWeek`, `daysInWeek`, `daysInMonth`, `daysInYear`, `monthsInYear`, `inLeapYear`, calendarId, `era`, `eraYear`, `with`, `equals`, `add`, `subtract`, `until`, `since`, `toString`, `toJSON`, `toPlainDateTime` | built | ISO calendar only (docs/temporal.md): an immutable value, written as `Temporal.PlainDate.from("2024-01-31")`; `JSON.stringify` writes its ISO string. Strings parsed as both polyfills read them; another calendar, a string, array or object JavaScript would convert, and `===` fault. |
| `Temporal.PlainDate`, `PlainTime`, `PlainDateTime`: `add`, `subtract` (`overflow`), `until`, `since` (`largestUnit`, `smallestUnit`, `roundingIncrement`, `roundingMode`) | built | docs/temporal.md: a date adds years and months, the day constrained to the month (or rejected), then weeks and days; a time wraps around the clock. Differences in the units of the type, rounded as the 2024 revision rounds (every unit, increment and mode). |
| `Temporal.PlainDate`: `toLocaleString`, `valueOf`, `toZonedDateTime`, `toPlainYearMonth`, `toPlainMonthDay`, `withCalendar` | refused | Locale data; `valueOf` throws in JavaScript; no ZonedDateTime, PlainYearMonth or PlainMonthDay, and only the ISO calendar. Registered, so each faults. |
| `Temporal.PlainTime`: constructor, `from`, `compare`, `hour`, `minute`, `second`, `millisecond`, `microsecond`, `nanosecond`, `with`, `equals`, `add`, `subtract`, `until`, `since`, `round`, `toString`, `toJSON` | built | docs/temporal.md: written as `Temporal.PlainTime.from("12:30:00")`. Strings parsed as both polyfills read them (a time alone, or a date and time's); `round` and `toString` take every unit, increment and rounding mode JavaScript does; `overflow` on bags. |
| `Temporal.PlainDateTime`: constructor, `from`, `compare`, the getters of a PlainDate and of a PlainTime, `with`, `withPlainTime`, `equals`, `add`, `subtract`, `until`, `since`, `round`, `toString`, `toJSON`, `toPlainDate`, `toPlainTime` | built | ISO calendar only (docs/temporal.md): written as `Temporal.PlainDateTime.from("2024-01-31T12:30:00")`, within Temporal's limits (a nanosecond past -271821-04-19T00:00 to +275760-09-13T23:59:59.999999999), which rounding must keep. |
| `Temporal.PlainTime`, `PlainDateTime`: `toLocaleString`, `valueOf`; `toZonedDateTime`, `withCalendar` of a PlainDateTime | refused | As a PlainDate's. Registered, so each faults. |
| `Temporal.Duration`: constructor, `from`, `compare` (`relativeTo`), `years` to `nanoseconds`, `sign`, `blank`, `with`, `negated`, `abs`, `add`, `subtract`, `round`, `total`, `toString`, `toJSON` | built | docs/temporal.md: written as `Temporal.Duration.from("P1DT2H")`; exact to 2^53 seconds (wide integers, no 128-bit type); `round` and `total` relative to a PlainDate, a PlainDateTime, a string or a bag (the 2024 revision's rounding), every unit, increment and mode; a ZonedDateTime `relativeTo` (a time zone) faults. |
| `Temporal.Duration`: `toLocaleString`, `valueOf` | refused | Locale data; `valueOf` throws in JavaScript. Registered, so each faults. |
| `Temporal.Instant` | to build | UTC. An immutable value type, on the design of docs/temporal.md. |
| `Temporal.Now` (`instant`, `plainDateISO`, `plainDateTimeISO`, `plainTimeISO`) | to build | Read only the host clock. |
| `Temporal.ZonedDateTime`, IANA time zones | defer | Needs time zone data, unless the host supplies offsets. |
| `Date.UTC`, `Date.parse` (ISO strings), `toISOString`, UTC getters | build later | Follows Temporal. |
| `Date.now()`, `new Date()` with no arguments | build later | Read only the host clock. |
| `Date` local-time methods | refused | Need a time zone the host has not declared. |
| `Date` setters | build later | Mutate their receiver, so they fall under the owned-mutation rules. |

## Global

| Name | Status | Note |
|---|---|---|
| `parseInt`, `parseFloat`, `isNaN`, `isFinite`, `encodeURI`, `encodeURIComponent`, `decodeURI`, `decodeURIComponent` | built | |
| `escape`, `unescape` | build later | Annex B; pure and cheap. Not registered. |
| `globalThis` | refused | Gives access to every global; no ambient state. |
| `console`, `setTimeout` | refused | I/O and time. |
| `Boolean`, `Number`, `String`, `BigInt`, `Map`, `Set`, `Array`, `Object` as functions | built | `Array()` and `Object()` as functions are refused where they would need holes or prototypes. |

## Protocols

These are refused. The reason is in the rules, not in a missing representation.
A user iterator needs state that survives across `next` calls. That state would
have to be changed by a user method that changes its receiver, and the
ownership rules do not allow that for user methods today. Making it allowed is
a new rule set, not a fix to the value model. A fixed-key stand-in for
`Symbol.iterator` was considered and rejected, because it is not the real
protocol: programs written for JavaScript's protocol would still fail.

| Name | Status | Note |
|---|---|---|
| user iterables (`[Symbol.iterator]`) | refused | See above. Built-in iteration over arrays, strings, sets, maps and iterator values stays. |
| `Symbol.asyncIterator`, async iteration | refused | No `Promise`, no `async`. |
| `Symbol.toPrimitive`, `valueOf`, `toString` on a user object | refused | Coercion of an object faults; staticjs stops where JavaScript would call a user method. |
| `Symbol.hasInstance`, `instanceof` | refused | Needs prototype chains. |
| `Symbol.toStringTag` | refused | A user object could change how it prints. |
| `Symbol.species` | refused | No classes. |
| `Symbol.isConcatSpreadable` | refused | A user object could change `concat`. |
| `Symbol.match`, `matchAll`, `replace`, `search`, `split` | refused | A user object could change what the regex and string methods do. |
| `Symbol.unscopables` | refused | Depends on `with`. |
| `then` (thenable) | refused | No `Promise`, so nothing calls `then`. Plain data named `then` is fine. |
| `__defineGetter__`, `__defineSetter__`, `__lookupGetter__`, `__lookupSetter__`, `__proto__`, `hasOwnProperty`, `isPrototypeOf`, `propertyIsEnumerable`, `constructor` | refused | Prototype machinery. |
| `valueOf` (user object) | refused | Coercion hook; see above. |

## Symbol

| Name | Status | Note |
|---|---|---|
| `Symbol()`, `Symbol.for`, `Symbol.keyFor`, `description`, well-known symbols | refused | No symbol keys, no protocol hooks, and `Symbol.for` is a registry shared across the runtime. |

## Promise, async and generators

| Name | Status | Note |
|---|---|---|
| `Promise`, `async`, `await`, `function*`, `yield` | refused | Out of scope: they need scheduling and suspended state across host calls. |

## Out of scope

| Name | Status | Note |
|---|---|---|
| `Proxy`, `Reflect` | refused | Intercept object operations; out of the value model. |
| `Intl` | refused | Locale data. |
| `WeakMap`, `WeakSet`, `WeakRef`, `FinalizationRegistry` | refused | Reference semantics the value model cannot express. |
| `Atomics`, `SharedArrayBuffer` | refused | Shared mutable memory. |
| `structuredClone` | refused | Needs object identity across a copy. |
| `ArrayBuffer`, `DataView`, typed arrays | defer | A new value kind; the value model may not fit it. Decide with the value-kind design. |
| `Error`, `TypeError`, `RangeError`, `SyntaxError`, `URIError`, … | refused | staticjs faults rather than throwing objects. |
| `Function` constructor, `eval` | refused | Compiles code at run time. |
