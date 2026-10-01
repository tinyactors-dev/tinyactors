# S01: Fanout

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

One click sends five notifications through
[`campaign.scxml`](../charts/campaign.scxml), with
[`progress.scxml`](../charts/progress.scxml) watching. Three are
acknowledged, one is rejected and one never reaches its relay. The relay
is sent again when the attempt times out, and the click is answered.

**Parameters:** `poolEnvelopes` 64, `nameAreaBytes` 4096,
`admissionWatermark` 75% (48 envelopes), `fairnessRatio` 8, `h2cBytes` 64 KiB, `c2hBytes`
64 KiB, `telBytes` 256 KiB, `maxRecordBytes` 4096. Nothing is recorded.

Nothing here comes near a bound. [S02](S02-full-pool.md) runs the same
chart into one.

## Boot

| Step | Ring | Record | What happens |
|---|---|---|---|
| S01.1 | h2c | [`h2c.spawn`][h2c.spawn] `0x0103` `{campaign/42}` | The host creates the campaign. |
| S01.2 | h2c | [`h2c.spawn`][h2c.spawn] `0x0103` `{progress/42, data: v0 = {campaignID: 42}}` | Then the tracker, after the campaign, so its first mail has somewhere to go. |
| S01.3 | c2h | [`c2h.spawned`][c2h.spawned] `0x0201` s1, [`c2h.fence`][c2h.fence] `0x0208` `{s1.m1, stable}` | The campaign's bootstrap. It waits in `idle`. |
| S01.4 | c2h | [`c2h.spawned`][c2h.spawned] s2, [`c2h.fence`][c2h.fence] `{s2.m1, mail: 2}`, [`c2h.wake`][c2h.wake] `0x0209` `{due: +5s}` | The tracker enters `syncing`. `progress.sync` to s1 is an envelope, and so is `syncTimeout`, a delayed send. **pool 2/64.** |
| S01.5 | c2h | [`c2h.fence`][c2h.fence] `{s1.m2, mail: 1}` | s1 takes `progress.sync` in `idle` and answers `progress.snapshot` with zero counts. **pool 2/64:** one freed at the fence, one added. |
| S01.6 | c2h | [`c2h.send`][c2h.send] `0x0203` `{x-ws, progress.update}`, [`c2h.fence`][c2h.fence] `{s2.m2}`, [`c2h.wake`][c2h.wake] `{none}` | s2 takes the snapshot and goes `live`. Leaving `syncing` cancels `syncTimeout`, and the envelope is removed. **pool 0/64.** |

## The click

| Step | Ring | Record | What happens |
|---|---|---|---|
| S01.7 | h2c | [`h2c.time`][h2c.time] `0x0101` `{at: t0}`, [`h2c.event`][h2c.event] `0x0102` `{target: campaign/42, name: send.requested, origin: http:req/9f, origin_type: x-http-reply, data: v1}` | The HTTP handler loads five notification objects into the JavaScript heap as `v1` and posts the event. The request stays open. |
| S01.8 | – | – | Core admits it: **pool 1/64.** The name's 14 bytes and the origin's 11 go in the name area. Nothing is resolved yet: names are resolved when the envelope is taken ([names](../reference/names.md)). |
| S01.9 | – | – | s1 takes it (`s1.m3`). Core looks up `send` and `requested`, both authored atoms, so the descriptor `send.requested` matches. `idle` → `active` → `attempt` → `sending`. Six `assign`s and a `script` go through the data model vtable. They are not on any ring. |
| S01.10 | – | – | `attempt`'s `onentry` schedules `t` (`sending.timeout`, due t0+30s), before any fanout. **pool 2/64.** |
| S01.11 | c2h | 5 × [`c2h.send`][c2h.send] `0x0203` `{x-notify, notify:, notification, data: n1…n5, sendid: #_send.2, .4, .6, .8, .10}` | The `foreach`: each `x-notify` send is a c2h record. Its `idlocation` assigns the sendid in the data model. Each `progress.sent` is an envelope for s2. **pool 7/64.** |
| S01.12 | c2h | [`c2h.fence`][c2h.fence] `{s1.m3, stable, effects: 5, mail: 6}`, [`c2h.wake`][c2h.wake] `{due: t0+30s}` | The macrostep commits. The `send.requested` envelope is freed: **pool 6/64.** |
| S01.13 | – | – | The host acts on nothing before the fence. Now it hands the five notifications to the notify processor. The processor stores a key `(recipient, n.id)` for each, with a time to live, and dispatches them. |
| S01.14 | c2h | 5 × ([`c2h.send`][c2h.send] `{x-ws}`, [`c2h.fence`][c2h.fence] `{s2.m3…m7}`) | s2 takes five `progress.sent` envelopes and pushes five updates. **pool 1/64:** only `t` is left. |

## Outcomes

| Step | Ring | Record | What happens |
|---|---|---|---|
| S01.15 | h2c | 3 × [`h2c.event`][h2c.event] `{name: notify.ack, origin: notify:, data: {id}}` for n1, n2, n3 | The processor reports three deliveries. |
| S01.16 | h2c | [`h2c.event`][h2c.event] `{name: notify.rejected, data: {id: n4, reason: "bad address"}}` | The processor refuses n4. This is final. |
| S01.17 | h2c | [`h2c.event`][h2c.event] `{name: error.communication, type: platform, sendid: #_send.10}` | n5's relay is unreachable. The I/O processor's error comes back as an input ([macrostep](../reference/macrostep.md): sends are fire and forget). |
| S01.18 | c2h | 5 × ([`c2h.fence`][c2h.fence] `{s1.m4…m8, mail: 1}`) | s1 takes each outcome. Each removes its ID from `pending`, or, for n5, from `sent`. Each sends one delta to the tracker (`#_send.12` to `.16`). The three acknowledgements and the rejection also raise `outcome.received`, which `sending` ignores; the `error.communication` for n5 raises nothing. `pending = {n5}`. |
| S01.19 | c2h | 5 × ([`c2h.send`][c2h.send] `{x-ws}`, [`c2h.fence`][c2h.fence]) | The tracker shows acked 3, rejected 1, failed 1. **pool 1/64.** |

## The retry and the reply

| Step | Ring | Record | What happens |
|---|---|---|---|
| S01.20 | h2c | [`h2c.time`][h2c.time] `{at: t0+30s}` | The host's single timer, set from S01.12's `c2h.wake`, fires. |
| S01.21 | – | – | `t` is due. Its envelope becomes runnable, with source `timer` ([envelope](../reference/envelope.md)). |
| S01.22 | c2h | [`c2h.send`][c2h.send] `{x-notify, n5, #_send.17}`, [`c2h.fence`][c2h.fence] `{s1.m9, effects: 1, mail: 2}`, [`c2h.wake`][c2h.wake] `{t0+60s}` | `sending.timeout`: `retrying` (attempt 2, `sent` cleared) → `attempt`, with a new `t` → `sending`. `todo` is `[n5]`. Exiting `attempt` cancelled the old `t`, which was the envelope being taken, so the cancel does nothing. **pool 2/64.** |
| S01.23 | h2c | [`h2c.event`][h2c.event] `{notify.ack, n5}` | The relay is back. |
| S01.24 | c2h | [`c2h.send`][c2h.send] `{x-http-reply, target: http:req/9f, name: ok}`, [`c2h.fence`][c2h.fence] `{s1.m10}`, [`c2h.wake`][c2h.wake] `{none}` | `pending` is empty. The eventless transition goes to `complete`. Exiting `attempt` cancels `t`, and the envelope is removed. `complete` replies to the click and sends `progress.done`. |
| S01.25 | – | – | After the fence, the host writes HTTP 200 on request `9f`. If the browser gave up after 30 seconds, the host posts `error.communication` for that sendid. `active` has gone, but `complete` ignores errors because it has no transition for them. The click was idempotent anyway ([S03](S03-crash-unrecorded.md)). |

## Who drops what, who retries how

| What failed | Dropped or refused by | Told how | Retried by |
|---|---|---|---|
| n4, bad address | notify processor | `notify.rejected` (S01.16) | nobody: it is final |
| n5, relay unreachable | notify processor | `error.communication` as an [`h2c.event`][h2c.event] (S01.17) | the chart, on its timeout (S01.22) |
| The reply, if the browser left | the host | `error.communication`, ignored | the user, by clicking again ([S08](S08-cancel-and-second-click.md)) |

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
[c2h.fence]: ../protocol/generated/MESSAGES.md#c2hfence
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
