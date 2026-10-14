---
title: Examples
description: "Whole C programs and a browser page, built and checked by the test suite."
---

# Examples

Whole programs that use staticjs, each with what it prints. The
repository's tests build every one exactly as
[Getting started](./getting-started.md) says, from `staticjs.c` and
`staticjs.h` alone, and check what it prints, so they stay correct as the
library changes.

| Example | Shows | Run |
|---|---|---|
| [hello.c](https://github.com/dhamidi/tinyactors/blob/main/staticjs/examples/hello.c) | the smallest whole program: a context, an expression compiled, evaluated on fuel, its value written | `./hello` |
| [eval.c](https://github.com/dhamidi/tinyactors/blob/main/staticjs/examples/eval.c) | a command-line evaluator: expressions and modules, error reports with positions, a fuel budget, the clock from the host, `Math.random`'s seed | `./eval '[3, 1, 2].toSorted((a, b) => a - b)'` |
| [host.c](https://github.com/dhamidi/tinyactors/blob/main/staticjs/examples/host.c) | staticjs inside a host that reacts to events: a library module, a guard compiled once and evaluated per event, globals built through the API, a native `In()`, a module imported by name, the clock pushed by the host | `./host` |
| [stream.c](https://github.com/dhamidi/tinyactors/blob/main/staticjs/examples/stream.c) | every stage pushed: source in 7-byte chunks, a sink of its own before the compiler, evaluation in slices of fuel, output through a 16-byte buffer | `./stream` |
| [wasm/](https://github.com/dhamidi/tinyactors/tree/main/staticjs/examples/wasm) | `staticjs.wasm` from JavaScript: a wrapper (`staticjs.mjs`), a page (`index.html`) and a Node script (`node.mjs`) | `node wasm/node.mjs staticjs.wasm staticjs.api.json` |

## Building them

From a source release, with clang:

```sh
clang -std=c11 -O2 -I. hello.c staticjs.c -o hello
```

With a prebuilt library, with any C11 compiler:

```sh
cc -std=c11 -Iinclude hello.c lib/libstaticjs.a -o hello
```

In this repository, `mise run examples` builds them all into
`build/examples/` and checks their output. [Browser and Node](./wasm.md)
says how to run the wasm example.

## hello.c

The smallest whole program: a context, an expression compiled, evaluated on
fuel, and its value written as text.

<<< ../../examples/hello.c

It prints:

<<< ../../examples/expected/hello.txt{text}

## stream.c

Every stage pushed by the host: the source in 7-byte chunks, a sink of its
own in front of the compiler, evaluation in slices of fuel, and output
through a 16-byte buffer.

<<< ../../examples/stream.c

It prints:

<<< ../../examples/expected/stream.txt{text}

## eval.c

A command-line evaluator, with expressions and modules, error reports with
positions, a fuel budget, the host's clock and `Math.random`'s seed. It
prints:

<<< ../../examples/expected/eval.txt{text}

::: details The source of eval.c
<<< ../../examples/eval.c
:::

## host.c

staticjs inside a host that reacts to events: a guard compiled once and
evaluated per event, globals built through the API, a native function
`In()`, and a module imported by name. It prints:

<<< ../../examples/expected/host.txt{text}

::: details The source of host.c
<<< ../../examples/host.c
:::
