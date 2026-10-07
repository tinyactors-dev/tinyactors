# Durable actors in PostgreSQL — design

The [PostgreSQL binding](../bindings/postgres/README.md) makes tinyactors
actors durable by making them rows, and makes processing an event part of a
transaction. This note explains why it is built the way it is, and what each
decision costs.

## Goals

1. **Durable by default.** An actor survives crashes, restarts, failovers —
   with no extra infrastructure beyond the database the application already
   has.
2. **Transactional.** Business data and actor state change together:
   `INSERT INTO payments …; SELECT tinyactors.send('order-42', 'paid');` either
   both happen or neither.
3. **Idiomatic SQL.** Actors are rows you can query, index, join, back up and
   replicate; the API is a handful of functions.
4. **Effects without lies.** The database must not pretend to have sent an
   email inside a transaction that may roll back. Effects are requested
   transactionally and performed by the application, at least once.
5. **No new operational surface.** No background worker to deploy, no
   daemon, no separate store.

## The central decision: no resident actors

An embedded actor system usually keeps actors in memory and persists
snapshots. In a database that is the wrong shape:

- PostgreSQL runs a process per connection. Resident actors would live in one
  backend; every other connection would have to route to it.
- The source of truth must be the table, or a rollback in one backend and a
  cached actor in another disagree.
- Shared memory for actors would need its own locking and recovery — a second
  database inside the database.

So **an actor exists in memory only while one statement processes it**.
Processing = lock the row, adopt the actor's exported state into a fresh,
tiny `ta_system` (8 actor slots, a few KB, created per actor), deliver
events, run to a stable point, export, update the row, discard the system.

This is affordable because of choices made long before this binding:

| property of tinyactors | why it matters here |
|---|---|
| per-actor state is small and portable (`ta_actor_export`: configuration, history, datamodel bytes, pending timers, name — hundreds of bytes) | a row holds the whole actor; no snapshot of a "world" |
| systems are cheap to create (`ta_system_init` carves one region; no allocation afterwards) | one system per processing costs microseconds |
| the core never does I/O and never reads a clock | the host (the extension) decides time and persistence |
| run-to-completion is cooperative and fuel-bounded | a runaway chart becomes an SQL error, not a hung backend |
| delayed sends carry absolute due times | a timer armed in one transaction is valid in any later one, in any process |

The cost: every event pays a load and a store (export is a compact binary
write; the datamodel is rewritten as `jsonb` for queryability). Measured with
`pgbench` on a 4-core orb (PostgreSQL 17, default settings, 8 clients): one
hot actor — serialized by its row lock — processes about **1 800 events/s**;
the same clients spread over 10 000 actors reach about **8 900 events/s**
(0.90 ms per event, load to commit), with 5 million actors stored — see
*Scale* below. That grows with cores and connections because different actors
never wait for each other.

## Events: an inbox when they must wait

An event an actor sends to another actor is a row in `tinyactors.inbox`
first, then processed — immediately if the receiver is free, later if not.
An event from SQL (`send()`) goes straight to the actor it addresses: the
statement waits for that actor's lock anyway, so a row inserted and deleted
by the same statement would only cost two writes (and, under a busy actor,
dead index entries that every later lookup walks past — see *Scale*). Events
the actor deferred earlier are delivered first, so order is kept. Two
alternatives to the inbox were rejected:

- *Deliver directly, recursively* (actor A's send processes B immediately):
  unbounded recursion, and A's transaction would have to lock B before A's
  own processing is complete; locking order becomes the application's
  problem.
- *Queue only, process later* (a worker drains the inbox): durable but
  surprising — `send()` would not return the effect of the event, and every
  deployment would need a worker.

The inbox gives both: `send()` queues and then processes immediately (the
event's effects are visible when it returns), while events that cannot be
processed now (their actor is busy) are already durable and wait for the
next opportunity. The inbox is the *mailbox* of SCXML's external queue,
persisted.

## Locking: wait once, skip the rest

The statement waits for the lock of the actor it addresses — that is what a
caller expects (serialized events, read-your-writes). Actors reached through
events from that actor are locked with `SKIP LOCKED`: if busy, their event
stays in their inbox (committed together with the sender) and is processed by
their next `send` or by `tinyactors.run()`.

A statement therefore never holds one actor's lock while waiting for
another's: **within one statement, actors cannot deadlock**, whatever charts
send to whom. (Across statements, locks accumulate until `COMMIT` like any
row locks — including the cascade's; multi-statement transactions use
`tinyactors.cascade = 0`, see docs/concurrency.md.) The price
is that a cascade can be deferred — an event can sit in an inbox until
`run()` runs. Applications that need a cascade complete within the
transaction can check `SELECT count(*) FROM tinyactors.inbox WHERE actor = …`.

Lock strength matters: actor rows are locked `FOR NO KEY UPDATE`, not
`FOR UPDATE`. The inbox row's foreign key takes `FOR KEY SHARE` on the
actor; with `FOR UPDATE` two senders to one actor deadlock (each holds the
share lock from its insert and waits for the other's update lock — the
first version of this binding did exactly that under `pgbench`). `NO KEY
UPDATE` is also what PostgreSQL takes for the row update itself, since the key
never changes.

## Effects: the transactional outbox

SCXML already has the right abstraction: `<send type="…">` names an Event I/O
Processor. Types listed in `tinyactors.processors` are processors *the
application implements*; a send to one becomes an outbox row in the same
transaction. The application claims rows (`FOR UPDATE SKIP LOCKED`, with a
lease so a crashed worker's rows are taken over), performs them, acks.

- **Exactly-once is not on offer; idempotency is.** Each row has a unique
  `idempotency_key` = `<sending session>:<step>.<n>` — the actor's step (its
  `steps` column: processings so far) and the effect's index in that step.
  The same event retried after a rollback yields the same key, because the
  step rolled back too; a later event that reuses an explicit `id` gets a new
  key (a retried charge is a new effect, never silently dropped). Automatic
  sendids are `auto.<step>.<n>`. Downstream systems dedupe on the key.
- **Failure is an SCXML event.** `fail()` delivers `error.communication` with
  the `sendid`, exactly what SCXML specifies when a processor cannot deliver.
  Charts already know how to handle it.
- `NOTIFY tinyactors_outbox` on insert wakes workers; notifications are
  delivered on commit, so workers never see rows that will roll back.

Sends between actors of the same database never touch the outbox: the SCXML
processor's own addressing (`#_scxml_<id>`) routes them to inbox rows. Only
sends to *other* systems' SCXML sessions leave as outbox rows of type `scxml`.

## Time

The actors' clock is epoch milliseconds (`ticks_per_second = 1000`), taken
from `clock_timestamp()` — or from the `tinyactors.now` setting, which makes
tests and simulations deterministic. Due times are absolute, stored in the
actor and summarized in `next_due_at` (indexed), so finding due actors is an
index range scan. `run()` processes them with `SKIP LOCKED`: any number of
schedulers (application loop, cron, pg_cron) may call it concurrently.

There is deliberately no background worker in the extension: scheduling is an
operational choice (pg_cron exists, many platforms forbid custom background
workers, applications usually have a loop anyway).

## Errors

Two kinds, kept apart as SCXML does:

- **Inside the chart** — an expression throws, an assignment targets an
  undeclared variable, a send names an unknown processor: SCXML's
  `error.execution` / `error.communication`, handled by the chart. SQL does
  not see them.
- **Around the chart** — the chart does not compile, the actor exceeds its
  fuel, it cannot be stored: SQL errors that abort the transaction, so nothing
  half-done commits. "Cannot be stored" names the variable and why, e.g.
  `variable 'helper' cannot be stored: it holds a function (line 12); only
  top-level function declarations are stored ...` — `var`/`let`/`const`
  variables and top-level `function` declarations in `<script>`s are stored.

## Scale: cost per work, not per actor

The goal is a table of millions of actors of which only some are ever hot,
with every operation costing what it does, not what is stored. The first
version met that on paper (one row per actor, nothing resident) but not in
practice; measuring millions of actors found five things, each fixed where it
belongs:

| found at millions | cause | fix |
|---|---|---|
| `run()` slows down with the population | `next_due_at <= now OR EXISTS (inbox)` cannot use an index: a scan of every active actor per call | two index walks: due timers from `actors_due` in due order, then deferred events from the inbox's primary key; both `LIMIT … SKIP LOCKED`. Idle `run()`: 0.06 ms at 100 000 and at 5 000 000 actors |
| a call's memory grows with the actors it processes (`run(5000)`: 600 MB) | everything allocated for an actor lived until the end of the call | an SPI level per actor: its system region, rows and strings go when it is done |
| backend memory grows ~1.5 KB per actor ever processed | the ECMAScript datamodel kept every chart attached to it (`chart_detach` did nothing), and a system per actor attaches the chart each time | the datamodel shares an attached chart (same image), frees it when unused — keeping a few idle ones, so the next actor reuses compiled expressions (faster too). A unit test cycles 2 000 systems over one datamodel |
| each statement re-planned | `SPI_execute_with_args` plans every call | statements prepared once per backend (`SPI_keepplan`) — with the chart reuse above, spawns went from about 3 500/s to 13 000/s per backend (43 000/s with 4) |
| one hot actor degrades over a minute (650 → 120 events/s) | every event inserted and deleted an inbox row, and updated a row on a full page: dead index entries for that one key pile up between vacuums, which the default (20% of the rows changed) postpones for millions of rows | `send()` without an inbox row; `fillfactor = 80` so updates can stay on their page; autovacuum at 1% of the table. Now flat (~1 800 events/s on the reference run below) |

Later, an event's own cost: statements were still planned per event. The
chart cache check (the chart row's `xmin`) was a one-shot query; once store
links went through the C host router, every processing that sent anything
read the peers table with another one, needed only for sends that leave the
database; and storing the `datamodel` column compiled a JavaScript function
per variable. Now the chart row version and the I/O processors come with the
statement that locks the actor, the router asks for the peers
(`TA_ROUTE_PEERS`) only to choose a next hop, and variables are read, not
compiled: per event (callgrind) a touch costs 23% fewer instructions, a send
to another actor 20%, a timer 16%. `test/harden.py` H24 counts the statements
an event costs (pg_stat_statements: 5 for a touch, 11 for a send between two
actors) and fails when one is added or planned per event.

The router host lives in the statement's memory context: its `ta_allocator`
is `MemoryContextAllocExtended` (without raising on out of memory) and
`pfree`, so an error or rollback frees it with the context (its reset
callback calls `ta_host_free` first), and an allocation PostgreSQL refuses is
`TA_ERROR_NO_MEMORY` from the host rather than an ERROR in the middle of it.

What remains proportional to the population is what should be: disk (about
700 bytes per small actor with indexes) and the size of the indexes walked by
primary-key lookups (logarithmic). Reference (`mise run pg:scale`,
`TA_SCALE_N=5000000`, 4 CPUs, default settings):

| Actors | Spawn rate | Idle `run()` | Hot set events/s | Cold events/s | One hot actor | `run(5000)`, all due | Backend memory |
|---|---|---|---|---|---|---|---|
| 5 000 000 | 43 500/s | 0.18 ms | 8 869 (0.90 ms) | 7 392 (1.08 ms) | 1 749/s | 416 ms | flat (4.9 MB) |

Measure private memory, not RSS: a backend's RSS includes every shared
buffer it has touched (up to `shared_buffers`), which grows with the table
it reads and says nothing about leaks.

## Invoked sessions: the row holds a session tree

An actor with running `<invoke>`d SCXML sessions is exported with them — the
core's export writes the whole session tree (each session's state, timers,
name, session id, and its parent's session id and invoke slot) and adoption
re-links it (tinyactors/system.h, `ta_actor_export`). So the row holds the tree, and
nothing changes for the transaction model: one lock, one row, one update.

Two alternatives were rejected. *Children as rows of their own* would make a
parent/child exchange (an invoked child replying to its parent) a
multi-row, multi-lock conversation — and a child's `#_parent` send must not
wait for, or deadlock on, the parent it is part of. *A snapshot of the whole
system per actor* would store far more than the tree.

Session ids: invoked sessions are created inside the transaction by the
system processing the actor, so their ids must be unique without a round
trip per invocation. A processing of a chart that can invoke draws a block of
2^20 ids from `tinyactors.session_seq` (starting at 2^62: never an actor id)
and hands them to the system (`ta_system_set_next_serial`). The `sessions`
column (GIN-indexed) maps an invoked session to its actor, so `#_scxml_<id>`
from any actor and `send(<id>, ...)` from SQL reach it: the event is queued
on the actor's row with `inbox.session` set and delivered to that session
when the tree is loaded. The outbox key is the sending session's, so two
sessions of one tree may use the same `sendid`.

`<invoke src>` resolves against `tinyactors.charts` at `define` time
(compile-time resolution, as any embedder of the compiler does with its own
resolver): the invoked chart's SCXML becomes part of the invoking chart's
image. Resolving at run time instead would make an actor's behaviour depend
on when it happened to invoke — a running parent could get a child of a
different version than the one it was tested with — and would need the
compiler in the hot path. Pinning (`worker@3`) and re-defining the invoking
chart are explicit instead.

## What is not done (yet)

- **Live migration** (`ta_blob_migrate` → `error.platform.upgrade`) exists in
  the core and the other bindings; a `tinyactors.migrate(chart)` here would
  rewrite each actor's `state` under its row lock.
- **Hibernation** is unnecessary here: every actor is "hibernated" between
  events by construction.
