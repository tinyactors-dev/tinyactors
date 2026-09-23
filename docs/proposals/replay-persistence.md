# Replay-based persistence for named actors

**Status:** Sketch (2026-09-23; storage cross-check added 2026-09-24)
**Touches:** tinyactors-core (one ADR), tinyactors-napi (scheduler, pump, router, effects), later the host kernel
**Not a plan:** nothing here is decided or scheduled. ADRs and roadmaps win.

## Goal

A named actor can be evicted from the system at any time without capturing
anything. When mail arrives for it, the host reads the actor's full message
history and replays it through the **current** revision of its chart, and
the actor carries on. New chart revisions are picked up without any extra
step, because the state is re-derived from history each time the actor is
summoned.

Decisions already made in the discussion:
- **Named actors only.**
- **History grows without bound.** There are no snapshots. Each summon
  replays everything.
- **No separate replay system.** Replay runs inside the live system, as a
  mode the actor is in.

## Why "named only" helps

- **Origins are addresses.** Every origin core writes for a named actor is
  its address (core ADR 0065), so replies follow the name. The session ID
  may change on every summon, and that doesn't matter. Charts must address
  named actors by name, never by `#_scxml_<id>`.
- **The directory is the summon trigger.** Mail for a name with no resident
  actor is the unloaded-entry case. It is handled by the loader hook left
  open by napi roadmap step 14.
- **The log is keyed by address,** and an address survives restarts.

## Recording

- Every live delivery of an event to a named actor that core accepts
  (`ta_actor_deliver` returns `TA_OK`) appends one record to that actor's
  log: `{time, event, sendid, origin, invokeid, data}`.
  - This covers posts, messages from other actors, delayed sends that
    fired, invoker reports and I/O results. In napi all of these arrive as
    events.
  - Internal events are not logged. Core regenerates them during replay.
- **Write-ahead with group commit.** Appends are committed once per turn,
  before that turn's effects are released. Napi already dispatches effects
  after the turn (napi ADR 0007, step 13), so there is a natural place for
  the commit.
- **Commits run off the event loop.** `node:sqlite` and `bun:sqlite` are
  synchronous, and an fsync inside a turn would stall the loop for
  milliseconds. So a commit runs on a worker thread, natively in the host
  kernel, or through an asynchronous database driver. Turns pipeline: the
  effects of turn N wait for commit N, and turn N+1 runs meanwhile. This
  adds commit latency (about 1–5 ms on a local SSD) to every effect, but it
  does not limit throughput.
- **Mail in flight must also be logged when it is accepted.** Napi's
  mailboxes are in memory. If actor A sends to B and the process crashes
  before B's delivery, the message is lost: A's replay suppresses its sends,
  and B never logged the message. So an event is also recorded when it is
  accepted into a mailbox (a durable mailbox), and the record is marked as
  delivered or dead-lettered later. In the same transaction as the turn's
  delivery records, this makes messages between actors survive a crash.
  The alternative is to document in-flight mail as at-most-once.
- Routing outcomes need no record. Napi's router never refuses a delivery:
  unknown and finished targets become dead letters (napi ADR 0006). Async
  failures of I/O processors come back as events and are logged like any
  other event.
- When the actor finishes, the log records that it finished. A later summon
  then dead-letters new mail with reason `done`, instead of replaying into a
  final state again.

## Summoning: history as a prefix of the mailbox

1. Mail arrives for an evicted name.
2. The host spawns the current revision of the chart under that name, and
   sets the scheduling entry (napi ADR 0005) to a new state, `replaying`.
3. The host streams the log into the front of the actor's mailbox, in pages
   when the store is asynchronous. Each replay envelope carries its original
   time and a replay flag.
4. New live mail waits behind the history, so order is kept.
5. The scheduler delivers replay envelopes like any others. The work is
   spread over turns within the normal budgets, so a long history never
   blocks the event loop.
6. When the last replay envelope has been delivered, the actor leaves
   replay mode (see core support below) and live mail flows.

**Effects are suppressed while a replay envelope is being processed:**
- the router drops deliveries whose origin is replaying;
- I/O and log effects are tagged and dropped;
- invoker starts are held back.

When replay ends, the host starts services only for invocations that are
still active in core, because that is the live state. Invocations started
and cancelled during replay never start. Invoked SCXML child actors come
back fresh.

## Core support: an actor can be replayed in place

Replaying inside the live system breaks on time in two ways:

1. **Due times.** Core computes `due = now + delay` from the system clock.
   Replaying last week's event today would arm last week's timeout for
   today plus its delay.
2. **Sends that already fired.** A send that fired while the actor was
   resident would be armed again during replay. The live system's dispatch
   would then fire it a second time, in the middle of the replay, at
   whatever turn it happened to fall on.

A core ADR would add three small capabilities:

| Capability | Purpose |
|---|---|
| A **time override per resume** (for example `now` in `ta_actor_resume_params`) | sends armed during replay get their original due times |
| **Hold an actor's delayed sends out of dispatch** while it replays (take its entry out of the owner heap of core ADR 0054) | nothing fires in the middle of a replay |
| At the end of replay, **withdraw pending sends due at or before a given time** (the eviction time), then release the rest | sends that fired while the actor was resident do not fire again. Sends that fell due while it was evicted fire late, as after `unload`/`load`. Future sends stay armed. |

A self-send that fired before eviction is withdrawn, and its delivery is in
the log, so it is replayed exactly once.

Everything else uses interfaces that exist today: spawn, deliver, resume,
the router, effects, the directory, and the scheduling table with one more
state.

## The chart author's contract

- **Determinism.** A chart's behaviour must depend only on its events and
  on logical time. JS expressions must not read `Date.now()`,
  `Math.random()` or other outside state; such values must arrive as event
  data. A SQL data model must not read shared tables in expressions.
- **Address by name.** Never keep or send a named actor's session ID.
- **Revisions change history.** Replaying old history through a new
  revision re-derives the state. Effects that would have been different in
  the past are silently dropped; only the resulting state carries over.
  Events that the new chart does not handle are discarded (normal SCXML
  behaviour). Renamed or reshaped events need **upcasters**: host functions,
  keyed by family and revision, that rewrite old log records.

## Where the pieces go

| Piece | Where |
|---|---|
| Replay in place: time override, held sends, withdrawal by time | core (one ADR) |
| The `replaying` state, history ahead of live mail, effect suppression, starting still-active services at the end | host kernel scheduler (napi ADR 0005 plus a new napi ADR) |
| Log append and group commit before effects | host kernel pump |
| Log storage, upcasters, eviction policy | a host adapter and the JS layer |
| Summoning on mail for an evicted name | napi step 14's open loader hook |

## Costs

- **Storage:** unbounded by choice.
- **Summon time:** every summon replays the whole history. At the measured
  0.3–1.3 µs per event (`tinyactors-napi/docs/scale.md`), plus the cost of
  reading the log, a million-event history takes about a second of budgeted
  turns. The eviction policy (how long an actor must be idle before it is
  evicted) decides how often that cost is paid.
- **Recording:** one log append per accepted event and one per delivered
  event, with one durable commit per turn.
- **Volume:** at napi's measured rate for a websocket-shaped load (about
  385k messages per second) and 100–200 bytes per record, the log grows by
  roughly 40–80 MB/s, several TB per day at full load. Unbounded history
  suits workflow actors. Chatty per-connection actors need a storage budget.
- **Encoding:** every logged event with data passes through a value codec
  (JSON, or `v8.serialize` / structured clone), about a microsecond per
  event with a payload.

## Storage layouts (cross-check, 2026-09-24)

Four layouts were compared, with the napi host and up to 4M named actors:

| | 1. SQLite file per named actor | 2. SQLite file per chart family | 3. One store per system | 4. One log per system, in S3 |
|---|---|---|---|---|
| Files | one per actor (4M, three each in WAL mode) | tens | one | objects |
| Commit per turn | one fsync per actor touched, up to 1024 per turn; throughput collapses unless `synchronous=NORMAL` | one per family touched | **one**: a real group commit | one PUT per turn: 10–100 ms, about $400 a day at 1000 turns/s |
| Atomic across actors | no | per file only (ATTACH's atomic commit does not work in WAL mode) | **yes**: logs, durable mailbox and outbox in one transaction | no |
| Summon read | one file, sequential | range scan on `(address, seq)` | the same range scan | records interleaved across segments; many GETs unless compacted per actor |
| Open handles | cannot keep 4M open; LRU plus an open per summon | a few | one | none |
| Delete or export one actor | delete or ship the file, which suits network handoff | `DELETE` a range, then vacuum | the same as 2 | rewrite segments |
| Queries across actors | none | within a family | everything | a separate engine |
| Upcasters | per actor | naturally per family | per family, through a column | per family |
| Verdict | only for thousands of actors, or as the archive format for evicted actors | good | **best default**: the only one that is crash-consistent for mail in flight | not as the primary log; good as a tier behind 3 |

Recommendation from the cross-check:
- **Primary:** layout 3, committed off the event loop once per turn, with
  mail logged when it is accepted in the same transaction.
- **S3 as a tier, not the log:** replicate the system's store to S3
  asynchronously (as Litestream does) for disaster recovery, and/or compact
  a cold, evicted actor's history into one object per actor, so a summon is
  a single GET. Only S3 Express One Zone supports appends, so do not depend
  on appending.
- **Measure it:** a `replay` variant in `tinyactors-napi/test/scale.js`
  that logs to layout 3, reporting cost per message with logging, commit
  latency per turn, and summon time against history length.

### Layout 3 on PostgreSQL instead of SQLite

The per-system store could be a PostgreSQL database instead of a local
SQLite file. The napi process stays the runtime; PostgreSQL only stores the
log. This is the "PostgreSQL as storage only" alternative from the
[PostgreSQL host](postgres-host.md) proposal.

What gets better:
- **Commits are asynchronous by nature.** Node's PostgreSQL drivers never
  block the event loop, so the off-the-loop commit comes for free. The
  native side hands the turn's encoded records to JS, the driver sends them
  (one multi-row `INSERT … SELECT unnest(…)` or `COPY` per turn), and on
  acknowledgement JS tells the native side to release that turn's effects.
- **Durability beyond one machine:** replication, point-in-time recovery,
  backups and managed services. The log survives the loss of the machine
  running napi.
- **Several napi processes can share one log.** Named actors are summoned
  wherever their mail arrives, which is the distributed virtual-actor
  model. This needs ownership: a directory table (address → owner process,
  epoch) with fencing. The log's primary key `(address, seq)` gives cheap
  fencing on its own: `seq` is assigned by the owning process, so a second
  writer collides on the key instead of forking the history.
- **Queries across all actors** in SQL, from the same place the application
  keeps its other data.

What gets worse:
- **Summon locality.** PostgreSQL's heap is not clustered. One actor's
  records are inserted between everyone else's, so a summon that reads a
  million records follows the `(address, seq)` index into up to a million
  scattered heap pages. SQLite's `WITHOUT ROWID` table keeps them together.
  Mitigations:
  - a covering index (`INCLUDE` the payload) for index-only scans, which
    makes the index as large as the table;
  - a compaction job that folds an actor's records into chunk rows (for
    example one `bytea` of many encoded events per row), so a summon reads
    a few rows. This is the same idea as the per-actor S3 object;
  - periodic `CLUSTER`, which takes a lock and is unsuitable while running.
- **Write throughput.** One connection with batched inserts reaches roughly
  100–300k rows per second (a rough estimate, to be measured), below napi's
  385k messages per second for the websocket-shaped load. Spreading writes
  over several connections, partitioned by address hash, raises the ceiling
  but splits one turn into several transactions, which gives up
  atomicity across partitions.
- **Effect latency:** each turn's effects wait for a network round trip plus
  the server's WAL flush, about 1–3 ms in the same availability zone.
- **Operating it:** an unbounded insert-only table wants time partitioning
  for maintenance, and anti-wraparound vacuums on a very large table are
  expensive. A summon reads every partition, because it needs all of the
  history.
- **Codec:** `bytea` with `v8.serialize` is fastest. `jsonb` is queryable
  but costs more per event.

Verdict: **PostgreSQL is the better layout-3 store once more than one napi
process or real disaster recovery is needed**, and it opens the way to
distributed named actors. **Local SQLite is the better store for a single
process**: faster summons thanks to clustered storage, lower effect
latency, and no server. The adapter interface should allow both. The
compaction into per-actor chunks is worth designing once, because
PostgreSQL, S3 and cold archives all need it.

## Open questions

- A send that was due before eviction but had not been dispatched yet
  (because of a budget or a router retry) is withdrawn as if it had fired.
  Is that acceptable, or does the log need a record of dispatched sends?
- Where the log lives. The cross-check above recommends one store per
  system: local SQLite for one process, PostgreSQL for several processes
  or disaster recovery, with S3 as a tier. It could share storage with the
  [SQLite](sqlite-host.md) and [PostgreSQL](postgres-host.md) hosts.
- Whether mail in flight is logged when it is accepted (a durable mailbox)
  or documented as at-most-once.
- How per-actor compaction into chunks works, and when it runs.
- For several napi processes sharing PostgreSQL: how ownership, leases and
  fencing work, and how mail for an actor owned elsewhere is forwarded.
- The log records event data, so the log store needs a value codec. That is
  the same codec that network handoff and the database hosts need.
- Whether eviction destroys the actor or unloads it, and what the directory
  entry says while the actor is evicted.
- How replay interacts with `finished: "destroy"`, `send()` promises that
  are pending at eviction, and quarantine.
