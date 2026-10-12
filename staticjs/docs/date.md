# Date

How staticjs holds and computes JavaScript's `Date`. Read `builtins.md`,
`extending.md` ("To add a value kind") and `temporal.md` first: a Date is
held as a value of the Temporal kind, and reads the host clock that
`Temporal.Now` reads. The code is `src/date.c`.

Scope: UTC only. A Date's time value is UTC in JavaScript too; what reads or
writes local time (the host's time zone) or the host's locale faults,
deliberately, as does a string JavaScript reads by heuristics of its own.
Everything else gives exactly what Node 22 gives with `TZ=UTC`.

## The representation

A Date is a time value: whole milliseconds since 1970-01-01T00:00Z, within
8.64 x 10^15 either way (+275760-09-13 to -271821-04-20), +0 never -0, or
NaN for an invalid date. It is a value of the kind `SJS_TEMPORAL` with the
type `SJS_TEMPORAL_DATE` in `h.flags` (after Temporal's five), its time
value the double `as.time` of `sjs_temporal`.

Why the Temporal kind and not a kind of its own: a Date is an object that
JavaScript compares by identity and converts through its methods, as a
Temporal value is, and every switch on kind already treats a Temporal value
that way. So without a line of its own:

- `typeof` is `"object"`; it is truthy; not a primitive. `===` on two
  faults (identity), and so do a set member and a map key; `<`, `-`, unary
  `+`, `+` with a string, a template, `String(d)` and `Number(d)` fault
  (JavaScript calls `valueOf` or `toString`, a conversion of an object).
- It has no own properties: `{ ...d }` is `{}`, `Object.hasOwn(d, k)` is
  `false`, `d.year` is `undefined`; `Object.keys(d)` and `in` fault, as on
  any value but an object or an array.
- Releasing it frees one block; nothing in it is a reference.

A new kind would have added a case to each of those switches, a number to
`sjs_kind` (and `SJS_VERSION_MINOR`), and nothing a program could tell
apart. What a Date has of its own goes through the type: its methods
(`METHOD_TYPES` in `temporal.c` gives each Date method the type's bit, and
`sjs_temporal_method` hands a Date's to `sjs_date_method`), the writer and
`JSON.stringify` (`sjs_temporal_text`, which gives a Date's ISO string, or
nothing for an invalid date). None of Temporal's getters or methods is a
Date's but the names they share: `toJSON`, `toLocaleString`, `toString`
and `valueOf`.

A Date is the one value of the kind that changes: its setters ("Setters",
below; the owned-mutation rules, docs/mutation.md) write `as.time` of a
Date that one variable of the function holds alone. Every Temporal type
stays immutable.

The writer writes a Date as JavaScript that makes an equal one:
`new Date("2024-01-31T00:00:00.000Z")` (its `toISOString()`), and
`new Date(NaN)` for an invalid date; diff-node's `show()` writes the same.
`JSON.stringify` writes its ISO string, quoted, or `null` for an invalid
date (its `toJSON`).

## Functions

`Date` is a namespace (`NS_DATE` in `src/compiler.c`) with `now`, `parse`
and `UTC`; alone it is a function, as `String` is, which faults when
called. `new Date(...)` is the constructor `sjs_builtin_lookup("new",
"Date")`, as `new Temporal.PlainDate(...)` is; a program that binds `Date`
itself is unsupported there.

- **`Date.now()` and `new Date()`** read the host clock (`vm->clock`,
  docs/temporal.md "Host clock") in milliseconds toward negative infinity:
  its seconds times 1,000 plus its whole milliseconds past them, so a
  nanosecond before the epoch is -1. Never set, they fault, as
  `Temporal.Now` does. The clock's limits are a Date's.
- **`new Date(value)`**: a Date's time value (a copy); a string read as
  `Date.parse` reads it; anything else TimeClip of ToNumber: a number, a
  boolean, `null` (0) or `undefined` (NaN), truncated toward zero, NaN past
  8.64 x 10^15, -0 made +0. An object, an array or a Temporal value
  faults (JavaScript converts it to a primitive; a Temporal value's
  `valueOf` throws), and a BigInt (a TypeError).
- **`new Date(y, m, ...)`** with two or more arguments reads its fields in
  local time: it faults, deliberately. `new Date(Date.UTC(y, m, ...))` is
  the UTC one.
- **`Date()`** called as a function gives the date and time as a local time
  string: it faults, deliberately, whatever its arguments.
- **`Date.UTC(year, month, date, hours, minutes, seconds, ms)`** follows
  V8's `MakeDay`, `MakeTime` and `MakeDate` (src/date/date.cc) operation for
  operation: each argument ToNumber (a string or an object faults; the
  arguments past seven are not read), the year NaN without one, the month
  0, the date 1 and the rest 0; a year that truncates to 0 to 99 is 1900
  plus it. MakeDay takes a year within 10^6 and a month within 10^7 either
  way (else NaN, as V8 gives where the specification would go on), carries
  the month into the year, and counts the days to the month's first in int
  arithmetic as V8 does (C's division truncates where a year past the
  limits makes the count negative, so a date that brings it back within
  the limits gives V8's value); MakeTime is `h * 3600000 + m * 60000 +
  s * 1000 + ms` in doubles, each truncated, NaN if one is not finite;
  MakeDate `time + day * 86400000` (just `day * 86400000` when the time is
  0); then TimeClip.

## Strings: Date.parse and new Date(string)

staticjs reads the ECMAScript Date Time String Format and nothing else
(`date_parse`):

- a date: `YYYY`, `YYYY-MM` or `YYYY-MM-DD`, the year four digits or a sign
  and six (`+002024`, `-000001`; `-000000` is not a year), the month 01 to
  12 and the day one its month has;
- alone, a date is UTC (`"2024-01-31"` is midnight UTC);
- or a date, `T`, `HH:mm`, `HH:mm:ss` or `HH:mm:ss.sss` (hours 00 to 24,
  24 only as `24:00`, `24:00:00` or `24:00:00.000`, the next midnight;
  minutes and seconds 00 to 59; exactly three digits of milliseconds),
  then `Z` or an offset `+HH:mm` or `-HH:mm` (hours 00 to 23), applied.

What it gives is exact: the days to the date (`date_days_from`) times
86,400,000 plus the time, minus the offset, in an int64; NaN past the
limits, as JavaScript gives (`"+275760-09-13T00:00:00.001Z"`,
`"+999999-12-31"`). That is the one NaN staticjs gives for a string.

Every other string faults:

- **A date and time without `Z` or an offset** is local time in
  JavaScript (a date alone is UTC, a date and time local: the
  specification's choice): it faults, deliberately, rather than read the
  host's zone or assume one. With `TZ=UTC` Node reads it as UTC, but a
  program's value must not depend on the host.
- **A string outside the format**: Node 22 reads many by V8's legacy
  heuristics (`"Jan 31 2024"`, `"2024/01/31"`, `"2024-1-31"`, RFC 2822,
  `"2024-01-31 12:30Z"`, a lowercase `t` or `z`, one or four digits of a
  fraction, even `"-000000-01-01"`, which it reads as 2001), and gives NaN
  for others (`""`, `"2024-13-01"`). Which is which is V8's to say, not a
  grammar staticjs can promise, so each faults, deliberately. So does a
  day past its month's end (`"2024-02-30"`), which V8 moves into the next
  month.
- `Date.parse` of anything but a string faults (JavaScript converts it to
  one first: `Date.parse(2024)` is 2024's first instant).

## Methods

| Method | Gives |
|---|---|
| `getTime`, `valueOf` | the time value (NaN for an invalid date) |
| `getUTCFullYear`, `getUTCMonth` (0 to 11), `getUTCDate`, `getUTCDay` (0 for Sunday), `getUTCHours`, `getUTCMinutes`, `getUTCSeconds`, `getUTCMilliseconds` | the field in UTC, from the days toward negative infinity and the milliseconds past them (`utc_fields_of`); NaN for an invalid date |
| `toISOString` | `YYYY-MM-DDTHH:mm:ss.sssZ`, a year outside 0 to 9999 as a sign and six digits; an invalid date faults (a RangeError) |
| `toJSON` | `toISOString`'s string, or `null` for an invalid date; its argument is not read |
| `toUTCString`, `toGMTString` (the same function) | `"Wed, 31 Jan 2024 12:34:56 GMT"`, the year in at least four digits after its sign (`-0001`), as V8 writes it; `"Invalid Date"` for an invalid date |

Refused, each a name of `NAMES` whose method faults (as a PlainDate's
`withCalendar` does):

- **Local time** (`SJS_METHOD_TEMPORAL_DATE_LOCAL`, and `toString`):
  `getFullYear`, `getMonth`, `getDate`, `getDay`, `getHours`,
  `getMinutes`, `getSeconds`, `getMilliseconds`, `getYear`,
  `getTimezoneOffset`, `toString`, `toDateString`, `toTimeString`,
  `toLocaleDateString`, `toLocaleTimeString`; and the local setters,
  `setFullYear`, `setMonth`, `setDate`, `setHours`, `setMinutes`,
  `setSeconds`, `setMilliseconds` and `setYear`, which read the local
  fields they keep. Each faults ("reads local time"); a local setter as a
  statement is refused when compiled, as any call that changes no
  variable is ("has no effect").
- **Locale data**: `toLocaleString`.

`toTemporalInstant` is not in Node 22: staticjs does not have it, and
calling it faults (a TypeError in Node). A Date and an Instant meet
through numbers: `Temporal.Instant.fromEpochMilliseconds(d.getTime())`
and `new Date(instant.epochMilliseconds)`. A Date where Temporal reads a
value (`Temporal.Instant.from(d)`, `Temporal.PlainDate.from(d)`) faults:
JavaScript converts it to its local time string, or reads it as a bag of
no fields, and throws.

## Setters

`setTime` and the UTC setters change their receiver, so they are
mutations (docs/mutation.md): called through `SJS_OP_MUTATE` on a variable
of the function that holds the Date alone, they write its time value
(`as.time`, one double in the Date's own block) in place and give it, a
number, not the receiver. The code is `sjs_date_set` (`src/date.c`), tagged
`MUTATION`; its cases are in `date.spec` (the arithmetic) and
`mutation.spec` (the forms).

| Setter | Sets | Keeps |
|---|---|---|
| `setTime(t)` | the time value: TimeClip of `t` | |
| `setUTCFullYear(y, m?, d?)` | the year, the month, the date | the time of day |
| `setUTCMonth(m, d?)` | the month, the date | the year, the time of day |
| `setUTCDate(d)` | the date | the year and month, the time of day |
| `setUTCHours(h, mi?, s?, ms?)` | the hours to milliseconds | the day |
| `setUTCMinutes(mi, s?, ms?)` | the minutes to milliseconds | the day, the hours |
| `setUTCSeconds(s, ms?)` | the seconds and milliseconds | the day to the minutes |
| `setUTCMilliseconds(ms)` | the milliseconds | the rest |

As V8 computes them (`src/builtins/builtins-date.cc`), so every value is
Node's:

- **Arguments:** each ToNumber, as `Date.UTC` reads them: a number, a
  boolean, `null` (0) or `undefined` (NaN); a string or an object faults
  (JavaScript converts it: `builtins.md`, section 7), and a BigInt (a
  TypeError). An argument left out keeps the Date's field; one given as
  `undefined` is NaN. The first is always read, given or not
  (`d.setUTCDate()` is NaN); those past the setter's last field are not
  read.
- **An invalid date** gives NaN and stays invalid, but for `setTime`,
  which sets a new time value, and `setUTCFullYear`, which starts from
  +0 (1970-01-01T00:00Z: month 0, date 1, midnight). staticjs converts
  every argument first, so a string or an object faults on an invalid
  date too, where V8 skips the later ones.
- **Then** the fields kept (`utc_fields_of`, the day toward negative
  infinity and the milliseconds past it) and the fields given go through
  `date_make_day` or `date_make_time` (V8's MakeDay and MakeTime: fractions
  truncated, a month or a date, an hour or a minute past its range carried,
  a negative one borrowed, a non-finite one NaN, a year past a million NaN,
  without `Date.UTC`'s 1900 to 1999 mapping), `date_make_date` and
  `date_clip` (NaN past 8.64 x 10^15 either way, -0 made +0).

**The forms** are those of an array's `push` (docs/mutation.md): the
compiler refuses a Date copied, stored, passed or captured (before or
after) as `unsupported`, and the evaluator faults "held elsewhere" when
the variable's Date has another holder (a caller's argument, a reduce's
initial value a variable still holds, the Date given as its own
argument). The setters' names have no namesake of another kind (a
Temporal type's), so the shared-name rules do not apply
(docs/mutation.md, "Shared names"). A setter called as any method is (on
a temporary, `new Date(0).setTime(1)`; on an element, `xs[0].setTime(1)`;
on a path, `o.d.setUTCDate(1)`, which the shape rules' `MUTATE_PATH` does
not name; a chain, `d.setTime(1).setTime(2)`, calls the next on a number)
faults, deliberately where JavaScript gives a value: its method in
`NAMES`' Temporal column is `SJS_METHOD_TEMPORAL_DATE_SETTER`, and the
mutation, its `mutation` column, which `sjs_mutation_of` gives for a Date
alone (another Temporal value has no setter: "does not have this
method"). A setter as a statement changes its variable, as `xs.push(x);`
does. A reduce's accumulator is the callback's alone, so
`xs.reduce((d, x) => { d.setUTCDate(x); return d; }, new Date(0))` sets it.

Fuel: a unit for the call.

## Testing

- `tests/spec/date.spec`: cases written by hand, every value from Node 22
  with `TZ=UTC`; `@clock` for `Date.now()` and `new Date()`. A case named
  "deliberate" faults where JavaScript gives a value. The setters'
  arithmetic is there, each with each count of arguments; their forms
  (held elsewhere, a path, a reduce's accumulator) are in `mutation.spec`.
- `tests/temporal_test.c` (its Date sweeps): `Date.parse` over a cross of
  years, dates, times and zones of the format and beside it, and each part
  varied around three strings (about 5,800 strings; one the generator
  reads as outside the format is a deliberate fault, so a fault anywhere
  else, or a value there, fails); `Date.UTC` over years around 0, 1900 to
  1999, the limits and V8's ranges, by months and days that balance, and
  each time field alone (about 9,400); and the getters, `toJSON`,
  `toISOString` and `toUTCString` over times at the ends of years and of
  February in years around 0, the centuries and the limits. diff-node
  evaluates each with Node's own `Date`.
- diff-node replaces the global `Date` the cases see with a subclass whose
  `Date.now()` and `new Date()` give the `@clock`'s milliseconds (floored),
  and throw in a case without one; every other use is Node's.
