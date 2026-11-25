# Proposals

Outlines of work that might happen later. **A proposal is not a decision and
not a plan.** Nothing in this directory is scheduled, and nothing here is an
instruction to implement anything.

Where a proposal disagrees with an ADR (`<package>/docs/adr/`) or a roadmap
(`<package>/ROADMAP.md`), the ADR or the roadmap wins.

## Where work of each kind lives

| Level | Where | Meaning |
|---|---|---|
| Idea or outline | `docs/proposals/<name>.md` | We might do this. Here is its shape. |
| Decision | a package's `docs/adr/` | Accepted. ADRs are append-only. |
| Committed plan | a package's `ROADMAP.md` | Steps someone is doing or will do next. |
| Small follow-up in one package | that package's roadmap, under "Open follow-ups" | A single item with no design of its own. |

## Status

Every proposal starts with a status line:

- **Sketch:** an outline from a discussion. It is incomplete, and it may be wrong.
- **Proposed:** complete enough to decide on.
- **Accepted:** the maintainer decided it, with the decisions written in
  the proposal; the work has not started.
- **Adopted → links:** work started. The decisions moved into ADRs and the
  steps into a roadmap, so the proposal is no longer edited and serves as
  history.
- **Dropped (why):** we decided against it. The file stays, so the idea is
  not proposed again from scratch.

## Index

| Proposal | Status | Touches |
|---|---|---|
| [The ring protocol](ring-proto/README.md) | Dropped: superseded by [ADR 0004](../adr/0004-replace-the-core-with-the-standalone-runtime.md) | tinyactors-core, tinyactors-host (deleted), tinyactors-napi, tinyactors-test |
| [staticjs: a pure, total subset of JavaScript as a data model](staticjs.md) | Proposed; the language is in `staticjs/` | a new staticjs, a new tinyactors-staticjs |
| [staticjs: the compiler and evaluator, from scratch](staticjs-compiler.md) | Sketch | staticjs (the compiler) |
| [staticjs: exceptions, and async functions over host promises](staticjs-exceptions-and-async.md) | Sketch | staticjs (every stage), staticjs-js |
| [A JavaScript API for staticjs.wasm](staticjs-js-api.md) | Adopted → [ADR 0013](../adr/0013-staticjs-javascript-package.md) | a new staticjs-js, staticjs's release packaging |
| [The JSON datamodel: one document per session, data paths, registered functions](json-datamodel.md) | Sketch | tinyactors-core (a new datamodel layer), every binding, tinyactors-test |
| [An application datamodel](application-datamodel.md) | Dropped: replaced by the JSON datamodel | — |
| [SQLite host](sqlite-host.md) | Sketch | tinyactors-core, a new tinyactors-sqlite |
| [PostgreSQL host](postgres-host.md) | Sketch | tinyactors-core, a new tinyactors-postgres |
| [Host kernel](host-kernel.md) | Adopted → kernel ADRs (deleted with tinyactors-host), [roadmap](../../ROADMAP.md) | tinyactors-napi, tinyactors-test, a new tinyactors-host (deleted) |
| [Replay-based persistence](replay-persistence.md) | Adopted → core ADRs 0088 to 0094 (deleted), napi ADRs 0032 to 0037 (deleted), kernel ADR 0002, "The kernel keeps the write-ahead log" (deleted) | tinyactors-core, tinyactors-host (deleted), tinyactors-napi |
| [A thin JavaScript binding over a deep native one](thin-js-binding.md) | Adopted → implemented in `tinyactors-core/bindings` (2026-10-10; deleted) | tinyactors-core (`bindings/shared`, `bindings/node`, `bindings/web` (deleted), `src/host`, `src/trace`) |
| [Trace context on inputs: a struct in, text only at the edges](trace-context-on-inputs.md) | Proposed | tinyactors-core (`system.h`, `host.h`, `src/system`, `src/host`, every caller) |
| [Distributed actor systems](distributed-systems.md) | Sketch | tinyactors-core (conformance gaps), tinyactors-napi, the browser build |
| [An idle actor carries no event](lean-actor-events.md) | Adopted → core ADR 0069, "An idle actor carries no event" (deleted) | tinyactors-core, tinyactors-napi |
| [Packaging and releases](packaging.md) | Adopted → [ADR 0001](../adr/0001-packaging-and-releases.md) | tinyxml, tinyactors-core, tinyactors-scxml, tinyactors-napi, root release tasks |
| [A system as a WASM module](wasm-modules.md) | Sketch | tinyactors-core (a module writer, the release's WASM builds, `tinyactors compile`), tinyactors-test |
| [Compiled charts: a chart's step as WASM code, frontloaded to compile time](wasm-aot.md) | Sketch | tinyactors-core (`src/core`'s step results and a runtime for generated code, a new `src/lower`, the module writer, the datamodels' compile-time preparation, `tinyactors compile --mode compiled`), tinyactors-test |
| [The command on the website: a reference from its own help, and its voice](cli-reference.md) | Sketch | website, copywriter, tinyactors-core (`tools/tinyactors/program.c`, `docs/cli.md`) |
| [`tinyactors run`: running charts from the command over JSON-RPC 2.0](cli-json-rpc.md) | Proposed | tinyactors-core (`src/cli`, `src/base/format`, `tools/tinyactors`, docs), tinyactors-test (an adapter) |
| [An MCP server](mcp-server.md) | Proposed; the DAP adapters it replaces are removed | tinyactors-core, tinyactors-host (deleted), tinyactors-napi, tinyactors-test |
| [Metrics, and trace policy as data](metrics-and-trace-policy.md) | Adopted → [ADR 0007](../adr/0007-metrics-and-trace-policy.md) | tinyactors-core (system, host, otlp, bindings) |
| [SCXML in core: a small XML writer beside the compiler's parser](scxml-in-core.md) | Adopted → commits `bb603ff8`, `77f39f31` | tinyactors-core, tinyactors-scxml (deleted) |
| [A debug adapter](debug-adapter.md) | Adopted, then removed (2026-09-30) for [the MCP server](mcp-server.md) | tinyactors-core (source spans, operator transitions), tinyactors-scxml, tinyactors-test, tinyactors-napi |
| [One host, two queues](one-host-two-queues/README.md): `ta_host` as the only interface, CBOR requests, bindings regenerated from a guide | Adopted → [ADR 0009](../adr/0009-one-host-two-queues.md), superseding [ADR 0008](../adr/0008-bindings-only-do-io.md) | tinyactors-core (`host.h`, `src/link`, `src/base/json`, a new `src/base/cbor`), `bindings/` (deleted), new tinyactors-js, tinyactors-napi, tinyactors-browser, tinyactors-cpython |
| [Memory accounting: core's costs from its definitions, the rest from counted allocations](memory-accounting.md) | Accepted (2026-11-25) | tinyactors-core (`tools/layout`, the fast pass, `ta_host_create`, `host.stats`, OTLP, quickjs's counter), tinyactors-napi, tinyactors-cpython (memory examples; `measure` retired), the published tables |
| [Atlas: core's types and calls from DWARF, and a snapshot's memory, seen](atlas.md) | Sketch (2026-11-25) | tinyactors-core (`mise/dwarf.rb`, `tinyactors inspect`, the snapshot pack), website |
