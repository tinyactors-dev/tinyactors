# Behaviors at a glance

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](README.md)

This page summarizes the [scenarios](scenarios/README.md). Every bound in
the system is fixed, so every overflow has to become **a refusal to
someone who can retry**. Nothing queues it out of sight. Three parties
can retry:

| Who retries | How they are told | What they do |
|---|---|---|
| **The external client** | the host refuses at its edge, or settles a correlated core admission refusal | retries, or the user clicks again |
| **The sending chart** | its own action fails in its own macrostep: `error.execution.capacity.*`, or `error.communication` for mail that could not be delivered | throttles, retries on a timer, or gives up: its structure decides |
| **Nobody** (an asynchronous reply) | it is dropped | the chart that waits for it has a timeout. That is what the timeout is for. |

## Who drops what

| Situation | Dropped or refused by | Told how | Retried by | Steps |
|---|---|---|---|---|
| The notify processor rejects a notification | the processor | `notify.rejected` as an [`h2c.event`][h2c.event] | nobody: final | [S01.16](scenarios/S01-fanout.md#outcomes) |
| A notification cannot reach its relay | the processor | `error.communication` as an [`h2c.event`][h2c.event] | the chart, on its timeout | [S01.17](scenarios/S01-fanout.md#outcomes), [S01.22](scenarios/S01-fanout.md#the-retry-and-the-reply) |
| A send finds the pool full | core, in the sender's macrostep | `error.execution.capacity.pool`; the rest of the block is abandoned | the chart: `throttled` resumes on the next outcome | [S02.5](scenarios/S02-full-pool.md#the-brutal-chart), [S02.15](scenarios/S02-full-pool.md#the-brutal-chart) |
| An action's record does not fit in c2h | nothing is dropped: the slot parks before the action | nobody: other slots go on, and `ta_run` answers `TA_RUN_C2H_FULL` when none can | core, after the host drains | [macrostep](reference/macrostep.md#a-full-c2h-pauses-the-slot) |
| An action's record is larger than `maxRecordBytes` | core, in the sender's macrostep | `error.execution.capacity.record` | nobody: it can never fit | [`maxRecordBytes`](#maxrecordbytes) |
| A computed name does not fit in the name area | core, in the sender's macrostep | `error.execution.capacity.names` | the chart | [S10](scenarios/S10-run-time-names.md#names-a-chart-computes) |
| The pool or name area is past its watermark, or an input cannot fit | core consumes and bounces the event after reserving its reply | `c2h.bounced` with `pool` or `names`; host retains its own original in a bounded table | host or external client | [S02.10](scenarios/S02-full-pool.md#the-brutal-chart), [S09](scenarios/S09-edge-backpressure.md) |
| h2c is full and a client posts | the host, at its edge | HTTP 503 | the client | [S02.12](scenarios/S02-full-pool.md#the-brutal-chart), [S09.5](scenarios/S09-edge-backpressure.md#filling-up) |
| h2c is full and a processor reports a result | the host's bounded buffer (its policy) | nobody | the waiting chart's timeout; the processor deduplicates | [S09.6](scenarios/S09-edge-backpressure.md#filling-up), [S09.10](scenarios/S09-edge-backpressure.md#draining) |
| h2c is full and the wake timer fires | nothing is dropped: `h2c.time` is posted late | – | the host, when there is room | [S09.7](scenarios/S09-edge-backpressure.md#filling-up) |
| Mail waits for a session that cannot be loaded | core, on [`h2c.load_failed`][h2c.load_failed] | `error.communication` to the sender, in the same envelope | the sending chart, if it cares | [S07.12](scenarios/S07-unloaded-tracker.md#the-load-fails) |
| An external event waits for a session that cannot be loaded | core | `c2h.input_refused` with its h2c sequence, plus `DEAD_LETTER` telemetry | host or client | [S07.13](scenarios/S07-unloaded-tracker.md#the-load-fails) |
| A recording snapshot cannot be made | core before input take | external input: `c2h.input_refused`; local mail: recycled `error.communication` | host/client or sending chart | [S11](scenarios/S11-semantic-boundaries.md#recording-and-ownership) |
| An unload would split an active local invocation | core, without mutation | `c2h.bounced(active_local_invoke)` | host, after the invocation ends | [unloading](reference/unloading.md#unloading) |
| The host withdraws an input that has not been taken (a client disconnected, an `AbortSignal` fired) | core, on [`h2c.withdraw`][h2c.withdraw] | `c2h.input_refused` with reason `withdrawn`; nothing if the input was already taken, bounced or refused | nobody: the host asked for it | [envelope](reference/envelope.md#withdrawing-an-admitted-input) |
| A session's input reaches the chain limit (a runaway) | core, between steps: the input is abandoned, the session lives on | `c2h.input_completed` status `runaway`; `TA_TRACE_RUNAWAY` on `tel`. The chart is not told | nobody: the host may destroy a session that keeps running away | [macrostep](reference/macrostep.md#runaways-the-chain-limit) |
| A mandatory internal event fits neither the floor nor a free ordinary entry | core, as at the chain limit: the input is abandoned, the session lives on | `c2h.input_completed` status `runaway`; `TA_TRACE_RUNAWAY` with `runaway_cause` `internal_queue`. The chart is not told | nobody: as for a runaway | [macrostep](reference/macrostep.md#sizing-mandatory-storage) |
| An event nobody listens for | the chart: it enables nothing | [`tel.trace`][tel.trace] `IGNORED` | nobody | [S10.4](scenarios/S10-run-time-names.md#under-the-proposal) |
| An h2c record core cannot run (a malformed address, too deep a name, …) | core | [`c2h.bounced`][c2h.bounced] with a reason | the host decides | [S10.8](scenarios/S10-run-time-names.md#when-the-names-come-faster-than-they-are-freed) |
| Telemetry the host does not read fast enough | core | [`tel.dropped`][tel.dropped] with a count | nobody: telemetry never slows the system | [telemetry](reference/telemetry.md) |
| A delayed send is cancelled | core removes the envelope | – | – | [S08.4](scenarios/S08-cancel-and-second-click.md#order-a-the-click-before-the-clock) |
| A replaying session's effects | core: they cross no border | [`tel.trace`][tel.trace] with `suppressed` | the chart's own `pending` and timeout | [S04.4](scenarios/S04-crash-campaign-recorded.md#restart), [S04.12a](scenarios/S04-crash-campaign-recorded.md#then-either-of-two-things-finishes-the-job), [S04.12b](scenarios/S04-crash-campaign-recorded.md#then-either-of-two-things-finishes-the-job) |
| Duplicate notifications after a retry or a crash | the notify processor, by its content key | not visible to the chart | – | [S03.5](scenarios/S03-crash-unrecorded.md#restart), [S09.12](scenarios/S09-edge-backpressure.md#draining) |

An idempotency key must come from the content of a send, such as
`(recipient, notification ID)`, **never from its sendid**. A fresh session
counts sendids from the start again, so the same sendid text names
different sends before and after a crash ([S03.4](scenarios/S03-crash-unrecorded.md#restart)).

## When each resource is full

| Resource | Bounded by | When it is full | Who notices | Steps |
|---|---|---|---|---|
| **h2c** | `h2cBytes` | The host cannot post. It refuses at its edge or keeps the input in its own bounded buffer. Core never waits on h2c. | the external client | [S09.5](scenarios/S09-edge-backpressure.md#filling-up) |
| **c2h** | `c2hBytes`, `maxRecordBytes` | Ordinary actions preserve the mandatory reserve. One that may write c2h checks for room for the largest record before it evaluates anything; without it, its slot parks before the action, which runs once after the host drains; other slots go on. Outside macrosteps, core can return for a drain before consuming or mutating a control operation. | the host driver (`TA_RUN_C2H_FULL`) | [ring](reference/ring.md), [macrostep](reference/macrostep.md#a-full-c2h-pauses-the-slot) |
| **tel** | `telBytes` | The record is dropped and counted in [`tel.dropped`][tel.dropped]. | the operator | [telemetry](reference/telemetry.md) |
| **Pool** | `poolEnvelopes`, `admissionWatermark` | Core bounces capacity-blocked h2c events. A local send or delayed send fails as `error.execution.capacity.pool`. Local invocations hold lifetime tokens acquired before startup. | host/client and sending chart | [S02](scenarios/S02-full-pool.md) |
| **Name area** | `nameAreaBytes` | Core bounces capacity-blocked h2c events. A computed name that does not fit fails as `error.execution.capacity.names`. | host/client and sending chart | [S10](scenarios/S10-run-time-names.md) |
| **Internal queue** | ordinary `internalQueueEvents` quota plus a compiler-sized mandatory floor | A chart raise exhausting its quota fails as `error.execution.capacity.internal`. Mandatory events beyond the floor take free ordinary entries; one that fits in neither ends the input as a runaway. | the chart for a raise; the host for a runaway (`c2h.input_completed` `runaway`) | [macrostep](reference/macrostep.md#the-internal-queue-compiler-sized-mandatory-storage) |
| **Macrostep slots** | `maxConcurrentInputs` | Runnable envelopes wait in the run queue, or on an active session's deferred list, until a slot frees. Nothing is refused. | nobody: it shapes latency | [macrostep](reference/macrostep.md#macrostep-slots) |
| **Park area** | `parkAreaBytes` (derived by default) | A slot that cannot park at the end of its slice keeps the thread. | nobody: it shapes latency | [parking](reference/macrostep.md#parking-one-running-workspace-and-a-park-area) |
| **Data model** (the JavaScript heap) | the binding | An operation that fails reports `TA_OP_ERROR`, and core raises `error.execution`. It is the data model's own memory, never core's. | the chart | [data model](reference/data-model.md) |
| **The host's own buffers** | the host's policy | The host drops something, and must accept that only a timeout will notice. | the waiting chart | [S09.6](scenarios/S09-edge-backpressure.md#filling-up) |

## Crashes

One crash, partway through the fanout ([S03](scenarios/S03-crash-unrecorded.md#at-the-crash)):
the campaign has n5 pending, two tracker deltas are in flight, and `t`
is due later.

Replay uses selected journal entries to seed new configuration and data
model values. The examples below show one outcome with the illustrated
definitions and resource conditions, not a promise to restore the old run.
The host may execute effects on receipt, before a macrostep end or journal write;
a crash can separate any of those observations and actions.

| | **Tracker not recorded** | **Tracker recorded** |
|---|---|---|
| **Campaign not recorded** | Both start fresh. The user's next click runs the whole campaign again, and the processor deduplicates n1–n3. Correct. [S03](scenarios/S03-crash-unrecorded.md) | The tracker replays its old view of a campaign that no longer exists. Pointless: never record a derived actor without its source. |
| **Campaign recorded** | The campaign is replayed, and `t` survives in the pool. The fresh tracker shows zero until its bootstrap resync, and is then correct. A click or `t` sends n5 again. [S04](scenarios/S04-crash-campaign-recorded.md) | Both are replayed. **The two in-flight deltas are lost by both sides**, and the replayed tracker is `live`, so it does not resync until told to. Worse than not recording it. [S05](scenarios/S05-crash-both-recorded.md#per-session-replay) |
| **Whole system recorded** (the h2c stream) | – | The stream seeds the sessions together. They generate new local mail; old in-flight deltas and exact state are not restored by contract. External effects are suppressed while seeding. [S05](scenarios/S05-crash-both-recorded.md#whole-system-replay) |

In every row, what crossed the border and was never acknowledged is
recovered the same way: the chart's `pending`, its timeout or the user's
click, and the processor's content key.

## What a chart must do

1. **Schedule a timeout before a fanout,** not after. A capacity error
   abandons the rest of the block ([S02.5](scenarios/S02-full-pool.md#the-brutal-chart)).
2. **Handle `error.execution.capacity`** where a fanout can hit it, and
   make the handler safe to fail again without looping
   ([S02.7](scenarios/S02-full-pool.md#the-brutal-chart)).
3. **Keep optional traffic out of essential blocks.** Clocking a big
   fanout by acknowledgements ([windowed chart](charts/campaign-windowed.scxml))
   is still good practice: it bounds the work in flight, the envelopes it
   holds and how long its slot waits. A full c2h does not fail a chart,
   so the window bounds load; it does not prevent errors.
4. **Resync derived state** instead of recording it
   ([S04.9](scenarios/S04-crash-campaign-recorded.md#restart), [S05.5](scenarios/S05-crash-both-recorded.md#per-session-replay)).
5. **Treat every asynchronous reply as one that may never come.**

## Parameters

Every bound above is a system parameter under core ADR 0092's rules: it is
set once, when the system is created; every buffer is bounded by one; and
`ta_param_info_of()` reports each one's name, unit, default, minimum and
maximum.

**Every default is a guess.** The real figures come from measuring napi's
suites, `scale.js` and the IRP suite, as ADR 0092 did for
`interpreterBytes`. The ADR that adopts them records them. The pool ADR
records the ring sizes, measured in step 4.2.

**A ring's size is a throughput setting, not a correctness one.** A full
h2c makes the host refuse at its edge, a full c2h makes a slot wait for
the host, and a full tel drops telemetry. None of them fails a chart. So
the rings are small by default. The pool and the name area keep large
bounds, because they fill on demand and cost nothing until used, and
because a full one fails the chart's action.

| Parameter | Default | Minimum | Maximum | When it runs out, who is told |
|---|---|---|---|---|
| [`poolEnvelopes`](#poolenvelopes) | 4,096 | 8 | 16,777,216 | the chart (`.pool`); the client, through h2c |
| [`nameAreaBytes`](#nameareabytes) | 262,144 | `maxRecordBytes` | 1 GiB | the chart (`.names`); the client, through h2c |
| [`admissionWatermark`](#admissionwatermark) | 50% | 1% | 99% | the client, through h2c |
| [`fairnessRatio`](#fairnessratio) | 4 | 1 | 4,096 | nobody: it shapes latency |
| [`maxConcurrentInputs`](#maxconcurrentinputs) | 64 | 1 | 65,536 | nobody: inputs wait for a slot |
| [`parkAreaBytes`](#parkareabytes) | derived: `maxConcurrentInputs` × the largest parked execution | 0 (no parking) | the derived ceiling | nobody: a slot that cannot park keeps the thread; a runaway that cannot park is abandoned |
| [`slotSlice`](#slotslice) | 1,024 | 1 | 1,048,576 | nobody: it shapes latency |
| [`internalQueueEvents`](#internalqueueevents) | 256 ordinary entries, plus a compiler-sized mandatory floor | 0 ordinary entries | 65,536 ordinary entries | the chart (`.internal`) |
| [`h2cBytes`](#h2cbytes) | 16,384 | `maxRecordBytes` | 2 GiB | the client (the host refuses) |
| [`c2hBytes`](#c2hbytes) | derived: (3 × `maxConcurrentInputs` + 2) × `maxRecordBytes`; 794,624 with the defaults | 5 × `maxRecordBytes` | 2 GiB | nobody: the slot waits; the host (`TA_RUN_C2H_FULL`) |
| [`telBytes`](#telbytes) | 16,384 | 0, or else `maxRecordBytes` | 2 GiB | the operator (`tel.dropped`) |
| [`maxRecordBytes`](#maxrecordbytes) | 4,096 | 256 | `c2hBytes` / 5 | the chart (`.record`); the host (`c2h.bounced`) |
| [`telemetryValueBytes`](#telemetryvaluebytes) | 1,024 | 0 | `maxRecordBytes` − 256 | the operator (`truncated`) |
| [`maxPendingSends`](#maxpendingsends) (changed from today) | no quota | 0 | `poolEnvelopes` | the chart (`.pool`) |

### `poolEnvelopes`

- **What it bounds:** the envelopes in the pool. That covers admitted
  external input, mail between local sessions, delayed sends of resident
  sessions, and envelopes waiting for a load
  ([envelope.md](reference/envelope.md)). Unit: envelopes, about 88 bytes
  each plus 4 for the deadline index.
- **It also bounds waiting entries:** one per address that has envelopes
  waiting for a load, about 40 bytes each, filled on demand with the pool.
  No more addresses can wait than there are envelopes, so `poolEnvelopes`
  is their ceiling. The table that finds them grows in the pool's refill,
  with room for one key per envelope the pool holds (64 KiB when the
  default pool is full)
  ([envelope.md](reference/envelope.md#the-waiting-entry-one-copy-of-the-address)).
- **Default 4,096, about 368 KiB including the deadline index.** napi resumes up to 1,024 actors per turn
  today (`resumesPerTurn`, and `eventBlockCache` 1,024). Four times that
  lets each of them hold its input and have a few mail messages or timers
  pending. With the default watermark, 2,048 are for admitted input and
  2,048 are headroom for what that input causes.
- **Minimum 8.** At the default watermark that leaves 4 envelopes for input
  and 4 for mail. Below that, almost any fanout fails.
- **Maximum 16,777,216 (2^24), about 1.2 GiB.** A system that needs more
  pending envelopes than that has more work in one thread than one thread
  can serve. Split it into several systems.
- **What it drives:** how much mail, how many timers and how much admitted
  input can be pending at once. Together with `admissionWatermark`, it sets
  when ordinary admission is refused.
- **How it surfaces:**
  - **chart:** a `<send>` to a local session, a delayed `<send>`, or an
    `<invoke>` of a local child that finds no envelope fails as
    `error.execution.capacity.pool` ([S02](scenarios/S02-full-pool.md)),
    and so does a `<send>` to an address that is not resident and finds
    no free waiting entry;
  - **host:** `pool.free` and `pool.high_water` in the control block, and
    `admitting` turns false;
  - **client:** h2c fills, and the host answers 503.

### `nameAreaBytes`

- **What it bounds:** the bytes for text that has no ID: event names,
  origins, delayed sends' targets, the addresses that envelopes wait for
  (one copy per waiting address, not per envelope), invocation error
  reasons, and the trace parent of a traced envelope
  ([names.md](reference/names.md#the-name-area)). It is made of 64-byte
  chunks, 60 of them text.
- **Default 262,144 (256 KiB), one chunk per envelope at the default pool.**
  Every envelope carries at least its event name, and most fit in one
  chunk. An area smaller than `poolEnvelopes` × 64 would run out before the
  pool does, and charts would see `.names` where `.pool` is the real limit.
  Raise it when external input carries long origins or names.
- **Long addresses cost little while their actors are unloaded.** Mail to
  an unloaded actor holds only its own text. The address is held once, in
  the address's waiting entry, however many envelopes wait for it. A burst
  of mail to one unloaded actor costs name area like the same burst to a
  resident one, plus one copy of the address
  ([envelope.md](reference/envelope.md#the-waiting-entry-one-copy-of-the-address)).
- **A traced system needs more.** An envelope with a trace context holds
  its trace parent as one more text field of 40 bytes
  ([envelope.md](reference/envelope.md#the-trace-parent)). With a short name it
  takes two chunks, not one. A system that traces most of its inputs and
  mail sets `nameAreaBytes` to `poolEnvelopes` × 128 (512 KiB at the
  default pool). An untraced envelope costs nothing extra.
- **Minimum `maxRecordBytes`,** so the text of any one record fits.
- **Maximum 1 GiB.** Chunk indexes are `u32`. Past that, the system is too
  big for one thread, as for the pool.
- **What it drives:** how much text pending envelopes can hold. Admission
  stops at `admissionWatermark` of it, as for the pool.
- **How it surfaces:**
  - **chart:** a computed name, target or reason that doesn't fit fails as
    `error.execution.capacity.names` ([S10](scenarios/S10-run-time-names.md));
  - **host:** `names.used` in the control block, and `admitting`;
  - **client:** h2c fills, and the host answers 503.

### `admissionWatermark`

- **What it bounds:** the share of the pool (envelopes and name area)
  above which core stops moving h2c records into the pool. The rest is
  headroom for what sessions send each other. Unit: percent.
- **Default 50%.** How much mail one input causes depends on the chart,
  and the stress-test fanout causes many. An even split is the guess that
  is least wrong for unknown charts.
- **Minimum 1%:** 0 would admit nothing. **Maximum 99%:** 100 would leave
  no headroom, so external load alone could make charts' sends fail. Their
  capacity errors would then reflect other people's traffic rather than
  their own behaviour.
- **What it drives:** the balance between taking new work and finishing
  started work. Lower means more room for chart traffic, and the client
  is refused sooner.
- **How it surfaces:**
  - **host:** `admitting` in the control block, then h2c fills;
  - **client:** 503;
  - **chart:** nothing directly. A lower watermark means fewer capacity
    errors.

### `fairnessRatio`

- **What it bounds:** how many envelopes core runs from the run queue for
  each record it admits from h2c, while both have work
  ([envelope.md](reference/envelope.md#fairness)). Unit: envelopes.
- **Default 4.** Work already started goes first, which drains the pool.
  One admission in every five keeps two actors that mail each other
  forever from shutting out external input.
- **Minimum 1:** alternate one for one. **Maximum 4,096:** it is roughly
  how many macrosteps an external input may wait behind internal work.
  Past a few thousand, two chatty actors look like an outage to every
  client.
- **What it drives:** latency for external input against throughput for
  chains of internal mail. A seed may use a different value and derive a
  different order and state.
- **How it surfaces:** no error anywhere. **Host:** the gap between an
  h2c record's `at` and its `c2h.input` or first effect. **Client:**
  slower answers.

### `maxConcurrentInputs`

- **What it bounds:** how many sessions may be inside an input's execution
  at once, each in its own slot ([macrostep.md](reference/macrostep.md#macrostep-slots)).
  Unit: slots. A slot is a record of about 48 bytes, plus, while it is
  parked, its continuation and pending internal events in the park area
  (about 1 KiB for a chart of 100 states). Only the running slot uses the
  one running workspace.
- **Default 64.** Slots cost almost nothing until they park, so the
  default is generous: one long or looping macrostep holds one slot out of
  64. Most hosts never set it.
- **Minimum 1:** plain run to completion. A long macrostep holds the whole
  system until it ends, and a loop holds it until the chain limit.
  **Maximum 65,536:**
  past that, the rotation through parked slots gives each of them a turn
  too rarely to be called isolation; a host wants more systems.
- **What it drives:** isolation between sessions.
- **Filled on demand,** as the pool is: core adds a slot record, between
  macrosteps, when every existing one is busy and an envelope could run.
  A failed allocation means the envelope waits.
- **How it surfaces:** no error. **Host:** inputs that wait longer while
  slots are busy.

### `parkAreaBytes`

- **What it bounds:** the bytes of parked executions: the continuations
  and pending internal events of slots waiting for their turn
  ([macrostep.md](reference/macrostep.md#parking-one-running-workspace-and-a-park-area)).
  Unit: bytes, in 256-byte chunks.
- **Default: derived.** Core computes the most the area can ever need,
  `maxConcurrentInputs` × (the largest registered definition's
  continuation + a full internal queue), at registration, and uses that
  as the ceiling. Nobody has to choose a number, and storage stays bounded
  by parameters and compiled requirements.
- **Minimum 0:** no parking. That is plain run to completion. **Maximum: the derived ceiling;** a larger
  value changes nothing.
- **Filled on demand,** in chunks, between macrosteps only: after each
  macrostep end and at admission, core tops the free list up to one parked
  execution of the largest definition. Chunks come back to the free list
  when an execution resumes. The host gives them back to the allocator
  with `ta_system_trim` ([macrostep.md](reference/macrostep.md#parking-one-running-workspace-and-a-park-area)).
- **What it drives:** how much isolation a host buys with memory. A host
  that wants a hard memory figure sets a value below the ceiling.
- **How it surfaces:** no error. A slot that finds no free chunk at the
  end of its slice keeps the thread, which is run to completion for that
  macrostep.

### `slotSlice`

- **What it bounds:** how many steps one slot runs before core moves to
  the next slot that can run, within one `ta_run`
  ([macrostep.md](reference/macrostep.md#pauses-the-budget-and-the-slice)).
  Unit: steps: microsteps, selection passes and units of executable
  content, as ADR 0061 counts them.
- **Default 1,024.** Short macrosteps finish inside one slice and never
  rotate. A long one gives the others a turn about every thousand steps.
- **Minimum 1:** rotate after every step, for tests of interleaving.
  **Maximum 1,048,576:** past that, slots barely isolate anything.
- **What it drives:** latency for other sessions while one runs long,
  against the cost of switching slots.
- **How it surfaces:** no error anywhere.

### `internalQueueEvents`

- **What it bounds:** ordinary chart-generated internal events, and
  mandatory errors and `done.state.*` events beyond their floor. The floor
  is compiler-sized storage for one round of every phase, allocated outside
  macrosteps. A mandatory event that fits neither the floor nor a free
  ordinary entry ends the input as a runaway. Queues preserve FIFO order across both
  classes; this is a capacity distinction, not a priority distinction.
  There is one top-level workspace, the running workspace, and any
  required nested workspaces; a parked slot keeps only its pending entries,
  in the park area.
  Unit: ordinary events, about 48 bytes each.
- **Default 256 ordinary entries, about 12 KiB plus the mandatory floor.** A macrostep rarely holds more
  than a handful of internal events at once. One that holds 255 is almost
  always a chart raising in a loop. The microstep limit would catch that
  much later.
- **Minimum 0:** chart raises are refused, but mandatory errors still have
  their floor, and an overflow beyond it is a runaway. **Maximum
  65,536:** no chart needs more, and a bigger queue only delays the error
  that tells the author.
- **What it drives:** how many internal events one input may have pending
  beyond the mandatory floor. A chart that raises a lot leaves less room
  for its own mandatory events
  ([macrostep](reference/macrostep.md#sizing-mandatory-storage)).
- **How it surfaces:**
  - **chart:** `<raise>` fails as `error.execution.capacity.internal`;
  - **host:** a `tel.trace` `ACTION_FAILED`; a mandatory overflow is
    `c2h.input_completed` `runaway`, with `TA_TRACE_RUNAWAY`
    `runaway_cause` `internal_queue`.

### `h2cBytes`

- **What it bounds:** the h2c ring ([ring.md](reference/ring.md)). Unit:
  bytes.
- **Default 16,384 (16 KiB).** An `h2c.event` is 128 fixed bytes plus its
  name and origin, about 152 bytes in all, so about 107 inputs fit. Every
  `ta_run` moves h2c into the pool, so h2c only has to cover the inputs
  that arrive during one turn of the host's loop. A busy server that sees
  503s while the pool has room raises it.
- **Minimum `maxRecordBytes`,** so the largest record fits. **Maximum
  2 GiB:** a ring that big means the host is not calling `ta_run`, and no
  size fixes that.
- **What it drives:** how many inputs can wait between host turns, and how
  much input waits while core is not admitting.
- **How it surfaces:**
  - **client:** the host can't post, so it answers 503, stops reading the
    socket, or keeps the input in its own bounded buffer
    ([S09](scenarios/S09-edge-backpressure.md));
  - **chart:** nothing, except that asynchronous replies the host drops
    are covered by the chart's timeouts.

### `c2hBytes`

- **What it bounds:** the c2h ring. Unit: bytes.
- **Default: derived.** Core computes `(3 × maxConcurrentInputs + 2) ×
  maxRecordBytes` at creation. Each slot's smallest mandatory batch (input
  observation, `c2h.macrostep_ended`, `c2h.input_completed`) costs at most
  3 × `maxRecordBytes` with one pad per record
  ([ring](reference/ring.md#the-c2h-reserve)). The headroom, one ordinary
  record plus its pad, costs 2 ×. So every slot can hold its reservation
  at once. The ring is fixed at creation, before any definition exists,
  so the formula uses parameters only. With the defaults it is 794,624
  bytes (776 KiB). With 1 KiB records it is 194 KiB. A test suite that
  sets `maxConcurrentInputs` 1 gets 20 KiB without choosing a number,
  so systems created many at a time stay small.
- **An explicit value wins.** A value below the derived one means fewer
  slots hold reservations at once. A definition whose batch is larger
  (cancellations for active invocations, autoforwards) reserves more, with
  the same effect. Both cost throughput, not correctness.
- **A full ring does not fail.** A `c2h.send` is 120 fixed bytes plus
  sendid, type, target and name, about 184 bytes in all. A fanout bigger
  than the ring's free space waits while the host drains, and other slots
  go on ([macrostep](reference/macrostep.md#a-full-c2h-pauses-the-slot)).
  During budget pauses the host may execute effects and release ring
  space before the macrostep end. A bigger ring means fewer waits and
  fewer returns from `ta_run`, not fewer errors.
- **Minimum 5 × `maxRecordBytes`:** one slot's smallest batch plus the
  headroom. Definition and session setup also check their actual
  mandatory batch: input observation, cancellations, autoforwards,
  completion and end records, each with its pad, plus the headroom of
  2 × `maxRecordBytes` ([ring](reference/ring.md#the-c2h-reserve)). A
  batch that cannot fit an empty ring is refused; the factor of five alone
  is not a guarantee. **Maximum 2 GiB,** for the same reason as h2c.
- **What it drives:** how much one macrostep can say to the outside world
  before the host drains, and how many slots can hold their mandatory
  reserve at once. An input whose reserve does not fit waits in the run
  queue. Step 4.2 checks the derived size against napi's suites.
- **How it surfaces:**
  - **chart:** nothing. A slot that waits for c2h holds its input longer;
  - **host:** `ta_run` returns `TA_RUN_C2H_FULL` when nothing can go on
    until c2h has room: every slot that could run waits for c2h, or the
    reserve for the next input is not there. Drain and call again.

### `telBytes`

- **What it bounds:** the tel ring ([telemetry.md](reference/telemetry.md)).
  Unit: bytes.
- **Default 16,384 (16 KiB).** With every trace kind on, one microstep
  writes several records. A `tel.trace` record is at least 224 bytes, its
  fixed part, before its tail of names and values. 16 KiB holds at most
  about 73 between drains, enough for a quiet turn or a sampled one. tel stays lossy: a host
  that traces every kind in a busy turn raises it, or sees `tel.dropped`.
- **Minimum 0, or else `maxRecordBytes`,** so the largest record fits.
  0 means no `tel` ring at all: the kind masks must be zero, and the
  native call that sets them between `ta_run` calls refuses non-zero
  masks ([telemetry.md](reference/telemetry.md#masks-and-sampling)). Nothing
  a host needs is lost, because every outcome also reaches c2h
  ([telemetry.md](reference/telemetry.md#a-system-without-tel)).
  **Maximum 2 GiB.**
- **What it drives:** how much telemetry survives a slow exporter.
- **How it surfaces:**
  - **operator:** `tel.dropped` with the count and the first missing seq;
  - **chart and client:** nothing. Telemetry never slows the system.

### `maxRecordBytes`

- **What it bounds:** the largest record on any ring, header and tail
  included. Every admitted or locally sent event must fit its expanded
  `c2h.input` representation, even when its receiver is unrecorded. Mandatory
  output batch sizes are checked separately. Unit: bytes.
- **Default 4,096.** Event names, targets and origins in practice are well
  under 1 KiB. 4 KiB leaves room while keeping the reserve small. The
  derived `c2hBytes` scales with it, so it is never the limit.
- **Minimum 256:** the largest fixed part (`tel.trace`, 224 bytes) plus a
  short name. **Maximum `c2hBytes` / 5,** the reserve constraint.
- **What it drives:** the longest name, target or origin one record can
  carry.
- **How it surfaces:**
  - **chart:** a `<send>` whose record would be larger fails as
    `error.execution.capacity.record`. Retrying can't help, so the chart
    should give up on that send;
  - **host:** an h2c record over the limit is refused by the host's own
    writer before posting. One that gets through anyway is a protocol
    error (`c2h.bounced`, `invalid`).

### `telemetryValueBytes`

- **What it bounds:** the text kept of a value in a `tel.trace`. Unit:
  bytes.
- **Default 1,024:** enough for a small JSON value in a trace view.
- **Minimum 0:** values are left out, and records carry kinds and positions
  only. **Maximum `maxRecordBytes` − 256,** so the rest of the record
  still fits.
- **How it surfaces:** **operator:** the record carries the `truncated`
  flag.

### `maxPendingSends`

An existing core parameter whose meaning changes.

- **What it bounds:** the pending delayed sends of one session, as a
  quota inside the shared pool, so one session can't take every envelope
  with timers. Today it bounds the pending delayed sends of one actor.
  Unit: delayed sends.
- **Default: no quota** (today's `INT32_MAX`). The pool already bounds
  the total. The quota is for operators who run untrusted charts.
- **Minimum 0:** the session may not schedule delayed sends. **Maximum
  `poolEnvelopes`,** past which it means nothing.
- **How it surfaces:** **chart:** the delayed `<send>` fails as
  `error.execution.capacity.pool`, as if the pool were full for that
  session.

### Constraints between them

Creation checks these and answers `TA_ERR_INVALID`, naming the parameter
(ADR 0092):

- `maxRecordBytes` ≤ `c2hBytes` / 5;
- `maxRecordBytes` ≤ `h2cBytes`, `nameAreaBytes`, and `telBytes` unless it is 0;
- `telemetryValueBytes` ≤ `maxRecordBytes` − 256;
- `admissionWatermark` from 1 to 99;
- `microstepLimit` from 1, never 0 or none.

### Memory per system, with the defaults

| Part | Bytes |
|---|---|
| pool: 4,096 envelopes × 88, plus 4 each for the deadline index | 376,832 |
| name area: 4,096 chunks × 64 | 262,144 |
| running workspace: `interpreterBytes` 65,536 + 256 ordinary internal entries × about 48, plus the compiler-sized mandatory floor (one round of every phase), once | 77,824 plus the floor |
| slot records: up to 64 × about 48, on demand | at most 3,072 |
| park area: a reserve of one parked execution, on demand; more only while slots are parked | about 1 KiB for a chart of 100 states |
| h2c + tel: 2 × 16,384 | 32,768 |
| c2h: (3 × 64 + 2) × 4,096 | 794,624 |
| **total before the mandatory floor** | **about 1,512 KiB** |

The running workspace is run to completion's single continuation, scratch
and internal queue, which core has today. Concurrency adds the slot
records and the park area, both on demand. With 64 slots all parked, a
chart of 100 states costs about 64 KiB of continuations, plus whatever
internal events they had pending; the derived ceiling bounds the worst
case.

**The trade-off.** At their bounds, the defaults come to about 1.5 MiB,
more than the roughly 1 MB per system that ADR 0093 judged too much for
quiet systems and for test suites that create many. Most of it is c2h,
and the pool and the name area at their bounds. Two things keep the
figure a system actually pays small:

- **The pool and the name area fill on demand, up to their bounds**
  (ADR 0093's shape). They grow only between macrosteps: at admission and
  after a macrostep end. A send inside a macrostep only takes what is
  already free, so the macrostep invariant holds
  ([macrostep.md](reference/macrostep.md)). A quiet system pays for its
  peak, not for the bound.
- **The rings are fixed at creation,** because the host holds positions in
  them. h2c and tel are 16 KiB each by default, because their size is a
  throughput setting. c2h is derived from `maxConcurrentInputs` and
  `maxRecordBytes`, so every slot can hold its reserve: 776 KiB with the
  defaults, about 20 KiB with one slot. A busy server raises h2c and tel;
  a quiet one, or a test suite, lowers `maxConcurrentInputs`.

### Changed parameters

| Parameter | What changes |
|---|---|
| `microstepLimit` | It counts one input's whole chain: every microstep, every selection pass and every startup pass, from take until input completion. It must be finite: 0 and `TA_MICROSTEP_LIMIT_NONE` are refused, for the system and for a definition. At the limit core abandons the input with status `runaway`; the session lives on and takes its next input ([macrostep](reference/macrostep.md#runaways-the-chain-limit)). |
| `eventNameDepth` (default 64, at most 64) | It also sizes the scratch that resolves a run-time name's segments ([names.md](reference/names.md)). A deeper name is invalid: `c2h.bounced` (`invalid`) at admission, plain `error.execution` at a `<send>`. |
| `interpreterBytes`, `interpreterNesting` | Nested interpreter workspaces are sized and allocated outside macrosteps: at registration, and at the spawn or load of a session whose definition needs them, never on first use inside a macrostep (ADR 0102; today ADR 0092 allocates them on first use, while a cancellation runs). |
| `renderBytes` | Smaller. Names and send IDs are written into ring tails or the name area, not rendered here. |
| `maxPendingSends` | A per-session quota inside the pool ([above](#maxpendingsends)). |
| `compileMessageBytes`, `compileTextBytes`, `compileNearestBytes`, `maxActors`, `maxDefinitions` | Nothing. Compiling, spawning and loading are not macrosteps. |

### Removed parameters

| Parameter | Owner today | Replaced by |
|---|---|---|
| `eventBlockCache` (ADR 0069/0093) | core | `poolEnvelopes`. The taken event stays in its envelope until the macrostep end. |
| `valueBytes` (`TAH_PARAM_VALUE_BYTES`) | kernel | `telemetryValueBytes` |
| `traceStagingBytes` | kernel | `telBytes` for the ring. The OTLP encoder's staging is a setting of `tinyactors-otlp` itself, not a core parameter ([telemetry.md](reference/telemetry.md#otlp-tinyactors-otlp)). |
| `resumeBudget`, `actionBudget` | kernel | the budget arguments of `ta_run`, passed per call |
| `resumesPerTurn`, `dueBudget`, `messageBudget` | kernel | `ta_run`'s budget, and `fairnessRatio` for the split between mail and admitted input |
| `maxQueuedMessages`, `maxRetainedBytes` | kernel | `poolEnvelopes`, `nameAreaBytes`, `admissionWatermark`. Retained data is the data model's memory, which core does not count. |
| napi's loader `maxWaiting` (per family) | napi | the pool ([unloading.md](reference/unloading.md)) |

napi's own parameters (`registeredDatamodels`, `datamodelNameBytes`, and
the rest of `tinyactors-napi/src/napi_params.c`) are unaffected. With the
kernel gone, napi's `parameterInfo()` lists two owners, `core` and `napi`.

[h2c.event]: ../../../tinyactors-core/src/generated/MESSAGES.md#h2cevent
[h2c.load_failed]: ../../../tinyactors-core/src/generated/MESSAGES.md#h2cload_failed
[h2c.withdraw]: ../../../tinyactors-core/src/generated/MESSAGES.md#h2cwithdraw
[c2h.bounced]: ../../../tinyactors-core/src/generated/MESSAGES.md#c2hbounced
[tel.trace]: ../../../tinyactors-core/src/generated/MESSAGES.md#teltrace
[tel.dropped]: ../../../tinyactors-core/src/generated/MESSAGES.md#teldropped
[c2h.macrostep_ended]: ../../../tinyactors-core/src/generated/MESSAGES.md#c2hmacrostep_ended
