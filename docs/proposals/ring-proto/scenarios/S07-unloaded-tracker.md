# S07: Unloaded tracker

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

The host pages the idle tracker out. The campaign's mail to it waits in
the pool, and core asks for the tracker once. The host loads it, and the
mail runs. Then a second run where the load fails: the waiting mail is
recycled, each envelope becoming the `error.communication` for its own
sender.

**Parameters:** as in [S01](S01-fanout.md): `poolEnvelopes` 64,
`admissionWatermark` 75% (48 envelopes). Nothing is recorded. The host keeps images of
unloaded sessions in SQLite: `unloaded(address, image, wake_at)`, indexed
on `wake_at` ([unloading](../reference/unloading.md)).

## Unloading

| Step | Ring | Record | What happens |
|---|---|---|---|
| S07.1 | h2c | [`h2c.unload`][h2c.unload] `0x0107` `{progress/42}` | The tracker has been `live` and idle for a minute. That is the host's policy. |
| S07.2 | c2h | [`c2h.unloaded`][c2h.unloaded] `0x020D` `{progress/42, image: b3}` | `has_wake` is clear, because `live` has no delayed sends. The host writes the row and frees `b3`'s memory. |
| S07.3 | – | – | Had the tracker been in `syncing`, its `syncTimeout` would have gone into the image, and the record would have carried `has_wake` and `wake_at`. The host's index would then post [`h2c.load`][h2c.load] when `wake_at` came. Core's own deadline index covers resident sessions only. |

## Mail waits, and the load succeeds

| Step | Ring | Record | What happens |
|---|---|---|---|
| S07.4 | h2c | [`h2c.event`][h2c.event] `{campaign/42, send.requested, 5 notifications}` | As in S01.7. |
| S07.5 | c2h | 5 × [`c2h.send`][c2h.send] `{x-notify}`, [`c2h.fence`][c2h.fence] `{s1.m3, mail: 6}`, [`c2h.load_needed`][c2h.load_needed] `0x020B` `{progress/42, waiting: 5}` | During the macrostep, the five `progress.sent` envelopes sit on its outgoing list. At the fence they move to the waiting list. Their target address is not resident, so core writes `load_needed` **once**, after the fence ([envelope](../reference/envelope.md#mail-between-local-sessions)). The host may execute notification sends on receipt, independently of that local scheduling boundary. **pool 6/64:** `t` and five waiting envelopes. |
| S07.6 | – | – | The host reads the row from SQLite. This takes a few milliseconds, and the system runs other envelopes meanwhile. |
| S07.7 | h2c | [`h2c.load`][h2c.load] `0x0105` `{progress/42, image: b3'}` | |
| S07.8 | c2h | [`c2h.loaded`][c2h.loaded] `0x020C` `{progress/42, cause_seq}` | The five waiting envelopes become runnable, in the order they were sent. |
| S07.9 | c2h | 5 × ([`c2h.send`][c2h.send] `{x-ws}`, [`c2h.fence`][c2h.fence] `{s2…}`) | The tracker catches up. **pool 1/64.** |

Waiting envelopes count against the pool. A slow loader with many
sessions waiting can push the pool past `admissionWatermark`, and then
admission pauses as in [S02.10](S02-full-pool.md#the-brutal-chart). A
slow store applies backpressure at the edge. It does not hide a queue
anywhere.

## The load fails

| Step | Ring | Record | What happens |
|---|---|---|---|
| S07.10 | – | – | As S07.4–S07.5, with the tracker unloaded again. Five `progress.sent` envelopes are waiting, and one [`c2h.load_needed`][c2h.load_needed]. A browser's `progress.resync` from the outside ([`h2c.event`][h2c.event] `{progress/42}`) also waits. **pool 7/64.** |
| S07.11 | h2c | [`h2c.load_failed`][h2c.load_failed] `0x0106` `{progress/42, reason: "sqlite: disk I/O error"}` | |
| S07.12 | – | – | **Recycling.** Each of the five mail envelopes is rewritten in place as `error.communication` for its sender, s1, with the original `sendid`. Reporting the failure needs no new envelope and no name-area bytes beyond what the envelope already held ([envelope](../reference/envelope.md#recycling-an-envelope-that-cant-be-delivered-becomes-its-own-error)). **pool 7/64.** |
| S07.13 | tel | [`tel.trace`][tel.trace] `0x0301` `{DEAD_LETTER, progress.resync, reason}` | The external event has no sender to tell, so it is dropped and reported in telemetry. The browser's websocket reconnect retries it. **pool 6/64.** |
| S07.14 | c2h | 5 × [`c2h.fence`][c2h.fence] `{s1…}` | s1 takes five `error.communication`. Their sendids are not in `idOf`, so the bare transition ignores them: progress is optional ([chart notes](../charts/README.md#campaignscxml)). **pool 1/64.** |
| S07.15 | – | – | The notifications themselves are unaffected. |

## Who drops what, who retries how

| What was lost | Dropped by | Told how | Retried by |
|---|---|---|---|
| Five tracker deltas | core, on [`h2c.load_failed`][h2c.load_failed] | `error.communication` to the sender, recycled in place (S07.12) | nobody: the chart chose to ignore it. A loaded tracker comes back `live` from its image and does not resync on its own, so the host sends it `progress.resync` after the next load, as in [S05.6](S05-crash-both-recorded.md#per-session-replay). |
| The browser's `progress.resync` | core, on `load_failed` | [`tel.trace`][tel.trace] `DEAD_LETTER`, for the operator (S07.13) | the browser, on reconnect |

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
