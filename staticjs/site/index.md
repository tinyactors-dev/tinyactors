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

Like JavaScript, staticjs has numbers, BigInt, strings, template literals, arrays, objects, Set, Map, Temporal and Date, arrow functions, destructuring with defaults and rest, spread, optional chaining and the usual operators. A program is one expression, or a module of imports, const declarations and exports. Unlike JavaScript, it has no loops and no recursion.

staticjs leaves out loops (for, while, do), recursion, classes, async/await and promises, generators, try/catch/throw, var, labels, switch, eval, this, prototypes, getters and setters, and I/O. A function may change only a value it owns, such as an array it made; shared values never change.

- **Total.** Every evaluation ends: the evaluator runs on fuel the host pushes, and when the fuel runs out it pauses and resumes from where it stopped, inside a callback included.
- **Exact.** A program staticjs accepts is valid JavaScript. Where JavaScript would coerce, compare objects by identity or look on a prototype, it faults with a line, a column and a fixed message.
- **Hermetic.** A program reads only the globals the host passes in by name, calls only the native C functions it adds and imports only the modules it provides. It has no file system, network, clock or randomness of its own.
- **Deterministic.** The same program with the same inputs gives the same value and uses the same fuel on every machine.
- **Bounded.** Every byte comes from the allocator the host gives, and every construct has a limit set by the host.

What staticjs gives for four valid JavaScript programs:

```js
[1, 2] === [1, 2]     // fault: === on two objects compares identity, which staticjs does not have
"5" * 2               // fault: arithmetic in staticjs is on numbers
({}).a.b              // fault: cannot read a property of undefined
() => { for (;;) {} } // refused when compiled: staticjs has no loops
```

## Install

staticjs is freestanding C11: it needs no C library, only stdint.h, stddef.h and stdbool.h, and has one public header, staticjs.h.

- **Prebuilt libraries.** Static and shared libraries for Linux and macOS on x86_64 and arm64. Link with any C11 compiler: `cc -std=c11 -Iinclude app.c lib/libstaticjs.a`. See [Getting started](/guide/getting-started).
- **Amalgamation.** staticjs.c and staticjs.h, two files to add to a C project. They build with clang: `clang -O2 -I. app.c staticjs.c`.
- **npm.** For Node and browsers: staticjs as WebAssembly, with JavaScript values in and out: `npm install @tinyactors/staticjs`. See [Browser and Node](/guide/wasm).

## Performance

On a 4-core x86_64 machine, evaluating a compiled small expression from C takes about 0.14 µs, and compiling it takes 6.4 µs. Node's vm.Script.runInContext takes 1.4 µs for the same expression, and creating a fresh V8 context takes 330 µs. Over 33 evaluation cases, Node with its JIT takes 0.43 of staticjs's time, so Node's JIT is faster. Node with --jitless takes about as long as staticjs, and QuickJS-NG compiled to WebAssembly takes 4.8 times as long. See the [benchmark report](/guide/performance#compared-with-other-engines).

### Live measurement

On the page, staticjs and QuickJS-NG (the npm package quickjs-wasi), both compiled to WebAssembly, run eight programs in a web worker in the reader's browser: a small expression, filter and map over 10,000 orders, a sort of 5,000 rows by a key, group and count over 10,000 numbers, a sum of 100,000 numbers with reduce, work on 2,000 strings, a regular expression over 2,000 addresses, and a JSON round trip of 1,000 records. A row is timed only when both engines return the same result. Each time is the median of five samples of at least 40 ms. staticjs is called from JavaScript once per run, as a host calls the C API. QuickJS-NG loops inside its engine and is called once per sample. The times depend on the browser and the machine, so this markdown does not quote them.

## Limitations

- A program written for Node usually needs rewriting with map, filter and reduce. What the overview lists as left out is refused when the program is compiled, as an unsupported error.
- Valid JavaScript that coerces or compares objects by identity faults instead of running, as the examples in the overview show.
- Temporal supports the ISO calendar with UTC and fixed offsets. IANA time zones are not available. Date is UTC only.
- Regular expressions run in linear time, without backtracking. Lookahead, lookbehind, backreferences, `\p{...}` and the `v` flag are not supported.
- The WebAssembly module (about 950 KiB) is larger than QuickJS's (about 620 KiB).
- staticjs is at version 0.9.0, and the API may change.

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

- CEL also has type checking and formal verification.
- Starlark also runs threads in parallel over frozen shared data.

## How it works

staticjs has four stages: the parser, the compiler, the evaluator and the writer. A caller pushes into each one: bytes into the parser, which sends events to a sink, operands before their operator; the compiler is that sink and makes bytecode as the events arrive; fuel into the evaluator; buffer space into the writer. None blocks or calls back for more input, so a host can stream source in chunks, and there is no recursion anywhere in the C code. The host takes the evaluator's value and reads it in place, kind by kind and element by element, or has the writer write it as JavaScript source.

The compiler also tags functions whose shape a built-in can run in C, such as `x => x.key` or `(a, b) => a.k - b.k`. A tagged function saves fuel and never changes a value. A host can pause an evaluation and read its frames, for example the program, map at element 2 of 4, and the callback, each with a position.

The [pipeline explorer](/explore/pipeline) runs staticjs.wasm in the browser and shows the parser's events, the compiler's functions and shapes with the fuel used with and without them, the evaluator one unit of fuel at a time with its frames, and the result read through the value API and written in 16-byte pieces.

The host pushes bytes into the parser, the parser streams events into the compiler, and the compiler finishes a program, which the evaluator reads without copying. The host pushes fuel into the evaluator and takes its value, the result. The host reads the result in place through the value API, or hands it to the writer, which streams text back to the host in pieces. All of it lives in memory from the host's allocator.

### Frames of an evaluation

On the page, `[[3, 1], [2]].map((row) => row.toSorted((a, b) => a - b).map((n) => n * 10))` runs one unit of fuel at a time, and its frames are drawn as a waterfall on an axis of fuel: the program, the outer `map`, each `(row) => …` callback, `toSorted`, the inner `map` and each `(n) => …` callback. The comparator `(a, b) => a - b` has a shape that `toSorted` runs in C, so it opens no frame of its own.

### Memory

On WebAssembly, everything staticjs has is in the module's one linear memory, which JavaScript sees as one ArrayBuffer (`memory.buffer`). A pointer is a byte offset into it, so `new Uint8Array(memory.buffer, address, 16)` is a value's 16 bytes. The site's module starts at 17 pages of 64 KiB (1,114,112 bytes). The allocator grows it a page at a time, and WebAssembly memory never shrinks.

| Region | From | Size | Holds |
|---|---|---|---|
| C stack | 0 | 1 MiB | the C call stack, growing down |
| Data | 1 MiB | about 60 KB | the built-ins' tables and constants |
| Heap | after the data | the rest of the memory | the context, the evaluator, the program, results and their objects, in power-of-two blocks |

A result is one 16-byte value, with the fields kind, internal, reserved, boolean and as. The page prints it in hex, with the addresses of the context, the evaluator, the program, the result and its object marked on the start of the heap. For `0.1 + 0.2` the 8 bytes of `as` are the double itself. For `"tiny" + "actors"` and for `[3, 1, 2].toSorted((a, b) => a - b)` they are a pointer to a heap object.

Every value is a 16-byte `sjs_value`: its kind, a boolean, and a number or a pointer to a reference-counted heap object. Byte order is little-endian. Copying the struct does not retain the object.

| Offset | Size | Type | Field |
|---:|---:|---|---|
| 0 | 1 | `uint8_t` | `kind` |
| 1 | 1 | `uint8_t` | `internal` |
| 2 | 2 | `uint16_t` | `reserved` |
| 4 | 4 | `uint32_t` | `boolean` |
| 8 | 8 | `double` or `sjs_heap *` | `as` |

## Documentation

- [Guide (Getting started)](/guide/getting-started)
- [The language](/guide/language)
- [Faults and refusals](/guide/faults)
- [Embedding](/guide/embedding)
- [staticjs.h](https://github.com/dhamidi/tinyactors/blob/main/staticjs/include/staticjs.h)
- [Pipeline explorer](/explore/pipeline)
-->
