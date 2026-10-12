# tinyactors for Python

SCXML statecharts as actors, from Python: the `tinyactors` extension
module drives the tinyactors host, and chart logic is written in Python.

**Status: not usable yet.** This directory holds the project's skeleton:
its toolchain, tasks, lint rules and the empty mapping table. There is no
module to import.

## What the first milestone delivers

M1 of the bindings' plan ([ROADMAP.md](../ROADMAP.md), step 8) is a
walking skeleton:

- the `tinyactors` module and its `Host` type, created from Python
  options, with images, boot, spawn, send and terminate;
- the loop with every run status, and the `DONE`, `ERROR` and `LOG`
  effects;
- `request` with `TinyactorsError` (code, reason, path);
- values converted between CBOR and Python ([docs/values.md](docs/values.md)),
  with `Address` for an actor reference and `UNDEFINED` beside `None`;
- the `"python"` datamodel: `<data>`, `cond`, `assign`, `<script>`,
  `_event`, `In()`, `<send>` with params, `<foreach>` in Python, and the
  ECMAScript datamodel (quickjs) beside it;
- a simulated clock for testing charts (`SimulatedClock`,
  `increment(ms)`).

Later milestones add the W3C suite in Python (M2), every effect and both
drivers (M3), hibernation, migration and peers (M4), and the release:
one abi3 wheel per platform for Python 3.11 and later (M5).

## Limits, to be documented here

Chart code is trusted: CPython cannot be preempted in the middle of an
expression or limited in memory, and Python values lie outside the
host's allocator. Values nest at most 64 deep, which stops cycles but
not a value that shares subtrees.

## License

MIT, as the repository ([LICENSE](../LICENSE)).
