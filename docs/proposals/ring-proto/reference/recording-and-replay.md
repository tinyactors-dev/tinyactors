# Recording and replay

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

Replay is an application decision. Core does not know where its inputs
come from. It gives the application three things:

- a lossless record of the inputs a recorded session took;
- a way to feed them back without crossing any border;
- a way to put the rebuilt session on the route.

Everything else is the application's: whether to record, where to store
the records, when to truncate, and which sessions to replay.

## Which sessions are recorded

A session is recorded when its address matches a **recording selector**
at the time it is spawned or loaded:

- a selector is a family (`campaign`), or a family and a name
  (`campaign:42`);
- core keeps one bit per actor, set when the session is created or loaded
  and never changed afterwards;
- records about a recorded session carry the common flag `recorded`
  ([MESSAGES.md, common header](../protocol/generated/MESSAGES.md#common-header-32-bytes)).

This is napi's `setRecorder(recorder, { families })` (kernel ADR 0006
section 2, `tinyactors-napi/index.d.ts`) moved into core as a per-actor
bit. The list of selectors is a system setting, not a parameter: it can
change at run time, and the change applies to sessions created or loaded
afterwards.

Anonymous actors have no address, so they are never recorded. That
matches today's recorder, which records named actors only.

## The log is `c2h.input`

When a recorded session takes an envelope, core writes
[`c2h.input`](../protocol/generated/MESSAGES.md#c2hinput) before any
effect of the macrostep that envelope starts. The record has everything
replay needs:

| Field | Why replay needs it |
|---|---|
| header `at` | the session's clock when it took the input (core ADR 0094) |
| `macrostep` | the order, and a check that nothing is missing |
| `name`, `type`, `sendid`, `origin`, `origin_type`, `invokeid` | `_event`, exactly as the chart saw it |
| `data` | `_event.data`, as a vref the recorder encodes |
| `source`, `source_seq`, `from_session` | where it came from: an h2c record, mail, a timer, a platform event, an invocation report |

A recorded session's log is:

1. [`c2h.spawned`](../protocol/generated/MESSAGES.md#c2hspawned) (the
   address and the definition revision);
2. its `c2h.input` records, in order;
3. optionally [`c2h.state`](../protocol/generated/MESSAGES.md#c2hstate)
   records, which let the log be truncated (see [Checkpoints](#checkpoints)).

This replaces the kernel's recorder (`tinyactors-host/src/recorder.c`) and
core's input log byte layout (core ADR 0090). The ring record *is* the
byte layout.

### Why record from c2h and not from h2c

A host could log every h2c record it writes. That is not the session's
history:

| | On h2c? | Did the session take it? |
|---|---|---|
| An `h2c.event` core [bounced](../protocol/generated/MESSAGES.md#c2hbounced) | yes | no |
| An `h2c.event` that waits in the pool for a load, then is dropped by [`h2c.load_failed`](../protocol/generated/MESSAGES.md#h2cload_failed) | yes | no |
| Mail from another local session ([envelope.md](envelope.md)) | **no** | yes |
| A delayed send that fell due | **no** | yes |
| `error.communication` made from recycled mail | **no** | yes |
| `done.invoke` from a local child | **no** | yes |

Only core knows which envelope a session took, in which order, and at
what clock. The order also depends on core's fairness rule between mail
and admitted input ([envelope.md](envelope.md)), which the host does not
see.

### Persist before acting on the fence

A recorded session's `c2h.input` comes before its
[`c2h.fence`](../protocol/generated/MESSAGES.md#c2hfence), as do its
effects. The host:

1. reads up to the fence;
2. encodes and persists the `c2h.input`;
3. then releases the effects (sends, invocations, logs).

The result is that **a session's memory is a superset of what it did**:

- a crash before step 2 loses the input and its effects together, so
  nothing happened;
- a crash between steps 2 and 3 replays an input whose effects never left.
  The chart must recover from that, with a timeout or a retry, and
  [S04](../scenarios/S04-crash-campaign-recorded.md) shows how.

For a session that is **not** recorded, the fence gives no guarantee
that matters. The host may release its effects as it reads them.

This refines napi ADR 0040, which removed the write-ahead hold and made
effects at least once. Here the hold comes back, for recorded sessions
only, and costs memory for one macrostep's records.

## Replay

Replay feeds the log back under a session prepared to replay (core ADR
0089):

```
h2c.spawn   {address, revision, flags: replaying_from_clock, clock: <spawned.at>}
h2c.event   {name, type, sendid, origin, ..., data: <decoded>}   at = input.at   ×N
h2c.invoke_report {...}                                          for source invoke_report
h2c.publish {address, at: <time the run being replayed stopped>}
```

- [`h2c.spawn`](../protocol/generated/MESSAGES.md#h2cspawn) with the
  `replaying_from_clock` flag prepares the session. Its clock starts at
  `clock`, and moves with each input's header `at` (core ADR 0094).
- **Every input is fed as h2c, whatever its source.** Mail, timers and
  platform events are fed back as `h2c.event`, keeping their `type`,
  `sendid` and `origin`. The sessions that sent them are not involved.
- **A replaying session crosses no border.** It writes no `c2h.send`,
  `c2h.invoke` or `c2h.log`. In a session replay it also puts no mail in the
  pool: its receivers are not being replayed. In a whole-system replay
  ([below](#session-replay-and-whole-system-replay)), mail between replaying
  sessions is delivered as envelopes, because it never leaves the process. Effects it
  would have had go to `tel` with the `suppressed` flag
  ([telemetry.md](telemetry.md)).
- **Its delayed sends are held in the pool, with their due times.** A
  held send due by an input's time is dropped, because the input stream
  says what happened next.
- [`h2c.publish`](../protocol/generated/MESSAGES.md#h2cpublish) puts it
  on the route:
  - held delayed sends due by `at` are dropped (core ADR 0094 section 1),
    and the rest stay in the pool;
  - active invocations are retired with
    `error.communication.invoke.<id>.unavailable` (ADR 0089);
  - [`c2h.published`](../protocol/generated/MESSAGES.md#c2hpublished)
    reports the counts.

Because timers are envelopes in the pool, replay rebuilds them without
help. No host-side reducer is needed. That is one reason this proposal
keeps delayed sends in core ([envelope.md](envelope.md), "Delayed
sends").

### What replay does not restore

- **Mail the session sent that was still in flight.** Its receivers did
  not take it, and replay does not send it again.
- **Mail sent to the session that it had not taken yet.** It never
  appears in the log.
- **Effects released after the last fence persisted.** They happened, but
  the replayed session believes they did not happen, or the reverse.

Each actor's memory stays consistent with what that actor did. Two actors
can still disagree with each other. See
[S05](../scenarios/S05-crash-both-recorded.md).

## Checkpoints

[`h2c.checkpoint`](../protocol/generated/MESSAGES.md#h2ccheckpoint) asks
for a stable recorded session's state. Core answers with
[`c2h.state`](../protocol/generated/MESSAGES.md#c2hstate) (core ADR 0091):

- the image holds the configuration, history, active invocations, the
  data model root (through `value_write`), **and the session's delayed
  sends**, because they are envelopes the session owns;
- `input_seq` names the last `c2h.input` it includes.

A checkpoint is one artifact. Replay from it is
[`h2c.load`](../protocol/generated/MESSAGES.md#h2cload) `{image}` with its
`replaying` flag, followed by the inputs after `input_seq`, followed by
[`h2c.publish`](../protocol/generated/MESSAGES.md#h2cpublish).

The application decides when to checkpoint, for example every N inputs or
when the log passes a size. It may then delete the log's inputs up to
`input_seq`. [S06](../scenarios/S06-checkpoint.md) walks through it.

## Session replay and whole-system replay

| | Session replay | Whole-system replay |
|---|---|---|
| What is recorded | `c2h.input` of the recorded sessions | every h2c record, plus the system parameters and the definitions' revisions |
| What it rebuilds | one session at a time, alone | every session, together, including mail that was in flight between them |
| Requires | a deterministic data model | a deterministic data model, the same parameters (a capacity error depends on `poolEnvelopes`), the same definitions |
| Cost | one record per input of the recorded sessions | one record per external input, time move, load and unload of the whole system |

Whole-system replay works because h2c is the only way anything enters the
system ([ring.md](ring.md)), and core's scheduling of mail is a
deterministic function of what is in the pool ([envelope.md](envelope.md)).
Given the same h2c stream, core makes the same macrosteps in the same
order.

It closes the gap that session replay leaves: mail that was in flight
between two actors at the crash is reconstructed, because its sender's
macrostep is replayed in the same system as its receiver.

### How a whole-system replay runs

1. **Every h2c record of the recorded stream is posted again with the
   `replaying` flag.** Sessions that a replaying `h2c.spawn` creates are
   replaying sessions.
2. **Mail between replaying sessions is delivered**, as envelopes on the
   run queue, exactly as it was the first time.
3. **Effects that would leave the process are suppressed:** no `c2h.send`,
   `c2h.invoke` or `c2h.log`. They go to `tel` with the `suppressed` flag.
4. **One [`h2c.publish`](../protocol/generated/MESSAGES.md#h2cpublish) with
   an empty address** ends the replay, and publishes every replaying
   session at once.

**Why it works this way:**

- **Mail is delivered because it is internal state.** Mail between
  sessions never crossed the border the first time, so delivering it again
  repeats nothing the outside world saw. It is also the whole point:
  delivering it is what rebuilds the mail that was in flight at the crash.
- **Effects are suppressed for the same reason as in a session replay.**
  They already happened, or the charts' own retries cover them if they did
  not. A replay must not send a notification twice.
- **One flag on every record, not a mode of the system,** so the stream
  replays the same way whether the host feeds it at once or in pieces. Core
  needs no state beyond what the records say.
- **One publish for all.** Publishing sessions one by one would let a
  published session's mail reach a session still replaying, or the other
  way round. One publish switches the whole system to live at a single
  point in the input order.

**Open:**

- Whether effects of sessions that never crashed (inputs after the last
  fence the host acted on) should be released at publish instead of
  suppressed.
- How it interacts with sessions that were loaded from images that were
  written outside the recorded stream.

## Derived actors resync instead of replaying

A **derived actor** holds a view of other actors' state, like the
progress tracker in [S01](../scenarios/S01-fanout.md). Recording it gains
little. Replay restores what it took, but not mail that was in flight to
it, so its view is still behind ([S05](../scenarios/S05-crash-both-recorded.md)).

The pattern is in the chart:

1. The derived actor's bootstrap sends a sync request (`progress.sync`) to
   the actor it mirrors.
2. That actor answers with its current state, from its data model.
3. The derived actor replaces its view.

This works whether the source was replayed, started fresh, or never
crashed. Derived actors should not be recorded.

## Checking a replay for divergence

During replay, `tel` carries the replayed session's CONDITION, ASSIGN and
MICROSTEP records, if the kind masks enable them. A host that also kept
the original run's `tel` records for the session can compare the two
streams:

- the same `macrostep`, `microstep`, `transition` and `state` fields
  mean the chart took the same path;
- a different `value_text` for an ASSIGN means the data model diverged.

Core does nothing special for this. Because `tel` is lossy, a check can
only report a divergence when both streams are complete (no
[`tel.dropped`](../protocol/generated/MESSAGES.md#teldropped) in either
stream). It is a debugging aid, not a guarantee.
