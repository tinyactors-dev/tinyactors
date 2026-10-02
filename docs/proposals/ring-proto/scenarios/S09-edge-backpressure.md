# S09: Edge backpressure

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

A big fanout in another campaign keeps the pool above its watermark, so
core stops reading h2c. h2c fills up. The host refuses new clicks with
HTTP 503, and drops a notify acknowledgement it has no room for. The
campaign's timeout covers the lost acknowledgement, and the processor's
key keeps the retry from sending the notification twice.

**Parameters:** `poolEnvelopes` 256, `admissionWatermark` 75% (192 envelopes),
`fairnessRatio` 8, `nameAreaBytes` 8 KiB, **`h2cBytes` 2 KiB** (about 20
`h2c.event` records), `c2hBytes` 64 KiB, `telBytes` 64 KiB,
`maxRecordBytes` 1024. Nothing is recorded. The host keeps processor
results it cannot post in a buffer of at most 32. It drops the oldest
when that buffer is full.

## Filling up

| Step | Ring | Record | What happens |
|---|---|---|---|
| S09.1 | – | – | `campaign/42` is in `attempt`/`sending` with `pending = {n1…n5}`. All five went out at t0, and `t` is due at t0+30s. |
| S09.2 | h2c | [`h2c.event`][h2c.event] `0x0102` `{campaign/7, send.requested, 400 notifications}` | campaign/7 runs the brutal chart. Its fanout fills the pool to 256, takes a capacity error and parks in `throttled` ([S02.5](S02-full-pool.md#the-brutal-chart)). **pool 256/256.** |
| S09.3 | – | – | The pool is above the watermark, so **core reads nothing from h2c** until the tracker of campaign/7 has drained the pool below 192 ([envelope](../reference/envelope.md#admission-from-h2c-into-the-pool)). |
| S09.4 | h2c | 20 × [`h2c.event`][h2c.event] | Clicks and processor results pile up. h2c is full. |
| S09.5 | – | – | The host fails to post a new HTTP click and answers **`503 Retry-After: 1`**. The browser holds the retry. Nothing about it is queued in the process. |
| S09.6 | – | – | The processor reports `notify.ack` for campaign/42's n2. The host can't post it, so it goes into the host's buffer. That buffer fills during a long stall, and the oldest entry, **`notify.ack n2`, is dropped**. The host may do this ([BEHAVIORS](../BEHAVIORS.md#who-drops-what)): an asynchronous reply has nobody to retry it except the chart that waits for it. |
| S09.7 | – | – | The host's wake timer fires, but h2c is full. The host keeps the latest time and posts [`h2c.time`][h2c.time] `0x0101` as soon as there is room. A delayed send then fires late. It is never lost. |

## Draining

| Step | Ring | Record | What happens |
|---|---|---|---|
| S09.8 | c2h | many × ([`c2h.send`][c2h.send] `{x-ws}`, [`c2h.macrostep_ended`][c2h.macrostep_ended]) | campaign/7's tracker drains the pool below 192. Admission resumes, and core reads h2c again. |
| S09.9 | h2c | [`h2c.event`][h2c.event] `{notify.ack n1, n3, n4, n5}` | Four of the five acknowledgements reach campaign/42. `pending = {n2}`. |
| S09.10 | h2c | [`h2c.time`][h2c.time] `{t0+30s}` | `t` fires: `retrying` → attempt 2 → `sending`, with `todo = [n2]`. |
| S09.11 | c2h | [`c2h.send`][c2h.send] `{x-notify, n2}` | |
| S09.12 | – | – | The processor has the key `(recipient, n2)`, so it **acknowledges without sending again**. |
| S09.13 | h2c | [`h2c.event`][h2c.event] `{notify.ack n2}` | `pending` is empty, and the campaign is `complete`. |

## What did not happen

- **No queue hid the overflow.** h2c's bytes, the pool and the host's
  buffer of 32 are all fixed. Each overflow became a refusal to someone
  who retries: the browser (S09.5) or the chart (S09.10).
- **No input was half-admitted.** Core took records from h2c only when it
  had room to run them.
- **The stall in campaign/7 did not stop campaign/42 permanently.** Run
  to completion and the fairness ratio share the system once admission
  resumes ([macrostep](../reference/macrostep.md)).

## Who drops what, who retries how

| What was lost | Dropped by | Told how | Retried by |
|---|---|---|---|
| New clicks | the host, at its edge (S09.5) | HTTP 503 | the browser |
| `notify.ack n2` | the host's bounded buffer (S09.6) | nobody | the chart's timeout (S09.10); the processor deduplicates (S09.12) |
| `h2c.time` | delayed, not dropped (S09.7) | – | the host, when h2c has room |

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
