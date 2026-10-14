---
layout: page
sidebar: false
title: staticjs
description: "Run expressions and rules written by someone else, as JavaScript that always ends, reaches nothing of the host, and gives Node's value or a fault. A freestanding C library and a WebAssembly module."
---

<!-- markdown
**Let others write the rules. Every one of them ends.**

Guards, filters, pricing formulas, computed fields: code someone else writes and your application runs. staticjs runs it in a subset of JavaScript that has no loop to hang on, nothing of the host to reach and no clock of its own, and gives exactly the value Node gives, or a fault that says why. It is a freestanding C library, and a WebAssembly module that imports nothing.

For example, this program, with its result:
-->

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
## What running someone else's code costs you, and what staticjs takes away

A sandbox around a JavaScript engine leaves you some of these problems, and a rule language of its own leaves you others. staticjs is built so that none of them can happen.

- **A rule that never finishes.** A timeout depends on the machine it runs on, and in Node's vm a promise callback outlives it. staticjs has no loops and no recursion: a program runs on fuel you push, pauses where it runs out, and resumes when you push more.
- **A way out of the sandbox.** A sandbox around a whole engine must take away the network, the files, the globals and the prototypes, and every one it misses is an escape. A staticjs program starts with nothing: it reaches only what you pass in.
- **Memory that grows until the process dies.** Every byte comes from your allocator, every construct has a limit, and nothing recurses, so no program can overflow the C stack. With a small stack, an evaluator holds about 41 KiB.
- **A different answer on another machine.** Time limits depend on the machine, and engines disagree on the last bits of Math. staticjs gives the same value and spends the same fuel everywhere; the clock and Math.random's seed are yours.
- **A new language for every author.** Rule languages are pure and end, but each has a syntax of its own to learn. staticjs is JavaScript: authors test a rule in a browser console, and their editor already knows it.
- **A wrong value that looks right.** In JavaScript, `"5" * 2` is `10`. Where JavaScript would coerce, compare identities or look on a prototype, staticjs stops with a fault that says why. Every other value is the one Node gives, and the test suite checks it against Node.

## staticjs or QuickJS?

QuickJS runs all of JavaScript, and if your users write whole programs, use it. staticjs gives up loops, classes and I/O, and in exchange guarantees what a host running someone else's rules needs.

| | QuickJS-NG | staticjs |
|---|---|---|
| Language | All of modern JavaScript | Expressions and modules of pure JavaScript: no loops, recursion, classes, async or I/O |
| A program that never ends | Stopped by an interrupt handler you write | Ends: each evaluation runs on the fuel you push, and pauses where it runs out |
| Memory | A limit you set on the runtime | Your allocator, and a limit on every construct |
| `"5" * 2` | `10`, as JavaScript gives | A fault that says why |
| Time, both as WebAssembly | Measured live on the page, in the reader's browser | Measured live on the page, in the reader's browser |
| Time, staticjs as a C library | 4.8 times staticjs's, over 33 evaluation cases | About 5 times faster than QuickJS-NG in WebAssembly |

Measured on one 4-core x86_64 machine, staticjs as a C library and QuickJS-NG compiled to WebAssembly and run by Node, on the same programs and inputs: proportions, not a promise. Node with its JIT is faster than both, taking 0.43 of staticjs's time; without its JIT, Node is about as fast as staticjs. See [Performance](/guide/performance#compared-with-other-engines).

## The same rules, run side by side

On the page, staticjs and QuickJS-NG (the npm package quickjs-wasi), both compiled to WebAssembly, run the same eight programs in the reader's browser: a guard, a filter and map over 10,000 orders, a sort of 5,000 rows by a key, a group and count, a sum with reduce, string work, regular expressions and a JSON round trip. Each program builds its own data, both engines must give the same result before its time counts, and each figure is the median of five samples. The times depend on the browser and the machine, so this markdown does not quote them.

## A fault, never a plausible wrong value

Where JavaScript would coerce, compare identities or look on a prototype, staticjs stops and says why.

| Program | JavaScript | staticjs |
|---|---|---|
| `[1, 2] === [1, 2]` | `false` | fault: === on two objects compares identity, which staticjs does not have |
| `"5" * 2` | `10` | fault: arithmetic in staticjs is on numbers |
| `({}).a.b` | TypeError | fault: cannot read a property of undefined |
| `() => { for (;;) {} }` | never ends, if called | refused when compiled: staticjs has no loops |

## The same library, wherever it runs

No C library and no imports: one C file builds for Linux, macOS and WebAssembly from the same source.

- **A C library** (any C11 compiler): `cc -std=c11 -Iinclude app.c lib/libstaticjs.a`. See [Getting started](/guide/getting-started).
- **One C file** (clang): `clang -O2 -I. app.c staticjs.c`, the amalgamation.
- **WebAssembly** (browsers and Node): `staticjs.wasm` imports nothing and brings its own allocator. See [Browser and Node](/guide/wasm).

## Every stage is pushed, none recurses

Bytes into the parser, events into the compiler, fuel into the evaluator, buffer space into the writer. Nothing blocks or calls back for more input, so the host decides when each step happens. The [pipeline explorer](/explore/pipeline) shows a program travelling through them.
-->
