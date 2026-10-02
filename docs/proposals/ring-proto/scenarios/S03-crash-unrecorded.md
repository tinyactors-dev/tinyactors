# S03: Crash, nothing recorded

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

The process dies partway through [S01](S01-fanout.md). Nothing was
recorded, so everything in the process is lost. The user clicks again,
and the notify processor's own idempotency key keeps anyone from getting
a notification twice.

**Parameters:** as in [S01](S01-fanout.md). No session is recorded.

## At the crash

At t0+2s, after S01.18:

| | State |
|---|---|
| s1 (campaign) | It has taken `send.requested`, acknowledgements for n1–n3, the rejection of n4 and `error.communication` for n5. `pending = {n5}`. |
| s2 (tracker) | It has taken 9 of its 11 messages. `progress.rejected` (n4) and `progress.failed` (n5) are still in the pool. |
| pool | 3/64: `t` and the 2 tracker envelopes |
| The notify processor | It sent n1–n3. It holds their keys in a durable store with a time to live. That is its own design ([BEHAVIORS](../BEHAVIORS.md#who-drops-what)). |
| The browser | It is waiting on request `9f`. |

Then **the process crashes.**

| Lost | Survives |
|---|---|
| The JavaScript heap: `v1`, every notification object, both data models | The three notifications that were delivered |
| Core: both sessions, the pool (with `t` and the 2 tracker envelopes), and all three rings, which lived in the process | The processor's key store |
| The HTTP connection: the browser sees a network error | |

## Restart

| Step | Ring | Record | What happens |
|---|---|---|---|
| S03.1 | h2c, c2h | [`h2c.definition_register`](../protocol/generated/MESSAGES.md#h2cdefinition_register) `0x010B`, [`c2h.definition_result`](../protocol/generated/MESSAGES.md#c2hdefinition_result) `0x0212`, twice | The host creates the system and installs native integrations with API calls, then submits both complete specs and waits for successful registration results before spawning. |
| S03.2 | h2c | [`h2c.spawn`][h2c.spawn] `0x0103` `{campaign/42}`, [`h2c.spawn`][h2c.spawn] `{progress/42}` | As in S01.1–S01.6. The tracker syncs and shows zero, which is true of the new campaign session. |
| S03.3 | h2c | [`h2c.time`][h2c.time] `0x0101`, [`h2c.event`][h2c.event] `0x0102` `{campaign/42, send.requested, origin: http:req/a2, data: v1'}` | The user clicks again. The handler loads the same five notifications from the database. |
| S03.4 | c2h | 5 × [`c2h.send`][c2h.send] `0x0203` `{x-notify, n1…n5, sendid: #_send.2 … #_send.10}`, [`c2h.macrostep_ended`][c2h.macrostep_ended] `0x0208` | A fresh run fans out all five. **The sendids are the same text as before the crash**, because the new session's counter starts again. A processor that used the sendid as its key would mistake this run for the old one, or a genuinely new run for this one. The key has to come from the content ([names](../reference/names.md), [BEHAVIORS](../BEHAVIORS.md)). |
| S03.5 | – | – | The processor computes `(recipient, n.id)`. It already has n1–n3, so it **acknowledges them without sending**, and the chart can't tell the difference. It refuses n4 again. It sends n5, whose relay is back. |
| S03.6 | h2c | 4 × [`h2c.event`][h2c.event] `{notify.ack}`, 1 × `{notify.rejected}` | |
| S03.7 | c2h | [`c2h.send`][c2h.send] `{x-http-reply, http:req/a2, ok}` | `complete` replies to the second click. |

## If the processor's keys were in the process too

The keys are lost with the crash, and n1–n3 go out twice. The protocol
can't prevent this, and the chart can't see it. It is the application's
choice where its I/O processor keeps state.

The same is true of an I/O processor that the new process does not
register at all. Every send to it comes back as `error.communication`,
and the chart's retry covers it.

## Who drops what, who retries how

| What was lost | Lost by | Told how | Retried by |
|---|---|---|---|
| Both sessions, `t`, the tracker's 2 envelopes | the crash | nobody is told | the user's second click (S03.3) |
| The reply to request `9f` | the crash | the browser's network error | the user's second click |
| Duplicates of n1–n3 | removed by the notify processor (S03.5) | not visible to the chart | – |

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
