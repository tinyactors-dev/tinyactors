---
layout: page
sidebar: false
title: staticjs
description: "A pure, total subset of JavaScript, as a freestanding C library and a WebAssembly module."
---

<HomePage>
<template #demo>

```js live
[
  { id: 1, open: true, total: 120, due: "2024-02-10" },
  { id: 2, open: false, total: 80, due: "2024-01-10" },
  { id: 3, open: true, total: 300, due: "2024-02-25" },
]
  .filter((order) => order.open)
  .toSorted((a, b) => b.total - a.total)
  .map((order) => ({
    id: order.id,
    late: Temporal.PlainDate.from(order.due)
      .until("2024-03-01").days,
  }))
```

</template>
</HomePage>

<!-- markdown
staticjs is a pure, total subset of JavaScript, as a freestanding C library and a WebAssembly module. It evaluates programs written by someone other than the host (guards, rules, filters, computed fields) and gives exactly the value JavaScript would, or a fault that says why.

## The rule

A program staticjs accepts is a valid JavaScript program, and evaluating it gives the same result as JavaScript does, or a fault. Never a different value. The test suite checks this against Node.

## What it guarantees

- **The value JavaScript gives, or a fault.** Where JavaScript would coerce, compare identities or look on a prototype, staticjs stops.
- **Every evaluation ends.** No loops but the built-ins over collections, no recursion; a program runs on fuel the host pushes, pauses where it runs out, and resumes.
- **No side effects.** A program reaches only what the host passes in; the clock and Math.random's seed are the host's.
- **Deterministic.** The same program on the same inputs gives the same value and the same fuel on every machine.
- **Bounded memory.** Memory comes from the host's allocator, every construct has a limit, and nothing recurses.
- **Freestanding.** No C library; the wasm module imports nothing.

## Where it stops

| Program | JavaScript | staticjs |
|---|---|---|
| `[1, 2] === [1, 2]` | `false` | fault: === on two objects compares identity, which staticjs does not have |
| `"5" * 2` | `10` | fault: arithmetic in staticjs is on numbers |
| `({}).a.b` | TypeError | fault: cannot read a property of undefined |
| `() => { for (;;) {} }` | never ends, if called | refused when compiled: staticjs has no loops |

## Three ways in

- **A C library** (any compiler): `cc -std=c11 -Iinclude app.c lib/libstaticjs.a`. See [Getting started](/guide/getting-started).
- **One C file** (clang): `clang -O2 -I. app.c staticjs.c`, the amalgamation.
- **WebAssembly** (browser, Node): `staticjs.wasm` imports nothing. See [In the browser](/guide/wasm).

## Inside

Every stage is pushed, none recurses: bytes into the parser, events into the compiler, fuel into the evaluator, buffer space into the writer. The [pipeline explorer](/explore/pipeline) shows a program travelling through them.
-->
