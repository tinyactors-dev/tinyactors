# ADR 0105: Recorded sessions expose input observations on c2h

**Status:** Accepted (2026-10-05); not yet implemented. Refines ADRs 0089, 0091 and 0094; supersedes ADR 0095. What changes for each, per the [migration table](../../../docs/proposals/ring-proto/reference/migration.md#core-adrs-superseded-or-refined):
- ADRs 0089 and 0094: h2c inputs seed a new execution before publish, and exact restoration is not promised. A replaying session, "sent is what happened", time on every input, held timers and invocations retired at publish stay;
- ADR 0091: its checkpoint is kept, as `c2h.state`, and now holds the session's delayed sends;
- ADR 0095: nothing is refused after the fact, because a send fails in its own macrostep, so a checkpoint has no refused sends to restore;
- ADR 0090 stays removed by [kernel ADR 0006](../../../tinyactors-host/docs/adr/0006-actors-record-their-events-through-the-application-and-are-rebuilt-by-replaying-them.md). `c2h.input` is an observation, not a journal format.

It also replaces kernel ADR 0006's recorder with the `recorded` bit and `c2h.input`. It keeps [napi ADR 0040](../../../tinyactors-napi/docs/adr/0040-effects-are-not-held-and-send-resolves-once-its-message-is-done-and-committed.md)'s rule that effects are not held.
**Date:** 2026-10-05

## Context

Kernel ADR 0006 took durability away from the runtime. The runtime
records the events an actor takes. The application journals them. A
prepared actor that crosses no border replays them, and is then
published. The kernel implements this with a recorder
(`tinyactors-host/src/recorder.c`, 509 lines) and a replay driver
(`replay.c`, 247 lines). The recorder keeps each taken event until its
macrostep ends, then hands the turn's records over in one batch.

The [ring protocol proposal](../../../docs/proposals/ring-proto/README.md)
deletes the kernel. Core talks to its host through three rings: h2c,
c2h and tel. Mail between local sessions, delayed sends and invocation
reports stay inside core's pool. So the host cannot see what a session
took by watching the records it writes:

- an `h2c.event` that core bounced, or that waited for a load that
  failed, is on h2c, but no session took it;
- mail from another local session, a delayed send that fell due,
  `error.communication` made from recycled mail and `done.invoke` from a
  local child are not on h2c, but a session took them.

Only core knows which envelope a session took, in which order and at
which clock. The order also depends on core's fairness rule between mail
and admitted input, which the host does not see.

Core still has replay machinery of its own from ADR 0089. A replaying
actor's effects are held in `ta_activation_effects` (`actor.h:14-43`,
`actor.c:124-200`, and replay-only send paths in `interpreter.c`). Under
the proposal, a replaying session writes no effects at all, so nothing
needs holding.

ADR 0089 also promised an exact rebuild. Its Scope says a replay under the
same revision rebuilds the actor's state when every input was committed
before its effects were released. The proposal gives that up. Effects run
on receipt. A new execution may use a new definition, new parameters and
new resource conditions. A send that failed for a full pool before may
succeed in the new execution. Budgets and ring draining also change what
happens. So the result of a replay is a seed for new execution, not a
restoration of the old one.

ADR 0095 restored sends the router refused when a checkpoint is
restored. Kernel ADR 0003 already withdrew most of it. Under the proposal
no send is refused after its macrostep: a full pool fails the send in the
sender's macrostep as `error.execution.capacity.pool`.

The sources are
[recording-and-replay.md](../../../docs/proposals/ring-proto/reference/recording-and-replay.md),
[ARCHITECTURE.md, "Recording and replay"](../../../docs/proposals/ring-proto/ARCHITECTURE.md#recording-and-replay),
[BEHAVIORS.md, "Crashes"](../../../docs/proposals/ring-proto/BEHAVIORS.md#crashes)
and the scenarios
[S03](../../../docs/proposals/ring-proto/scenarios/S03-crash-unrecorded.md) to
[S06](../../../docs/proposals/ring-proto/scenarios/S06-checkpoint.md).

## Decision

1. **Recording is one bit on each session.** A session is recorded when
   its address matches a recording selector at the time it is spawned or
   loaded.
   - A selector is a family (`campaign`) or a family and a name
     (`campaign:42`).
   - The bit is set when the session is created or loaded, and does not
     change afterwards.
   - The list of selectors is a system setting, not a parameter
     (ADR 0092). It can change at run time. A change applies to sessions
     created or loaded afterwards.
   - Anonymous actors have no address, so they are never recorded, as
     with kernel ADR 0006's recorder.
   - Every record about a recorded session carries the common flag
     `recorded` (bit 9 of the
     [common header](../../src/generated/MESSAGES.md#common-header-32-bytes)).

2. **A recorded session's inputs appear as
   [`c2h.input`](../../src/generated/MESSAGES.md#c2hinput).** Core writes
   it when the session takes an envelope, before any effect of the
   macrostep that envelope starts. The record holds what replay needs:
   - header `at`: the session's clock when it took the input (ADR 0094);
   - `macrostep`: the order, and a check that nothing is missing;
   - `name`, `type`, `sendid`, `origin`, `origin_type`, `invokeid`:
     `_event` as the chart saw it;
   - `data`: a vref to a snapshot of `_event.data` (item 3);
   - `source`, `source_seq`, `from_session`: where the input came from
     (h2c, mail, a timer, a platform event, an invocation report);
   - `report_invokeid`, `report_reason`, `report_kind`, `report_ends`:
     the original invocation report, for source `invoke_report`;
   - the trace parent the macrostep runs under, in `h2c.event`'s form. For
     mail and timers, `parent_span_id` is the span ID of the sending step.
     A replay fed back as `h2c.event` runs under the same trace ID and
     parent span (ADRs 0067 and 0073).

   A recorded session's log is its
   [`c2h.spawned`](../../src/generated/MESSAGES.md#c2hspawned), then its
   `c2h.input` records in order, then optional `c2h.state` records
   (item 7). Core writes no `c2h.input` for an input a replaying session
   takes: that input is already in the log being replayed.

3. **The data is a snapshot taken before the input runs.** Core calls the
   data model's `copy_out` (ADR 0097) before it takes a recorded input,
   and before `_event` is installed or `<finalize>`, autoforwarding or
   chart code runs. The observation owns the snapshot. The chart uses the
   original. So the host can encode the data after `ta_run` returns, even
   when the chart changed its live `_event.data`.
   - If the snapshot fails, core takes no input, runs no chart code and
     writes no observation.
   - An external submission then gets
     [`c2h.input_refused`](../../src/generated/MESSAGES.md#c2hinput_refused).
   - Local mail or a local delayed send is recycled into a data-free
     `error.communication` for its sender, or discarded if the sender is
     gone.
   - It is not an `error.execution`: no macrostep began.

4. **Journaling is the application's policy.** Core gives a lossless
   observation of each taken input. The application decides whether to
   record, where and when to store, when to truncate, and which inputs to
   use as a seed. The ring is an observation format. There is no input
   log byte layout: the application chooses its journal format, and
   encodes vrefs with the binding's codec. It releases the observation's
   vref when it has used it.

5. **Effects execute on receipt; there is no write-ahead hold.** In live
   execution the host may execute each c2h effect when it reads it,
   including when `ta_run` returns during a budget pause. It waits for
   nothing: not for
   [`c2h.macrostep_ended`](../../src/generated/MESSAGES.md#c2hmacrostep_ended),
   not for the recorder, not for storage. This holds for recorded and
   unrecorded sessions alike (napi ADR 0040, kept). Neither recording, a
   macrostep end nor input completion is a durability acknowledgement.
   - A crash may leave an executed effect with no journaled input, or a
     journaled input whose effect never executed.
   - A `c2h.input` is written when the input is taken. It may describe a
     macrostep that never reached its end.
   - [`c2h.input_completed`](../../src/generated/MESSAGES.md#c2hinput_completed),
     `c2h.input_refused` and `c2h.bounced` give every external submission
     exactly one terminal disposition, whether its session is recorded,
     unrecorded or replaying. They do not depend on the recording
     selector.

6. **Replay seeds new execution before publish.** Core feeds supplied
   inputs into a prepared session to derive a chart configuration and data
   model values. Publishing starts live execution from that result.
   - [`h2c.spawn`](../../src/generated/MESSAGES.md#h2cspawn) with the
     `replaying_from_clock` flag prepares the session. Its clock starts at
     `clock`, and moves with each input's `at` (ADR 0094).
   - Every input is fed back as h2c, whatever its source: mail, timers and
     platform events as `h2c.event`, keeping their `type`, `sendid` and
     `origin`; invocation reports as `h2c.invoke_report`. Replay inputs
     use `invocation_seq = 0`. The live token is a process-local sequence,
     not durable event metadata.
   - A replaying session crosses no border. It writes no `c2h.send`,
     `c2h.invoke` or `c2h.log`. In a session replay it puts no mail in the
     pool. Its effects go to `tel` with the `suppressed` flag. This is
     ADR 0089's "sent is what happened", applied to records.
   - Its delayed sends are held in the pool with their due times. A held
     send due by an input's time is dropped.
   - [`h2c.publish`](../../src/generated/MESSAGES.md#h2cpublish) puts the
     session on the route. Core refuses it while any supplied seed input
     has no terminal disposition. Held sends due by `at` are dropped; the
     rest stay in the pool. Active invocations are retired with
     `error.communication.invoke.<id>.unavailable` (ADR 0089).
     [`c2h.published`](../../src/generated/MESSAGES.md#c2hpublished)
     reports the counts.
   - **The result is a seed, not a restoration.** It may differ from the
     earlier execution as definitions, data models, resources, budgets and
     ring draining change. Replay does not restore mail the session sent
     that was still in flight, mail sent to it that it had not taken, or
     which effects executed or became durable. This is expected and
     accepted.

7. **Whole-system replay feeds an h2c stream into a new system.** The
   application records every h2c record it posted, in order.
   - Each record is posted again with the common flag `replaying`.
     Sessions a replaying `h2c.spawn` creates are replaying sessions.
   - Mail between replaying sessions is delivered as envelopes, as the new
     execution generates it. Effects that would leave the process are
     suppressed and go to `tel`.
   - The host chooses a finite input cut and waits for the terminal
     dispositions of the supplied inputs. Generated mail need not drain.
   - One `h2c.publish` with an empty address publishes every replaying
     session at once, so no published session's mail reaches one still
     replaying. Generated mail still pending at the cut continues in live
     execution.
   - It does not restore the old system's scheduling, budget boundaries,
     crash cut or in-flight mail. A recorded `h2c.load` names a blob of
     the old process. The host gives core the image the matching
     `h2c.unload` produced in the new execution, and treats a missing or
     incompatible image as a load failure.

8. **A checkpoint is `c2h.state`.**
   [`h2c.checkpoint`](../../src/generated/MESSAGES.md#h2ccheckpoint) asks
   for a stable recorded session's state. Core answers with
   [`c2h.state`](../../src/generated/MESSAGES.md#c2hstate) (ADR 0091).
   - The image holds the configuration, history, active invocations, the
     data model root through `value_write`, and the session's delayed
     sends, which are envelopes the session owns. A checkpoint is one
     artifact.
   - `input_seq` names the last `c2h.input` it includes.
   - Replay from it is `h2c.spawn {state}` as a replaying session, then
     selected inputs after `input_seq`, then `h2c.publish`.
   - The application decides when to checkpoint and may then delete the
     log's inputs up to `input_seq`.

9. **Unload images bound memory; they are not persistence.** An image
   `c2h.unloaded` writes is valid only in the process that wrote it
   (ADR 0074). `h2c.load` never restores anything across a restart. After
   a crash there are no unloaded sessions. A session comes back through
   replay or a checkpoint, or starts fresh
   ([unloading.md](../../../docs/proposals/ring-proto/reference/unloading.md#unloading-bounds-memory-it-is-not-persistence)).

10. **Idempotency keys come from content, never from the sendid.** A
    receiver or I/O processor that deduplicates keys on the content of a
    send, such as `(recipient, notification ID)`. A fresh or seeded
    session counts generated sendids from the start again (ADR 0089
    section 5 numbers them per session). The same sendid text then names
    different sends before and after a crash: a key on it would take a
    new run for the old one, or the old one for a new run
    ([S03.4](../../../docs/proposals/ring-proto/scenarios/S03-crash-unrecorded.md#restart)).
    Retries, timeouts, resync and business-key idempotency recover what a
    crash separates; the protocol adds no hold for them.

### What this does not decide

- How or where an application journals, encodes or truncates: its own
  policy (item 4).
- Debugger interventions. Operator transitions and abandons are outside
  the production input protocol. A log of ordinary events does not
  reproduce them; their audit and replay limits are in
  [debugging.md](../../../docs/proposals/ring-proto/reference/debugging.md).
- Which actors to record. Derived actors, such as a progress tracker,
  resync from their source in the chart rather than being recorded
  ([S05](../../../docs/proposals/ring-proto/scenarios/S05-crash-both-recorded.md#per-session-replay)).
  That is application guidance, not a rule core enforces.
- Divergence checking. `tel` lets a host compare a replay's trace with
  the original's; core does nothing special for it.
- The ring, envelope and macrostep-slot decisions this rests on. Each has
  its own ADR from the proposal.

## Consequences

- **Core sees what the host cannot.** Mail, timers and platform events
  appear in a recorded session's log, in the order the session took them.
  A recorded sender's `c2h.input` is on c2h before its receiver can take
  the mail, because mail waits for the sender's macrostep end.
- **Recording and effects are independent.** A host never delays an
  effect for a journal. A caller that needs durability awaits its own
  flush.
- **A crash's cost is explicit.** Per
  [BEHAVIORS.md's crash matrix](../../../docs/proposals/ring-proto/BEHAVIORS.md#crashes):
  - nothing recorded: sessions start fresh, and the user's retry and the
    processor's content key recover;
  - the primary actor recorded: it is seeded and published, its pending
    timer survives in the pool, and a fresh derived actor resyncs;
  - both recorded per session: in-flight mail is lost by both sides, and
    the replayed derived actor does not resync until told. Recording a
    derived actor makes it worse;
  - the whole system recorded: sessions are seeded together and generate
    new local mail; the old in-flight mail is not restored.

  In every case what crossed the border and was never acknowledged is
  recovered by the chart's pending state, its timeout or a user's retry,
  and the receiver's content key.
- **Charts and applications do more.** They resync derived state, treat
  every asynchronous reply as one that may never come, and key
  idempotency on content.
- **Delayed sends survive in a checkpoint without a host-side reducer.**
  They are envelopes in the image, so a checkpoint is one artifact
  ([S06](../../../docs/proposals/ring-proto/scenarios/S06-checkpoint.md)).
- **Callers change:** napi's `setRecorder`, `PreparedActor.replay` and
  loader `{ replay }` read `c2h.input` and post replay records
  (`src/napi_recorder.c`, `src/napi_replay.c`); the test host's hosted
  recording and replay become ring scripts.
- **Costs:**
  - removed from core: held effects, `ta_activation_effects` and its
    paths, about 300 lines. A replaying session writes no effects, so
    nothing is held. Before, ADR 0089 recorded `ta_activation_effects`
    at 64 bytes, for unpublished actors only;
  - removed with the kernel: its recorder and replay driver, 756 lines
    (509 and 247);
  - per recorded input: one `c2h.input` of 152 fixed bytes, plus its
    texts (name, sendid, origin, origin type, invokeid, report texts) in
    the record's tail. It is part of the input's mandatory c2h batch, so
    it counts against the c2h reserve (`c2hBytes`). Before, the kernel's
    recorder kept the taken event until its macrostep ended and handed it
    over once per turn; kernel ADR 0006 measured 67 journal bytes per
    event and 787 native bytes per actor for that path in napi's
    `input_log.js`;
  - per recorded input with data: one `copy_out` call. Core allocates
    nothing for it; a mutable data model may;
  - an unrecorded session writes no `c2h.input`. It pays one bit test per
    input;
  - per actor: the `recorded` bit. Step 5.5 records whether it fits in an
    existing flags field, which leaves bytes per actor unchanged.
- **Measured in step 5.5**
  ([IMPLEMENTATION.md](../../../docs/proposals/ring-proto/IMPLEMENTATION.md#phase-5-rings-replace-the-vtables)):
  bytes per actor before and after; the bytes and time per recorded input
  with and without data, with `bench-compare` against the commit before
  the step, on Node and Bun; that an unrecorded session costs the same as
  before; and the lines removed. Step 5.5's implementation notes give
  those figures.
