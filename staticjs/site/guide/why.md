---
title: Why staticjs?
description: "When staticjs fits, when it does not, and how it compares with running JavaScript in an engine, a sandbox, or a rule language."
---

# Why staticjs?

staticjs runs code that someone other than you wrote: a guard in a
statechart, a filter a user typed, a pricing rule, a computed field. That
code should be easy to write, should not be able to harm the host, and
should give the answer its author expects. staticjs gives that code
JavaScript's syntax and values, and takes away whatever could go wrong.

## When does it fit?

- **Short expressions and small modules, run many times.** Evaluating a
  compiled guard from C takes well under a microsecond.
- **Code you do not trust, or do not want to review.** A program reaches
  only what the host passes in. It has no I/O, no globals of the host and
  no time of its own, and the host's limits bound its fuel and memory.
- **Results that must agree everywhere.** The same program on the same
  inputs gives the same value and spends the same fuel on every machine,
  in C or in the browser.
- **Hosts without a JavaScript engine.** staticjs is freestanding C with no
  C library, so it builds for microcontrollers, servers and WebAssembly
  alike.

## When does it not fit?

- **Whole applications.** There are no loops, no classes, no `async`, no
  I/O and no modules from npm. Use Node, Deno or a browser.
- **Code that needs to change shared state.** A function may change only a
  value it owns. Effects belong to the host.
- **Very long computations.** staticjs's evaluator is about as fast as
  Node without its JIT. That is plenty for a rule, but slow for number
  crunching. See [Performance](./performance.md).

## Why not a JavaScript engine in a sandbox?

An engine runs all of JavaScript, so the host has to take away what it
must not do: the network, the file system, the clock, endless loops,
unbounded memory. Each of those is a leak to plug, and a missed one is a
security bug. staticjs starts with nothing and adds only what is pure.
Every program ends, because there is no loop and no recursion, and the
host pushes the fuel it may spend.

An engine also gives JavaScript's answer where JavaScript is surprising:
`"5" * 2` is `10`, and `[] + {}` is `"[object Object]"`. staticjs stops with
a fault in those places instead, so a mistake shows up as a mistake and
never as a plausible wrong value.

```js live fault
"5" * 2
```

## Why not a rule language?

Rule languages, such as JSON Logic or CEL, are also pure and also end.
Their authors, though, have to learn a new syntax and new semantics.
staticjs is JavaScript: an author can test a program in a browser console,
and an editor already highlights it. Any program staticjs
accepts gives the value Node gives, and the test suite checks this
against Node itself.

## What does it cost a host?

- **Code size.** One C file, the amalgamation, and one header.
- **Memory.** The host gives the allocator and the limits. The size of an evaluator
  is fixed by its stack limit (about 41 KiB with `max_stack = 256`), and
  nothing grows without a limit.
- **Integration.** The host compiles once, evaluates many times, pushes
  fuel, and reads the result as a value or as text. See
  [Getting started](./getting-started.md).

## Is the output always correct?

A program staticjs accepts gives Node's value or a fault. It never gives
a different value. That rule is checked case by case: every case in staticjs's own test
suite runs in Node too, and the two results must match. Where staticjs and JavaScript could differ, staticjs refuses the
program or faults.
