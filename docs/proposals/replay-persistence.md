# Replay-based persistence for named actors

**Status:** Sketch (2026-09-23)
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
- **Recording:** one log append per delivered event, with one durable commit
  per turn.

## Open questions

- A send that was due before eviction but had not been dispatched yet
  (because of a budget or a router retry) is withdrawn as if it had fired.
  Is that acceptable, or does the log need a record of dispatched sends?
- Where the log lives: an append-only file per actor, SQLite, or
  PostgreSQL. It could share storage with the [SQLite](sqlite-host.md) and
  [PostgreSQL](postgres-host.md) hosts.
- The log records event data, so the log store needs a value codec. That is
  the same codec that network handoff and the database hosts need.
- Whether eviction destroys the actor or unloads it, and what the directory
  entry says while the actor is evicted.
- How replay interacts with `finished: "destroy"`, `send()` promises that
  are pending at eviction, and quarantine.
