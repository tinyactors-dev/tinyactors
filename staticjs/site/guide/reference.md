---
title: Quick reference
description: "staticjs on one page: the two kinds of program, what to write instead of what it leaves out, the built-ins by group, and the C calls in the order a host makes them."
---

# Quick reference

The whole language and API on one page. Each section ends with a link to
the full account in the guide.

## A program

A program is one of two kinds. An **expression** is read as JavaScript reads
`(` source `)`. A **module** holds imports, `const` declarations and exports,
and its value is the object of its exports.
[More](./language.md#expressions-and-statements)

```js live
[3, 1, 2].toSorted((a, b) => a - b).map((n) => n * 10)
```

```js live module
export const greeting = "hello";
export default greeting.toUpperCase();
```

## What to write instead

staticjs leaves out the parts of JavaScript that loop without end, change a
shared value or have an effect. Each has a form that says the same thing.
A program that uses one of them is refused when compiled, as
`unsupported`. [More](./faults.md#refused-when-compiled)

| Instead of                                   | write                                                        |
|----------------------------------------------|--------------------------------------------------------------|
| `for`, `while`, `for…of`                     | `map`, `filter`, `reduce`, `some`, `every`, `find`, `flatMap` |
| `let` with a running total                   | `reduce`, or a `const` per step                              |
| `switch`                                     | `?:`, `if` with `return`, or an object of cases              |
| `throw`                                      | return a value that says what went wrong                     |
| `function f() {}`                            | `const f = () => {}`                                         |
| `class`                                      | an object literal, and functions that take it                |
| `Array(n)`, `new Array(n)`                   | `Array.from({ length: n }, (_, i) => i)`                     |
| `a.push(x)` on a value it was given          | `[...a, x]`, or push to an array the function made itself    |
| `x == y`, `"5" * 2`                          | `===`, and `Number(s)` before arithmetic                      |

A running total, without a loop or a `let`:

```js live
[12, 5, 30].reduce((sum, price) => sum + price, 0)
```

A `switch`, as an object of cases:

```js live
((status) => ({ open: "Waiting", paid: "Done", void: "Cancelled" })[status] ?? "Unknown")("paid")
```

A list of a given length:

```js live
Array.from({ length: 5 }, (_, i) => i * i)
```

## Where it stops

Where JavaScript would convert a value, compare two objects by identity or
look something up on a prototype, staticjs stops with a fault rather than
guess. [More](./faults.md)

```js live fault
[1, 2] === [1, 2]
```

## Built-ins

| Group               | Has                                                                                  |
|---------------------|--------------------------------------------------------------------------------------|
| Arrays              | `map` `filter` `reduce` `find` `toSorted` `with` `flat` …; `push` `sort` `splice` … on an array the function owns |
| Strings             | `slice` `split` `replace` `padStart` `trim` `includes` …, templates; nothing that depends on a locale |
| Sets and maps       | `new Set` `new Map`, the set algebra (`union` `intersection` …), `Map.groupBy`        |
| Objects             | `Object.keys` `values` `entries` `fromEntries` `groupBy` `assign` `freeze` …         |
| Numbers and `Math`  | the whole of `Math` (V8's bits), `toFixed` `toPrecision`, `parseInt` `parseFloat`    |
| BigInt              | literals, arithmetic, `BigInt()`, `asIntN` `asUintN`                                 |
| Text                | regular expressions (linear time), `JSON`, `encodeURI…`                              |
| Time                | `Temporal` (ISO calendar, UTC and fixed offsets) and `Date` (UTC), on the host's clock |

The full list, with each method, is under
[Built-ins](./language.md#built-ins).

## The C API, in the order a host calls it

| Step                     | Calls                                                                  |
|--------------------------|------------------------------------------------------------------------|
| Make a context           | `sjs_context_init`, with an allocator and `sjs_limits`                 |
| Compile                  | `sjs_compile`; or `sjs_parser_push` into `sjs_compiler_sink` to stream |
| Prepare an evaluation    | `sjs_vm_init`, `sjs_vm_start`; `sjs_vm_set_clock`, `sjs_vm_set_seed`   |
| Run on fuel              | `sjs_vm_run(vm, fuel)` until it is `SJS_RUN_DONE` or `SJS_RUN_FAULT`   |
| Read the result          | `sjs_vm_take_result`, or `sjs_vm_error` and its `sjs_error`            |
| Read a value             | `sjs_length`, `sjs_array_at`, `sjs_object_get`, `sjs_string_text`, …    |
| Write it as text         | `sjs_writer_init`, `sjs_writer_write` into an `sjs_buffer`             |
| Let go                   | `sjs_release`, `sjs_program_release`, `sjs_vm_destroy`, `sjs_context_destroy` |

Each declaration in
[`staticjs.h`](https://github.com/tinyactors-dev/tinyactors/blob/main/staticjs/include/staticjs.h)
has a comment, and [Embedding](./embedding.md) explains each step.
