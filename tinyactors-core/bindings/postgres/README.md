# tinyactors for PostgreSQL

**Durable SCXML actors as rows.** Define a state chart, start actors from it,
send them events — from SQL, in your transactions. An actor's state, the
events it sends to other actors, its timers and its effects on the outside
world commit together with the transaction that caused them, or not at all.

```sql
CREATE EXTENSION tinyactors;

SELECT tinyactors.define('order', $scxml$ ... $scxml$);           -- a chart (versioned)
SELECT tinyactors.spawn('order', '{"customer": "ann"}', 'order-1'); -- an actor
SELECT * FROM tinyactors.send('order-1', 'add', '{"sku": "kb-01", "price": 89}');
SELECT id, configuration, datamodel FROM tinyactors.actors;         -- actors are rows
```

The engine is the [tinyactors](../../README.md) SCXML runtime (full W3C
semantics, the ECMAScript datamodel on quickjs-ng) linked into the extension.
Nothing runs outside your transactions; there is no background worker to
operate.

## Installable distribution

Packages are not built at the moment: the pipeline that made them used
CMake and is to be rebuilt on the repository's release machinery
([distribution](../../docs/distribution.md)). In this repository, `mise run pg:build` (in `tinyactors-core`) builds the
extension and installs it into the pinned PostgreSQL 17; PostgreSQL 17 is
tested. No binary ABI compatibility across PG majors is assumed.

- [Quick start](#quick-start)
- [How it works](#how-it-works)
- [Data model](#data-model)
- [Transactions](#transactions)
- [Concurrency and locking](#concurrency-and-locking)
- [The outbox: effects on the outside world](#the-outbox-effects-on-the-outside-world)
- [Timers](#timers)
- [Reference](#reference)
- [Operations](#operations)
- [Limits](#limits)
- [Build and test](#build-and-test)

Design rationale: [docs/postgres.md](../../docs/postgres.md).

## Quick start

In this repository, one command builds the extension and runs a PostgreSQL 17
instance with it (an orb service when running in an Amp orb):

```sh
mise run pg:start   # 127.0.0.1:54320, database "tinyactors", extension created
psql -h 127.0.0.1 -p 54320 -U postgres -d tinyactors
```

A chart is an SCXML document. This one is an order: items go into a cart,
checkout asks the payment service (an *effect*, see the outbox) and waits at
most 15 minutes for the answer.

```sql
SELECT tinyactors.define('order', $scxml$
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="ecmascript" name="order" initial="cart">
  <datamodel>
    <data id="items" expr="[]"/>
    <data id="total" expr="0"/>
  </datamodel>
  <state id="cart">
    <transition event="add">
      <script>items.push(_event.data.sku)</script>
      <assign location="total" expr="total + _event.data.price"/>
    </transition>
    <transition event="checkout" cond="items.length &gt; 0" target="paying">
      <send type="outbox" target="payments" event="charge" id="charge">
        <param name="amount" expr="total"/>
      </send>
    </transition>
  </state>
  <state id="paying">
    <onentry><send event="payment.timeout" delay="900s" id="timeout"/></onentry>
    <transition event="paid" target="shipped"><cancel sendid="timeout"/></transition>
    <transition event="payment.timeout" target="cart"/>
    <transition event="error.communication" target="cart"/>
  </state>
  <final id="shipped"><donedata><param name="total" expr="total"/></donedata></final>
</scxml>
$scxml$);
```

Start an order and put things into it. `send` processes the event before it
returns and returns the actor's row:

```sql
SELECT tinyactors.spawn('order', '{}', 'order-1');

SELECT configuration, datamodel FROM tinyactors.send('order-1', 'add', '{"sku": "kb-01", "price": 89}');
--  configuration |              datamodel
-- ---------------+--------------------------------------
--  {cart}        | {"items": ["kb-01"], "total": 89}

SELECT configuration FROM tinyactors.send('order-1', 'checkout');
--  {paying}
```

Checkout put a row into the outbox. Your payment worker takes it from there:

```sql
SELECT id, type, target, event, data FROM tinyactors.claim(10, 'payment-worker');
--  id |  type  |  target  | event  |      data
-- ----+--------+----------+--------+----------------
--   1 | outbox | payments | charge | {"amount": 89}

-- ... charge the card ...
SELECT tinyactors.ack(1);
SELECT status, output FROM tinyactors.send('order-1', 'paid');
--  done   | {"total": 89}
```

Actors are rows, so the rest is SQL:

```sql
SELECT count(*) FILTER (WHERE 'paying' = ANY (configuration)) AS awaiting_payment,
       sum((datamodel->>'total')::numeric)                     AS value_in_carts
  FROM tinyactors.actors
 WHERE chart = 'order' AND status = 'active';
```

## How it works

Every actor lives in a row. Processing an actor is a short cycle inside the
transaction that asks for it:

```
 SELECT tinyactors.send('order-1', 'add', '{...}')
        │
        ▼
 ┌──────────────┐   ┌────────────────────┐   ┌────────────────────────────┐
 │ the event    │──▶│ lock the actor row │──▶│ load it into a fresh, tiny │
 │              │   │ FOR NO KEY UPDATE  │   │ in-memory actor system     │
 └──────────────┘   └────────────────────┘   └──────────────┬─────────────┘
                                                            ▼
 ┌──────────────────────────────┐   ┌──────────────────────────────────────┐
 │ UPDATE the row: state,       │◀──│ deliver deferred events (its inbox)   │
 │ configuration, datamodel,    │   │ and the event, run the chart to a     │
 │ next_due_at                  │   │ stable point (SCXML run-to-completion)│
 └──────────────────────────────┘   └──────────────┬───────────────────────┘
                                                    │ effects
                                                    ├─▶ events for other actors → their inbox rows
                                                    ├─▶ sends to I/O processors → outbox rows
                                                    └─▶ <log> → the server log (or NOTICE)
```

Events another actor receives are processed in the same call (breadth first,
up to `tinyactors.cascade` actors) if that actor is free, so a conversation
between actors usually completes inside one transaction. Nothing about actors
is kept in memory between statements; only immutable things are cached per
backend (compiled charts and their expressions, prepared statements).

## Data model

| table | one row per | notes |
|---|---|---|
| `tinyactors.charts` | chart version | `name`, `version`, the SCXML, its datamodel, the compiled `image` |
| `tinyactors.actors` | actor (SCXML session) | `id` (the session id), `chart`/`version`, `name`, `status`, `configuration text[]`, `datamodel jsonb`, `output jsonb`, `next_due_at`, `events`, `state bytea`, `sessions bigint[]` (the sessions it invoked, while they run) |
| `tinyactors.inbox` | event not processed yet | normally empty outside running transactions; `session`: for one of the actor's invoked sessions |
| `tinyactors.outbox` | effect for the application | at-least-once, idempotency key `<session>:<step>.<n>` (the sending session — the actor's id, or an invoked session's: `session` —, the actor's step and the effect's index: stable when a rolled-back event is retried) |
| `tinyactors.processors` | I/O processor type | sends of these types go to the outbox |

`configuration` (the active atomic states) and `datamodel` (the chart's
`<data>` variables) are rewritten whenever the actor changes, so you can
filter, index and join on them:

```sql
CREATE INDEX ON tinyactors.actors ((datamodel->>'customer')) WHERE chart = 'order';
SELECT id FROM tinyactors.actors WHERE datamodel->>'customer' = 'ann' AND 'cart' = ANY (configuration);
```

Treat these tables as read-only views of the actors: change actors with
`send`, `spawn` and `stop`. `state` is the actor itself (its configuration,
variables, history, pending timers) in tinyactors' portable export format;
it is `NULL` once the actor finished.

**Addresses.** An actor's SCXML address is
`tinyactors://<system_id>/#_scxml_<id>`, or `tinyactors://<system_id>/@<name>`
for a named actor (see [the protocol](../../docs/protocol.md#addresses)). The
system id is the setting `tinyactors.system_id`; by default it is derived from
the database name (lowercased, other characters than a-z, 0-9, `-` and `.`
become `-`: `my_app` → `my-app`);
set it per database when other systems address this one:
`ALTER DATABASE shop SET tinyactors.system_id = 'orders';`. Inside charts, `#_scxml_<id>` reaches
another actor of the same database; `_event.origin` is the sender's address,
so `<send targetexpr="_event.origin" .../>` replies.

**Invoked sessions.** `<invoke>` works as in any SCXML processor: the actor
and the sessions it invoked (and theirs) are stored together in its row — a
session tree — and carry on across transactions, with `#_<invokeid>`,
`#_parent`, `done.invoke.<id>`, `autoforward`, `<finalize>` and delayed sends
between them. Invoked sessions get session ids of their own range (from
2^62, never an actor id), listed in the actor's `sessions` column while they
run; they are addressable like actors — `#_scxml_<id>` from any actor,
`tinyactors.send(<id>, ...)` from SQL (which returns the actor's row) — and
their timers and outbox effects are the actor's. The actor's
`configuration` and `datamodel` columns describe the actor itself.

```sql
SELECT id, sessions FROM tinyactors.actors WHERE sessions IS NOT NULL;  -- actors with running invocations
SELECT tinyactors.send(4611686018427387904, 'ping');                     -- an invoked session, directly
```

**Invoking defined charts.** `<invoke src="worker">` invokes the chart
`worker` from `tinyactors.charts` — its newest version, or a given one with
`src="worker@3"` (`src="tinyactors:worker"` is the same as `worker`). The
named chart is compiled into the image of the chart that invokes it when
that chart is defined, so:

- a misspelled `src` fails `define`, with the line;
- the invoked version is fixed per version of the invoking chart: defining
  `worker` again does not change running or newly spawned `boss` actors
  until you define `boss` again (or pin `worker@N` on purpose);
- `<invoke srcexpr="...">` can choose among the charts the image already
  holds (those named by a `src` in the document); any other name is
  `error.execution` in the chart.

```sql
SELECT tinyactors.define('worker', $$<scxml ... name="worker"> ... </scxml>$$);
SELECT tinyactors.define('boss', $$<scxml ...>
  <state id="s"><invoke id="w" src="worker"><param name="job" expr="'first'"/></invoke> ...
</scxml>$$);
```

**Versions.** `define` adds a version. Running actors stay on the version
they started on; `spawn` uses the newest. `tinyactors.current_charts` shows
the newest version of each chart.

## Transactions

An event is processed inside the transaction that sends it.

- **Atomic.** The actor's new state, the events it sends to other actors (and
  their processing), its timers and its outbox rows commit together. A
  rollback undoes all of it: no actor saw the event, no effect was requested.
- **Read your writes.** `send` returns the actor after the event; anything
  queried later in the transaction sees the new state.
- **One event at a time per actor**, in order: an actor is locked while it is
  processed; events it could not take earlier (its inbox) come first, oldest
  first.
- **Run-to-completion.** A row is written only at SCXML stable points; there
  is no half-processed actor in the table.
- **Bounded.** An actor that does not settle within `tinyactors.fuel` (an
  eventless loop) fails the transaction with an error instead of running
  forever; its row is unchanged.

```sql
BEGIN;
INSERT INTO payments (order_id, amount) VALUES (42, 89);
SELECT tinyactors.send('order-42', 'paid');     -- shipped only if the payment row commits
COMMIT;
```

## Concurrency and locking

The unit of parallelism is the connection: transactions working on
different actors run in parallel; transactions on the same actor take turns.
[docs/concurrency.md](../../docs/concurrency.md#5-postgresql-actors-as-rows-parallel-by-connection)
builds the whole picture, with what to do for a counter many writers update
or a long computation. The mechanics:

Actor rows are locked with `FOR NO KEY UPDATE` while they are processed.

- **Senders to one actor are serialized**: the second waits for the first
  transaction to finish, then processes its event on the committed state. No
  lost updates — the test suite sends 2000 events from 8 concurrent clients to
  one actor and checks that all 2000 counted.
- **Cascades never wait.** Actors reached through events from the actor being
  processed are locked with `SKIP LOCKED`. If one is busy, its event stays in
  its inbox (committed with the sender) and is processed by its next send or
  by `tinyactors.run()`. A statement waits for at most one lock — the actor
  it addresses — so within one statement actors cannot deadlock each other.
  Across statements of one transaction the locks add up (also the cascade's):
  use `SET LOCAL tinyactors.cascade = 0` in multi-statement transactions and
  send in a consistent order (docs/concurrency.md).
- **Queueing never waits.** `FOR NO KEY UPDATE` does not conflict with the
  `FOR KEY SHARE` lock an inbox row's foreign key takes, so events for a busy
  actor are queued without blocking.
- **Outbox workers don't collide**: `claim` uses `FOR UPDATE SKIP LOCKED`.

```
 tx A: send(x)          tx B: send(x)               tx C: send(y), y sends to x
 lock x ───────┐        lock x … waits for A        lock y, process y
 process x     │                                    x is busy (A or B): SKIP LOCKED
 commit ───────┘──────▶ process x (sees A's work)   x's event stays in x's inbox
                        commit                      commit ─▶ run() delivers it later
```

## The outbox: effects on the outside world

Charts talk to the outside world through SCXML `<send>` with a `type` (an I/O
processor) listed in `tinyactors.processors`. Such a send becomes an outbox
row in the same transaction; the application performs it.

```xml
<send type="email" target="smtp" event="welcome" id="welcome"><param name="to" expr="email"/></send>
```

```sql
INSERT INTO tinyactors.processors VALUES ('email', 'the mailer service');
```

A worker loop:

```sql
LISTEN tinyactors_outbox;                          -- a notification per new row (after commit)
SELECT * FROM tinyactors.claim(50, 'mailer-1');     -- takes rows; others skip them
-- perform each, then:
SELECT tinyactors.ack(id);                          -- done
SELECT tinyactors.fail(id, 'mailbox full');         -- failed for good: the actor gets error.communication
```

- **At least once.** A claim is a lease (`lease` argument, default 30 s): rows
  claimed by a worker that died are claimed again. Make effects idempotent
  with `idempotency_key` (`<session>:<step>.<n>`, unique; the same retried event
  produces the same key).
- **Answers are events.** Report results back with `tinyactors.send(actor,
  'charge.ok', '{...}')`; failures with `fail()`, which delivers
  `error.communication` with the original `sendid` (`_event.sendid`), as
  SCXML specifies for undeliverable events.
- Registered by default: `outbox` (generic) and the BasicHTTP processor type
  (`http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor`, the application
  makes the request). Unregistered types raise `error.execution` in the chart.
- Sends to SCXML sessions of other systems (`tinyactors://other/...`) become
  outbox rows of type `scxml`.

## Timers

`<send delay="...">` (CSS2 times: `s`, `ms`) and `<cancel>` are durable. The
due time is absolute and stored with the actor; `next_due_at` is the
earliest one, indexed. Call `tinyactors.run()` periodically — from your
application, a cron job or `pg_cron` — to process actors whose timers are due
(and any queued events nobody processed):

```sql
SELECT cron.schedule('tinyactors', '1 seconds', 'SELECT tinyactors.run()');  -- with pg_cron ≥ 1.5
```

`run()` takes actors with `SKIP LOCKED`, so several schedulers can run it at
once. Timers armed before a server restart fire after it.

**Testing with time.** `SET tinyactors.now = '2030-01-01 00:00:00+00'` makes
the actors' clock say so (`tinyactors.now()` shows it); advance it and call
`run()`.

## Scale: millions of actors, a few hot

The table can hold millions of actors of which only some are ever busy; the
cost of an event, a timer or a `run()` does not depend on how many are stored:

- **An event costs one row**: a primary-key lookup, a lock, an update. An
  actor's state is a few hundred bytes; there is no per-actor memory between
  statements, and no per-actor memory growth within one: each actor processed
  in a call gets its own memory, freed when it is done (a `run(100000)` uses
  the memory of `run(1)`).
- **`run()` finds work through indexes**: actors whose earliest timer is due,
  oldest first (`actors_due`), then actors with deferred events, oldest event
  first. Millions of idle actors with pending timers are never looked at.
- **`send()` writes no inbox row**: the event goes straight to the locked
  actor (deferred events, if any, first).
- **Charts compile once per backend**, prepared statements are planned once:
  an event costs a fixed number of statements, none of them planned — 5 for
  a `send()` to an actor, 11 for an actor's send to another actor (both
  processed). `test/harden.py` (H24) keeps it that way.
- **The table is tuned for it**: `fillfactor = 80` leaves room for
  updates that stay on their page, and autovacuum is driven by what changed
  (1%), not by the size of the table (20%).

Measured with `mise run pg:scale` (test/scale.sh) on the instance of this
machine: 4 CPUs, PostgreSQL 17 with default settings, 8 pgbench clients; every
actor is a session with a small set of ids and a timeout that every touch
resets — so all 5 million have a pending timer.

| Actors | Spawn rate | Size per actor | Idle `run()` | Hot set (10,000) | Cold (random among all) | One hot actor | `run(5000)`, all due |
|---|---|---|---|---|---|---|---|
| 5,000,000 | 43,500/s | 718 B with indexes | 0.18 ms | 8,869 events/s, 0.90 ms | 7,392 events/s, 1.08 ms | 1,749 events/s, flat over 45 s | 416 ms (12,000 timers/s), backend memory flat |

The same numbers hold at 200,000 actors: nothing in the processing path grows
with the population (there, on the same machine: 43,500 spawns/s, 7,900
events/s hot and 8,000 cold, `run(5000)` 437 ms). `run()` processes every
actor in a subtransaction of its own (one failing actor cannot stop the
others — *Production safety*). Cold events are as fast as hot ones while the
table fits in memory (the operating system's cache counts); beyond that a
cold event costs a page read.

**Very hot actors.** One actor's events are serialized by its row lock
(that is the actor model's guarantee), so one actor handles about 1,800
events/s here, independent of the population. Each of its events leaves a dead
row version until vacuum removes it; with the table settings above the rate
holds. Under sustained thousands of events per second to a single actor,
lowering `autovacuum_naptime` (server-wide) keeps the versions in check. Spread
work over actors when you can: 8 clients on 10,000 actors reach 8,900
events/s, and that grows with cores.

**Sizing.** About 700 bytes per small actor (row, TOAST and indexes): 5
million ≈ 3.4 GB. Bigger datamodels cost what their JSON costs (the
`datamodel` column) plus the exported state.

## Store links: other systems

The database is a system like any other (docs/protocol.md): its actors
send to `tinyactors://<other>/...` and receive from other systems, through
a *store link* — a process with a SQL connection that moves envelopes.

```
 Node server "web"                        PostgreSQL "vault"
 ┌───────────────────┐   deliver(envelope) ┌──────────────────────┐
 │ router            │ ──────────────────▶ │ inbox → actor (row)  │
 │   ▲   store link  │                     │                      │
 │   │               │ ◀────────────────── │ outbox (peer = web)  │
 └───┼───────────────┘   claim_for('web')  └──────────────────────┘
     │ WebSocket link     + ack, NOTIFY
  browsers
```

- **In:** `SELECT tinyactors.deliver('{"target": "tinyactors://vault/@counter",
  "name": "add", "data": {...}, "origin": "tinyactors://web/#_scxml_7",
  "msgid": "..."}')` processes the event in your transaction. The origin
  is kept, so the actor can reply with `<send targetexpr="_event.origin">`.
  Repeats of a `msgid` (or `dedup` key) within `tinyactors.dedup_ttl` are
  answered `'duplicate'` and change nothing.
- **Out:** a send to another system writes an outbox row with type
  `scxml`, `system` (the target's), `peer` (who takes it) and the complete
  `envelope`. A store link says `hello` with its id and the systems it
  reaches, then claims its rows with `claim_for(peer, …)` (in order), sends
  them, `ack`s them, and reports refusals with `deliver_error` — the sending
  actor gets `error.communication`. `NOTIFY tinyactors_outbox` carries the
  peer, so a link wakes only for its own rows.
- **Guarantees:** durable and at-least-once (a crash between sending and
  `ack` repeats an envelope, with the same `msgid`, which the receiver drops);
  ordered per peer with one claimer per peer; no transaction spans systems
  (docs/concurrency.md, section 6).
- **Node.js:** `createStoreLink(system, { query, listen })` (bindings/node)
  is a complete store link for any PostgreSQL client; the chess example
  keeps its game records in an actor here and replies to browsers through
  it (examples/chess).

## Reference

### Functions

| function | returns | what |
|---|---|---|
| `tinyactors.define(name text, scxml text)` | `integer` | compile and store a new version of a chart; errors point at the line |
| `tinyactors.spawn(chart text, input jsonb = '{}', name text = NULL)` | `bigint` | start an actor of the newest version; `input` overrides `<data>` initial values; `name` is an optional unique handle |
| `tinyactors.send(actor bigint, event text, data jsonb = NULL)` | `tinyactors.actors` | process the event now (and the actors it sends to); `_event.name` = `event`, `_event.data` = `data` |
| `tinyactors.send(name text, event text, data jsonb = NULL)` | `tinyactors.actors` | the same, by name |
| `tinyactors.deliver(actor bigint, event text, data jsonb = NULL)` | `boolean` | what `send` does, without returning the row: `false` if there is no such actor (no error). Both accept the id of an invoked session too |
| `tinyactors.process(actor bigint)` | `integer` | process the actor's queued events and due timers (waits for its lock); actors processed |
| `tinyactors.run(max_actors integer = 1000)` | `integer` | process up to `max_actors` actors with due timers or queued events that nobody is processing; costs what it finds, not what is stored |
| `tinyactors.stop(actor bigint)` | `void` | end an actor without a done event; drops its queued events and timers |
| `tinyactors.claim(n integer = 10, worker text = NULL, lease interval = '30 s')` | `SETOF tinyactors.outbox` | lease undelivered outbox rows |
| `tinyactors.ack(id bigint)` | `void` | mark an outbox row delivered |
| `tinyactors.fail(id bigint, error text = 'failed')` | `void` | mark it failed; the actor receives `error.communication` |
| `tinyactors.compile(scxml text, name text = 'chart')` | `bytea` | compile without storing (validation) |
| `tinyactors.now()` | `timestamptz` | the actors' clock |
| `tinyactors.deliver(envelope jsonb, peer text = NULL)` | `text` | an envelope from another system (store links; `peer`: the link it came through — its origin must be that peer or a system its hello announced): `'ok'`, `'duplicate'` (seen within `dedup_ttl`), or the refusal reason (`unknown-target`, `unreachable`, `malformed`, `denied`) |
| `tinyactors.hello(id text, reach text[] = '{}')` | `text` | a store link announces itself and the systems it reaches; returns this database's system id |
| `tinyactors.claim_for(peer text, n integer = 100, worker text = NULL, lease interval = '30 s')` | `SETOF tinyactors.outbox` | lease envelopes for one peer, oldest first (`envelope` column) |
| `tinyactors.deliver_error(err jsonb)` | `boolean` | an `err` from another system about an envelope sent from here: `error.communication` for the sender |
| `tinyactors.prune_seen()` | `integer` | drop dedup entries older than `tinyactors.dedup_ttl` |
| `tinyactors.system_id()` | `text` | this database's system id |
| `tinyactors.address(text)` | `(system, kind, session, name)` | parse an actor address |
| `tinyactors.data_ok(jsonb)` | `boolean` | event data an actor accepts (nested at most 100 levels) |
| `tinyactors.grant(role name, what text = 'app')` | `void` | give a role the `app`, `worker`, `link` or `admin` privileges (see *Privileges*) |
| `tinyactors.grant_peer(peer text, role name)` | `void` | allow a role (with `link`) to act as a store-link peer |

### Settings

| setting | default | what |
|---|---|---|
| `tinyactors.fuel` | 1000000 | work an actor may do per processing; beyond it the transaction fails |
| `tinyactors.cascade` | 1000 | actors processed per call after the first (the rest waits for `run()`) |
| `tinyactors.log` | `log` | where `<log>` goes: `debug`, `log`, `info`, `notice`, `warning` or `off` |
| `tinyactors.now` | empty | the actors' clock (a timestamptz); empty: `clock_timestamp()`; superuser |
| `tinyactors.system_id` | empty | this database's system id in actor addresses; empty: derived from the database name (`my_app` → `my-app`); superuser; set per database |
| `tinyactors.dedup_ttl` | `7 days` | how long `deliver(envelope)` remembers message ids (`tinyactors.seen`; `run()` prunes) |
| `tinyactors.js_memory_limit` | `256MB` | memory the backend's JavaScript may use; beyond it the allocating script fails (`error.execution`) |
| `tinyactors.max_state_bytes` | `64MB` | largest stored actor (state + datamodel column); beyond it its transaction fails |
| `tinyactors.max_system_bytes` | `256MB` | largest in-memory system of one processing (endless `<invoke>`s, queues) |
| `tinyactors.inbox_batch` | 1000 | queued events delivered per processing; a backlog is worked off in bounded steps |
| `tinyactors.max_failures` | 10 | failed processings in a row after which `run()` marks an actor `failed` (0: never) |
| `tinyactors.log_limit` | 100 | `<log>` lines per processing (the rest are counted in one line; messages cut at 1000 bytes) |
| `tinyactors.notify` | `on` | `NOTIFY tinyactors_outbox` on new outbox rows (skipped anyway while the queue is half full) |
| `tinyactors.outbox_retention` | `7 days` | delivered and failed outbox rows older than this are deleted by `run()` |
| `tinyactors.store_datamodel` | `on` | write the `datamodel` jsonb column on every event (`off`: NULL, saves the rewrite and its WAL for big datamodels) |
| `tinyactors.max_chart_bytes` | `1MB` | largest SCXML document `define()` compiles; superuser |

### The SCXML you can use

All of SCXML 1.0 with the `ecmascript` or `null` datamodel — states,
parallel and final states, history, eventless transitions, `<assign>`,
`<script>`, `<if>`, `<foreach>`, `<send>` with delays and `<cancel>`,
`<raise>`, `<log>`, `<donedata>`, `In()`, `_event`, `_sessionid`,
`_name`, `_ioprocessors` — and `<invoke>` of SCXML sessions: inline
`<content>`, or `src` naming a chart of the definitions table (see *Invoked
sessions* and *Invoking defined charts*).

## Production safety

The extension runs charts inside your database's backends, so it is built to
fail small: a broken or hostile chart costs its own transaction, never the
server. Each of these is a live reproduction in `test/harden.py`
(`mise run pg:test`; gate 22, tools/check_pg_hardening.py):

| risk | what happens |
|---|---|
| a script that never ends (`while (true) {}`) | JavaScript checks PostgreSQL's interrupt flag: `statement_timeout`, `pg_cancel_backend` and `pg_terminate_backend` stop it within milliseconds; every 100 operations cost one unit of `tinyactors.fuel`, so a processing is bounded even without a timeout |
| a script that allocates without end | `tinyactors.js_memory_limit` (256MB): the allocation fails, the chart gets `error.execution`; stored state is capped by `tinyactors.max_state_bytes`, the in-memory system by `tinyactors.max_system_bytes` |
| a trigger on a tinyactors table that calls back into tinyactors | a clean error (`cannot be called while an actor is being processed`), never a crash |
| one actor that always fails (a runaway, a bug) | `run()` processes each actor on its own: the failure is rolled back and recorded (`failures`, `last_error`, `retry_at`, backoff up to an hour; `failed` after `tinyactors.max_failures`); the others go on. A failing actor reached through a cascade does not abort your transaction |
| a backlog of millions of queued events | worked off `tinyactors.inbox_batch` events per processing, memory flat, order kept; `send()` to a backlogged actor queues behind it |
| deeply nested event data | refused at the boundary (`send`: an error; `deliver(envelope)`: `malformed`); a value a chart builds too deep to store fails its transaction instead of bricking the actor |
| strings jsonb cannot hold (`\u0000`, lone surrogates) | stored as U+FFFD |
| a redefined or rolled-back chart | the per-backend image cache checks the row version on every use |
| many queued actors, one of them locked | `run()` scans distinct actors (a loose index scan) from where it stopped: nobody starves |
| a stalled `LISTEN`er | notifications are skipped while the queue is half full; effects never fail on it |
| `<log>` floods | `tinyactors.log_limit` lines per processing |
| huge delays (`delay="300000000000s"`) | saturate to "never" (`next_due_at = infinity`), never wrap around to fire early |
| huge SCXML documents | `tinyactors.max_chart_bytes`; below it compiling takes time linear in the document (milliseconds per megabyte), and `statement_timeout`, `pg_cancel_backend` and `pg_terminate_backend` stop it |
| tables that only grow | `run()` prunes delivered/failed outbox rows (`tinyactors.outbox_retention`) and the dedup window (`tinyactors.dedup_ttl`), 10,000 rows a minute at most per backend |

### Privileges

Nothing in the schema is executable by `PUBLIC`. The API functions run
`SECURITY DEFINER` (as the extension's owner, `search_path` fixed), so roles
need no privileges on the tables — only on the functions, handed out in
groups:

```sql
CREATE ROLE app LOGIN;     SELECT tinyactors.grant('app', 'app');        -- spawn, send, deliver, run, stop; read actors and charts
CREATE ROLE mailer LOGIN;  SELECT tinyactors.grant('mailer', 'worker');  -- claim, ack, fail: the application's outbox
CREATE ROLE web LOGIN;     SELECT tinyactors.grant('web', 'link');       -- a store link: hello, claim_for, deliver(envelope, peer), ...
                           SELECT tinyactors.grant_peer('web', 'web');   -- ... as peer "web" only
CREATE ROLE ops LOGIN;     SELECT tinyactors.grant('ops', 'admin');      -- all of it, define, and the tables
```

- A link role can act only as the peers it was granted: `hello`,
  `claim_for`, `ack`/`fail` of its envelopes and `deliver(envelope, peer)`
  (whose origin must be that peer or a system its hello announced) refuse
  every other peer id. A hello cannot announce another granted peer's system
  (so its envelopes cannot be redirected). Peer rights are checked against
  the session's login role (`session_user`).
- `tinyactors.now` (the actors' clock) can only be set by superusers.
- Charts are code: only give `admin` (which includes `define`) to roles you
  would let deploy code.

## Operations

- **Install**: `CREATE EXTENSION tinyactors;` (schema `tinyactors`; needs
  the shared library from `mise run pg:build` or `make install`).
- **Scheduling**: one `SELECT tinyactors.run()` per second (or as often as
  your timers need resolution) from any number of schedulers.
- **Monitoring**:
  ```sql
  SELECT status, count(*) FROM tinyactors.actors GROUP BY 1;                       -- population
  SELECT count(*) FROM tinyactors.inbox;                                           -- deferred events
  SELECT count(*) FROM tinyactors.actors WHERE next_due_at < now() - interval '1 minute';  -- late timers
  SELECT count(*), min(created_at) FROM tinyactors.outbox WHERE delivered_at IS NULL;      -- outbox lag
  ```
- **Retention**: `run()` deletes delivered and failed outbox rows after
  `tinyactors.outbox_retention` (7 days) and dedup entries after
  `tinyactors.dedup_ttl`. Finished actors keep their row (`status`, `output`,
  last `datamodel`); delete them on your schedule:
  ```sql
  DELETE FROM tinyactors.actors WHERE status <> 'active' AND updated_at < now() - interval '90 days';
  ```
- **Failing actors**: `SELECT id, failures, last_error, retry_at FROM
  tinyactors.actors WHERE failures > 0;` — fix the chart (define a new
  version and `migrate`) or `stop()` them.
- **Backups, replicas, point-in-time recovery**: actors are ordinary rows,
  so everything PostgreSQL does for your data it does for them.
- **Logs**: `<log>` output goes to the server log as
  `tinyactors <id>: <label>: <message>` (`SET tinyactors.log = notice` sends
  it to the client instead).

- **Scale testing**: `mise run pg:scale` (`TA_SCALE_N=5000000` for the
  table above) on the `pg:start` instance, in a database of its own that it
  drops afterwards (`TA_SCALE_KEEP=1` keeps it). Live, in the torture chamber:
  `mise run torture postgres-millions` grows a durable population to 2
  million while traffic runs (`--param population=…`).
- **Load and failure testing, live**: `mise run torture postgres` (or the
  PostgreSQL card on the [torture chamber](../../tools/torture/README.md#postgresql)
  home page) runs 2,000 account actors under concurrent pgbench transfers with
  the scheduler and an outbox worker as separate processes — the deployment
  shape described above — checks every second that money is conserved, and
  restarts PostgreSQL in the middle. The page shows whether this instance is
  up and starts it if it is not. Reference: ~7,400 events/s on 4 CPUs, drift 0
  across the restart.

## Limits

- **Invoked sessions** are stored with their actor, so a very large tree
  makes a large row; one processing may start up to 1,048,576 invoked
  sessions. `src` names charts of the definitions table; there is no file
  system or network to load other documents from.
- **Payloads are JSON.** Event data, `<data>` values in `input`, outbox data
  and the `datamodel` column are `jsonb`; values JSON cannot express (functions,
  `undefined`) do not survive the round trip.
- **Cross-actor error reporting** for events to actors that do not exist is
  immediate (`error.communication` in the sender); an actor that finished
  after the event was queued drops it silently.
- **Charts are trusted code**: ECMAScript in charts runs inside the database
  backend (without I/O: no file, network or process access), bounded by
  `tinyactors.fuel`, `tinyactors.js_memory_limit` and cancellation. Only let
  trusted roles call `define`.
- **One evaluation** (an expression or a `<script>`) runs at most about
  10^8 JavaScript operations; event data nests at most 100 levels.

## Build and test

```sh
mise run pg:build    # position-independent static libraries + the extension (PGXS), installed into the pinned PostgreSQL 17
mise run pg:test     # a throwaway server: pg_regress suites (test/sql, test/expected)
                     # + test/concurrency.sh: pgbench contention, skipped locks, rollback, restart
mise run pg:start    # the instance for development (port 54320)
mise run pg:scale    # millions of actors on that instance (TA_SCALE_N, default 2,000,000)
```

The toolchain (PostgreSQL 17 with server headers, from conda-forge) is pinned
in `mise.toml` here; 17 is what the tests run against. For another
installation: build the libraries (`mise -C bindings/postgres run libs`), then
`make PG_CONFIG=/path/to/pg_config install` (the code uses APIs available
since PostgreSQL 15, but only 17 is tested).
