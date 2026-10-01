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
| An action's record does not fit in c2h | core, in the sender's macrostep | `error.execution.capacity` | the chart | [envelope](reference/envelope.md#the-internal-queue-is-not-in-the-pool) |
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
| **c2h** | `c2hBytes`, `maxRecordBytes` | An action whose record does not fit fails as `error.execution.capacity`. A macrostep starts only when c2h has room for its reserve. A run never waits. | the sending chart | [ring](reference/ring.md), [macrostep](reference/macrostep.md) |
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

| | **Tracker not recorded** | **Tracker recorded** |
|---|---|---|
| **Campaign not recorded** | Both start fresh. The user's next click runs the whole campaign again, and the processor deduplicates n1–n3. Correct. [S03](scenarios/S03-crash-unrecorded.md) | The tracker replays its old view of a campaign that no longer exists. Pointless: never record a derived actor without its source. |
| **Campaign recorded** | The campaign is replayed, and `t` survives in the pool. The fresh tracker shows zero until its bootstrap resync, and is then correct. A click or `t` sends n5 again. [S04](scenarios/S04-crash-campaign-recorded.md) | Both are replayed. **The two in-flight deltas are lost by both sides**, and the replayed tracker is `live`, so it does not resync until told to. Worse than not recording it. [S05](scenarios/S05-crash-both-recorded.md#per-session-replay) |
| **Whole system recorded** (the h2c stream) | – | Everything in core is rebuilt, including the in-flight deltas. Only effects that left the process are suppressed. [S05](scenarios/S05-crash-both-recorded.md#whole-system-replay) |

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

[h2c.event]: protocol/generated/MESSAGES.md#h2cevent
[h2c.load_failed]: protocol/generated/MESSAGES.md#h2cload_failed
[c2h.bounced]: protocol/generated/MESSAGES.md#c2hbounced
[tel.trace]: protocol/generated/MESSAGES.md#teltrace
[tel.dropped]: protocol/generated/MESSAGES.md#teldropped
