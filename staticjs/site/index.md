---
layout: page
sidebar: false
title: staticjs
description: "A small, embeddable and fast JavaScript engine for a pure subset of JavaScript: every program ends, reaches only what you pass in, and returns Node's exact value or a fault. Freestanding C and WebAssembly."
---

<!-- markdown
**A small, embeddable and fast JavaScript engine**

A pure subset of JavaScript, for running code your users write. Every program ends, sees only what you hand it, and returns what Node returns or a fault with a line and column.

One C file, no libc · Wasm module, no imports · 0.14 µs to evaluate a guard · Runs on fuel you push

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
## What you get

- **Small.** One C file, one header, no dependencies, no libc. The language is small as well: no loops, recursion or classes. An evaluator at max_stack = 256 is about 41 KiB.
- **Embeddable.** Freestanding C11. Static and shared libraries for Linux and macOS on x86_64 and arm64, or one wasm module that imports nothing. Runs in browsers and Node.
- **Fast.** A compiled guard evaluates from C in about 0.14 µs. Compiling it takes 6.4 µs. Against QuickJS-NG, both as wasm in Chromium, staticjs won 7 of 8 programs.
- **Total.** Every evaluation ends. The host pushes fuel, one unit per instruction and per element a built-in walks. When it runs out the program pauses, and it resumes from any point once more arrives.
- **Exact.** A program staticjs accepts is valid JavaScript and gives the value Node gives, or a fault that says why. `"5" * 2` faults instead of giving `10`. About 4,200 test cases also run in Node and must match.
- **Hermetic.** A program sees only the values the host passes in. No file system, network, clock or randomness of its own; the host supplies the clock and Math.random's seed. Same program and inputs give the same value and the same fuel on every machine.

## How staticjs compares

Basically CEL with JavaScript syntax and user functions. Next to QuickJS it can't run away; next to Starlark it's JavaScript instead of a Python dialect.

| | staticjs | QuickJS | CEL | Starlark |
|---|---|---|---|---|
| Syntax | JavaScript (a pure subset) | JavaScript (ES2025) | Its own C-like syntax | A dialect of Python |
| Functions users write | Arrow functions, no recursion | All of JavaScript | No; macros and host functions | def functions, no recursion |
| Loops | None; map, filter, reduce, find | All of JavaScript | None; map, filter, all, exists | for over finite collections |
| Runaway programs | Cannot run away; host-pushed fuel | Host's interrupt handler stops it | Non-Turing complete | — |
| Reaches | Only values the host passes in | — | Only data the host provides | No file system, network, clock |
| Memory | Host allocator; limit per construct | Limit set on the runtime | — | — |
| Same result everywhere | Same value and same fuel | — | — | Deterministic evaluation |
| Coercion | Stops with a fault | — | — | — |
| Embeds in | Freestanding C11, one file; wasm | A few C files, no dependency | Go, Java, C++ | Go, Java, Rust |

- QuickJS if you need all of JavaScript. It passes nearly all of the ES2025 tests.
- CEL if you need formal verification, or a library for Go, Java or C++.
- Starlark if you want Python-style configuration, as Bazel uses it.

What we say about QuickJS, CEL and Starlark comes from their own pages. The race on the page runs in the reader's browser; the [benchmark report](/guide/performance#compared-with-other-engines) has one machine's numbers.

## staticjs against QuickJS-NG

On the page, eight programs run in staticjs and QuickJS-NG (the npm package quickjs-wasi), both compiled to WebAssembly, in the reader's browser: a guard, filter and map over 10,000 orders, a sort of 5,000 rows by a key, group and count over 10,000 numbers, a sum of 100,000 numbers with reduce, work on 2,000 strings, a regular expression over 2,000 addresses, and a JSON round trip of 1,000 records. A row is timed only if both return the same result. Each figure is the median of five samples, each repeating the program for at least 40 ms. staticjs is called from JavaScript once per run, the way a host calls its C API; QuickJS-NG loops inside the engine and is called once per sample, so it crosses the boundary less. The times depend on the browser and the machine, so this markdown does not quote them.

## When a rule is wrong

Mistakes in a rule come back as a fault with a line and column.

| Program | JavaScript | staticjs |
|---|---|---|
| `[1, 2] === [1, 2]` | `false` | fault: === on two objects compares identity, which staticjs does not have |
| `"5" * 2` | `10` | fault: arithmetic in staticjs is on numbers |
| `({}).a.b` | TypeError | fault: cannot read a property of undefined |
| `() => { for (;;) {} }` | never ends, if called | refused when compiled: staticjs has no loops |

[All faults and refusals](/guide/faults)

## Three ways in

- **Prebuilt libraries.** Static and shared libraries for Linux and macOS, x86_64 and arm64. Link with any C11 compiler: `cc -std=c11 -Iinclude app.c lib/libstaticjs.a`. See [Getting started](/guide/getting-started).
- **Amalgamation.** staticjs.c and staticjs.h, the whole library. Add both to your project and build with clang: `clang -O2 -I. app.c staticjs.c`.
- **WebAssembly.** staticjs.wasm imports nothing and brings its own allocator. Runs in browsers and Node: `await WebAssembly.instantiate(bytes, {})`. See [Browser and Node](/guide/wasm).

## Inside the pipeline

Step through the parser, compiler and evaluator on a program of your own in the [pipeline explorer](/explore/pipeline). Push the parser 1, 4 or 16 bytes at a time, look at the compiled function shapes, spend fuel one unit at a time, and read the output in 16-byte pieces. Runs on staticjs.wasm.

Parser: bytes → events. Compiler: events → bytecode. Evaluator: bytecode → value. Writer: value → JS source.
-->
