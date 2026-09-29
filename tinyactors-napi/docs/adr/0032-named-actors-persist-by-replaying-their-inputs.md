# ADR 0032: Named actors persist by replaying their inputs

**Status:** Accepted and implemented (2026-09-29); see the implementation notes. Refined by ADR 0033: a batch is `{ entries, wakes }` without `turn` or `horizon`; `fired`, `dropped` and `jump` records; a loader answers `{ replay: { spawn, inputs } }`; `requeue()` is gone, a replay delivering what the log shows owed; `evict()` arms the actor's wake. Applies core ADR 0088 (a session ID names its family) and core ADR 0089 (an actor is rebuilt by replaying its inputs before it is published). Would amend:
- ADR 0020: a loader is also asked for a session that has gone, and can answer with a replay;
- ADR 0022: mail to a gone session of a family with a loader goes to that loader;
- the replacement API (`prepareReplacement`, `activate`), which becomes `prepare` and `publish`.

It serves the feedback from granary in `docs/proposals/replay-persistence.md`.
**Date:** 2026-09-29

## Context

Core ADRs 0088 and 0089 give a host what it needs to rebuild a named actor
from a log of its inputs:
- session IDs that name their family and system;
- an unpublished actor that crosses no border while it replays, on a clock
  of its own (`ta_actor_set_time()`);
- inputs accepted only between macrosteps;
- `ignored` on resume;
- `ta_dm_host_time()`.

Core keeps no log. The host records what core accepted, commits it before
the turn's effects leave, and replays it when the actor is needed again.

Today napi has none of the host's half:
- **Nothing tells JavaScript which inputs an actor accepted.** Traces come
  close, but a value is cut at `valueLimit`, traces can be sampled, and they
  arrive after the effects have left.
- **Effects leave at the end of a turn** (ADR 0007) whether or not anything
  was recorded.
- **Loaders** (ADR 0020) answer with an image, offloaded bytes, or
  `{ spawn, binding, pendingSends }`. There is no answer that means "replay
  this history".
- **ERROR reports** from invocations (`unavailable`, `quarantined`) go
  straight to `ta_actor_report_invoke()` (`src/napi_services.c`,
  `src/system.c`), which may now answer `TA_ERR_BUSY`.
- **The SCXML invoker fails inside `start`** when a child cannot be
  compiled or spawned (`src/adapters/invoker.c`). Core ADR 0089 narrows
  `start` to OK or out-of-memory.
- **`sessionID` is rendered in decimal** in several places: the evaluation
  context, inspection, and the OTLP attributes (`src/otlp.c`).
- **The router refuses a gone runtime target** whatever its family
  (`tna_router_unreachable`).

## Decision

### 1. Session IDs and the incarnation

- `createSystem({ incarnation })` sets core's incarnation (core ADR 0088):
  a safe integer or a bigint. By default it is 64 random bits from the
  platform's random source. Tests that compare session text pass
  `incarnation: 0`.
- Every place napi shows a session ID uses core's
  `ta_session_id_format()`. That covers `EvaluationContext.sessionID`,
  `ActorInspection.sessionID`, `scxml.session_id` and
  `scxml.cause.session_id` in OTLP, and the debug adapter's session names.
  Numeric session IDs stay internal.

### 2. A gone session goes to its family's loader

`tna_router_unreachable` routes a runtime destination as follows:
- **the session is resident, has not finished, and is of the reference's
  family:** the mail is delivered, as today;
- **otherwise, if the family has a loader:** the mail waits, keyed by the
  session's text, while the loader runs. It is delivered, in order, to the
  instance the loader starts;
- **otherwise, including a reference with no family:** ERROR, as today
  (ADR 0022), so the sender gets `error.communication`.

A loader receives the target as its first argument:

```ts
type LoadTarget = ActorAddress | { readonly family: string; readonly sessionID: string };
type LoadReason = "mail" | "timer" | "session";
```

For a session (`why` = `"session"`), a loader may answer:
- `{ spawn, binding? }`: a new instance, without a name, which takes the
  waiting mail;
- `"not-found"`, `"reject"` or `{ reject }`: a dead letter, as for a name.

Any other answer is a `load-failed` dead letter. One load runs per
session's text.

### 3. Every input reaches core between macrosteps

- **ERROR reports go through the mailbox.** `unavailable()` and a
  quarantined child's report (ADR 0025) become envelopes in the parent's
  mailbox, in order with its mail. The scheduler offers them with
  `ta_actor_report_invoke()` when the parent is idle, and `TA_ERR_BUSY`
  keeps them queued, as it keeps mail. EVENT and DONE reports already go
  through the mailbox.
- **The SCXML invoker reports its failures.** A child chart that cannot be
  compiled at invoke time, or a child that cannot be spawned, is reported as
  `error.communication.invoke.<id>.failed` (core ADR 0081), with data
  `{ code, message }` and `ends`. It is no longer `error.execution` in the
  invoking macrostep. `docs/scxml.md` says so.
- **Out of memory is not a failure.** "Could not queue" from an invoker's
  `start` or an I/O processor's `send` answers `TA_OP_NOMEM`, which core
  retries (core ADR 0070), instead of a failure.

### 4. What a macrostep tells JavaScript

- **`SendResult.consumed`** is present when `status` is `"stable"` or
  `"done"`. It is false when the message enabled no transition (core ADR
  0089 section 8).
- **`EvaluationContext.time`** is `ta_dm_host_time()`: the logical time of
  the running macrostep, in milliseconds. It is the clock expressions
  should read, instead of `Date.now()`.

### 5. Prepared actors: `prepare` and `publish`

These replace `prepareReplacement` and `activate` (core ADR 0089 section 1):

```ts
system.prepare<Data>(definition: Definition<Data>, options: {
  readonly incumbent?: ActorID;       // replace this resident named actor
  readonly address?: ActorAddress;    // or take this address, which has no resident actor
  readonly binding?: Partial<Data> | Data;
  readonly at?: number;               // a replaying actor, whose clock starts here
}): PreparedActor<Data>;

system.publish<Data>(prepared: PreparedActor<Data>, options?: {
  readonly at?: number;               // a replaying actor's last clock move (the moment it stopped)
}): Actor<Data>;
```

- One of `incumbent` and `address` is required.
- `binding` defaults to the incumbent's data root, as today.
- Publishing with an incumbent moves its waiting mail and destroys it, as
  `activate()` does today.
- `PreparedActor` gains:
  - `readonly replaying: boolean`;
  - `setTime(t)`, which is `ta_actor_set_time()`.
- `Actor.prepareReplacement` is removed. The debug adapter's upgrade and
  the tests migrate.

### 6. The input log

```ts
system.setInputLog(sink: InputLog | null, options: { readonly families: readonly string[] }): void;

interface InputLog {
  // Once per turn that accepted inputs for the listed families, in turn order,
  // one at a time. The turn's outward effects wait for the returned promise.
  commit(batch: InputBatch): void | Promise<void>;
}

interface InputBatch {
  readonly turn: number;
  readonly horizon: number;                  // core ADR 0089 section 2
  readonly inputs: readonly InputRecord[];   // in the order core accepted them
}

type InputRecord = { readonly time: number; readonly address: ActorAddress } & (
  | { readonly kind: "spawned"; readonly revision: string; readonly binding?: unknown }
  | { readonly kind: "queued"; readonly seq: number; readonly name: EventName;
      readonly data?: unknown; readonly sendID?: string; readonly origin?: string;
      readonly originType?: string; readonly invokeID?: string }
  | { readonly kind: "event"; readonly seq?: number; readonly name: EventName;
      readonly type: "external" | "platform"; readonly data?: unknown; readonly sendID?: string;
      readonly origin?: string; readonly originType?: string; readonly invokeID?: string }
  | { readonly kind: "report"; readonly invokeID: string; readonly reason: string;
      readonly data?: unknown; readonly ends: boolean }
  | { readonly kind: "withdrawn"; readonly sendID: string }
  | { readonly kind: "abandoned" }
  | { readonly kind: "released" }             // a runaway resumed without its microstep limit
  | { readonly kind: "evicted" }
  | { readonly kind: "finished" }
);
```

- **What is recorded:** for each named actor of a listed family, napi
  records every input core accepted:
  - each delivery (`ta_actor_deliver()` answered `TA_OK`);
  - each invoker report;
  - each host operation that changes the actor;
  - its spawn, eviction and finishing.

  The records are made natively and handed to JavaScript once per turn, as
  traces are (ADR 0015). A value is handed as the value core holds, and the
  sink encodes it: JSON, `v8.serialize`, or `serialize` (ADR 0031).
- **Write-ahead:**
  - the turn's outward effects are held until `commit` returns or its
    promise resolves. These are I/O processor sends, invoker starts,
    cancels and forwards, and log-sink records;
  - batches are committed one at a time, in turn order, and later turns run
    meanwhile, with their effects queued behind;
  - mail between actors of the same system is not held, because it never
    leaves the process. It is recorded instead, as the **durable mailbox**
    below;
  - a promise that rejects stops the system's effects and faults every
    actor with inputs in that batch (ADR 0008).
- **The durable mailbox.** A mailbox lives in memory, so a message accepted
  for a persisted actor but not yet delivered is lost in a crash. Its
  sender's batch may already say it was sent. So napi records `queued`, with
  a sequence number per system, when a message for a named actor of a
  listed family is accepted into its mailbox, or waits for its loader.
  This covers mail from another actor, a `post()`, and a due timer. The
  record goes into the batch of the turn that accepted the message, the
  same batch as the sender's own input. When the message is delivered, its
  `event` record carries the same `seq`. After a crash, the sink re-queues
  every `queued` record that has no matching `event`, with
  `system.requeue(records)`, which puts each message back in its
  address's mailbox with its origin, send ID and data. The message is
  then delivered once: either its delivery was committed and it is not
  re-queued, or it was not, and the receiver's input was lost with it.
- **The horizon** is `min(now, next deadline − 1)` after the turn's
  dispatch of due sends. A sink stores the latest horizon it committed.
  After a crash, that is the moment every actor stopped (core ADR 0089
  section 2).
- **With no sink installed,** nothing is recorded and nothing is held, so
  the cost is zero, as with traces off.

### 7. Replay

Replay works on one actor, the prepared replaying actor it is called on;
there is no replay of a system:

```ts
prepared.replay(inputs: Iterable<InputRecord> | AsyncIterable<InputRecord>): Promise<ReplayReport>;

interface ReplayReport {
  readonly inputs: number;
  readonly ignored: readonly InputRecord[];   // events the current revision did not use
  readonly lastTime?: number;
}
```

napi drives the replay through the pump, within its budgets, so a long
history does not block the event loop. For each record, it calls
`setTime(record.time)`, then performs the input:
- an event is delivered, with its origin rebuilt from `origin` and
  `originType`;
- a report goes to `ta_actor_report_invoke()`;
- an operation calls the matching core function.

It then runs the actor until it is idle. `spawned` and `evicted` records
only move the clock, and `queued` records are skipped: a queued message
reaches the actor as its `event` record. Nothing the replaying actor does
crosses the border (core ADR 0089 section 3), and nothing it does is recorded in the input
log.

**A loader can answer with a replay:**

```ts
{ readonly replay: { readonly spawn: Definition; readonly binding?: unknown;
                     readonly inputs: Iterable<InputRecord> | AsyncIterable<InputRecord>;
                     readonly stoppedAt: number;
                     readonly onReport?: (report: ReplayReport) => void } }
```

napi then:
1. prepares a replaying actor at the address, starting its clock at the
   first record's time;
2. replays the inputs;
3. publishes it with `at: stoppedAt`;
4. delivers the waiting mail.

A replay that fails ends the waiting mail as `load-failed`.

**Upgrading a live actor:** prepare with `incumbent` and `at`, replay the
log into the new revision, then publish with `at: system.time`.

### 8. Eviction

`system.evict(actor)` removes a stable named actor so that its log becomes
its only state:
1. napi dispatches the system's due sends;
2. it records `evicted` at `system.time`;
3. it destroys the actor and removes its directory entry.

It throws `busy` while the actor has waiting mail, is inside a macrostep,
or is quarantined. Mail for the address then goes to the family's loader.

## Consequences

- An application persists named actors with a storage adapter and a loader:
  - it commits batches;
  - it answers loads with `{ replay }`;
  - it evicts idle actors.

  napi does the recording, the write-ahead hold, the replay and the
  publish.
- **Effects wait for commits,** so their latency grows by the store's commit
  time: about 1 to 5 ms for a local SQLite file (proposal). Throughput is
  unchanged, because later turns run while a commit is in flight.
- **Behaviour changes without an input log:**
  - `sessionID` text changes;
  - an SCXML child that fails to start is reported as
    `error.communication.invoke.<id>.failed`;
  - `prepareReplacement`, `activate` and `Actor.prepareReplacement` are
    replaced;
  - error reports queue behind mail.
- **Costs:**
  - with no input log: none per event;
  - with one: one record per accepted input, plus one `queued` record per
    message for a persisted actor, kept until its batch is committed, plus
    the held effects;
  - replay costs about what the original run did, 0.3–1.3 µs per event
    (`docs/scale.md`), plus decoding.

## Considered and rejected

- **A synchronous callback for each input** (granary's `onDeliver`). It
  would cross into JavaScript once per delivery, and it cannot wait for
  an asynchronous commit.
- **Logging in core.** Core ADR 0037 keeps logs, storage and replay with the
  host, and core ADR 0089 confirms that.
- **Holding mail between actors until commit.** It would not help: the
  receiver's mailbox is in memory, so the message is lost in a crash either
  way. The `queued` record makes it durable at the moment it is accepted,
  in the same batch as the input of the sender that sent it.

## Implementation notes

### Sections 7 and 8 (2026-09-29)

- **Where:** `lib/system.js` (`PreparedActor.replay()`, the driver behind
  it, `#settleWindow`, `#replayed`, `#untilIdle`, a loader's `{ replay }`
  in `#applyLoad` and `#replayAt`, `System.evict()`, `loggedOrigin`, and
  `prepare()`'s binding for a replay), `src/napi_system.c` (`replaySend`;
  `tna_logged_origin` and `tna_logged_send_id`, now shared with
  `requeue`; `dispatchDue`), `src/scheduler.{c,h}` (a report envelope
  follows its macrostep with a waiter; `replay_times`, a replayed message's
  time; `tna_sched_replay_time`), `src/inputs.c` (an unpublished actor is
  never logged), `src/system.{c,h}` (`tna_system_dispatch_due`, the turn's
  dispatch, which `evict()` also calls), `index.d.ts`, `README.md`
  ("Persistence by replay"), `test/replay.js`, `test/types/usage.ts`.
  Commits `1c9c78a`, `fa23f94`, `464625c` and the one that adds these
  notes.
- **A replay starts from the binding it is given (not in section 5).**
  `prepare()` with an incumbent and `at` does not default the binding to
  the incumbent's data root, as a replacement does: the replay rebuilds the
  data from its history, so it starts from the `spawned` record's binding.
  Without that, an upgrade by replay counted the history twice.
- **Records are queued with their times.** An `event` or `report` record
  goes into the replaying actor's mailbox with a waiter, as a `send()`
  does, and its time goes into the scheduler's `replay_times` map, keyed by
  the envelope. Just before the actor takes the message, the scheduler
  calls `ta_actor_set_time()`, between macrosteps. So the driver queues up
  to 256 records (`REPLAY_WINDOW`) and waits for them together, instead of
  one pump turn per record. The map is empty, and never read, without a
  replay; a live message costs one count check at delivery. A time core
  refuses (before the actor's clock) dead-letters the message as `refused`
  and faults the actor, as a refused delivery does. The first driver moved
  the clock from JavaScript before each record and took 51 µs per event
  on Node.
- **Host operations settle the window first.** `withdrawn`, `released`,
  `spawned`, `evicted` and `finished` wait for the queued records, then
  the actor to be idle (a poll on `setImmediate`, since a bootstrap and a
  released macrostep have no settlement), then move the clock and act.
  `abandoned` abandons first and moves the clock after, since a runaway's
  macrostep is still open and the clock moves only between macrosteps.
- **Faults.** A replayed macrostep that faults (a runaway) resolves its
  record with the error. The next record must be `abandoned` or `released`,
  as the operator's action left it in the live run; anything else rejects
  the replay with the fault. A fault with records still queued behind it
  rejects too: the log has nothing that resolves it.
- **Matching invocations.** `replaySend` looks up the replaying actor's
  active invocation whose ID reads the record's `invokeID` (inspection and
  `ta_system_invoke_id_text`, natively), so `<finalize>` runs. An event
  whose invocation is not active is delivered without one; a report whose
  invocation is not active rejects the replay with `not-found`.
- **The loader's `{ replay }`** peeks at the first record for the clock's
  start, prepares at the address, replays, publishes at `stoppedAt` (a
  `stoppedAt` before the last record's time fails the load), drops bytes
  `unload()` kept for the address, and completes the load as `loaded`, so
  metrics count it as a load and the waiting mail follows. The loader's
  `timeout` covers the loader's answer, not the replay. A failure destroys
  the prepared actor and ends the waiting mail as `load-failed`. A pending
  send already past due fires at the next dispatch, behind the waiting
  mail.
- **`evict()`** dispatches due sends with `tna_system_dispatch_due`, the
  code a turn runs after its deliveries, then refuses with `busy` if that
  put mail in the actor's mailbox (a timer to itself) or left one of its
  sends due (a dispatch budget or a full queue), besides the section's
  cases; `invalid` for an actor without a name. It records `evicted` with
  the address, so the record survives the destroy.
- **Replay cost** (Apple M4 Max; one actor, a counter taking `inc`; the
  same actor's live run with the input log installed for comparison):

  | Events | Node replay | Node live | Bun replay | Bun live |
  |---|---|---|---|---|
  | 10,000 | 22.2 µs/event | 24.9 µs/event | 5.8 µs/event | 8.0 µs/event |
  | 100,000 | 37.0 µs/event | 63.6 µs/event | 21.6 µs/event | 47.5 µs/event |

  One actor takes one message per pump turn, live or replayed (ADR 0005),
  so a replay costs about what the actor's own live run did, not the many-
  actor figures of `docs/scale.md`. A long history is replayed over many
  turns, within the budgets, and other actors keep running.
- **Tests:** `test/replay.js` (Node and Bun):
  - end to end: live orders and a ledger, with a timeout to itself, a
    ping to the other persisted actor, a JavaScript invocation whose
    `<finalize>` takes its result, and an operator's withdrawal; the log
    committed; the order evicted and summoned by mail through `{ replay }`
    with its configuration, data and pending sends (ids and due times)
    unchanged; no ping, log or service start from the replay, nothing but
    the new mail logged; the waiting mail delivered after; the reminder
    firing at its time after publish;
  - an upgrade by replay into a revision that ignores `bump`, reported as
    ignored;
  - the four timelines of core ADR 0089 section 2: a timeout to itself; a
    ping that fired before the actor stopped, not sent again; one pending
    when it stopped, firing once, late; a crash between commits, in a new
    system of another incarnation, with the queued hello requeued and the
    ledger taking the hello and the lost ping once each;
  - a report and a runaway's abandon and release replayed;
  - refusals of `replay()` and `evict()`.

### Section 6 (2026-09-29)

- **Where:** `src/inputs.{c,h}` (the records, their arena, the per-system
  `seq`, the envelope-to-`seq` map, the scheduler's observer, host
  operations), `src/scheduler.{c,h}` (`tna_sched_observer` and its notes
  at a queued message, a delivery, a taken report and a finished actor;
  the per-actor cache bits `TNA_SCHED_LOG_KNOWN` and `TNA_SCHED_LOGGED`;
  `tna_sched_quarantine`), `src/system.{c,h}` (the log in the system; the
  horizon after the turn's dispatch; a system with records waiting is not
  quiet), `src/napi_inputs.{c,h}` (the batch handed to the effects handler
  as kind `inputs` after the services; `setInputLog`, `inputHost`),
  `src/napi_system.c` (`requeue`, `quarantine`), `src/pump.c`,
  `lib/system.js` (`setInputLog()`, `requeue()`, the held services and the
  commit queue, the records of spawns, withdrawals, abandons and
  releases), `index.d.ts`. Commit `cbce803`.
- **Which actors are logged** is decided once per actor and kept in two
  bits of its scheduling entry. The first message, delivery or operation
  for an actor asks core (`ta_actor_inspect`, read-only, so it may run
  inside the router callback that queues mail between actors). The bits
  are cleared when the families change. Mail waiting for a loader is
  logged by its address's family. A session load's mail (section 2) is
  not: an instance of a session has no name.
- **A batch is handed every turn while the log is on,** so the turn's held
  services are sealed with it. `commit` is called when the batch has
  inputs or the turn dispatched delayed sends, which is when the horizon
  can have passed a send. So `turn` numbers the turns napi handed, and a
  sink may see gaps.
- **The hold is in lib.** The services arrive from the native queue as
  before and wait in `#held`. Each batch takes the services held since the
  last one; batches wait in `#commits` and commit one at a time; a
  synchronous commit releases its services at once. A service with no
  commit queue and no sink runs at once, as before. Removing the sink
  releases what it held once the commits before it are done.
- **A failed commit** (it throws, or its promise rejects) drops the
  services held and queued, stops later services for good, and
  quarantines each actor with a record in the batch through the scheduler
  (`tna_sched_quarantine`, counted under fault code 0). The system's
  `fault` hook gets `{ actor, operation: "input-log", code: "input-log",
  message }`. `release()` puts an actor back; the system's services stay
  stopped.
- **A spawn's binding is a structured clone** taken at the spawn, because
  the actor's data root changes from its bootstrap on and the batch is
  handed after the turn. A binding that cannot be cloned (functions, class
  instances) is handed as the live value. Every other value is the one
  core holds.
- **`queued` skips report envelopes:** a report is recorded as `report`
  when core takes it. An operator transition (a debugger's jump, ADR 0026)
  is recorded as the `event` it arrives with, without the transition.
- **`requeue(records)`** posts each record to its address through the
  router (so a loader summons an evicted actor), with its origin parsed
  back by core (`{ actor: <URI> }` for the SCXML processor, `{ type, uri }`
  as a message origin for any other), its send ID when this system can
  parse it (a literal ID it never interned is dropped), and `platform`
  when the record was a platform event. An invocation's ID is not kept:
  the invocation did not survive the crash. Each requeued message is
  recorded as `queued` again, with a new `seq`.
- **A queued message that becomes a dead letter** has no `event` record,
  so after a crash a sink would requeue it. Its dead letter happened before
  the crash; requeuing it gives it another chance. Not guarded against.
- **Benchmark** (`mise run bench-compare 7c9a707`, 100,000 actors, two
  alternating runs each, medians, no input log installed): nothing outside
  the 5% rule.

  | | Node (machine 84c362297e3d) | Bun (machine bbe6bf531954) |
  |---|---|---|
  | untraced ns/event | 669 → 682 (+1.9%) | 545 → 561 (+2.9%) |
  | untraced native B/actor | 329 → 329 | 329 → 329 |
  | untraced RSS B/actor | 757 → 758 (+0.1%) | 1,010 → 1,044 (+3.3%) |
  | untraced heap B/actor | 116 → 116 | 125 → 125 |
  | summary ns/event | 1,205 → 1,237 (+2.7%) | within 5% |

  The scheduling entry is still 16 bytes (the cache uses two free flag
  bits), and an envelope did not grow: the `seq` of a queued message lives
  in the log's map, only while a log is installed.
- **Costs with a log installed:** per logged message, a `queued` and an
  `event` record in the log's arena (about 120 bytes each, plus the
  origin's text) and one map entry while it is queued; per logged
  delivery, the origin rendered once (`ta_origin_text`); per actor, one
  `ta_actor_inspect` the first time it is met; the held services until
  their batch commits.
- **Tests:** `test/input-log.js` (each kind of record in order, for the
  logged families only, with seqs paired and origins kept; a report, an
  abandon and a release; services held until the commit's promise resolves;
  a failed commit quarantines with an "input-log" fault and runs nothing it
  held; the horizon held before the sends a dispatch budget left due; mail
  waiting for a loader queued under its address, delivered with the same
  seq; a crash: the first system closed with mail queued, the second
  requeues what has no event and each message arrives once; requeue keeps
  an actor's origin, so the reply reaches its sender; without a sink
  nothing is held, and removing the sink releases what it held).

### Section 7's method moved at review (2026-09-29)

- Replay works on one actor, so it is `PreparedActor.replay(inputs)`, not
  `System.replay(prepared, inputs)` as first written: the user decided at
  review that there is no replay of a system. Section 7's text was edited
  in place while it was unimplemented, and `index.d.ts` declares the
  method on `PreparedActor`, marked unimplemented until section 7 lands.

### Sections 3 and 4 (2026-09-29, with core ADR 0089's sections 4, 5, 8 and 9)

- **Where:** `src/scheduler.{c,h}` (`tna_report_kind`, a report envelope's
  `report`, `tna_sched_report_one`, reports dropped instead of dead-lettered,
  `tna_settlement.ignored`), `src/napi_services.c` (`unavailable()` queues;
  `tna_fault_data`), `src/system.{c,h}` (quarantine reports queue;
  `fault_data`; `tna_quarantine_report.reported` removed),
  `src/adapters/invoker.c` (`tna_scxml_start` reports; service 0 is
  cancelled and forwarded to as nothing), `src/adapters/io.c` and the
  JavaScript invoker (out of memory is `TA_OP_NOMEM`), `src/pump.c`
  (`consumed`), `src/napi_datamodel.c` (`time`), `index.d.ts`,
  `docs/scxml.md`. Commit `1c0991b`.
- **`unavailable()` returns true once the report is queued,** as `done()`
  already did, and false once the invocation has ended. A queued report
  whose invocation ends before the parent takes it is dropped at delivery
  (core answers `TA_ERR_NOT_FOUND`), and a report for a gone parent is
  dropped, never a dead letter.
- **An SCXML child that cannot start** answers start with `TA_OP_OK` and
  service 0, and queues `failed` with `{ code, message }` from the child's
  fault. A report that cannot be queued answers `TA_OP_NOMEM`, so core starts
  the invocation again.
- **"Could not queue" is `TA_OP_NOMEM`,** except while the system closes, when
  the I/O processor still answers `TA_OP_ERROR` ("the system is closing"),
  so a closing system does not retry sends forever.
- **Tests:** `test/session-inputs.js` (`consumed`, `time`, a queued
  `unavailable()`), `test/invoke.js` (`refusals` now expects `failed`
  reports and both invocations ended).

### Section 5 (2026-09-29, with core ADR 0089's sections 1, 2, 3, 6 and 7)

- **Where:** `lib/system.js` (`System.prepare()`, `System.publish()`,
  `PreparedActor.replaying` and `setTime()`), `lib/actor.js`
  (`Actor.prepareReplacement` removed), `bindings.rb` and `src/hooks.{c,h}`
  (`ta_actor_prepare` puts the actor on the runnable queue; the `published`
  hook moves an incumbent's waiting mail and wakes an actor left a
  macrostep, such as a replay whose invocations were retired),
  `src/gen/ta_napi.{c,h}` (regenerated), `lib/debug.js` (the upgrade
  prepares with `{ incumbent }` and publishes), `index.d.ts`
  (`PrepareOptions` replaces `ReplacementOptions`), `docs/design.md`,
  `mise/lib/api_status_probe.js`. Commit `fb908fd`.
- **`publish()` without an incumbent destroys nothing,** and mail that waited
  for the address under a loader is not moved yet: that is section 7's
  loader answer, which prepares, replays and publishes inside the load and
  then delivers the waiting mail.
- **`PreparedActor.activate()` is gone** with `System.activate()`; publishing
  is one call, `System.publish(prepared, { at })`.
- **Tests:** `test/prepare.js` (an evicted actor replayed by hand a week
  later: the timeout held, dropped as the clock passes it, taken from the
  log; no log written; the clock refusing to go back; publish at the stop;
  a send pending at the stop delivered after publish; prepare's and
  publish's refusals); `test/persist.js` migrated.

### Sections 1 and 2 (2026-09-29)

- **Where:** `lib/system.js` (`incarnationOf`, `System.incarnation`,
  `sessionNumber`, inspection's and capture's `sessionID`, `watchTraces`,
  `#runSessionLoad`), `src/napi_system.c` and `src/system.{c,h}`
  (`tna_system_create(incarnation, ...)`), `src/otlp.c`
  (`tna_session_text`, `tna_key_session`), `lib/tracing.js` (`stepSpanID`
  takes the text), `lib/debug.js` and `lib/debug/variables.js` (attach by
  the text, threads keyed by it), `src/router.c`
  (`tna_router_session_loads`, `tna_router_post_session`),
  `src/loads.{c,h}` (`TNA_LOAD_WHY_SESSION`, `tna_loads_route_session`,
  `tna_loads_family_loads`, a load's `keyed`), `src/napi_loads.c` (`why`
  "session"), `index.d.ts` (`SystemOptions.incarnation`,
  `System.incarnation`, `LoadTarget`, `LoadReason`), the VS Code launch
  schema (`sessionID` is the text). Commits `8c8e3fc` and `6c2bf1a`.
- **A cause's family is carried in core's step key (the decision section 1
  asked for).** A span's `scxml.cause.session_id` names the sending step,
  whose session may be another family's and already gone, so looking its
  family up at encoding time can fail. `ta_step_key` gained `ta_atom family`,
  set wherever core makes a key for a session. It fills the key's padding,
  so a key stays 24 bytes (static-asserted in `src/actor.c`), and it is not
  part of a span ID (`ta_step_key_span_id` mixes the other fields). The
  encoder renders both attributes from their keys. A host's cause and a
  definition record have family 0 and are written as the decimal number, as
  before. Core ADR 0088's notes record the change to the key.
- **Numbers stay where they are identities:** `ActorInspection.step.session`
  and the `cause` of a traceparent object are the numbers `stepSpanID()`
  hashes, and an I/O processor's `{session}` placeholder (core ADR 0068)
  stays the session's number, because core's `ta_io_text_params` carries no
  family.
- **The default incarnation** is 64 bits from `crypto.getRandomValues()`,
  which Node and Bun both have.
- **A gone session's load is keyed by an interned address,** `{ family,
  <session text> }`, which lets section 2 reuse the loads of ADR 0020
  (waiting mail, `maxWaiting`, timeouts, metrics). It is never a directory
  entry. A dead letter of mail that waited for it names that address as its
  target. An application name equal to a session's text in the same family
  would share its load; such a name contains the incarnation, the session
  number and the family, so this is not guarded against.
- **A session this system cannot name** (another incarnation's, or one core
  routes to session 0 because another family holds its ID) is shown to the
  loader as `<incarnation>.0.<family>` and gets a load per message, kept out
  of the address map, since two such messages may be for different
  sessions.
- **flush() waits for loads in flight,** as it does for named loads, so a
  test that holds a loader's promise lets turns run on a timer instead.
- **Costs:**
  - a message to a runtime destination: one check of the reference's family
    atom; with a loader registered, one more scheduler lookup;
  - a message for a gone session of a family with a loader (the miss
    path): the family's text (`ta_system_atom_text`), the session's text
    into a 256-byte stack buffer (heap only for a longer family), an
    address lookup (`ta_address_intern`, which allocates the first time a
    session's key is seen), and the load's map lookup;
  - a loaded session: one address record (its header plus the family and
    the text) and one load record (`tna_load`) while the load is in
    flight, both released when it finishes;
  - OTLP: each span's session attribute grows from about 10 to about 30
    bytes, plus the family's length;
  - with no loader for a family: nothing changes.
- **Tests:** `test/sessions.js` (the incarnation option and its refusals,
  the text in inspection, capture and `_sessionid`, a family holding `/`
  and `.`, OTLP's session and cause attributes, `stepSpanID()` from the
  text); `test/session-loads.js` (a reply to a finished requester starts a
  new instance that gets it; without a loader the sender gets
  error.communication; "not-found" is a dead letter; another incarnation's
  session goes to the loader as session 0; three replies for one session
  wait on one load and reach one instance in order);
  `test/native/loads_test.c` (`a_gone_session_loads_a_new_instance`); the
  OTLP golden bytes and the tests that compared decimal session IDs.
