# Changelog: staticjs-js

What changed in @tinyactors/staticjs in each release. The summary of a release
is written for someone upgrading; the lists below it are generated from the
commits and from the public API (docs/adr/0002-changelogs.md).

## [0.9.0] - 2026-10-15

### Summary

The first release of @tinyactors/staticjs: staticjs, the small engine for a
pure, total subset of JavaScript, as a WebAssembly module for Node 22 or later and
browsers, with an API in JavaScript above it.

A context holds values, programs and runs under its own limits. A program is
compiled once, as an expression or a module, and started with named globals;
a run evaluates on the fuel each step gives it, so a long evaluation takes
turns with the event loop and can be cancelled between steps. Values cross
both ways as JavaScript values: numbers, strings, BigInts, arrays, plain
objects, Maps, Sets and Dates, and Temporal values where the runtime has
Temporal. What staticjs has no value for is a `TypeError`. A JavaScript
function among the globals is a native the program can call. Values also
read from and write to JSON and CBOR without running anything. Faults and
refused programs throw a `StaticJSError` with a kind, a position and, for a
fault, the frames where it happened.
