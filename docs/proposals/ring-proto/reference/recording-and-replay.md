# Recording and replay

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

**Replay is for seeding new execution.** Core feeds supplied inputs into a
prepared session to derive a chart configuration and data model values.
Publishing starts live execution from that result. The result may differ
from the earlier execution: replay does not promise to restore its state,
resource failures, scheduling or in-flight messages.

The application chooses the inputs, their times, the compatible definition
and the new system's parameters. Core gives it three things:

- a lossless record of the inputs a recorded session took;
- a way to feed them back without crossing any border;
- a way to put the seeded session on the route.

Everything else is the application's: whether to record, where and when to
store the records, when to truncate, and which inputs to use as a seed.
Recording does not hold effects or require persistence before execution.

Debugger interventions are outside the production input protocol. Their
audit and replay limits, including the use of a stable post-intervention
checkpoint, are described in [debugging.md](debugging.md). A log of ordinary
events alone does not specify a forced operator transition.

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

## Input observations are `c2h.input`

When a recorded session takes an envelope, core writes
[`c2h.input`](../protocol/generated/MESSAGES.md#c2hinput) before any
effect of the macrostep that envelope starts. The record has everything
replay needs:

| Field | Why replay needs it |
|---|---|
| header `at` | the session's clock when it took the input (core ADR 0094) |
| `macrostep` | the order, and a check that nothing is missing |
| `name`, `type`, `sendid`, `origin`, `origin_type`, `invokeid` | `_event`, exactly as the chart saw it |
| `data` | a snapshot of `_event.data` before any processing of this input, as a vref the recorder encodes |
| `source`, `source_seq`, `from_session` | where it came from: an h2c record, mail, a timer, a platform event, an invocation report |
| `report_invokeid`, `report_reason`, `report_kind`, `report_ends` | for source `invoke_report`, the original report needed to reconstruct h2c.invoke_report; these are separate from `_event` fields |

A recorded session's log is:

1. [`c2h.spawned`](../protocol/generated/MESSAGES.md#c2hspawned) (the
   address and the definition revision);
2. its `c2h.input` records, in order;
3. optionally [`c2h.state`](../protocol/generated/MESSAGES.md#c2hstate)
   records, which let the log be truncated (see [Checkpoints](#checkpoints)).

It takes the place of today's kernel recorder
(`tinyactors-host/src/recorder.c`). There is no input log byte layout:
kernel ADR 0006 removed core ADR 0090's. The ring is an observation format; the application chooses its
journal format, encoding vrefs with the binding's codec.

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

### Recording and effects are independent

A recorded session's `c2h.input` precedes the effects of the macrostep it
starts. The host may execute each effect on receipt, including when
`ta_run` returns during a budget pause. It need not wait for
[`c2h.macrostep_ended`](../protocol/generated/MESSAGES.md#c2hmacrostep_ended), the recorder or
storage. The same rule applies to unrecorded sessions.

Core snapshots recorded input data synchronously with the data model's
`copy_out` before taking the input or running `<finalize>`, autoforwarding
or chart code. The observation owns that snapshot, and chart processing
uses the original input. Retaining an immutable value is sufficient; a
mutable data model must isolate the snapshot from chart mutations. Thus
the host can encode after `ta_run` returns, even when the chart changed
its live `_event.data` during that call. See
[data-model.md](data-model.md#recorded-input-snapshots).

If the snapshot fails, no input is taken, no chart code runs and no input
observation is committed. An external submission receives
`c2h.input_refused`; local mail or a local delayed send is recycled into
a data-free `error.communication` for its sender, or discarded if the
sender is gone. Recording this error requires no data copy.
The application chooses whether to retry a refused external submission.
This failure is not an `error.execution` in a macrostep that never began.

The application chooses when to encode and persist the stable observation.
It releases the observation's vref when finished. Snapshots impose no
persistence-before-effects condition.

A crash may leave an executed effect with no journaled input, or a
journaled input whose effect never executed. A `c2h.input` is written when
the input is taken, so it may also describe a macrostep that never reached
its macrostep end. The application may use completion reports to select seeds,
but neither recording, a macrostep end nor input completion is a durability acknowledgement.
Charts and I/O processors use their own retries, resync and business-key
idempotency. The protocol introduces no write-ahead hold.

### Completion does not depend on recording

Every external submission has exactly one terminal disposition, correlated
by its h2c sequence: `c2h.bounced` before admission,
`c2h.input_refused` after admission but before take, or
`c2h.input_completed` after processing finishes. The completion status is
`stable`, `done`, `runaway` or `abandoned`. These reports exist for recorded,
unrecorded and replaying sessions alike. A refusal includes load failure,
destruction before take, snapshot failure and withdrawal by the host
([`h2c.withdraw`](../protocol/generated/MESSAGES.md#h2cwithdraw)); lossy dead-letter telemetry
does not replace it.

`c2h.macrostep_ended` reports an individual macrostep. Input completion
follows any startup followups required to finish processing that input;
it is the report a host uses to settle an individual `send()` request.
`c2h.input` is the separate, optional observation of a taken input.
Source h2c sequences survive admission, waiting and startup followups so
none of these reports relies on the recording selector.

The host keeps a bounded outstanding table keyed by h2c sequence, with
separate original references if retry is desired. A terminal disposition
settles and removes that entry. When the table is full, the host refuses
at its edge rather than growing an overflow queue.

## Replay

Replay feeds the application's seed inputs into a session prepared to
replay (core ADR 0089). A journal is one source of seeds; fixtures and
application-selected inputs are others:

```
h2c.spawn   {address, revision, flags: replaying_from_clock, clock: <spawned.at>}
h2c.event   {name, type, sendid, origin, ..., data: <decoded>}   at = input.at   ×N
h2c.invoke_report {...}                                          for source invoke_report
h2c.publish {address, at: <time chosen for the seeded session>}
```

- [`h2c.spawn`](../protocol/generated/MESSAGES.md#h2cspawn) with the
  `replaying_from_clock` flag prepares the session. Its clock starts at
  `clock`, and moves with each input's header `at` (core ADR 0094).
- **Every input is fed as h2c, whatever its source.** Mail, timers and
  platform events are fed back as `h2c.event`, keeping their `type`,
  `sendid` and `origin`. The sessions that sent them are not involved.
- **Invocation seeds use `invocation_seq = 0`.** The live token is a
  process-local `c2h.invoke` sequence, not durable SCXML event metadata;
  never feed an old execution's token into the new one. Synthetic replay
  events keep their SCXML `invokeid`. Replay report processing resolves
  `report_invokeid` against the seeded session's active invocation and
  uses the recorded `report_reason`, `report_kind` and `report_ends`.
  It never parses an invocation identity out of a dotted event name.
  Error reports leave the event's `invokeid` empty, while their separate
  report identity remains available for replay. The zero token
  is permitted only for synthetic replay inputs, not live service replies.
- **A replaying session crosses no border.** It writes no `c2h.send`,
  `c2h.invoke` or `c2h.log`. In a session replay it also puts no mail in the
  pool: its receivers are not being replayed. In a whole-system replay
  ([below](#session-replay-and-whole-system-replay)), mail between replaying
  sessions is delivered as envelopes, because it never leaves the process. Effects it
  would have had go to `tel` with the `suppressed` flag
  ([telemetry.md](telemetry.md)).
- **Its delayed sends are held in the pool, with their due times.** A
  held send due by an input's time is dropped. The supplied input stream
  determines what the new session processes during seeding.
- [`h2c.publish`](../protocol/generated/MESSAGES.md#h2cpublish) puts it
  on the route:
  - every previously supplied seed input must have a terminal disposition;
    core refuses publication while any supplied seed input is outstanding,
  - held delayed sends due by `at` are dropped (core ADR 0094 section 1),
    and the rest stay in the pool;
  - active invocations are retired with
    `error.communication.invoke.<id>.unavailable` (ADR 0089);
  - [`c2h.published`](../protocol/generated/MESSAGES.md#c2hpublished)
    reports the counts.

Because timers are envelopes in the pool, replay derives pending timers
without a host-side reducer. That is one reason this proposal
keeps delayed sends in core ([envelope.md](envelope.md), "Delayed
sends").

### The result is a seed, not an exact restoration

Replayed actions execute under the new definition and resource conditions.
For example, a send that failed for a full pool in the old execution may
succeed during seeding, producing different configuration and data model
values. Budgets, ring draining, effect suppression and data model behavior
can also change the result. This is expected and accepted.

Per-session input observations also do not restore:

- **Mail the session sent that was still in flight.** Its receivers did
  not take it, and replay does not send it again.
- **Mail sent to the session that it had not taken yet.** It never
  appears in the log.
- **Which effects executed or became durable.** Input observations and
  completion reports do not establish either fact.

The seeded state is the result of the supplied inputs in the new execution.
It need not match the earlier actor or the outside world. Applications
resync and retry where that matters. See
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
[`h2c.spawn`](../protocol/generated/MESSAGES.md#h2cspawn) `{state: image}`
as a replaying session, followed by selected inputs after `input_seq`, then
[`h2c.publish`](../protocol/generated/MESSAGES.md#h2cpublish).

The application decides when to checkpoint, for example every N inputs or
when the log passes a size. It may then delete the log's inputs up to
`input_seq`. [S06](../scenarios/S06-checkpoint.md) walks through it.

## Session replay and whole-system replay

| | Session replay | Whole-system replay |
|---|---|---|
| What is recorded | `c2h.input` of the recorded sessions | every h2c record, plus the system parameters and the definitions' revisions |
| What it seeds | one session at a time, alone | several sessions together, including local mail the new execution generates |
| Requires | inputs and times, a compatible definition and data model | an input stream, definitions and data models, and parameters chosen for the new execution |
| Cost | one record per input of the recorded sessions | one record per external input, time move, load and unload of the whole system |

Whole-system replay feeds an h2c stream into a new system. The stream can
come from an earlier recording, but it does not determine the old run's
budget boundaries, ring draining, resource availability or exact crash cut.
Suppressed external effects also use different resources. Even with the
same definitions and parameters, the new configuration, values and pending
mail may differ. Exact reconstruction is not the contract.

### How a whole-system replay runs

1. **Every h2c record of the recorded stream is posted again with the
   `replaying` flag.** Sessions that a replaying `h2c.spawn` creates are
   replaying sessions.
2. **Mail between replaying sessions is delivered**, as envelopes on the
   run queue, as generated by the new execution.
3. **Effects that would leave the process are suppressed:** no `c2h.send`,
   `c2h.invoke` or `c2h.log`. They go to `tel` with the `suppressed` flag.
4. **Choose a finite input cut.** Stop supplying seed inputs and wait for
   their terminal dispositions. Generated local mail need not be exhausted;
   a self-sending chart need not reach system-wide quiescence.
5. **One [`h2c.publish`](../protocol/generated/MESSAGES.md#h2cpublish) with
   an empty address** ends the replay, and publishes every replaying
   session at once. Core refuses it while a supplied seed input remains
   outstanding. Generated mail still pending at the cut continues in live
   execution after publication.

**Why it works this way:**

- **Mail is delivered because it is internal state.** Mail between
  sessions never crossed the border the first time, so delivering it again
  produces no external effect while seeding. Delivering it lets the
  sessions derive state together; it does not restore an old queue.
- **Effects are suppressed while seeding.** Configuration and values are
  derived before live execution begins. Publish does not release those
  suppressed effects; live charts decide what to send next.
- **A flag identifies replay inputs.** Feeding the stream at once or in
  pieces may change resource conditions and the state that is derived.
- **One publish for all.** Publishing sessions one by one would let a
  published session's mail reach a session still replaying, or the other
  way round. One publish switches the whole system to live at a single
  point after the selected supplied inputs have completed. It does not
  require exhausting the generated mail queue.

**Unloading inside a whole-system replay.** Unloading only bounds memory, and
an image never outlives its process
([unloading.md](unloading.md#unloading-bounds-memory-it-is-not-persistence)).
For a whole-system seed that starts from an empty system, a recorded
[`h2c.load`](../protocol/generated/MESSAGES.md#h2cload) may refer to an image
that an [`h2c.unload`](../protocol/generated/MESSAGES.md#h2cunload) earlier
in the recording produced. The new execution runs that unload
and produces its own image, which may differ from the old one. The recorded
`h2c.load` holds a blob handle from the old process, which means nothing
in the new one. The replaying host gives core the image the matching unload just
produced. No image is ever stored for replay.

The application maps load records to images produced in the new execution.
It must handle an unavailable or incompatible image as a load failure.
Unload and load inputs influence waiting mail and pool usage; they do not
establish the earlier system's scheduling or state.


## Derived actors resync instead of replaying

A **derived actor** holds a view of other actors' state, like the
progress tracker in [S01](../scenarios/S01-fanout.md). Recording it gains
little. Replay derives a view from the supplied inputs without establishing
the source's current state ([S05](../scenarios/S05-crash-both-recorded.md)).

The pattern is in the chart:

1. The derived actor's bootstrap sends a sync request (`progress.sync`) to
   the actor it mirrors.
2. That actor answers with its current state, from its data model.
3. The derived actor replaces its view.

This reads the source's current state whether it was seeded by replay,
started fresh, or never crashed. Starting derived actors fresh is a useful
application policy; recording them is not required for resync.

## Checking a replay for divergence

During replay, `tel` carries the replayed session's CONDITION, ASSIGN and
MICROSTEP records, if the kind masks enable them. A host that also kept
the original run's `tel` records for the session can compare the two
streams:

- the same `macrostep`, `microstep`, `transition` and `state` fields
  mean the chart took the same path;
- a different `value_text` for an ASSIGN means the data model diverged.

Differences are expected when the new execution uses different conditions;
they are not a replay contract violation. Core does nothing special for
this comparison. Because `tel` is lossy, a check can
only report a divergence when both streams are complete (no
[`tel.dropped`](../protocol/generated/MESSAGES.md#teldropped) in either
stream). It is a debugging aid, not a guarantee.
