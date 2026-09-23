# SQLite host: the runtime inside the database

**Status:** Sketch (2026-09-23)
**Touches:** tinyactors-core (no changes expected), a new tinyactors-sqlite
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

Run tinyactors inside SQLite as a loadable extension. Actors are rows, and
their data lives in tables. Expressions and scripts are SQL. Charts are
written for this host. Importing standard SCXML charts with ECMAScript
expressions is not a goal.

SQLite is the first database host (before [PostgreSQL](postgres-host.md))
for three reasons:
- it runs in the application's own process;
- it has a single writer;
- a failing statement rolls back only itself, so turning a SQL error into
  `error.execution` is cheap.

## Shape

Core runs inside the database connection. Actors are virtual actors: they
are loaded when an event arrives and unloaded when it has been processed
(core ADRs 0014, 0051, 0066). For one event:

1. `SELECT ta_post('order', 'o-42', 'pay', …)`, inside the caller's
   transaction.
2. Load the actor's session image into a `ta_system` that is cached per
   connection. Deliver the event, and run to stable within a budget.
3. Expressions and scripts run as SQL against the actor's data row, in the
   same transaction.
4. Capture the image. Update the actor's next deadline. Release the actor
   from memory.
5. The commit makes the state change, the data change and every outgoing
   message durable together.

## Storage

| Table | Holds |
|---|---|
| `ta_definition` | compiled charts by family and revision. Charts could also be authored as rows (states, transitions) and compiled through core's spec. |
| `ta_actor` | session ID, address, definition, image (blob), status, `next_due` (indexed) |
| `<chart>_data` | one row per actor, with typed columns from the chart's `<data>` declarations |
| `ta_mailbox` | queued events: target, event, payload, sequence |
| `ta_outbox` | effects for the outside world, written by I/O processors |

## The SQL data model

It is one more implementation of core's `ta_datamodel_vtable`, beside null,
JavaScript and ECMAScript. No core change is expected.

| Operation | SQL data model |
|---|---|
| `prepare_definition` | prepare one statement per expression (`sqlite3_prepare_v2`), with the actor key and `_event` bound as parameters |
| `declare` / `set_named` | a `<data>` entry is a column of the chart's table; its initializer runs at spawn |
| `eval_condition` | `SELECT (balance >= :amount) FROM wallet WHERE session = :sid` |
| `eval_value` | a scalar or row query |
| `read_location` / `assign` | a column of the actor's row |
| `execute_script` | any SQL, with `:sid`, `:_event_name` and `:_event_data` bound |
| `foreach_snapshot` | the result rows of a query |
| values (`ta_value`) | reference-counted `sqlite3_value_dup` copies |
| `inspect` | the actor's row |

Example:

```xml
<datamodel><data id="balance" expr="0"/></datamodel>
<transition event="withdraw" cond="balance >= :amount" target="open">
  <assign location="balance" expr="balance - :amount"/>
  <script>INSERT INTO ledger(session, delta) VALUES (:sid, -:amount)</script>
</transition>
```

## What comes almost for free

- **Atomic messaging:** a `<send>` to another actor is an insert into
  `ta_mailbox` in the same transaction. This is the transactional outbox
  pattern.
- **Exactly-once state changes:** a transaction that fails rolls back the
  event's state change, data change and sends together.
- **Timers are a column:** `next_due` is indexed, and the application polls
  for due actors. SQLite has no background process.
- **Persistence:** the image holds only core's control state (configuration,
  history, queues, timers). The data model is already in tables, so it
  needs no value codec.
- **Operations are SQL:** inspection is a query. A new chart revision is a
  migration plus `prepareReplacement`.

## Costs and open questions

- **Throughput:** each expression executes a statement (microseconds). The
  limit is transactions per second, not millions of messages per second.
  That fits durable workflows. It is a different design point from the napi
  host's 4M resident actors.
- **Isolation:** SQL can read and write any table. The working suggestion is
  that conditions and values may read anywhere, and only `<script>` may
  write. An alternative is to restrict expressions to the actor's own row,
  which keeps actors isolated and replays deterministic.
- **Types:** column types come from `<data>` (for example a `type`
  attribute) or from a naming convention.
- **Pump:** `ta_post` could run the target to stable inline, and a budgeted
  `ta_run()` could drain `ta_mailbox`. It is undecided which the
  application calls, and when.
- **Reuse from napi:** core itself, the generator discipline (with a
  SQL-function emitter), event-name interning, dispatching effects after the
  core call returns, and core ADR 0062's cursor as a virtual table. The
  in-memory scheduler does not carry over.

## Working suggestions from the discussion

SQLite first, expressions as SQL text, reads anywhere, and writes only in
scripts. These should become the first ADRs of `tinyactors-sqlite` when the
work starts.
