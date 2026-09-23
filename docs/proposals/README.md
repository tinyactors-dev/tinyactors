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
| [SQLite host](sqlite-host.md) | Sketch | tinyactors-core, a new tinyactors-sqlite |
| [PostgreSQL host](postgres-host.md) | Sketch | tinyactors-core, a new tinyactors-postgres |
| [Host kernel](host-kernel.md) | Sketch | tinyactors-napi, future language hosts |
| [Replay-based persistence](replay-persistence.md) | Sketch | tinyactors-core, tinyactors-napi |
| [An idle actor carries no event](lean-actor-events.md) | Proposed | tinyactors-core, tinyactors-napi |
| [Packaging and releases](packaging.md) | Sketch | tinyactors-napi, tinyactors-core, root release tasks |
