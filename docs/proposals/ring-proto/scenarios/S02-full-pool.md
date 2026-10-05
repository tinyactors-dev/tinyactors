# S02: Full pool

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

[`campaign.scxml`](../charts/campaign.scxml) fans out 10 notifications in
a pool of 8 envelopes. A tracker message finds no room, which fails as
`error.execution.capacity` and abandons the rest of the fanout. Core refuses
ordinary admission until the pool drains, and the host refuses HTTP at
its edge. Then [`campaign-windowed.scxml`](../charts/campaign-windowed.scxml)
does the same job and never comes near the bound.

**Parameters:** `poolEnvelopes` 8, `admissionWatermark` 50% (4 envelopes),
`fairnessRatio` 8, `nameAreaBytes` 1024, `h2cBytes` 4 KiB (about 40
`h2c.event` records), `c2hBytes` 64 KiB, `telBytes` 64 KiB,
`maxRecordBytes` 1024. Nothing is recorded.

**Start:** both sessions spawned, the tracker `live`, **pool 0/8**
([S01.1–S01.6](S01-fanout.md#boot)).

## The brutal chart

| Step | Ring | Record | What happens |
|---|---|---|---|
| S02.1 | h2c | [`h2c.event`][h2c.event] `0x0102` `{campaign/42, send.requested, origin: http:req/b7, data: v1 = 10 notifications}` | Admitted, since the pool is below the watermark. **pool 1/8.** |
| S02.2 | – | – | `s1.m3` starts. `attempt`'s `onentry` schedules `t` first. **pool 2/8.** |
| S02.3 | c2h | 6 × [`c2h.send`][c2h.send] `0x0203` `{x-notify, n1…n6}` | Iterations 1 to 6 each write a notify send to c2h and put a `progress.sent` envelope in the pool. **pool 8/8.** |
| S02.4 | c2h | [`c2h.send`][c2h.send] `{x-notify, n7}` | Iteration 7: the notify send fits in c2h, and the `script` adds n7 to `sent`. The `progress.sent` send finds the pool full. |
| S02.5 | tel | [`tel.trace`][tel.trace] `0x0301` `{ACTION_FAILED}`, `{RAISED error.execution.capacity.pool}` | The send fails as `error.execution.capacity.pool`. The event uses compiler-sized mandatory internal storage ([macrostep](../reference/macrostep.md#the-internal-queue-compiler-sized-mandatory-storage)); it does not compete with ordinary raises. SCXML abandons the rest of this block: iterations 8 to 10 do not run. Other blocks still execute. `sent = {n1…n7}`. `t` is safe, because it was scheduled first. |
| S02.6 | – | – | Same macrostep. `attempt`'s transition takes the error to `throttled`. `t` is untouched, because `throttled` is inside `attempt`. |
| S02.7 | tel | [`tel.trace`][tel.trace] `{ACTION_FAILED}` | `throttled`'s `onentry` tries to schedule `r` (`throttle.resume`, 2s). The pool is still full, so a second capacity error is raised. `throttled`'s own targetless transition takes it. Without that transition, the error would re-enter `throttled` and loop until the microstep limit stopped it ([chart notes](../charts/README.md#campaignscxml)). |
| S02.8 | c2h | [`c2h.macrostep_ended`][c2h.macrostep_ended] `0x0208` `{s1.m3, stable, effects: 7, mail: 7}`, [`c2h.wake`][c2h.wake] `0x0209` `{t0+30s}` | The `send.requested` envelope is freed. **pool 7/8.** |
| S02.9 | – | – | The host releases n1 to n7 to the notify processor. |
| S02.10 | – | – | **Ordinary admission is refused.** 7 is at or above the watermark of 4, so the host gates new events. Any already submitted event core encounters is consumed and bounced with `pool`, after reserving its reply. Later load and time controls can still run ([envelope](../reference/envelope.md#admission-from-h2c-into-the-pool)). |
| S02.11 | c2h | 4 × ([`c2h.send`][c2h.send] `{x-ws}`, [`c2h.macrostep_ended`][c2h.macrostep_ended] `{s2.m…}`) | The tracker takes 4 of its 6 `progress.sent`. **pool 3/8**, which is below the watermark again. |
| S02.12 | h2c | – | The host answers new clicks with `503 Retry-After: 1` while `admitting` is false, h2c is full, or its bounded outstanding table is full. It settles bounced submissions by sequence. The browser holds the retry ([S09](S09-edge-backpressure.md)). Processor results may wait only in the host's bounded buffer. |
| S02.13 | – | – | Admission resumes. Core runs up to `fairnessRatio` envelopes from the pool for each record it admits from h2c, so the 2 remaining tracker envelopes run first ([fairness](../reference/envelope.md#fairness)). |
| S02.14 | – | – | s1 takes `notify.ack` n1 (`s1.m4`): `pending` loses n1, `progress.acked` goes in the pool, and `outcome.received` is raised. |
| S02.15 | c2h | 3 × [`c2h.send`][c2h.send] `{x-notify, n8, n9, n10}`, [`c2h.macrostep_ended`][c2h.macrostep_ended] `{s1.m4, effects: 3, mail: 4}` | Same macrostep, and its only macrostep end. `outcome.received` takes `throttled` → `sending`. Its `onentry` computes `todo` = pending minus sent = `[n8, n9, n10]`, and the fanout finishes. **pool 6/8**, so it fits. |
| S02.16 | – | – | The remaining acknowledgements arrive, `pending` empties, and `complete` replies to `http:req/b7` ([S01.24](S01-fanout.md#the-retry-and-the-reply)). |

The capacity error cost a few milliseconds and no notification. The
fanout resumed as soon as the system had drained.

## The windowed chart, with the same parameters

| Step | Ring | Record | What happens |
|---|---|---|---|
| S02.17 | h2c | [`h2c.event`][h2c.event] `{campaign/42, send.requested, data: 10 notifications}` | **pool 1/8.** |
| S02.18 | c2h | 4 × [`c2h.send`][c2h.send] `{x-notify, n1…n4}`, [`c2h.macrostep_ended`][c2h.macrostep_ended] `{s1.m3, effects: 4, mail: 2}` | `attempt`'s `onentry`: `t`, a window of 4 notifications, and one `progress.snapshot`. **pool 3/8** during the step, **2/8** after the macrostep end. |
| S02.19 | c2h | [`c2h.send`][c2h.send] `{x-notify, n5}`, [`c2h.macrostep_ended`][c2h.macrostep_ended] `{mail: 1}` | Each `notify.ack` raises `window.slot`, which sends one more notification and one snapshot. **pool ≤ 3/8** at every step. |
| S02.20 | – | – | Admission is never refused, because the pool stays below 4. c2h receives at most 4 send records per macrostep, plus mandatory end/completion/wake reports. With 10 million notifications these bounds are the same. |

## Who drops what, who retries how

| What failed | Dropped or refused by | Told how | Retried by |
|---|---|---|---|
| `progress.sent` n7, with iterations 8–10 abandoned | core: the pool is full | `error.execution.capacity.pool`, in the sender's own macrostep (S02.5) | the chart: `throttled` resumes on `outcome.received` (S02.15) |
| `throttle.resume` | core: the pool is full | a second capacity error, swallowed (S02.7) | the chart: the next acknowledgement, or `t` |
| New clicks while h2c is full | the host, at its edge | HTTP 503 (S02.12) | the browser |
| Nothing, in the windowed chart | – | – | – |

[h2c.time]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#h2ctime
[h2c.event]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#h2cevent
[h2c.spawn]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#h2cspawn
[h2c.publish]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#h2cpublish
[h2c.load]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#h2cload
[h2c.load_failed]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#h2cload_failed
[h2c.unload]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#h2cunload
[h2c.checkpoint]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#h2ccheckpoint
[h2c.invoke_report]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#h2cinvoke_report
[h2c.destroy]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#h2cdestroy
[c2h.spawned]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hspawned
[c2h.input]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hinput
[c2h.send]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hsend
[c2h.invoke]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hinvoke
[c2h.invoke_cancel]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hinvoke_cancel
[c2h.invoke_forward]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hinvoke_forward
[c2h.log]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hlog
[c2h.macrostep_ended]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hmacrostep_ended
[c2h.wake]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hwake
[c2h.bounced]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hbounced
[c2h.load_needed]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hload_needed
[c2h.loaded]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hloaded
[c2h.unloaded]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hunloaded
[c2h.state]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hstate
[c2h.published]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hpublished
[c2h.finished]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hfinished
[c2h.destroyed]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hdestroyed
[tel.trace]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#teltrace
[tel.dropped]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#teldropped
