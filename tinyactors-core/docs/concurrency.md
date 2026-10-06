# Concurrency and parallelism

How tinyactors runs many actors, and what you can rely on. Read this before
you put actors behind a web server, spread them over threads, or store them
in PostgreSQL. It builds from one actor up to a database full of them.

Two words first, because they are easy to mix up:

- **Concurrency** — many actors in progress at the same time, interleaved.
  tinyactors gives you that everywhere, with one thread.
- **Parallelism** — several actors executing at the same instant on
  different cores. You get it by running several *systems* (threads,
  workers, processes) or several PostgreSQL connections. One system never
  runs two actors at the same instant.

## 1. One actor: run to completion

An actor (an SCXML session) handles one event at a time. Processing an event
is a *macrostep*: the transition, all eventless transitions it enables, and
all internal events (`<raise>`) — until the actor is *stable*. Only then does
it look at its next external event.

```
 mailbox:  e3  e2  e1 ──▶ ┌────────────────────────────────────┐
                          │ macrostep(e1): transitions,        │
                          │ eventless transitions, <raise>d    │ ──▶ stable ──▶ take e2
                          │ internal events, until stable      │
                          └────────────────────────────────────┘
```

What that buys you, without a single lock in your charts:

- Inside a macrostep the actor's data cannot change under it. Nobody else
  can see or modify its `<datamodel>`.
- Events from one sender arrive in the order they were sent.
- `_event` is the event being processed, for the whole macrostep.

What it does **not** mean: that a macrostep runs uninterrupted on the CPU.
See the next section.

## 2. One system: many actors, one thread, cooperative

A `ta_system` holds any number of actors. It runs them on the thread that
calls `ta_system_run(system, budget)`, one at a time, from a *run queue* of
actors that have work:

```
 run queue:  [A] [B] [C]          ta_system_run(sys, 100000 fuel)
              │
              ▼
           run A for one quantum ──▶ A stable?  ── yes ─▶ A leaves the queue
                                       │
                                       no (still in its macrostep)
                                       ▼
                                 A goes to the back: [B] [C] [A]
```

- **Fuel, not time.** Every step of work costs fuel (a transition, a
  datamodel call, an executable-content instruction). Each actor gets a
  *quantum* of fuel per turn; the call gets a *budget*. Both are counts, so a
  run is deterministic: the same events give the same interleaving, on any
  machine.
- **Yielding is invisible.** When an actor's quantum runs out in the middle
  of a macrostep, it is parked where it was (a few cursors, no stack) and
  resumes later. Its macrostep is still one macrostep: no event is taken
  from its mailbox meanwhile, `_event` is unchanged. Another actor ran in
  between — but it could not see or touch this actor's state, so nothing can
  tell.
- **Nobody starves.** An actor in an endless eventless loop (`a → b → a …`)
  gets its quantum and goes to the back of the queue; the others still run,
  and `ta_system_run` returns to you when the budget is spent
  (`TA_RUN_YIELDED`). You decide when to call it again.
- **Sending is enqueueing.** `<send>` to another actor of the same system
  puts the event in its mailbox and makes it runnable; the sender carries
  on. Nothing runs recursively; there is no call stack between actors.

The intuition: *a system is an event loop for actors.* Like a JavaScript
event loop, it has no data races because only one thing runs at a time, and
it stays responsive because each thing runs only for a while.

### Your thread and the system

`ta_system_run` returns after the budget — with the effects of the run
(`TA_EFFECT_SEND` to the outside, logs, done, requests). In between calls the
system does nothing at all: no threads, no timers firing on their own.
Time moves when you call `ta_system_advance_time`; `ta_run.next_deadline`
tells you when the next delayed send is due, so you can sleep until then.
That is why the same library fits libuv, asyncio, a game loop, a database
backend or a microcontroller.

**Thread safety.** A system is not thread-safe, and does not need to be: it
is a plain block of memory. Use it from one thread at a time (any thread —
it has no thread affinity). Calls into it while it is running — from a
datamodel callback, say — are refused (`TA_ERROR_STATE`), so you cannot
re-enter it by accident; handle effects after `ta_system_run` returned. The library has no global mutable state: different systems on
different threads never interact.

## 3. Many systems: parallelism

To use more cores, run more systems — one per thread, worker or process —
and let them address each other:

```
 thread 1                         thread 2
 ┌─────────────────────┐          ┌─────────────────────┐
 │ system "orders"     │  send    │ system "billing"    │
 │ A  B  C ...         │ ───────▶ │ X  Y  Z ...         │
 └─────────────────────┘ (host)   └─────────────────────┘
```

- An actor's address is `tinyactors://<system>/#_scxml_<id>` (or
  `.../@<name>`). A send to another system goes to the router in `ta_host`,
  which hands it to the transport of the *peer* that reaches that system as
  a `TA_HOST_EFFECT_PEER` effect; the transport (a queue between threads, a
  socket, a `postMessage`, SQL) delivers it with `ta_host_deliver` on the
  other side. Replies use `_event.origin`. See
  [protocol.md](protocol.md) and section 6 below.
- Systems share nothing, so they need no locks between them. The only
  coordination is the hand-over of events, which you own.
- A datamodel instance belongs to one thread at a time: give each system (or
  each thread) its own `ta_datamodel_ecmascript`. The ECMAScript engine is not
  thread-safe, and does not have to be.
- Order is per sender and receiver: two events from A to X arrive in order;
  events from A and from B to X interleave in arrival order — as with any
  network of actors.

`ta_host` groups several systems that share one address and one datamodel
host (chart versions, segments) — on one thread. It is a convenience for
hot reload and growth, not a parallelism mechanism.

## 4. Node.js and browsers

The Node and web bindings run one host per `createSystem()` on the
JavaScript thread, in slices: a slice runs for about 4 ms of wall time (the
fuel budget adapts to the machine), then yields to the event loop
(`setImmediate` / a macrotask) and continues. So:

- Your server keeps answering requests while 100,000 actors work; a
  runaway chart slows its system down, it does not block the process.
- `actor.send()` from your code runs one slice right away: an ordinary event
  is fully processed when `send()` returns, and `getSnapshot()` shows the
  result. Work that does not fit in the slice continues on the next turn of
  the event loop (`await system.flush()` waits for all of it, in tests).
  Called from inside an action, `send()` is queued until the current slice
  returns — a chart never runs inside another chart's step.
- Datamodel code (your chart's expressions and `<script>`) runs on the
  JavaScript thread, inside the slice.
- Parallelism: one system per worker thread (`worker_threads`, Web Workers)
  and links between them, as in section 3. A single system never uses more
  than one core.

## 5. PostgreSQL: actors as rows, parallel by connection

In the PostgreSQL binding there are no resident actors. An actor is a row;
processing it is a short stretch of work inside the transaction that asks
for it:

```
 BEGIN
   SELECT tinyactors.send(42, 'pay', '{...}')
     lock row 42 (FOR NO KEY UPDATE) ─▶ load it ─▶ run the macrostep
     ─▶ events for other actors ─▶ their rows (if free, now; else their inbox)
     ─▶ store row 42 (and the outbox rows)
   ... your own SQL ...
 COMMIT   ◀── everything above becomes visible at once, or not at all
```

### What runs in parallel

PostgreSQL runs one process per connection, so **the unit of parallelism is
the connection**:

- Two connections working on *different* actors run in parallel, on
  different cores, without waiting for each other.
- Two connections sending to the *same* actor are serialized by its row
  lock: the second waits until the first commits (or rolls back), then sees
  its result. That is the actor model's guarantee — one event at a time per
  actor — enforced by PostgreSQL. An actor's rate is bounded by how fast one
  connection processes and commits one event (about 2,000 events/s on the
  reference machine); throughput comes from many actors, not from one.

```
 conn 1: send(A) ──[lock A]── run ── commit
 conn 2: send(B) ──[lock B]── run ── commit           parallel: different rows
 conn 3: send(A) ──── waits for A ────[lock A]── run ── commit
```

### Actors talking to each other

When actor A sends to actor B during `send(A)`:

- If B is free, B is processed in the same call and the same transaction
  (breadth first, up to `tinyactors.cascade` actors), so a conversation
  usually completes before `COMMIT`.
- If B is locked by another transaction, the event is written to B's inbox
  (with `SKIP LOCKED`: A's transaction does **not** wait) and B processes it
  on its next send or on `tinyactors.run()`.

So a statement waits for at most one lock — the actor it addressed — and
never holds one actor's lock while waiting for another's: **within one
statement, actors cannot deadlock each other**, whatever your charts send to
whom. The price: an event between actors can be deferred until `run()` comes
by.

Across statements it is different, as with any row locks: the locks a
statement takes are held until `COMMIT`, including the hidden ones of its
cascade (A's chart sent to B, so `send(A)` processed and locked B too). Two
transactions that each send to several actors, in different orders, can
deadlock; PostgreSQL detects it and aborts one of them (40P01). In
multi-statement transactions:

- set **`SET LOCAL tinyactors.cascade = 0`**: each `send()` then locks only
  the actor it addresses (the events it sends wait in inboxes for `run()` or
  their next send), so the only locks are the ones your statements name;
- and send to actors in a consistent order (by id), or retry on 40P01 — as
  for any transaction that updates several rows.

A failure of an actor reached through the cascade (a runaway chart, an
error) does not abort your transaction: that actor's processing is rolled
back on its own, recorded on its row (`failures`, `last_error`, `retry_at`)
and retried later by `run()`. Only the actor you addressed can fail your
statement.

### Transactions are the unit of truth

- An actor's new state, the events it sent to other actors (and what those
  did), its timers and its outbox rows commit together with *your* SQL in
  the same transaction — or roll back together. A rolled-back event never
  happened: no other actor saw it, no effect was requested.
- Readers never block: a plain `SELECT` on `tinyactors.actors` sees the last
  committed state, while a transaction holding the actor's lock works on it.
- Inside your transaction you read your own writes: `send()` returns the
  actor after the event.

### Timers and the outbox: workers you run

- `tinyactors.run(n)` processes up to `n` actors with due timers or deferred
  events, taking rows nobody else holds (`SKIP LOCKED`). Run it from as many
  schedulers as you like (a loop, cron, `pg_cron`); they share the work and
  never process an actor twice.
- The outbox works the same way: `claim(n)` leases undelivered effects with
  `SKIP LOCKED`, so any number of delivery workers share them; `ack` or
  `fail` closes them. Delivery is at least once; the idempotency key
  (`<session>:<sendid>`) lets the receiving side drop duplicates.

### Invoked sessions

An actor's `<invoke>`d sessions are stored in its row and processed under
its lock. A parent and its children therefore never run in parallel with
each other — they are one unit of concurrency, like one actor. If two
parts of your design must run in parallel, make them two actors (spawned,
talking by events), not parent and child.

### Choosing the shape

| You want | Do |
|---|---|
| more throughput | more actors, more connections; one actor is serial by design |
| a value many writers update (a counter, a balance) | one actor, if a few thousand events/s suffice; otherwise shard it into several actors and sum |
| a long computation | not in a macrostep (it holds the row lock); emit an outbox effect and receive the result as an event |
| work that must happen exactly once outside the database | the outbox, with the idempotency key on the receiving side |
| timers with second resolution | `run()` once a second from one or more schedulers |

## 6. Between systems: what each transport guarantees

Inside one system, SCXML's rules hold exactly. Between systems there is no
shared clock, no shared lock and no shared transaction, so the guarantees
are those of the transport that carries the envelope — and every transport
states them ([protocol.md](protocol.md), "Delivery"):

| transport | order | duplicates | loss | errors back |
|---|---|---|---|---|
| in-process (segments of one `ta_host`) | per sender and receiver | never | never | `error.communication` at once |
| WebSocket link (`createLink` / `serveLinks`) | per sender and receiver while connected; queued envelopes keep their order across a reconnect | never on one connection | possible if a process dies with envelopes in memory (at-most-once across crashes) | `err` frame → `error.communication`; queued past the TTL → `expired` |
| store link (PostgreSQL, `createStoreLink`) | per peer, in outbox order (one claimer per peer) | possible (at-least-once: a crash between delivery and ack repeats it) — dropped by the receiver's dedup window (`msgid`) | never once committed (outbox rows and the inbox are durable) | `deliver()` returns the reason / `deliver_error()` → `error.communication` |

What this means for you:

- **Nothing is dropped silently.** An envelope that cannot be delivered —
  unknown session or name, no route, refused by policy, queue full, expired —
  becomes `error.communication` (with the `sendid`) in the session that sent
  it, across any number of hops. Design charts to handle it.
- **Order holds per pair, not globally.** Two envelopes from A to X arrive
  in order; envelopes from A and from B to X interleave. Relays keep the
  order of each pair.
- **At-least-once needs idempotent receivers, and the router gives you
  that.** Every envelope has a `msgid` (`<origin>/<counter>`), stable across
  retries; each system keeps a dedup window (C: in snapshots; PostgreSQL:
  `tinyactors.seen` for `tinyactors.dedup_ttl`). An application key can
  replace it (`dedup`: "this payment, once").
- **No distributed transactions.** A send to another system is never part of
  a transaction spanning both. In PostgreSQL the envelope is written to the
  outbox in your transaction and leaves after `COMMIT`; it reaches the other
  system some time later, and its reply is a new event in a new transaction.
  "My write and the event, together" is the local system's job; "exactly
  once over there" is at-least-once plus dedup.
- **A store link is a peer like any other.** A Node server connected to a
  database of actors routes between the database, other servers and
  browsers: PostgreSQL → server → browser works because the server announced
  the browser to the database (`tinyactors.hello`) and relays for it.
- **Ephemeral peers resume.** A browser that reconnects with the same system
  id within `resumeMs` gets what was queued for it, in order; after that the
  senders get `error.communication` (`expired`). The window runs on the
  system's clock (with a `clock` option, simulated time), like the queue TTL.

## 7. What tinyactors never does

- It never starts a thread, never sleeps, never blocks on I/O, never reads a
  clock. Everything happens inside a call you make, and returns.
- It never runs two macrosteps of one actor at the same time, and never
  interleaves two events of one actor.
- It never lets one actor see another's data. All interaction is events.
- It never changes SCXML-observable behaviour because of scheduling: the
  interleaving decides *when* an actor runs, never *what* it computes from
  the events it received.

## Where to read more

- [architecture.md](architecture.md) — the continuation machine (§7), the
  scheduler, mailboxes and timers (§12), hibernation and hot reload (§16).
- [scenarios.md](scenarios.md) — step-by-step walkthroughs, including an
  infinitely looping actor sharing the thread.
- [postgres.md](postgres.md) — why the PostgreSQL binding locks the way it
  does; [bindings/postgres/README.md](../bindings/postgres/README.md) for the
  SQL reference.
- `tools/torture` — scenarios that measure all of this live: `chaos`
  (fairness), `federation` (several systems), `postgres` and
  `postgres-millions` (connections, contention, restarts).
