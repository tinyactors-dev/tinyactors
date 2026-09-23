# PostgreSQL host: the runtime inside the database

**Status:** Sketch (2026-09-23)
**Touches:** tinyactors-core (no changes expected), a new tinyactors-postgres
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

The same model as the [SQLite host](sqlite-host.md), as a PostgreSQL
extension. Actors are rows, and their data lives in tables. Expressions are
SQL or named functions. Charts are written for this host.

Most of the SQLite outline applies unchanged: the per-event flow, the
storage tables, the SQL data model, and what comes almost for free. This
file records only what differs.

## Differences from SQLite

| Concern | PostgreSQL |
|---|---|
| Concurrency | Many backend processes. An actor's row is locked (`SELECT … FOR UPDATE`) while it runs (core ADR 0017: external synchronization). |
| Timers | A background worker, or pg_cron, dispatches due actors with `… WHERE next_due <= now() FOR UPDATE SKIP LOCKED`. |
| Pump | The same background worker drains `ta_mailbox`. `ta_post` may also run the target inline. |
| Outbox | `ta_outbox`, plus `LISTEN/NOTIFY` to wake the consumers outside the database. |
| Memory | palloc memory contexts map onto the call and turn arenas of napi ADR 0014. A `ta_system` cached per backend lives in a long-lived context. |
| Values | Datum plus type OID, reference-counted. |
| Errors | Any SQL error aborts the whole transaction. See below. |

## The main cost: SQL errors abort the transaction

SCXML turns a failing expression into `error.execution`, and the macrostep
continues. In PostgreSQL, the failing statement aborts the transaction
unless it ran in a subtransaction. A subtransaction per expression is
expensive, and more than 64 in one transaction slow down the whole server.

Options:
- **Named functions as expressions.** For example, `cond="can_withdraw"`
  calls a PL/pgSQL function with the actor's row and the event. Each
  function catches its own errors once. Expressions stay typed and
  testable, and plans are cached. This is the working suggestion for
  PostgreSQL.
- **Validate at define time,** so that fewer expressions can fail at run
  time, and accept that an unexpected error aborts the event's transaction.
  The event is then retried or dead-lettered.
- **One subtransaction per macrostep** instead of one per expression. This
  gives a different error semantic: the whole macrostep fails, not just one
  block.

## Alternative: PostgreSQL as storage only

Run tinyactors in an application server (for example the Node addon) and
keep images, mailboxes, the directory and deadlines in PostgreSQL. This is
the durability layer core ADRs 0014 and 0028 describe. It is less work, and
it keeps the napi host's throughput. It gives up running inside the
database's transactions.

## Open questions

- SQL text, named functions, or both. SQL text suits SQLite, and named
  functions suit PostgreSQL.
- How the background worker is deployed and configured.
- Whether charts are authored as rows or as SCXML-shaped documents stored in
  a table.
