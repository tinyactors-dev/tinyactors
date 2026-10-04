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
| [SQLite host](sqlite-host.md) | Sketch | tinyactors-core, a new tinyactors-sqlite |
| [PostgreSQL host](postgres-host.md) | Sketch | tinyactors-core, a new tinyactors-postgres |
| [Host kernel](host-kernel.md) | Adopted → [kernel ADRs](../../tinyactors-host/docs/adr/README.md), [roadmap](../../ROADMAP.md) | tinyactors-napi, tinyactors-test, a new tinyactors-host |
| [Replay-based persistence](replay-persistence.md) | Adopted → core ADRs 0088 to 0094, napi ADRs 0032 to 0037, [kernel ADR 0002](../../tinyactors-host/docs/adr/0002-the-kernel-keeps-the-write-ahead-log.md) | tinyactors-core, tinyactors-host, tinyactors-napi |
| [Distributed actor systems](distributed-systems.md) | Sketch | tinyactors-core (conformance gaps), tinyactors-napi, the browser build |
| [An idle actor carries no event](lean-actor-events.md) | Adopted → [core ADR 0069](../../tinyactors-core/docs/adr/0069-an-idle-actor-carries-no-event.md) | tinyactors-core, tinyactors-napi |
| [Packaging and releases](packaging.md) | Adopted → [ADR 0001](../adr/0001-packaging-and-releases.md) | tinyxml, tinyactors-core, tinyactors-scxml, tinyactors-napi, root release tasks |
| [An MCP server](mcp-server.md) | Proposed; the DAP adapters it replaces are removed | tinyactors-core, tinyactors-host, tinyactors-napi, tinyactors-test |
| [A debug adapter](debug-adapter.md) | Adopted, then removed (2026-09-30) for [the MCP server](mcp-server.md) | tinyactors-core (source spans, operator transitions), tinyactors-scxml, tinyactors-test, tinyactors-napi |
