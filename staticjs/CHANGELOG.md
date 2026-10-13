# Changelog: staticjs

What changed in staticjs in each release. The summary of a release
is written for someone upgrading; the lists below it are generated from the
commits and from the public API (docs/adr/0002-changelogs.md).

## [0.9.0] - 2026-10-13

### Summary

This is the first release of staticjs: a pure, total subset of JavaScript, as a freestanding C library, and the language runtime a staticjs data model for tinyactors will build on. Its rule is that a program it accepts is valid JavaScript and evaluates to the value JavaScript gives, or to a fault, never to a different value. The test suite checks its spec cases against Node, and large sets of numbers, BigInts, JSON documents and regular expressions (random) and of number formats, Math functions, Temporal values and Dates (fixed lists) against Node too, Temporal against the temporal-polyfill run in Node.

A program is expressions, arrow functions, const and let, blocks, if and return, assignments and the calls that change a variable the function owns, in modules that import and export. Every evaluation ends: code jumps only forward, there are no loops but the built-ins over collections, a function cannot refer to itself, modules cannot import in a cycle, and a function already running cannot be entered again; fuel only pauses an evaluation. It covers numbers (formatted as Node does; the Math functions give V8's results without fused multiply-adds, which is Node on x86_64, on every target), strings, arrays, objects, Sets and Maps, BigInts, JSON, regular expression literals, Temporal in the ISO 8601 calendar and UTC (PlainDate, PlainTime, PlainDateTime, Duration, Instant and Temporal.Now) and Date in UTC, with the clock and Math.random's seed set by the host. Every stage is pushed and none recurses: bytes into the parser, fuel into the evaluator, buffer space into the writer. It calls no C library, allocates only through the embedder's allocator, and every unbounded construct has a limit the embedder sets.

Each release ships the header, a one-file amalgamation and an API description, a static and a shared library for aarch64-macos, x86_64-macos, x86_64-linux and aarch64-linux, and a wasm module for browsers that imports nothing and brings its own allocator.

