# ADR 0032: Named actors persist by replaying their inputs

**Status:** Accepted; sections 3 and 4 implemented (2026-09-29), see the implementation notes; the rest not yet. Applies core ADR 0088 (a session ID names its family) and core ADR 0089 (an actor is rebuilt by replaying its inputs before it is published). Would amend:
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

```ts
system.replay<Data>(prepared: PreparedActor<Data>,
                    inputs: Iterable<InputRecord> | AsyncIterable<InputRecord>): Promise<ReplayReport>;

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
reaches the actor as its `event` record. Nothing the replaying actor does crosses the border
(core ADR 0089 section 3), and nothing it does is recorded in the input
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
