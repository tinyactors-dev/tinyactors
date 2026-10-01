# Behaviors at a glance

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](README.md)

This page summarizes the [scenarios](scenarios/README.md). Every bound in
the system is fixed, so every overflow has to become **a refusal to
someone who can retry**. Nothing queues it out of sight. Three parties
can retry:

| Who retries | How they are told | What they do |
|---|---|---|
| **The external client** | the host refuses at its edge: h2c is full, so HTTP 503, or it stops reading the socket | retries, or the user clicks again |
| **The sending chart** | its own action fails in its own macrostep: `error.execution.capacity.*`, or `error.communication` for mail that could not be delivered | throttles, retries on a timer, or gives up: its structure decides |
| **Nobody** (an asynchronous reply) | it is dropped | the chart that waits for it has a timeout. That is what the timeout is for. |

## Who drops what

| Situation | Dropped or refused by | Told how | Retried by | Steps |
|---|---|---|---|---|
| The notify processor rejects a notification | the processor | `notify.rejected` as an [`h2c.event`][h2c.event] | nobody: final | [S01.16](scenarios/S01-fanout.md#outcomes) |
| A notification cannot reach its relay | the processor | `error.communication` as an [`h2c.event`][h2c.event] | the chart, on its timeout | [S01.17](scenarios/S01-fanout.md#outcomes), [S01.22](scenarios/S01-fanout.md#the-retry-and-the-reply) |
| A send finds the pool full | core, in the sender's macrostep | `error.execution.capacity.pool`; the rest of the block is abandoned | the chart: `throttled` resumes on the next outcome | [S02.5](scenarios/S02-full-pool.md#the-brutal-chart), [S02.15](scenarios/S02-full-pool.md#the-brutal-chart) |
| An action's record does not fit in c2h | core, in the sender's macrostep | `error.execution.capacity.c2h` (`.record` if it could never fit) | the chart | [ring](reference/ring.md#the-c2h-reserve) |
| A computed name does not fit in the name area | core, in the sender's macrostep | `error.execution.capacity.names` | the chart | [S10](scenarios/S10-run-time-names.md#names-a-chart-computes) |
| The pool or the name area is past its watermark | core stops reading h2c | nobody: h2c fills | – | [S02.10](scenarios/S02-full-pool.md#the-brutal-chart), [S10.7](scenarios/S10-run-time-names.md#when-the-names-come-faster-than-they-are-freed) |
| h2c is full and a client posts | the host, at its edge | HTTP 503 | the client | [S02.12](scenarios/S02-full-pool.md#the-brutal-chart), [S09.5](scenarios/S09-edge-backpressure.md#filling-up) |
| h2c is full and a processor reports a result | the host's bounded buffer (its policy) | nobody | the waiting chart's timeout; the processor deduplicates | [S09.6](scenarios/S09-edge-backpressure.md#filling-up), [S09.10](scenarios/S09-edge-backpressure.md#draining) |
| h2c is full and the wake timer fires | nothing is dropped: `h2c.time` is posted late | – | the host, when there is room | [S09.7](scenarios/S09-edge-backpressure.md#filling-up) |
| Mail waits for a session that cannot be loaded | core, on [`h2c.load_failed`][h2c.load_failed] | `error.communication` to the sender, in the same envelope | the sending chart, if it cares | [S07.12](scenarios/S07-unloaded-tracker.md#the-load-fails) |
| An external event waits for a session that cannot be loaded | core | [`tel.trace`][tel.trace] `DEAD_LETTER` | the client | [S07.13](scenarios/S07-unloaded-tracker.md#the-load-fails) |
| An event nobody listens for | the chart: it enables nothing | [`tel.trace`][tel.trace] `IGNORED` | nobody | [S10.4](scenarios/S10-run-time-names.md#under-the-proposal) |
| An h2c record core cannot run (no such address, too deep a name, …) | core | [`c2h.bounced`][c2h.bounced] with a reason | the host decides | [S10.8](scenarios/S10-run-time-names.md#when-the-names-come-faster-than-they-are-freed) |
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
| **c2h** | `c2hBytes`, `maxRecordBytes` | An action whose record does not fit fails as `error.execution.capacity.c2h`. A macrostep starts only when c2h has room for its reserve. A run never waits. | the sending chart | [ring](reference/ring.md), [macrostep](reference/macrostep.md) |
| **tel** | `telBytes` | The record is dropped and counted in [`tel.dropped`][tel.dropped]. | the operator | [telemetry](reference/telemetry.md) |
| **Pool** | `poolEnvelopes`, `admissionWatermark` | Past the watermark, core stops reading h2c, which keeps headroom for mail. When the pool is full, a send fails as `error.execution.capacity.pool`. | the external client (h2c fills) and the sending chart | [S02](scenarios/S02-full-pool.md) |
| **Name area** | `nameAreaBytes` | The same as the pool: admission stops at the watermark, and a computed name that does not fit fails as `error.execution.capacity.names`. | the external client and the sending chart | [S10](scenarios/S10-run-time-names.md) |
| **Internal queue** | `internalQueueEvents`, one fixed queue per running interpreter, with one reserved slot | A `<raise>` that does not fit fails as `error.execution.capacity.internal`. The reserved slot always holds the error itself. | the chart | [macrostep](reference/macrostep.md) |
| **Data model** (the JavaScript heap) | the binding | An operation that fails reports `TA_OP_ERROR`, and core raises `error.execution`. It is the data model's own memory, never core's. | the chart | [data model](reference/data-model.md) |
| **The host's own buffers** | the host's policy | The host drops something, and must accept that only a timeout will notice. | the waiting chart | [S09.6](scenarios/S09-edge-backpressure.md#filling-up) |

## Crashes

One crash, partway through the fanout ([S03](scenarios/S03-crash-unrecorded.md#at-the-crash)):
the campaign has n5 pending, two tracker deltas are in flight, and `t`
is due later.

Replay uses selected journal entries to seed new configuration and data
model values. The examples below show one outcome with the illustrated
definitions and resource conditions, not a promise to restore the old run.
The host may execute effects on receipt, before a fence or journal write;
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
3. **Keep optional traffic out of essential blocks,** or clock the
   fanout by acknowledgements ([windowed chart](charts/campaign-windowed.scxml)).
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
`interpreterBytes`. The ADR that adopts them records them.

| Parameter | Default | Minimum | Maximum | When it runs out, who is told |
|---|---|---|---|---|
| [`poolEnvelopes`](#poolenvelopes) | 4,096 | 8 | 16,777,216 | the chart (`.pool`); the client, through h2c |
| [`nameAreaBytes`](#nameareabytes) | 262,144 | `maxRecordBytes` | 1 GiB | the chart (`.names`); the client, through h2c |
| [`admissionWatermark`](#admissionwatermark) | 50% | 1% | 99% | the client, through h2c |
| [`fairnessRatio`](#fairnessratio) | 4 | 1 | 4,096 | nobody: it shapes latency |
| [`internalQueueEvents`](#internalqueueevents) | 256 | 2 | 65,536 | the chart (`.internal`) |
| [`h2cBytes`](#h2cbytes) | 65,536 | `maxRecordBytes` | 2 GiB | the client (the host refuses) |
| [`c2hBytes`](#c2hbytes) | 262,144 | 4 × `maxRecordBytes` | 2 GiB | the chart (`.c2h`); the host (`TA_RUN_C2H_FULL`) |
| [`telBytes`](#telbytes) | 262,144 | `maxRecordBytes` | 2 GiB | the operator (`tel.dropped`) |
| [`maxRecordBytes`](#maxrecordbytes) | 4,096 | 256 | `c2hBytes` / 4 | the chart (`.record`); the host (`c2h.bounced`) |
| [`telemetryValueBytes`](#telemetryvaluebytes) | 1,024 | 0 | `maxRecordBytes` − 256 | the operator (`truncated`) |
| [`maxPendingSends`](#maxpendingsends) (changed) | no quota | 0 | `poolEnvelopes` | the chart (`.pool`) |

### `poolEnvelopes`

- **What it bounds:** the envelopes in the pool. That covers admitted
  external input, mail between local sessions, delayed sends of resident
  sessions, and envelopes waiting for a load
  ([envelope.md](reference/envelope.md)). Unit: envelopes, about 72 bytes
  each plus 4 for the deadline index.
- **Default 4,096, about 304 KiB.** napi resumes up to 1,024 actors per turn
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
  when admission stops.
- **How it surfaces:**
  - **chart:** a `<send>` to a local session, a delayed `<send>`, or an
    `<invoke>` of a local child that finds no envelope fails as
    `error.execution.capacity.pool` ([S02](scenarios/S02-full-pool.md));
  - **host:** `pool.free` and `pool.high_water` in the control block, and
    `admitting` turns false;
  - **client:** h2c fills, and the host answers 503.

### `nameAreaBytes`

- **What it bounds:** the bytes for text that has no ID: event names,
  origins, unresolved addresses, invocation error reasons
  ([names.md](reference/names.md#the-name-area)). It is made of 64-byte
  chunks, 60 of them text.
- **Default 262,144 (256 KiB), one chunk per envelope at the default pool.**
  Every envelope carries at least its event name, and most fit in one
  chunk. An area smaller than `poolEnvelopes` × 64 would run out before the
  pool does, and charts would see `.names` where `.pool` is the real limit.
  Raise it when external input carries long origins or names.
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

### `internalQueueEvents`

- **What it bounds:** the entries of the fixed internal queue: `<raise>`,
  `done.state.*`, `error.*`. There is one queue for the top-level
  interpreter and one per nested interpreter, up to `interpreterNesting`.
  One entry is always kept free for `error.execution.capacity`
  ([macrostep.md](reference/macrostep.md#the-internal-queue-fixed-with-one-reserved-slot)).
  Unit: events, about 48 bytes each.
- **Default 256, about 12 KiB per queue.** A macrostep rarely holds more
  than a handful of internal events at once. One that holds 255 is almost
  always a chart raising in a loop. The microstep limit would catch that
  much later.
- **Minimum 2:** one usable entry plus the reserved one. **Maximum
  65,536:** no chart needs more, and a bigger queue only delays the error
  that tells the author.
- **What it drives:** how many internal events one macrostep may have
  pending.
- **How it surfaces:**
  - **chart:** `<raise>` fails as `error.execution.capacity.internal`;
  - **host:** a `tel.trace` `ACTION_FAILED`.

### `h2cBytes`

- **What it bounds:** the h2c ring ([ring.md](reference/ring.md)). Unit:
  bytes.
- **Default 65,536 (64 KiB).** An `h2c.event` is 88 fixed bytes plus its
  name and origin, about 128 bytes in all, so about 500 inputs fit. Every
  `ta_run` moves h2c into the pool, so h2c only has to cover the inputs
  that arrive during one turn of the host's loop.
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
- **Default 262,144 (256 KiB).** A `c2h.send` is 88 fixed bytes plus
  sendid, type, target and name, about 150 bytes in all. That allows about
  1,700 sends between drains. During budget pauses the host may execute
  effects and release ring space before the fence. A fanout of 1,000 fits
  as it is. Bigger fanouts
  should be clocked by acknowledgements
  ([windowed chart](charts/campaign-windowed.scxml)).
- **Minimum 4 × `maxRecordBytes`,** so the c2h reserve (input, fence and
  one record of the largest size) never takes more than half the ring.
  **Maximum 2 GiB,** for the same reason as h2c.
- **What it drives:** how much one macrostep can say to the outside world
  before the host drains.
- **How it surfaces:**
  - **chart:** an action whose record doesn't fit fails as
    `error.execution.capacity.c2h`;
  - **host:** `ta_run` returns `TA_RUN_C2H_FULL` when the reserve is not
    there before a macrostep. Drain and call again.

### `telBytes`

- **What it bounds:** the tel ring ([telemetry.md](reference/telemetry.md)).
  Unit: bytes.
- **Default 262,144 (256 KiB).** With every trace kind on, one microstep
  writes several records of about 170 bytes. 256 KiB holds about 1,500
  between drains, enough for a busy turn with sampling.
- **Minimum `maxRecordBytes`,** so the largest record fits. To turn tracing
  off, set the kind masks to zero; the ring stays. **Maximum 2 GiB.**
- **What it drives:** how much telemetry survives a slow exporter.
- **How it surfaces:**
  - **operator:** `tel.dropped` with the count and the first missing seq;
  - **chart and client:** nothing. Telemetry never slows the system.

### `maxRecordBytes`

- **What it bounds:** the largest record on any ring, header and tail
  included. It is part of the c2h reserve. Unit: bytes.
- **Default 4,096.** Event names, targets and origins in practice are well
  under 1 KiB. 4 KiB leaves room while keeping the reserve small.
- **Minimum 256:** the largest fixed part (`tel.trace`, 120 bytes) plus a
  short name. **Maximum `c2hBytes` / 4,** the reserve constraint.
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

- **What it bounds:** today, the pending delayed sends of one actor. Now
  it is a per-session quota inside the shared pool, so one session can't
  take every envelope with timers. Unit: delayed sends.
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

- `maxRecordBytes` ≤ `c2hBytes` / 4;
- `maxRecordBytes` ≤ `h2cBytes`, `telBytes`, `nameAreaBytes`;
- `telemetryValueBytes` ≤ `maxRecordBytes` − 256;
- `admissionWatermark` from 1 to 99.

### Memory per system, with the defaults

| Part | Bytes |
|---|---|
| pool: 4,096 envelopes × 72, plus 4 each for the deadline index | 311,296 |
| name area: 4,096 chunks × 64 | 262,144 |
| internal queue, top level: 256 × about 48 (nested interpreters add the same each, when they start) | 12,288 |
| h2c + c2h + tel | 589,824 |
| **total** | **about 1.1 MiB** |

That is more than napi's roughly 1 MB per system today, which ADR 0093
judged too much for quiet systems and test suites that create many. So:

- **The pool and the name area fill on demand, up to their bounds**
  (ADR 0093's shape). They grow only between macrosteps: at admission and
  after a fence. A send inside a macrostep only takes what is already free,
  so the macrostep invariant holds ([macrostep.md](reference/macrostep.md)).
  A quiet system pays for its peak, not for the bound.
- **The rings are fixed at creation,** because the host holds positions in
  them. Hosts that are not busy, such as test suites, create systems with
  16 KiB rings.

### Changed parameters

| Parameter | What changes |
|---|---|
| `microstepLimit` | Nothing. A runaway macrostep ends with [`c2h.fence`][c2h.fence] status `runaway`. |
| `eventNameDepth` (default 64, at most 64) | It also sizes the scratch that resolves a run-time name's segments ([names.md](reference/names.md)). A deeper name is invalid: `c2h.bounced` (`invalid`) at admission, plain `error.execution` at a `<send>`. |
| `interpreterBytes`, `interpreterNesting` | Reserved at creation rather than on first use, if nested interpreters can start inside a macrostep (cancelling an invocation). **To check:** that this is the only path. |
| `renderBytes` | Smaller. Names and send IDs are written into ring tails or the name area, not rendered here. |
| `maxPendingSends` | A per-session quota inside the pool ([above](#maxpendingsends)). |
| `compileMessageBytes`, `compileTextBytes`, `compileNearestBytes`, `maxActors`, `maxDefinitions` | Nothing. Compiling, spawning and loading are not macrosteps. |

### Removed parameters

| Parameter | Owner today | Replaced by |
|---|---|---|
| `eventBlockCache` (ADR 0069/0093) | core | `poolEnvelopes`. The taken event stays in its envelope until the fence. |
| `valueBytes` (`TAH_PARAM_VALUE_BYTES`) | kernel | `telemetryValueBytes` |
| `traceStagingBytes` | kernel | `telBytes`. The OTLP encoder's staging becomes a setting of that library ([telemetry.md](reference/telemetry.md)). |
| `resumeBudget`, `actionBudget` | kernel | the budget arguments of `ta_run`, passed per call |
| `resumesPerTurn`, `dueBudget`, `messageBudget` | kernel | `ta_run`'s budget, and `fairnessRatio` for the split between mail and admitted input |
| `maxQueuedMessages`, `maxRetainedBytes` | kernel | `poolEnvelopes`, `nameAreaBytes`, `admissionWatermark`. Retained data is the data model's memory, which core does not count. |
| napi's loader `maxWaiting` (per family) | napi | the pool ([unloading.md](reference/unloading.md)) |

napi's own parameters (`registeredDatamodels`, `datamodelNameBytes`, and
the rest of `tinyactors-napi/src/napi_params.c`) are unaffected. With the
kernel gone, napi's `parameterInfo()` lists two owners, `core` and `napi`.

[h2c.event]: protocol/generated/MESSAGES.md#h2cevent
[h2c.load_failed]: protocol/generated/MESSAGES.md#h2cload_failed
[c2h.bounced]: protocol/generated/MESSAGES.md#c2hbounced
[tel.trace]: protocol/generated/MESSAGES.md#teltrace
[tel.dropped]: protocol/generated/MESSAGES.md#teldropped
[c2h.fence]: protocol/generated/MESSAGES.md#c2hfence
