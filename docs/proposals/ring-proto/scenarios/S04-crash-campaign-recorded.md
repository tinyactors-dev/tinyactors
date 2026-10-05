# S04: Crash, campaign recorded

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

The same crash as [S03](S03-crash-unrecorded.md), but the host records
every session whose address matches `campaign/*`
([recording and replay](../reference/recording-and-replay.md)). The
campaign uses its inputs to seed a new execution and is then published.
For this seed its timeout is pending. The tracker is not recorded. It shows
zero until it resyncs, and its resync is what puts it right.

**Parameters:** as in [S01](S01-fanout.md). Recorded: `campaign/*`.

## The log, as the host persisted it

Every record of a recorded session carries the common flag `recorded`.
The host journals [`c2h.input`][c2h.input] `0x0202` observations on its
own schedule. It executes effects on receipt without waiting for a journal
write or [`c2h.macrostep_ended`][c2h.macrostep_ended]. In this example the entries below reached
storage before the crash. The binding's codec encodes `data`, because a
vref means nothing outside the process.

| Log entry | Record | Source |
|---|---|---|
| L1 | [`c2h.spawned`][c2h.spawned] `0x0201` `{campaign/42, revision}` | – |
| L2 | `c2h.input {s1.m2, progress.sync, origin: #_scxml_…progress}` | `mail`, from s2 |
| L3 | `c2h.input {s1.m3, send.requested, origin: http:req/9f, data: «the five notifications, encoded»}` | `h2c` |
| L4–L6 | `c2h.input {notify.ack n1}`, `{n2}`, `{n3}` | `h2c` |
| L7 | `c2h.input {notify.rejected n4}` | `h2c` |
| L8 | `c2h.input {error.communication, sendid: #_send.10}`, at t0+2s | `h2c` |

Then the process crashes, at the same point as in S03. The following seed
uses the same definition and reaches the illustrated state. Different
resource conditions or a compatible new definition may derive different
configuration and data model values; that is accepted.

## Restart

| Step | Ring | Record | What happens |
|---|---|---|---|
| S04.1 | h2c | [`h2c.spawn`][h2c.spawn] `0x0103` `{campaign/42, revision, flag replaying_from_clock, clock: L1's time}` | The campaign is prepared to replay (core ADR 0089). It is not on the route yet. |
| S04.2 | c2h | [`c2h.spawned`][c2h.spawned] `{flags: replaying, recorded}` | |
| S04.3 | h2c | 7 × [`h2c.event`][h2c.event] `0x0102` `{flag replaying, at: the logged time}` from L2–L8 | Each input carries its logged time (core ADR 0094), so no `h2c.time` is needed. L3's data is decoded into new JavaScript objects (`v1'`). |
| S04.4 | tel | [`tel.trace`][tel.trace] `0x0301` `{SENT, flag suppressed}`, … | The supplied inputs derive a new chart configuration and JavaScript data model values: data model calls are computation, not effects ([data model](../reference/data-model.md)). Effects do not cross the border while seeding: no `c2h.send`, no tracker mail. Telemetry shows them as suppressed. Core writes no `c2h.input` for replay inputs. |
| S04.5 | – | – | The delayed send `t` (due t0+30s) is held in the pool under the session's clock. **pool 1/64.** |
| S04.5a | c2h | 7 × [`c2h.input_completed`][c2h.input_completed] `{source_seq: each S04.3 h2c.event}` | The host waits for all seven before it publishes ([ARCHITECTURE](../ARCHITECTURE.md#recording-and-replay)): admission alone does not establish completion. A bounced or refused seed input would need a retry, or an explicit choice to omit it. |
| S04.6 | h2c | [`h2c.publish`][h2c.publish] `0x0104` `{campaign/42, at: t0+2s}` | `at` is the time of L8, the last thing the run did before the crash. |
| S04.7 | c2h | [`c2h.published`][c2h.published] `0x020F` `{replayed: 7, dropped_timers: 0}`, [`c2h.wake`][c2h.wake] `0x0209` `{t0+30s}` | `t` was due after `at`, so it is scheduled. Had it been due by `at`, it would have fired before the crash, and that firing would be in the log as an input. The campaign is in `attempt`/`sending` with `pending = {n5}`, `sent = {n1…n4}`, attempt 1, and `replyTo` = the dead `http:req/9f`. |
| S04.8 | h2c | [`h2c.spawn`][h2c.spawn] `{progress/42}` | The tracker is not recorded, so it starts fresh, after the publish so that its first mail has somewhere to go. |
| S04.9 | c2h | [`c2h.macrostep_ended`][c2h.macrostep_ended] `{s2.m1, mail: 2}` | `syncing`: `progress.sync` to s1. Until the snapshot arrives, the browser sees **zero**. |
| S04.10 | c2h | [`c2h.macrostep_ended`][c2h.macrostep_ended] `{s1.m…, mail: 1}` | s1 answers `progress.snapshot {sent 5, acked 3, rejected 1, failed 1, pending 1}`. |
| S04.11 | c2h | [`c2h.send`][c2h.send] `{x-ws}` | The tracker is `live` and correct. |

## Then either of two things finishes the job

| Step | Ring | Record | What happens |
|---|---|---|---|
| S04.12a | h2c | [`h2c.event`][h2c.event] `{send.requested, origin: http:req/a2}` | **The user clicks again** (the browser saw the failure). `active`'s second-click transition sets `replyTo` to `a2`, clears `sent` and re-enters `attempt`. `t` is cancelled exactly and a new `t` is scheduled ([S08](S08-cancel-and-second-click.md)). `todo = [n5]`. |
| S04.12b | h2c | [`h2c.time`][h2c.time] `{t0+30s}` | **Or nobody clicks.** `t` fires, `retrying` runs attempt 2, and n5 is sent again. The reply goes to the dead `9f`, the host posts `error.communication`, and the chart ignores it. |
| S04.13 | c2h | [`c2h.send`][c2h.send] `{x-notify, n5}` | The notification that the replay suppressed (it was sent as far as s1 knew, but it never reached its relay) goes out. The processor deduplicates it if it did. |

## Without the resync

Suppose the tracker had no `progress.sync`. It would show zero for this
campaign forever: the replay suppressed every `progress.*` message the
campaign regenerated, because to the campaign those were already sent.
Per-session replay derives state from the supplied inputs. It does not
establish which effects executed earlier or keep two sessions consistent
with each other. Derived actors resync ([S05](S05-crash-both-recorded.md)).

## Who drops what, who retries how

| What was lost | Lost by | Told how | Retried by |
|---|---|---|---|
| n5, never delivered | the crash; replay suppresses it | the chart's `pending` still holds it | a second click (S04.12a) or the timeout (S04.12b) |
| Tracker deltas the replay regenerated | replay: they cross no border | – | the tracker's resync (S04.9–S04.11) |
| The reply to `9f` | the crash | the browser's network error | the user |
| `t` | not lost: it is held in the pool while replaying and scheduled at publish (S04.7) | – | – |

[h2c.time]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2ctime
[h2c.event]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cevent
[h2c.spawn]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cspawn
[h2c.publish]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cpublish
[h2c.load]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload
[h2c.load_failed]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cload_failed
[h2c.unload]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cunload
[h2c.checkpoint]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2ccheckpoint
[h2c.invoke_report]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cinvoke_report
[h2c.destroy]: ../../../../tinyactors-core/src/generated/MESSAGES.md#h2cdestroy
[c2h.spawned]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hspawned
[c2h.input]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput
[c2h.send]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hsend
[c2h.invoke]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinvoke
[c2h.invoke_cancel]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinvoke_cancel
[c2h.invoke_forward]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinvoke_forward
[c2h.log]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hlog
[c2h.macrostep_ended]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hmacrostep_ended
[c2h.wake]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hwake
[c2h.bounced]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hbounced
[c2h.load_needed]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hload_needed
[c2h.loaded]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hloaded
[c2h.unloaded]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hunloaded
[c2h.state]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hstate
[c2h.published]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hpublished
[c2h.input_completed]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hinput_completed
[c2h.finished]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hfinished
[c2h.destroyed]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hdestroyed
[tel.trace]: ../../../../tinyactors-core/src/generated/MESSAGES.md#teltrace
[tel.dropped]: ../../../../tinyactors-core/src/generated/MESSAGES.md#teldropped
