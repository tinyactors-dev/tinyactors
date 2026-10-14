---
title: The pipeline
description: "Follow a program through the parser, the compiler, the evaluator and the writer, step by step, live."
aside: false
pageClass: pipeline-page
next: false
prev: false
sidebar: false
---

# The pipeline

A program travels through four stages, and each is pushed by its caller: bytes into the parser, events into the compiler, fuel into the evaluator, buffer space into the writer.

<!-- html -->
Pick an example, or write your own, and follow it through. Everything below is computed in this page by `staticjs.wasm`, through the library's public API.
<!-- /html -->

<PipelineExplorer />

<!-- markdown
The interactive explorer runs in a browser. What it shows, for a program you choose or write:

1. **Parser:** the source pushed in chunks of 1, 4 or 16 bytes, or whole; the events the parser sends its sink, in evaluation order (operands before their operator), each with the syntax node it belongs to and its position. The same events arrive however the source is cut.
2. **Compiler:** the compiler is the parser's sink. It shows the program's size, each function with where it begins and the shape the compiler tagged it with (`PLUCK` for `x => x.key`, `COMPARE_BY_KEY`, `COMPARE_NUMERIC`, `SUM`, ...), and the fuel the program uses with shapes and without: a shape changes fuel, never a value.
3. **Evaluator:** the evaluation run on one unit of fuel at a time, reading its frames after each unit (`sjs_vm_frame`): the program, each looping built-in (`map`, element 2 of 4) and each callback, with the source position each is at.
4. **Writer:** the value written as JavaScript source through a 16-byte buffer, in pieces.

An error shows at the stage that reported it: a syntax error at the parser, a name error or an unsupported construct at the compiler, a fault at the evaluator.

-->

<!-- pipeline example: "[\n  { name: \"ada\", age: 36 },\n  { name: \"grace\", age: 85 },\n  { name: \"linus\", age: 54 },\n]\n  .toSorted((a, b) => b.age - a.age)\n  .map((p) => p.name)" -->
