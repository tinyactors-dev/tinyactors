---
layout: page
sidebar: false
title: staticjs
description: "A small, embeddable and fast JavaScript engine for a pure subset of JavaScript: every program ends, reaches only what you pass in, and returns Node's exact value or a fault. Freestanding C and WebAssembly."
---

<!-- markdown
**A small, embeddable and fast JavaScript engine**

An engine for a pure subset of JavaScript, for code someone else writes. Every program ends, reaches only what you pass in, and returns Node's exact value or a fault that says why.

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
## Small, embeddable, fast, and it always ends

staticjs runs a pure subset of JavaScript, so you can embed it to evaluate code someone else wrote.

- **Small.** One C file and one header, no dependencies, no C library. The language is small too: no loops, recursion or classes. An evaluator with max_stack = 256 is about 41 KiB.
- **Embeddable.** Freestanding C11 with a one-file amalgamation. Static and shared libraries for Linux and macOS (x86_64, arm64), and a WebAssembly module that imports nothing, for browsers and Node.
- **Fast.** Evaluating a compiled guard from C takes about 0.14 µs; compiling one takes 6.4 µs. In WebAssembly in Chromium, staticjs was faster than QuickJS-NG on 7 of 8 programs.
- **Total.** Every evaluation ends. A program runs on fuel the host pushes, one unit per instruction and per element a built-in walks. It pauses when the fuel runs out and resumes when more arrives.
- **Exact.** A program staticjs accepts is valid JavaScript and gives the value Node gives, or a fault that says why. `"5" * 2` is a fault, not `10`. About 4,200 cases run in Node too and must match.
- **Hermetic.** A program reaches only the values the host passes in: no file system, network, clock or randomness. Same program and inputs give the same value and the same fuel on every machine.

## How staticjs compares

staticjs has CEL's guarantees with JavaScript syntax and functions users define. It is QuickJS's embeddability in a language that cannot run away, and Starlark's determinism for JavaScript.

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

- Choose QuickJS when you need to run whole JavaScript programs: it passes nearly all of the ES2025 test suite.
- Choose CEL when you need formal verification, or libraries for Go, Java or C++ with protobuf.
- Choose Starlark when you want Python-style configuration, as Bazel uses it.

The alternatives' facts come from their own pages. staticjs's speed is measured on the page, in the reader's browser, and on one machine in the [benchmark report](/guide/performance#compared-with-other-engines): proportions, not promises.

## Run both engines in your browser

On the page, eight programs run in staticjs and QuickJS-NG (the npm package quickjs-wasi), both compiled to WebAssembly, in the reader's browser: a guard, filter and map over 10,000 orders, a sort of 5,000 rows by a key, group and count over 10,000 numbers, a sum of 100,000 numbers with reduce, work on 2,000 strings, a regular expression over 2,000 addresses, and a JSON round trip of 1,000 records. A time counts only if both engines return the same result. Each figure is the median of five samples; each sample repeats the program for at least 40 ms. staticjs is called from JavaScript once per run, as a host calls its C API; QuickJS-NG loops inside the engine and is called once per sample, so it pays fewer crossings. The times depend on the browser and the machine, so this markdown does not quote them.

## Where staticjs stops instead of guessing

A mistake in someone else's rule becomes a fault with a line and column, not a plausible wrong value. Everything staticjs accepts gives exactly the value Node gives.

| Program | JavaScript | staticjs |
|---|---|---|
| `[1, 2] === [1, 2]` | `false` | fault: === on two objects compares identity, which staticjs does not have |
| `"5" * 2` | `10` | fault: arithmetic in staticjs is on numbers |
| `({}).a.b` | TypeError | fault: cannot read a property of undefined |
| `() => { for (;;) {} }` | never ends, if called | refused when compiled: staticjs has no loops |

[All faults and refusals](/guide/faults)

## Three ways in

Use prebuilt libraries, one C file, or WebAssembly. Pick the one that fits your build.

- **Prebuilt libraries.** Static and shared libraries for Linux and macOS, on x86_64 and arm64. Link them from any C11 compiler: `cc -std=c11 -Iinclude app.c lib/libstaticjs.a`. See [Getting started](/guide/getting-started).
- **Amalgamation.** The whole library as one file, staticjs.c, plus staticjs.h. Add both to your project and build with clang: `clang -O2 -I. app.c staticjs.c`.
- **WebAssembly.** staticjs.wasm imports nothing and brings its own allocator. It runs in browsers and Node: `await WebAssembly.instantiate(bytes, {})`. See [Browser and Node](/guide/wasm).

## Watch a program go through the pipeline

Pick an example or type a program and step through each stage: parser events pushed 1, 4 or 16 bytes at a time, compiled function shapes, the evaluator one unit of fuel at a time, and output in 16-byte pieces. It runs in the browser on staticjs.wasm: the [pipeline explorer](/explore/pipeline).

Parser: bytes → events. Compiler: events → bytecode. Evaluator: bytecode → value. Writer: value → JS source.
-->
