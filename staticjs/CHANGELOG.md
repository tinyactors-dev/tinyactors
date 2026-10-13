# Changelog: staticjs

What changed in staticjs in each release. The summary of a release
is written for someone upgrading; the lists below it are generated from the
commits and from the public API (docs/adr/0002-changelogs.md).

## [0.9.0] - 2026-10-13

### Summary

This is the first release of staticjs: a pure, total subset of JavaScript, as a freestanding C library, and the language runtime a staticjs data model for tinyactors will build on. Its rule is that a program it accepts is valid JavaScript and evaluates to the value JavaScript gives, or to a fault, never to a different value; the test suite checks its spec cases, and large random and fixed sets of numbers, BigInts, JSON documents, regular expressions and Temporal values, against Node (Temporal against the temporal-polyfill, run in Node).

A program is expressions, functions, const and let, if and return, and modules that import and export; there are no loops but the built-ins over collections, so every evaluation ends. It covers numbers, formatted and with Math functions computed as V8 computes them, strings, arrays, objects, Sets and Maps, BigInts, JSON, regular expression literals, Temporal and Date, with the clock and Math.random's seed set by the host. Every stage is pushed and none recurses: bytes into the parser, fuel into the evaluator, buffer space into the writer. It calls no C library, allocates only through the embedder's allocator, and has a limit for every construct a program controls.

Each release ships the header, a one-file amalgamation and an API description, a static and a shared library for aarch64-macos, x86_64-macos, x86_64-linux and aarch64-linux, and a wasm module for browsers that imports nothing and brings its own allocator.
