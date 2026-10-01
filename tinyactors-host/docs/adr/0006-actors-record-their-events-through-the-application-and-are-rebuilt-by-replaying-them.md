# ADR 0006: Actors record their events through the application, and are rebuilt by replaying them

**Status:** Accepted and implemented (2026-10-01); see the implementation notes (PLAN.md phase K). Supersedes kernel ADRs 0002 (the kernel keeps the write-ahead log) and 0005 (an actor's log is an inbox with acknowledgements) as a whole, and with them what was left of kernel ADR 0004. Supersedes napi ADRs 0032, 0033 and 0034, and napi ADR 0040 in part (section 3 here). Changes how core ADRs 0089, 0090 and 0091 are used (section 7). Keeps kernel ADR 0003 (a full mailbox overflows instead of refusing mail between actors). Points marked "(open)" are not settled.
**Date:** 2026-10-01

## Context

### How the log grew

Persistence started from granary's feedback
([`docs/proposals/replay-persistence.md`](../../../docs/proposals/replay-persistence.md)):
a journaled actor, such as the allow list of the people who may manage
pull requests, that survives restarts and crashes, and whose history
explains exactly why its state is what it is, without traces, which are
sampled and thrown away.

The runtime answered with an input log it owned: core's records in a
binary layout (core ADRs 0090 and 0091), moved to the kernel (kernel ADR
0002), handed to the application in batches it commits in turn order, and
replayed by core (core ADR 0089). Each crash window found since added a
mechanism:
- a send refused for a full mailbox was durable in neither log: core ADR
  0095, then kernel ADR 0003;
- a delivery's `delivered` committed before its macrostep ran: kernel
  ADR 0004's held first record;
- a macrostep paused by a budget repeated its sends after a crash: a hold
  on its sends, built and then dropped because it batched a long
  `<foreach>`'s messages instead of streaming them;
- napi's write-ahead hold delayed every effect by a commit, made effects
  at most once, and did not even cover the acknowledgement callers use:
  `send()` resolves before its batch commits (kernel ADR 0005, "Finding").

Kernel ADR 0005 simplified the records to an inbox with acknowledgements,
but kept the runtime as the owner of the log's format, batching, commit
order and durability.

### The end-to-end argument

Reliability that the endpoints must implement anyway is only an
optimization in a lower layer (Saltzer, Reed and Clark, 1984). An
application already times out and retries, because networks lose requests
and replies; its receivers already recognize repeats; its outward effects
already carry idempotency keys. Those measures also absorb a message
repeated after a crash. The owner accepted at-least-once delivery on
2026-10-01.

Under that contract the runtime need not own durability. It must own only
what the application cannot do itself:
- **see every event an actor takes.** The application sees what it posts,
  but not mail from other actors, timers firing, replies from I/O
  processors, or events from invoked services;
- **rebuild an actor without its effects firing.** An application that
  feeds its journal to a freshly spawned actor runs its I/O processors
  again. That is what granary could not get past: napi's
  `PreparedActor.replay()` took only bytes in the runtime's log layout, so
  an application's own journal had no way in.

### An actor does not care where its events come from

To a chart, an event is `_event`: a name, a type, a send ID, an origin and
its type, an invocation ID, and data. Whether it is happening now or is
read back from a journal makes no difference to the chart. Rebuilding an
actor is therefore: create it, pipe its events through it, then add it to
the system, and until then let nothing it does leave it.

## Decision

### 1. The model

> The runtime **records** the events an actor takes and hands them to the
> application. The application **journals** them, in storage and on a
> schedule it chooses. To rebuild an actor, the application **replays**
> its journal into a prepared actor, which crosses nothing until it is
> published.

The runtime keeps no log, batches no commits, holds no effects and waits
for no storage.

### 2. Recording

The application subscribes per family. In napi:

```js
system.setRecorder(recorder, { families });   // null removes it
// recorder(batch), once per turn:
// batch: { records: [{ address, event, at }], wakes: [{ address, at }] }
```

- **What is recorded:** every event a named actor of a subscribed family
  takes from its external queue: mail from the host (`post()`,
  `send()`), mail from other actors, delayed sends firing (to itself or
  from others), I/O processor replies, and events from invoked services.
  Each is recorded as napi's existing `ActorEvent` (`name`, `type`,
  `sendID`, `origin`, `originType`, `invocationID`, `data`): exactly what
  the chart saw as `_event`. Events core places on the internal queue
  itself (`raise`, `error.execution`, `done.state.*`) are not recorded:
  the replay produces them again. (open) Events core places on the
  internal queue because of the outside world, such as
  `error.communication` for a send the router could not deliver, are not
  produced again by a replay, whose sends go nowhere: check each, and
  record those.
- **`at`:** the time the event was delivered: the actor's clock for the
  macrostep it started.
- **When:** in the turn the macrostep the event started **ends**, after
  its effects ran. A crash before the application stores the record then
  repeats work (the event's source sends it again, or the application's
  client retries), and never leaves a journal claiming an event whose
  effects did not happen.
- **Batching:** one call per turn, with the turn's records in the order
  the actors took the events. Crossing into JavaScript once per event
  would cost too much; once per turn costs what the input log's
  `commit(batch)` cost.
- **Wakes:** each address that becomes unloaded with a pending delayed
  send, or with mail waiting, gets the time it must be woken, in the same
  batch, so the application can store them with its journal and call
  `wakeAt()` after a restart (napi ADR 0020). `null` removes one.
- **The recorder never makes the runtime wait.** It is called and
  returns; what it does with the batch is the application's (section 4).
  A recorder that throws is reported, as a failed trace sink is, and the
  system goes on.
- **Data:** a record shares the value the chart received; it is not
  copied. Event data is the receiver's own (core ADR 0097), so the chart
  may mutate it in a later macrostep. The recorder must serialize what it
  keeps during the call, before the next turn runs.
- **Order with `send()`:** the turn's batch is handed to the recorder
  before the turn's `send()` promises settle. A caller that awaited
  `send()` and then awaits its own journal's flush knows the message's
  record is in it.

### 3. Rebuilding

```js
const actor = system.prepare(definition, { address, data });  // as spawn() takes them
for (const { event, at } of journal) actor.replay(event, at);
system.publish(actor);
```

- **`prepare()`** makes the actor and runs its bootstrap, unpublished.
- **`replay(event, at)`** moves the actor's clock to `at` and delivers
  `event`, as the chart saw it, and runs the macrostep it starts to its
  end. It returns the `SendResult` a delivered message gets (`consumed`
  says whether a transition took it). `at` never goes back: an event
  earlier than the last one is refused.
- **Nothing leaves an unpublished actor:** its sends to other actors are
  dropped, I/O processors and invokers are never called, and its log and
  trace records are not emitted. This is core ADR 0089's replaying actor,
  unchanged: "sent is what happened".
- **Timers:** core drops a pending delayed send once the clock passes its
  due time (core ADR 0089 section 2): time passed, so it fired, and if
  its event mattered it is in the journal. Delayed sends due later stay
  pending, with the due times they had, and fire after publish.
- **Invocations:** an invocation the replay saw start is not started.
  At publish, one still open is reported to the chart as lost (core ADR
  0089 section 3), which the chart handles as a failed service.
- **`publish()`** adds the actor to the system at the last event's time,
  and from then on it is an ordinary actor.

A journal needs no particular origin: the recorder's batches, domain
facts a chart sends to a journal I/O processor itself, a test fixture, or
an export from another system all replay the same way, as long as each
entry is an `ActorEvent` and the time it happened.

### 4. What the application owns

- **Storage and flushing:** a transaction per batch, group commit across
  batches, a file synced on its own schedule. The trade between latency
  and throughput is the application's.
- **Durability acknowledgements:** `send()` resolves when its message's
  macrostep ended (napi ADR 0040, as amended in section 6). A caller that
  needs the message durable, such as an HTTP handler about to answer
  "accepted", awaits its journal's flush after `send()`.
- **What it journals:** every recorded event, the domain facts its charts
  choose to journal, or both. A chart that orchestrates its own
  persistence (validate a command, send the fact to a journal I/O
  processor, wait for "persisted", then reply) needs no recorder at all.
- **Snapshots:** core's state export and import (core ADR 0091's
  checkpoints, without the log), on the application's schedule, so a
  journal can be compacted.
- **Audit:** in the application's terms, kept as long as it likes.

### 5. The contract

What the runtime guarantees:
1. **Replay is deterministic.** The same events at the same times, under
   the same definition, rebuild the same state (core ADR 0089, within
   its Scope: expressions read only the data, the event and the host's
   time).
2. **Nothing leaves an actor before it is published.**
3. **Timers are rebuilt from event times,** and those still due fire
   after publish. A delayed send to the actor itself never fires twice
   because of the replay: its firing is an event in the actor's own
   journal. A delayed send to another target (another actor, an I/O
   processor) is at least once: it may repeat once, when the sender is
   rebuilt after it fired and before the sender took another event,
   because its firing is not an event of the sender and sends are fire
   and forget. (Corrected 2026-10-01; see the implementation notes.)
4. **An event is recorded only after its macrostep ended,** with its
   effects handed to their I/O processors and invokers.
5. **Repeats are recognizable:** a repeated event carries the same
   `origin`, and the same `sendid` when its `<send>` has an `id`.

What it does not guarantee: that a recorded event reaches the journal, or
that an event sent before a crash is received after it. Delivery is at
least once where the sender or the client retries, and at most once where
nothing does.

What applications do, end to end, for what matters (napi's README,
"Crashes and repeated messages"):
- requests that matter carry a timeout and are retried;
- receivers are idempotent on business keys, which also absorbs clients'
  retries;
- outward effects carry idempotency keys built from business keys;
- charts guard with states: an event with no transition in the current
  state is ignored.

### 6. What is removed, and what stays

Removed:
- **the kernel's input log:** its records (`accepted`, `delivered`,
  `dropped`, `fired`, `spawned`, the operator and lifecycle records, and
  the checkpoint records as log records), their binary layout and
  versions, batches of bytes per address, commit ordering, the replay
  driver over bytes, and the first-record hold of kernel ADR 0004;
- **in napi:** `setInputLog()`, `readInputLog()`,
  `PreparedActor.replay()` over bytes and its `ReplayReport`, the
  write-ahead hold and the quarantine of actors whose batch failed to
  commit;
- **the "committed" half of napi ADR 0040:** `send()` resolves when its
  message's macrostep ended, without `until` (the rest of napi ADR 0040,
  effects that are not held and no `until`, stands);
- **in the test host:** hosted mode's log, batches and checkpoint methods
  become recording and replay of events.

Stays:
- core's deterministic execution, its replaying actor (prepare, deliver,
  publish) and its state export and import;
- loaders and wakes (napi ADR 0020);
- kernel ADR 0003: mail between actors overflows a full mailbox instead
  of being refused, which prevents loss while the process runs;
- event data leaving a session is a copy (core ADR 0097).

### 7. Core's ADRs under this decision

- **Core ADR 0089:** a replaying actor now takes events the application
  hands it, not records read from a log. Its sections on the log's
  records and the dispatch horizon no longer apply; "sent is what
  happened", timers dropped as the clock passes them, and invocations
  retired at publish are what section 3 here relies on.
- **Core ADR 0090:** the input log's byte layout is no longer written or
  read.
- **Core ADR 0091:** a checkpoint is no longer a record of a log; the
  state it exports and imports is the application's snapshot.

### 8. Operator actions (open)

An operator's actions change an actor's state without an event: a jump to
a state (core ADR 0082), releasing a runaway, abandoning a macrostep,
withdrawing a pending delayed send (core ADR 0084). A journal of events
does not reproduce them. Proposed: after an operator acts on an actor of
a recorded family, the recorder's batch carries a snapshot of that actor
(its exported state), and the application journals it; a rebuild starts
from the latest snapshot and replays the events after it. To be settled
with the implementation.

## Options considered

- **Kernel ADR 0005: the runtime's log as an inbox with
  acknowledgements.** Rejected: it keeps the runtime as the owner of the
  log's format, batching, commit order and durability, which the
  end-to-end argument says the application owns, and it still could not
  replay an application's own journal.
- **Store an actor's state after every macrostep instead of replaying
  events.** Rejected as the runtime's mechanism: write amplification for
  actors with large data, and no history to explain the state. It stays
  available to applications as snapshots (section 4).
- **Persistence only in charts** (a chart journals its facts through an
  I/O processor, with no recorder). Kept as one way to journal, rejected
  as the only one: an application cannot see the events its charts take
  from timers, other actors and services.
- **Record at delivery, before the macrostep runs.** Rejected: a crash
  after the record is stored and before the effects run leaves a journal
  that claims the event, and a rebuild that applies it without its
  effects: silent loss.
- **Record per event instead of per turn.** Rejected: one crossing into
  JavaScript per event costs too much.
- **A new type for replayed events.** Rejected: `ActorEvent` is what the
  chart sees; the time is an argument beside it, as `publish()` takes
  `at`.

## Consequences

- **Much less runtime:** the kernel loses the input log, its layout, its
  replay driver over bytes and every hold; napi loses the input log API
  and the commit pipeline; kernel ADRs 0002, 0004 and 0005 and napi ADRs
  0032 to 0034 become history.
- **Cost per recorded event:** the runtime builds an `ActorEvent` and
  serializes nothing; the application serializes as it likes. It should
  cost less than the logged path's +3.2 µs per message on Node, most of
  which was the runtime's durable serialization of each value (napi ADR
  0034's addendum); to be measured with `bench-compare` against it.
- **Applications write more:** a recorder, a journal and a loader that
  replays it. napi's README gets the worked example: granary's allow
  list, its journal fed by the recorder, a rebuild with nothing firing,
  and a test that checks no effect ran during replay.
- **The journal is the application's audit trail,** in its own storage
  and terms; nothing needs the runtime's decoder.
- **What a crash costs is explicit:** events recorded but not yet stored
  are lost from the journal; the end-to-end measures in section 5 recover
  them.
- **`send()` means processed, not durable.** A caller that needs
  durability awaits its own flush (section 4).
- **Migration:** nothing is published, so nothing stored is read across
  the change.
- **Open points:** section 2's events placed on the internal queue because
  of the outside world, and section 8's operator actions.

## Superseded decisions

- **Kernel ADR 0002**, whole: the kernel no longer keeps a log.
- **Kernel ADR 0004**, whole (already superseded by kernel ADR 0005).
- **Kernel ADR 0005**, whole: its records, replay rule, operator records
  and contract are replaced by sections 2 to 5 and 8 here.
- **napi ADR 0032**, whole: named actors persist by the application's
  journal of events, not by the runtime replaying its inputs.
- **napi ADR 0033**, whole: there is no write-ahead log to store.
- **napi ADR 0034**, whole: there are no records as bytes.
- **napi ADR 0040**, in part: `send()` resolves when its message's
  macrostep ended, not once its batch committed; effects not held and no
  `until` stand.

## Implementation notes

### 2026-10-01: the recorder and event replay (PLAN.md phase K, commits 44 to 47)

- **Kernel:** `tah_replay_event()` and `tah_replay_events_done()`
  (`replay.h`), and the recorder (`recorder.h`, `src/recorder.c`): the
  event of each delivery and of each invocation error report is kept from
  the moment core takes it until its macrostep ends (`TAH_NOTE_ENDED`),
  then joins the turn's records. `tah_recorder_texts()` renders a record's
  `_event` fields, so every binding builds the same event.
- **napi:** `setRecorder()`, `PreparedActor.replay(event, at)`, and a
  loader's `{ replay: { spawn, at, binding, events } }`. The batch is
  handed over before the turn's `send()` promises settle.
- **`prepare()` needs `at`,** when the actor's life began: section 3's
  example left it out. The bootstrap runs at that time, so the due times
  of the timers it arms depend on it; the application journals it with
  the actor (a loader's `{ replay }` takes it as `at`).
- **Invocation error reports are recorded** as the event the chart takes,
  `error.communication.invoke.<id>.<reason>` of type platform, and
  `tah_replay_event()` reports them again, so core retires the invocation
  as it did live. An invocation's own events (`invocationID` set) replay
  as deliveries; one still open at publish is retired as lost.

### 2026-10-01: delayed sends to other targets (the owner's decision)

A delayed send to another actor or to an I/O processor fires without an
event of its sender. Kernel ADR 0002's `fired` records were not brought
back, and no stop time is journaled. Its repeat is accepted, and bounded:
- **At most once per timer.** A rebuild after it fired and before the
  sender takes another event publishes at the last event's time, before
  the due time, so the held send fires again. Once the sender takes
  another event, a rebuild's clock passes the due time and drops it.
- **The wake moves when it fires.** Without it the repeat would be
  unbounded: the stored wake would still name the fired send, `wakeAt()`
  would reload the actor at that time after every restart, and the send
  would repeat each time. The router and the I/O adapter call
  `tah_recorder_fired()` for a due send they took or that failed for good,
  so that turn's batch carries the sender's new wake (its next due time,
  or null). Test: `test/recorder.js`, `delayedSendToAnother`.
- **A delayed effect that must happen once** is a timeout to the actor
  itself, with the send made in its handler: the timeout is in the
  actor's journal, so a rebuild reproduces it exactly (napi README,
  "Crashes and repeated messages").

### 2026-10-01: section 2's open point, events core queues because of the outside world

- **Invocation error reports:** recorded (above).
- **A send the router refuses for good inside a macrostep**
  (`error.communication` raised in the sender's own macrostep): not
  recorded. A replay drops the send ("sent is what happened") and so
  raises nothing: a chart that reacts to it is not rebuilt exactly.
- **A delayed send that fails when it fires** (`error.communication`
  queued outside a run): not recorded, and a replay drops the held send
  as its clock passes the due time, so it raises nothing either.

Both are accepted for now under fire and forget: a sender does not learn
reliably whether its mail arrived, and an application that must act on a
failed delivery acts on an acknowledgement or a timeout instead. Recording
them would need a record that is not an event the actor took from the
kernel, which this ADR set out to avoid. They stay open for the owner.

### 2026-10-01: section 8, operator actions (left open)

An operator's jump (core ADR 0082) is not recorded: its event was not
taken by the chart's own transitions, and replaying the bare event would
take another path. Releasing a runaway, abandoning a macrostep and
withdrawing a pending send are not events either. So a rebuild from the
journal does not reproduce an operator's action; an application that
operates on a recorded actor exports its state afterwards and rebuilds
from that. The proposed snapshot in the batch is not built: it needs a
rebuild that starts from a state and then takes events, which napi's
`prepare()` has only for the input log's checkpoints, removed with it.
Test: `test/recorder.js`, `operatorJumpNotRecorded`.

### 2026-10-01: the input log removed (PLAN.md phase K, commits 48 to 54)

- **Test host:** hosted mode records and replays events (`hosted.record`,
  `hosted.records`, `hosted.restore` and `hosted.load` over a journal); its
  log, checkpoint and value-writer methods are gone. A scenario pins what
  a crash costs: mail a receiver had not taken is in no journal, and a
  rebuild of its sender does not send it again.
- **napi:** `setInputLog()`, `readInputLog()`, the byte replay,
  `checkpoint()`, the write-ahead hold and the durable value codec are
  gone (about 4,400 lines); `send()` resolves when its macrostep ended,
  without `until`.
- **Kernel:** `inputs.c`, `wal.c`, the byte replay and the checkpoint
  writer are gone (about 6,400 lines), and with them the scheduler's notes
  only the log read and the kernel's only `--full` check.
- **An empty journal** publishes after the bootstrap runs:
  `tah_replay_events_done()` runs a bootstrap no event ran.
- **Left in core:** the `durable` flag of a data model's value write and
  read (core ADR 0090) has no caller now; napi's data model answers that
  it has no durable form. Removing the flag changes core's data-model
  contract and the test host's jq model, a core change of its own.

**`bench-compare 5164e0f`** (the commit before the first code change), 2
alternating runs each; the "logged" variants are the input log on the old
build and the recorder on the new, whose recorder JSON.stringifies each
record as an application would, where the old sink only counted bytes:

| `test/bench/input_log.js`, 1,000 actors | Node (84c362297e3d) | Bun (bbe6bf531954) |
|---|---|---|
| logged, ns/event | 5,718 → 3,039 (−46.9%) | 4,537 → 1,916 (−57.8%) |
| logged without data, ns/event | 2,839 → 3,075 (+8.3%) | 2,335 → 1,866 (−20.1%) |
| logged, native B/actor | 1,129 → 787 (−30.3%) | 1,129 → 787 (−30.3%) |
| logged, RSS B/actor | 7,553 → 3,932 (−47.9%) | 14,647 → 8,323 (−43.2%) |
| logged, allocator calls/event | 2.10 → 0.01 | 2.10 → 0.01 |
| logged, journal bytes/event | 184 → 67 | 184 → 67 |
| unlogged, ns/event | 763 → 767 | 684 → 681 |

- **Node, logged without data, +8.3%:** the recorder builds an
  `ActorEvent` object per record and the bench serializes it, where the
  old sink copied bytes core had written. With data, the recorder wins
  by half, because it serializes nothing itself.
- **RSS flags in the tracing bench on Bun (+5.5 to +10.4%) and in Node's
  unlogged variant (+5.5%):** core, native and heap bytes per actor are
  identical, and no recorder runs there; an A/A run of `bench-compare`
  showed the working tree's side 3 to 4.6% higher in RSS on the same
  variants (kernel ADR 0001's notes), so they are read as its bias.
- **Send data and tracing throughput:** within ±3% on both runtimes.

`test:full` passed for the kernel's removal (commit `89fbaf8`): 549 test
host scenarios, `scale_test` flat to four million actors at 332 bytes
each, napi's suites and `scale.js` on Node and Bun.
