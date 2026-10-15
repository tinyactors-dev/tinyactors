---
layout: page
sidebar: false
title: staticjs
description: "A small, embeddable and fast JavaScript engine for a pure, total subset of the language: every program it accepts ends, and gives the value Node gives or a fault. Freestanding C11 and WebAssembly."
---

<!-- markdown
**A small, embeddable and fast JavaScript engine**

staticjs is a JavaScript engine for a pure, total subset of the language: every program it accepts ends, and gives the value Node gives or a fault.

This program, with its result:
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
## Overview

Like JavaScript, staticjs has numbers, BigInt, strings, template literals, arrays, objects, Set, Map, Temporal and Date, arrow functions, destructuring with defaults and rest, spread, optional chaining and the usual operators. A program is one expression, or a module of imports, const declarations and exports. Unlike JavaScript, it has no loops and no recursion, and every evaluation ends.

staticjs leaves out loops (for, while, do), recursion, classes, async/await and promises, generators, try/catch/throw, var, labels, switch, eval, this, prototypes, getters and setters, and I/O. A function may change only a value it owns, such as an array it made; shared values never change.

The host passes in the program's globals by name, adds native functions written in C, and provides the modules a program can import.

- **Total.** Every evaluation ends: the evaluator runs on fuel the host pushes, and when the fuel runs out it pauses and resumes from where it stopped, inside a callback included.
- **Exact.** A program staticjs accepts is valid JavaScript and evaluates to the value Node gives, or to a fault with a line, a column and a fixed message.
- **Hermetic.** A program reads only the values the host passes in and imports only modules the host provides, with no file system, network, clock or randomness of its own.
- **Deterministic.** The same program with the same inputs gives the same value and uses the same fuel on every machine.
- **Bounded.** Every byte comes from the allocator the host gives, and every construct has a limit set by the host.

Where JavaScript would coerce, compare by identity or look on a prototype, staticjs faults; a loop is refused when the program is compiled.

```js
[1, 2] === [1, 2]     // fault: === on two objects compares identity, which staticjs does not have
"5" * 2               // fault: arithmetic in staticjs is on numbers
({}).a.b              // fault: cannot read a property of undefined
() => { for (;;) {} } // refused when compiled: staticjs has no loops
```

## Install

staticjs is freestanding C11: it needs no C library, only stdint.h, stddef.h and stdbool.h, and has one public header, staticjs.h.

- **Prebuilt libraries.** Static and shared libraries with staticjs.h, for Linux and macOS on x86_64 and arm64. Link with any C11 compiler: `cc -std=c11 -Iinclude app.c lib/libstaticjs.a`. See [Getting started](/guide/getting-started).
- **Amalgamation.** staticjs.c and staticjs.h, two files to add to a C project. They build with clang and need no C library: `clang -O2 -I. app.c staticjs.c`.
- **staticjs.wasm.** A WebAssembly module for browsers and Node. It imports nothing and brings its own allocator: `await WebAssembly.instantiate(bytes, {})`. See [Browser and Node](/guide/wasm).

## How it works

staticjs has four stages: the parser, the compiler, the evaluator and the writer. A caller pushes into each one: bytes into the parser, which sends events to a sink, operands before their operator; the compiler is that sink and makes bytecode as the events arrive; fuel into the evaluator; buffer space into the writer. None blocks or calls back for more input, so a host can stream source in chunks, and there is no recursion anywhere in the C code.

The compiler also tags functions whose shape a built-in can run in C, such as `x => x.key` or `(a, b) => a.k - b.k`. A tagged function saves fuel and never changes a value. A host can pause an evaluation and read its frames, for example the program, map at element 2 of 4, and the callback, each with a position.

The [pipeline explorer](/explore/pipeline) runs staticjs.wasm in the browser and shows the parser's events, the compiler's functions and shapes with the fuel used with and without them, the evaluator one unit of fuel at a time with its frames, and the writer's output in 16-byte pieces.

Parser: bytes → events. Compiler: events → bytecode. Evaluator: bytecode, fuel → value. Writer: value → source text.

## Performance

On a 4-core x86_64 machine, evaluating a compiled small expression from C takes about 0.14 µs, and compiling it takes 6.4 µs. Node's vm.Script.runInContext takes 1.4 µs for the same expression, and creating a fresh V8 context takes 330 µs. Over 33 evaluation cases, Node with its JIT takes 0.43 of staticjs's time, so Node's JIT is faster. Node with --jitless takes about as long as staticjs, and QuickJS-NG compiled to WebAssembly takes 4.8 times as long. See the [benchmark report](/guide/performance#compared-with-other-engines).

### Comparison with QuickJS-NG

On the page, staticjs and QuickJS-NG (the npm package quickjs-wasi), both compiled to WebAssembly, run eight programs in a web worker in the reader's browser: a small expression, filter and map over 10,000 orders, a sort of 5,000 rows by a key, group and count over 10,000 numbers, a sum of 100,000 numbers with reduce, work on 2,000 strings, a regular expression over 2,000 addresses, and a JSON round trip of 1,000 records. A row is timed only when both engines return the same result. Each time is the median of five samples of at least 40 ms. staticjs is called from JavaScript once per run, as a host calls the C API. QuickJS-NG loops inside its engine and is called once per sample. The times depend on the browser and the machine, so this markdown does not quote them.

## Comparison

staticjs is basically CEL with JavaScript syntax and user functions.

| | staticjs | QuickJS | CEL | Starlark |
|---|---|---|---|---|
| Syntax | Subset of JavaScript | All of JavaScript (ES2025) | C-like syntax of its own | Dialect of Python |
| Functions in programs | Arrow functions | All JavaScript functions | None; the host provides them | `def` functions |
| Loops | None | All JavaScript loops | None; macros such as `all`, `map`, `filter` | `for` over finite sequences |
| Every evaluation ends | Yes; fuel the host pushes | No; the host's interrupt handler stops it | Yes; not Turing complete | Yes; no recursion, no unbounded loops |
| What a program can reach | Values and modules the host passes in | — | Variables and functions the host provides | Hermetic |
| Determinism | Same value and fuel on every machine | — | — | Deterministic |
| Memory limits | Host's allocator; limits set by the host | A memory limit on the runtime | — | — |
| Coercion and identity | Faults on both | Follows JavaScript: `"5" * 2` is 10 | — | — |
| Implementations | Freestanding C11, WebAssembly | A few C files; QuickJS-NG fork | Go, Java, C++ | Go, Java, Rust |

- QuickJS runs all of JavaScript, ES2025 included.
- CEL has type checking and formal verification.
- Starlark has `def` functions and runs threads in parallel over frozen shared data.

## Limitations

- staticjs runs a subset of JavaScript. The language leaves out what the overview lists, so a program written for Node usually needs rewriting with map, filter and reduce before staticjs accepts it.
- Some valid JavaScript faults. Where JavaScript would coerce or compare objects by identity, staticjs stops with a fault: `"5" * 2` and `[1, 2] === [1, 2]` both fault, and each fault carries a line, a column and a fixed English message.
- JavaScript that staticjs leaves out is refused when the program is compiled, as an unsupported error. It never reaches the evaluator.
- Temporal supports the ISO calendar with UTC and fixed offsets. IANA time zones are not available.
- Date is UTC only.
- Regular expressions run in linear time, without backtracking. Lookahead, lookbehind, backreferences, `\p{...}` and the `v` flag are not supported.
- staticjs is at version 0.9.0, and the API may change.
- staticjs is not faster than Node's JIT, and its WebAssembly module (about 950 KiB) is larger than QuickJS's (about 620 KiB).

### Documentation

- [Guide (Getting started)](/guide/getting-started)
- [The language](/guide/language)
- [Faults and refusals](/guide/faults)
- [Embedding](/guide/embedding)
- [staticjs.h](https://github.com/dhamidi/tinyactors/blob/main/staticjs/include/staticjs.h)
- [Pipeline explorer](/explore/pipeline)
-->
