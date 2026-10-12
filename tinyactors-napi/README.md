# @tinyactors/node

SCXML statecharts as actors, from Node.js and Bun: a Node-API addon that
drives the tinyactors host, with chart logic in JavaScript.

**Status: not usable yet.** This directory holds the project's skeleton:
its toolchain, tasks, lint rules and the empty mapping table. There is no
addon to load. The last published version, 0.2.0, was built from an
earlier binding with another API, which this package replaces.

## What the first milestone delivers

M1 of the bindings' plan ([ROADMAP.md](../ROADMAP.md), step 8) is a
walking skeleton:

- the package and its `Host` class, created from JavaScript options, with
  images, boot, spawn, send and terminate;
- the loop on the runtime's event loop, with every run status, and the
  `DONE`, `ERROR` and `LOG` effects;
- `request` with `TinyactorsError` (code, reason, path);
- values converted between CBOR and JavaScript ([docs/values.md](docs/values.md)),
  with `BigInt` for large integers and ids, `Uint8Array` for bytes, and an
  actor reference as its address, a string;
- the native ECMAScript datamodel, in the application's own engine:
  `<data>`, `cond`, `assign`, `<script>`, `_event`, `In()`, `<send>` with
  params, `<foreach>`; and the quickjs datamodel beside it;
- a simulated clock for testing charts (`SimulatedClock`,
  `increment(ms)`).

Later milestones add the whole W3C suite (M2), every effect, liveness and
shutdown (M3), hibernation, migration and peers (M4), and the release:
one prebuilt addon per platform as optional dependencies (M5).

## Limits, to be documented here

Chart code is trusted: it runs in the application's realm, and Node-API
cannot make another. Values nest at most 64 deep, which stops cycles but
not a value that shares subtrees.

## License

MIT, as the repository ([LICENSE](../LICENSE)).
