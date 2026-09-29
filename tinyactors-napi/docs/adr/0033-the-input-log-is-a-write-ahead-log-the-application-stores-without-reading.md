# ADR 0033: The input log is a write-ahead log the application stores without reading

**Status:** Accepted and implemented (2026-09-29); see the implementation notes. Refines napi ADR 0032 sections 6 to 8: the batch shape, the loader's `{ replay }` answer, `requeue`, and eviction. It replaces the dispatch horizon (core ADR 0089 section 2) with a record per fired timer, for the reasons below.
**Date:** 2026-09-29

## Context

Napi ADR 0032 lets an application persist named actors by replay. Using
it, the application has to *interpret* the log:
- **The loader answer** passes `binding` taken from the `spawned` record,
  and `stoppedAt` taken from the last `evicted` record, or from a stored
  horizon after a crash. It must also notice a `finished` record.
- **The system-wide `horizon`** must be kept, next to the per-actor logs.
- **After a crash**, the application must match `queued` against `event`
  records by `seq`, and call `requeue()` with the difference. `requeue` has
  three gaps: a literal send ID the new system never saw is dropped, a
  queued message that became a dead letter is requeued, and the invoke ID
  is dropped.
- **An operator transition** is logged as a plain `event`, without its
  targets, so replay does not reproduce it.

So every application would reimplement the same interpretation. And one
gap is not the application's to close. **An evicted actor's timers never
wake it:**
- `evict()` destroys an actor that still has future delayed sends, and
  nothing brings it back at the first one's due time. Its timeout fires
  only when mail next arrives, late.
- After a crash, a named actor with timers, or with mail it had not taken,
  is not brought back until new mail arrives for it.

The vocabulary of the log (`spawned`, `event`, `report`, `withdrawn`, and
so on) is what napi needs to rebuild an actor. The application needs
none of it, except as an audit trail.

## Decision

### 1. What the input log is

> `system.setInputLog()` installs a write-ahead log for named actors. Each
> turn, napi hands the application a batch, and nothing the turn caused
> leaves the process until the application has made that batch durable.
> The records are the log's own vocabulary: exactly what napi needs to
> rebuild an actor by replay. The application stores them per actor, in
> order, and hands them back when napi asks for an actor. It never
> interprets them, and chart authors never see them.

The application may read `event` records as an audit trail. Storage and
encoding stay its choice, as in ADR 0032.

### 2. A batch is entries and wakes

```ts
interface InputLog {
  commit(batch: InputBatch): void | Promise<void>;   // one transaction; in order, one at a time
}
interface InputBatch {
  readonly entries: readonly { readonly address: ActorAddress; readonly record: InputRecord }[];
  readonly wakes: readonly { readonly address: ActorAddress; readonly at: number | null }[];
}
```

- **`entries`:** append each record to its address's log, in order.
- **`wakes`:** for each address, set its wake time to `at`, or remove it for
  `null`. A batch holds the last wake of each address in that turn.
- **Both go in one transaction.**

`turn` and `horizon` leave the batch, and `seq` becomes a field inside
records that the application does not read. `InputRecord` stays declared
as the vocabulary, for anyone who reads the audit trail.

### 3. Records napi adds

- **`fired { sendID }`:** a delayed send of a logged actor was dispatched,
  to any target. Replay moves the actor's clock to that record's time,
  which drops the send as core ADR 0089 section 2 says.
  - This makes the log itself say which timers fired. Neither the horizon
    nor `stoppedAt` is needed, and a replayed actor is published at its
    last record's time.
  - It costs one record per fired timer, the alternative that core ADR 0089
    rejected. That ADR chose the horizon for its size: one record per turn,
    not one per timer. Here the horizon has the larger cost: it is state
    across the system, which every application would have to keep and pass
    back.
- **`dropped { seq }`:** a queued message will never be delivered. It
  became a dead letter, or a `send()` with a timeout withdrew it.
- **`jump { source, targets }`:** an operator transition (core ADR 0082),
  in place of the plain `event` ADR 0032's implementation wrote.

### 4. Wakes: when napi needs an actor back

For every logged actor, napi reports a wake whenever the time at which it
needs the actor changes. That time is the earlier of:
- the due time of its first pending delayed send;
- the time its oldest undelivered queued message was accepted.

An actor that has finished or been destroyed gets `null`.

- **In the running process,** `evict()` calls `wakeAt(address, time)` for
  the evicted actor's wake. When it comes due, the family's loader runs
  with `why: "timer"`, as for an unloaded actor (ADR 0020).
- **After a restart,** the application calls `system.wakeAt(address, at)`
  once for each wake it stored. An `at` in the past loads the actor on the
  next turn. The application does not know, or need to know, whether the
  wake is for a timer or for mail.

### 5. A loader answers with the log and nothing else

```ts
{ readonly replay: { readonly spawn: Definition;
                     readonly inputs: Iterable<InputRecord> | AsyncIterable<InputRecord>;
                     readonly onReport?: (report: ReplayReport) => void } }
```

- **No records:** a fresh actor, spawned under the address with no binding.
- **Otherwise:**
  - napi takes the binding from the `spawned` record and starts the clock
    at its time;
  - it replays the records, and publishes at the last record's time;
  - it delivers, in `seq` order, every queued message that has no `event`
    or `dropped` record, with its origin, send ID and data (a literal send
    ID is interned in this system);
  - then it delivers the mail that waited for the load.
- **An actor that finished** reaches its final state in the replay. Its
  publish answers `TA_ERR_DONE`, so napi destroys it and the waiting mail
  becomes dead letters with reason `done`.
- **A queued message from an invoked service** is delivered without its
  invoke ID: its invocation was retired as `unavailable` at publish.

The simplest loader is therefore:

```js
system.setLoader('issue', (address) => ({ replay: { spawn: issueChart, inputs: store.read(address) } }));
```

`inputs` may be an async iterable, such as a database cursor. Replay
reads it through in windows (ADR 0032's notes), so the log is never
held in memory whole.

### 6. Removed

- `system.requeue()`.
- `stoppedAt` and `binding` from the `{ replay }` answer.
- `turn` and `horizon` from `InputBatch`.
- Upgrading a live actor keeps `prepare`, `replay` and `publish`. `prepare`
  takes the binding from the `spawned` record when `binding` is omitted
  for a replaying actor, and `publish` defaults `at` to the last replayed
  record's time.

### 7. What survives a crash

- **Inputs, and mail between persisted actors:** exactly once. A message
  is either delivered and logged, or queued and not delivered, and then it
  is delivered after the replay.
- **Effects that leave the process** (I/O processor sends, invoker starts,
  cancels and forwards): **at most once**. They are released only after
  their batch commits. So a crash after the commit and before an effect
  was performed loses that effect, and replay does not repeat it, because
  its cause is committed. Core ADR 0089's Scope assumed the opposite order
  (released before committed, so at least once), but napi's hold rules
  that order out. An application that needs at-least-once delivery
  outward makes its I/O processors deliver from an outbox of its own. A
  napi outbox would be a later decision.
- **Unnamed actors, and actors of families not listed, are not persisted.**
  A session's load starts an unnamed instance, which is outside the log by
  design.

## Consequences

- **An application's persistence code** is a store with two tables, and
  four calls:
  - an append-only log per address;
  - a wake time per address;
  - the calls: commit a batch in one transaction; read an address's log in
    the loader; evict by its own policy; call `wakeAt` for every stored
    wake at startup.

  It interprets nothing.
- **Timers of evicted actors fire on time,** in the process and after a
  restart.
- **Costs:**
  - one `fired` record per delayed send a logged actor dispatches;
  - one wake entry per logged actor per turn in which its wake time
    changed;
  - replay also scans the records for undelivered queued messages, one
    pass that keeps only the set of `seq`s without an `event`;
  - nothing changes without an input log.
- **Callers change:** the tests, `README.md` and napi ADR 0032's example code
  change with the API; ADR 0032 gets a Status-line pointer.

## Considered and rejected

- **Keeping the horizon, but storing it under a reserved address.** The
  application would still have to hand napi that address's records in
  every load, so the log would stay something to interpret.
- **Writing the horizon into the log of every actor with timers.** That is
  one record per such actor per turn, where `fired` costs one per timer
  that actually fires.
- **Requeuing by scanning all logs at startup.** It is proportional to all
  history. Wakes bring back only the actors that owe something, and each
  finds its own undelivered mail when it replays.

## Implementation notes

- **Where:**
  - `src/inputs.{c,h}`:
    - the new kinds;
    - each queued message's address, and a count and first acceptance time
      of waiting mail per address;
    - the touched addresses and `tna_inputs_wakes()`;
    - `tna_inputs_fired()`;
    - `tna_inputs_redeliver()`;
    - the removal of the horizon.
  - `src/scheduler.{c,h}`:
    - `TNA_NOTE_DROPPED`, noted when an envelope is given back undelivered;
    - `TNA_NOTE_SETTLED`, noted when a macrostep ends stable.
  - `src/router.c` and `src/adapters/io.c`: `fired`, during
    `ta_system_dispatch_due()`.
  - `src/napi_inputs.c`:
    - the batch as `{ inputs, wakes }`, which lib turns into entries;
    - `jump` records with their state IDs;
    - `inputHost`'s wake.
  - `src/napi_system.c`:
    - `redeliver` replaces `requeue`;
    - `replaySend` takes an operator transition;
    - a literal send ID is interned through `ta_system_intern_atom()`.
  - `src/loads.{c,h}` and `src/napi_loads.c`: a `done` load outcome, which
    dead-letters with reason `done`.
  - `lib/system.js`:
    - the batch's entries and wakes, and seqs as text;
    - `jump` names;
    - the replay driver's `fired`, `dropped`, `jump`, `finished` and owed
      messages;
    - `#replayAt` and `#redeliver`;
    - `evict()`'s wake, `prepare({ spawned })`, and `publish()`'s default
      `at`.
  - `index.d.ts`, `README.md`, and the tests `test/input-log.js`,
    `test/replay.js` and `test/types/usage.ts`.
  - Commit `c9f90c5` and the one that adds these notes.
- **A wake is worked out when the turn's batch is taken, not as the actor
  changes.** Every address a record names in a turn is touched, and so is an
  actor whose macrostep ends stable (`TNA_NOTE_SETTLED`). A macrostep can end
  in a later turn than the delivery that started it, and it may arm or cancel
  sends with no record of its own. For a resident actor, the wake is the
  earlier of its first pending send's due time (from inspection) and the
  first acceptance time of its waiting mail. It is null when it has neither,
  or has finished. For an address with no resident actor, the wake is its
  waiting mail's time, and nothing is reported when it has none. A wake is
  reported for every touched address, not only when its value changes: the
  log keeps no per-address state for that. So a batch may repeat a wake the
  store already has. The first acceptance time is kept until the count of
  waiting mail drops to zero, so a wake may be earlier than needed, never
  later.
- **A seq is `<incarnation>.<n>`**, text that lib builds from the native
  number. It is unique across restarts, which the old per-system number was
  not.
- **A message a replay owes goes back under its own queued record.**
  `redeliver` posts it to the published actor with a fresh native seq and no
  `queued` record, and lib maps that seq back to the logged one. So its
  `event` (or `dropped`) record carries the seq of the queued record the log
  already has. The message also counts as waiting mail, since its original
  acceptance, for the wake.
- **Only another incarnation's messages are owed (not in the decision).**
  The loader's `inputs` may read records committed while the load runs,
  such as the `queued` records of the mail waiting for this very load. A
  message this system queued is still in memory and reaches the actor from
  there. So `#redeliver` skips every seq with this incarnation's prefix, and
  every seq it has already put back and not yet seen delivered. Without
  this, the mail that summons an actor would be delivered twice.
- **A history that ends in a final state stops the replay there.** The
  replayed actor is reclaimed when it finishes, like any actor. So the
  driver stops at the settlement whose status is `done`, skips the records
  after it, and reports `finished: true` (`ReplayReport` gained it). The
  loader's `{ replay }` then ends the load `done` without publishing.
- **Where `prepare` gets the spawn's binding:** `prepare(definition,
  { incumbent | address, spawned })` takes the log's `spawned` record. Its
  time starts the clock and its binding is the binding, unless `at` or
  `binding` is given. `publish()`'s `at` defaults to the last replayed
  record's time, owed `queued` records included.
- **`jump`:** the record carries the event it arrived with (name, data,
  origin, seq) and `source` and `targets` by state names. Lib names them from
  the definition while the batch is converted. A replay rebuilds the
  transition against the replaying actor's definition, by name.
- **`fired`** is recorded at every offer of a send during dispatch, a retry
  after RETRY included. A repeated record only moves the clock again.
- **Not recorded, as before:** a `destroy()` of a logged
  actor. Its wake stays in the store, and its log comes back to life if the
  wake fires. Out of scope here: the vocabulary has no `destroyed` record.
- **Benchmark** (`mise run bench-compare a127ba5`, 100,000 actors, two
  alternating runs each, medians, no input log installed):

  | | Node (machine 84c362297e3d) | Bun (machine bbe6bf531954) |
  |---|---|---|
  | untraced ns/event | 650 → 657 (+1.0%) | 545 → 547 (+0.3%) |
  | untraced native B/actor | 329 → 329 | 329 → 329 |
  | untraced RSS B/actor | 755 → 754 (-0.1%) | 1,010 → 1,020 (+1.0%) |
  | untraced heap B/actor | 116 → 116 | 125 → 125 |

  One figure is outside the 5% rule: Node's `decoded` RSS, 2,941 → 3,131
  B/actor (+6.5%), in the variant that decodes OTLP. It does not install an
  input log, and its throughput is unchanged (13,791 → 13,774 ns/event). Bun
  had nothing outside the rule.
- **Costs with a log installed:**
  - one `fired` record per dispatched send of a logged actor;
  - one inspection per touched resident address per batch, for its wake;
  - two map entries per waiting address;
  - one retained address per queued message until it is delivered or
    dropped;
  - a replay keeps the owed `queued` records, and only those, until their
    `event` or `dropped` record.

  Streaming 50,000 records of 2 KB each through `prepared.replay()` from an
  async generator grew the heap by less than 40 MB, measured after
  collection every 5,000 records; held whole, they would be over 100 MB.
- **Tests:**
  - `test/input-log.js`:
    - each kind of record, in order, in `{ entries, wakes }`;
    - seqs that name the incarnation;
    - `fired` under a dispatch budget, and for a send to another actor;
    - wakes following the first pending send, and null once none is;
    - `dropped` for a message a `send()` timeout withdrew while it waited
      for a loader, and for one buried when its actor finished;
    - `jump` by state names;
    - held services, a failed commit, loader waits, no sink.
  - `test/replay.js`, with a two-table store that interprets nothing, and
    whose loader is `(address) => ({ replay: { spawn, inputs } })`:
    - end to end (the same state after eviction and summons, the wake of
      its reminder, and only the summoning mail logged);
    - an evicted actor's timeout firing on time through its wake;
    - the same after a restart with the actor resident;
    - a crash with mail in flight (the owed hello delivered once, the lost
      ping sent once);
    - a withdrawn message not delivered, and a literal send ID kept in a
      system that never saw it;
    - a jump replayed;
    - a finished history ending the load `done`;
    - outward effects at most once across a crash;
    - an upgrade by replay with `spawned` and `publish()`'s default;
    - a report and a runaway's operations;
    - a 50,000-record stream;
    - refusals.
