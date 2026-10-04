# S09: Edge backpressure

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

A big fanout in another campaign keeps the pool above its watermark, so
core stops admitting ordinary events. Already-posted events that cannot
fit are consumed and bounced, so load answers and time updates can progress.
The host refuses new clicks with
HTTP 503, and drops a notify acknowledgement it has no room for. The
campaign's timeout covers the lost acknowledgement, and the processor's
key keeps the retry from sending the notification twice.

**Parameters:** `poolEnvelopes` 256, `admissionWatermark` 75% (192 envelopes),
`fairnessRatio` 8, `nameAreaBytes` 8 KiB, **`h2cBytes` 2 KiB** (about 20
`h2c.event` records), `c2hBytes` 64 KiB, `telBytes` 64 KiB,
`maxRecordBytes` 1024. Nothing is recorded. The host keeps processor
results it cannot post in a buffer of at most 32. It drops the oldest
when that buffer is full. Its separate submission table retains at most
32 posted requests, including original metadata and an owned data reference,
until `c2h.bounced`, `c2h.input_refused` or `c2h.input_completed` names
their `source_seq`. No input is accepted without a free tracking slot.

## Filling up

| Step | Ring | Record | What happens |
|---|---|---|---|
| S09.1 | – | – | `campaign/42` is in `attempt`/`sending` with `pending = {n1…n5}`. All five went out at t0, and `t` is due at t0+30s. |
| S09.2 | h2c | Four acknowledgements for campaign/42, then [`h2c.event`][h2c.event] `{campaign/7, send.requested, 400 notifications}` | This trace completes n1, n3, n4 and n5 first, leaving `pending = {n2}`. Campaign/7 then runs the brutal chart. Its fanout fills the pool, takes `error.execution.capacity.pool` and parks in `throttled` ([S02.5](S02-full-pool.md#the-brutal-chart)). |
| S09.3 | – | – | The pool is above the watermark, so `admitting` is false. The host stops posting ordinary events. This is a hint, not a reservation for events posted before the fanout ([envelope](../reference/envelope.md#admission-from-h2c-into-the-pool)). |
| S09.4 | h2c, c2h | Already-posted [`h2c.event`][h2c.event] records; [`c2h.bounced`][c2h.bounced] `{source_seq, reason: pool}` | Core reserves each bounce, consumes the refused input and releases its transferred vrefs. The host uses its bounded retained original to reject or retry it. If c2h fills, core yields without consuming the next input; draining c2h permits progress. |
| S09.5 | – | – | With `admitting` false, a full h2c or no submission slot, the host answers a new HTTP click with **`503 Retry-After: 1`**. The browser holds the retry. Nothing about it is queued in the process. |
| S09.6 | – | – | The processor reports `notify.ack` for campaign/42's n2. The host can't post it, so it goes into the host's buffer. That buffer fills during a long stall, and the oldest entry, **`notify.ack n2`, is dropped**. The host may do this ([BEHAVIORS](../BEHAVIORS.md#who-drops-what)): an asynchronous reply has nobody to retry it except the chart that waits for it. The host keeps draining c2h meanwhile: a host that stopped draining until h2c had room would wait for core while core waited for it ([macrostep](../reference/macrostep.md#a-full-c2h-pauses-the-slot)). |
| S09.7 | h2c | [`h2c.time`][h2c.time] `{t0+30s}` | The host retains the latest wake time and prioritizes it over new ordinary events. Refused events ahead of it bounce. Core reads the clock update even while the pool remains above the watermark. A timer needs no new envelope to become runnable. |

## Draining

| Step | Ring | Record | What happens |
|---|---|---|---|
| S09.8 | c2h | many × ([`c2h.send`][c2h.send] `{x-ws}`, [`c2h.macrostep_ended`][c2h.macrostep_ended]) | campaign/7's tracker drains the pool below 192. Admission resumes. The timeout made runnable in S09.7 can run during this drain. |
| S09.9 | host | – | The host retries retained processor acknowledgements as capacity permits. This trace chooses a schedule where n1, n3, n4 and n5 completed before S09.7; only n2 is missing when the timeout is taken. |
| S09.10 | – | – | `t` runs: `retrying` → attempt 2 → `sending`, with `todo = [n2]`. It uses the system clock at take. |
| S09.11 | c2h | [`c2h.send`][c2h.send] `{x-notify, n2}` | |
| S09.12 | – | – | The processor has the key `(recipient, n2)`, so it **acknowledges without sending again**. |
| S09.13 | h2c | [`h2c.event`][h2c.event] `{notify.ack n2}` | `pending` is empty, and the campaign is `complete`. |

## What did not happen

- **No queue hid the overflow.** h2c's bytes, the pool and the host's
  buffer of 32 are all fixed. Each overflow became a refusal to someone
  who retries: the browser (S09.5) or the chart (S09.10).
- **No input was half-admitted.** Core took records from h2c only when it
  had reserved the resources for admission or the complete refusal record.
- **The stall in campaign/7 did not stop campaign/42 permanently.** Run
  to completion and the fairness ratio share runnable work; capacity
  refusals let control commands progress even before admission resumes
  ([macrostep](../reference/macrostep.md)).

## No runnable work is required for control progress

Fill the admission allowance with waiting mail and future timers, leaving
the run queue empty. Post an event followed by `h2c.load_failed` for one
waiting address and `h2c.time`. Core consumes and bounces the event, then
processes the matching load failure and clock update. Recycling waiting
mail and dispatching timers produces runnable work without allocating an
overflow queue. A full c2h only requires the host to drain and resume.

Also test an input below the envelope watermark whose complete text cannot
fit the name area. Its `names` bounce must permit the same control progress;
checking the watermark alone does not establish that a particular record fits.

## Who drops what, who retries how

| What was lost | Dropped by | Told how | Retried by |
|---|---|---|---|
| New clicks | the host, at its edge (S09.5) | HTTP 503 | the browser |
| `notify.ack n2` | the host's bounded buffer (S09.6) | nobody | the chart's timeout (S09.10); the processor deduplicates (S09.12) |
| `h2c.time` | delayed, not dropped (S09.7) | – | the host, with priority when h2c has room |

[h2c.time]: ../protocol/generated/MESSAGES.md#h2ctime
[h2c.event]: ../protocol/generated/MESSAGES.md#h2cevent
[h2c.spawn]: ../protocol/generated/MESSAGES.md#h2cspawn
[h2c.publish]: ../protocol/generated/MESSAGES.md#h2cpublish
[h2c.load]: ../protocol/generated/MESSAGES.md#h2cload
[h2c.load_failed]: ../protocol/generated/MESSAGES.md#h2cload_failed
[h2c.unload]: ../protocol/generated/MESSAGES.md#h2cunload
[h2c.checkpoint]: ../protocol/generated/MESSAGES.md#h2ccheckpoint
[h2c.invoke_report]: ../protocol/generated/MESSAGES.md#h2cinvoke_report
[h2c.destroy]: ../protocol/generated/MESSAGES.md#h2cdestroy
[c2h.spawned]: ../protocol/generated/MESSAGES.md#c2hspawned
[c2h.input]: ../protocol/generated/MESSAGES.md#c2hinput
[c2h.send]: ../protocol/generated/MESSAGES.md#c2hsend
[c2h.invoke]: ../protocol/generated/MESSAGES.md#c2hinvoke
[c2h.invoke_cancel]: ../protocol/generated/MESSAGES.md#c2hinvoke_cancel
[c2h.invoke_forward]: ../protocol/generated/MESSAGES.md#c2hinvoke_forward
[c2h.log]: ../protocol/generated/MESSAGES.md#c2hlog
[c2h.macrostep_ended]: ../protocol/generated/MESSAGES.md#c2hmacrostep_ended
[c2h.wake]: ../protocol/generated/MESSAGES.md#c2hwake
[c2h.bounced]: ../protocol/generated/MESSAGES.md#c2hbounced
[c2h.load_needed]: ../protocol/generated/MESSAGES.md#c2hload_needed
[c2h.loaded]: ../protocol/generated/MESSAGES.md#c2hloaded
[c2h.unloaded]: ../protocol/generated/MESSAGES.md#c2hunloaded
[c2h.state]: ../protocol/generated/MESSAGES.md#c2hstate
[c2h.published]: ../protocol/generated/MESSAGES.md#c2hpublished
[c2h.finished]: ../protocol/generated/MESSAGES.md#c2hfinished
[c2h.destroyed]: ../protocol/generated/MESSAGES.md#c2hdestroyed
[tel.trace]: ../protocol/generated/MESSAGES.md#teltrace
[tel.dropped]: ../protocol/generated/MESSAGES.md#teldropped
