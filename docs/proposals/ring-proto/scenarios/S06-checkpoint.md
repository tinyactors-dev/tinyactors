# S06: Checkpoint

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

A recorded campaign with 10,000 notifications has a long log: one input
per acknowledgement. The host takes a checkpoint, truncates the log, and
after a crash restores the checkpoint and replays only the tail.

**Parameters:** `poolEnvelopes` 16384, `admissionWatermark` 75% (12,288 envelopes),
`fairnessRatio` 8, `nameAreaBytes` 64 KiB, `h2cBytes` 1 MiB, `c2hBytes`
4 MiB, `telBytes` 1 MiB, `maxRecordBytes` 4096. Recorded: `campaign/*`.
The pool is large enough for the brutal fanout. With the
[windowed chart](../charts/campaign-windowed.scxml) it could be small.

## Taking the checkpoint

| Step | Ring | Record | What happens |
|---|---|---|---|
| S06.1 | – | – | The log holds [`c2h.spawned`][c2h.spawned] and 6,001 [`c2h.input`][c2h.input] records: `send.requested` and 6,000 outcomes. The host's policy is to checkpoint every 5,000 inputs. |
| S06.2 | h2c | [`h2c.checkpoint`][h2c.checkpoint] `0x0108` `{campaign/42}` | Core runs macrosteps to completion, so the session is between two macrosteps when core reads the request. |
| S06.3 | c2h | [`c2h.state`][c2h.state] `0x020E` `{campaign/42, image: b9, input_seq: the c2h seq of the 6,001st input}` | Core writes the image through the host's byte sink (core ADR 0090), and the host gives it the blob ID `b9`. The image holds the configuration, history, active invocations, and **the delayed send `t`**, with its due time and data. The data model's root (`batch`, `pending`, `sent`, `idOf`, `counts`, …) is written through `value_write`, so the binding's codec encodes the JavaScript objects ([data model](../reference/data-model.md)). It is one artifact. |
| S06.4 | – | – | The host stores `b9` and then deletes the log entries up to `input_seq`, in that order. A crash between the two leaves a log that is too long, which is harmless. |

## Crash and restore

| Step | Ring | Record | What happens |
|---|---|---|---|
| S06.5 | – | – | 1,500 more outcomes are logged, and then the process crashes. |
| S06.6 | h2c | [`h2c.load`][h2c.load] `0x0105` `{campaign/42, image: b9, flag replaying}` | The checkpoint is restored as a replaying session. `t` goes back into the pool, held under the session's clock. |
| S06.7 | c2h | [`c2h.loaded`][c2h.loaded] `0x020C` `{flags: replaying, recorded}` | |
| S06.8 | h2c | 1,500 × [`h2c.event`][h2c.event] `{flag replaying}` | Only the tail is replayed. |
| S06.9 | h2c | [`h2c.publish`][h2c.publish] `0x0104` `{campaign/42, at: the last input's time}` | |
| S06.10 | c2h | [`c2h.published`][c2h.published] `{replayed: 1500, dropped_timers: 0}`, [`c2h.wake`][c2h.wake] | `t` is scheduled as in [S04.7](S04-crash-campaign-recorded.md#restart). |

## If the host kept the timers instead

The proposal rejects this alternative, where timers live in the host
rather than in the pool ([envelope](../reference/envelope.md)). Under it,
the image would not hold `t`. The host would have to store its own
pending timers for the session, encoded by its own codec, at the same
fence as `c2h.state`. That makes two artifacts that must be written
together. Keeping timers in the pool keeps a checkpoint to one artifact.

## Who drops what, who retries how

| What was lost | Lost by | Told how | Retried by |
|---|---|---|---|
| Log entries before `input_seq` | the host deletes them (S06.4) | – | not needed: the image replaces them |
| Effects of the 1,500 replayed inputs that left before the crash | replay suppresses them | the chart's `pending` | the timeout or a click, as in [S04](S04-crash-campaign-recorded.md) |

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
