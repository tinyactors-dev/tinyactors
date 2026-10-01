# S05: Crash, both recorded

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

The same crash, with `campaign/*` and `progress/*` both recorded per
session. Recording the tracker does not help: two deltas were in the
pool, and both sides' replays agree they never happened. A replayed
tracker is also worse off than a fresh one, because it doesn't resync on
its own. Then the same crash once more, recovered by **replaying the
whole system from its h2c stream**, which gets the two deltas back
exactly.

**Parameters:** as in [S01](S01-fanout.md).

## Per-session replay

The campaign's log is the one from [S04](S04-crash-campaign-recorded.md#the-log-as-the-host-persisted-it).
The tracker's log has the 9 inputs it took: `progress.snapshot`, 5 ×
`progress.sent` and 3 × `progress.acked`. At the crash,
`progress.rejected` (n4) and `progress.failed` (n5) were envelopes in the
pool. They were **in flight**: s1 had sent them, and s2 had not taken
them.

| Step | Ring | Record | What happens |
|---|---|---|---|
| S05.1 | h2c | [`h2c.spawn`][h2c.spawn] `0x0103` `{campaign/42, replaying_from_clock}`, 7 × [`h2c.event`][h2c.event] `{replaying}`, [`h2c.publish`][h2c.publish] `0x0104` | As in S04.1–S04.7. |
| S05.2 | h2c | [`h2c.spawn`][h2c.spawn] `{progress/42, replaying_from_clock}`, 9 × [`h2c.event`][h2c.event] `{replaying}`, [`h2c.publish`][h2c.publish] `{progress/42, at}` | The tracker is rebuilt from its 9 inputs. Its replayed bootstrap sent `progress.sync`, and replay suppresses that mail. Its replayed `syncTimeout` was cancelled by the replayed snapshot. It ends **`live`, showing sent 5, acked 3, rejected 0, failed 0.** |
| S05.3 | c2h | [`c2h.published`][c2h.published] `0x020F` × 2 | Both are live. |
| S05.4 | – | – | **The two deltas are gone.** s1's replay suppressed them, because to s1 they were sent. s2's log doesn't have them, because s2 never took them. Nothing in either log can bring them back. |
| S05.5 | – | – | A replayed tracker is `live`, so it doesn't resync. A fresh one would have started in `syncing` (S04.9). **Recording the derived actor made it worse.** |
| S05.6 | h2c | [`h2c.event`][h2c.event] `{progress/42, progress.resync}` | Something has to tell it. Here the host sends `progress.resync` to every recorded derived actor after publishing. A browser reconnect could do the same. |
| S05.7 | c2h | [`c2h.send`][c2h.send] `{x-ws}` | `syncing` → snapshot → `live`, correct. |

**Lesson:** record the actors whose state is primary, such as the
campaign. Let derived actors start fresh and resync. The fence plus
per-session replay gives consistency within a session, never between
two sessions.

## Whole-system replay

An application can instead record **the h2c stream itself**: every record
it posted, in order. With the same definitions and the same system
parameters, everything else core does follows from that stream. This
includes the order of mail between sessions, which pool envelope runs
next, and whether a send hits a capacity error
([recording and replay](../reference/recording-and-replay.md#session-replay-and-whole-system-replay)).
The data model must be deterministic, which is its own business.

| Step | Ring | Record | What happens |
|---|---|---|---|
| S05.8 | – | – | The persisted stream is S01.1–S01.17: both spawns, `h2c.time` t0, `send.requested`, and the processor's five results. It does not include S01.20, because the crash came first. |
| S05.9 | – | – | The host creates a system with the **recorded** parameters (`poolEnvelopes` 64, …) and registers the same revisions. |
| S05.10 | h2c | Every recorded record, each with the common flag `replaying` | Core runs both sessions together. Mail between them is delivered inside core as it was the first time. Only effects leaving the process (`c2h.send`, `c2h.invoke`, …) are suppressed. |
| S05.11 | – | – | The stream ends where the crash was. The pool now holds what it held then: `t` and **the two in-flight deltas**. **pool 3/64.** |
| S05.12 | h2c | [`h2c.publish`][h2c.publish] `{address: "", at: t0+2s}` | An empty address publishes every replaying session at once. |
| S05.13 | c2h | [`c2h.published`][c2h.published] × 2, then [`c2h.send`][c2h.send] `{x-ws}` × 2 | s2 takes `progress.rejected` and `progress.failed`, which it never saw before the crash. The tracker is exact, with no resync. |

What whole-system replay costs:
- The log holds every input of every session, including those of
  sessions nobody cares about.
- The system's parameters are part of the recording. Replaying with a
  larger pool could turn a capacity error into a success, and the run
  would diverge.
- Truncating the log needs a checkpoint of the whole system, not of one
  session ([S06](S06-checkpoint.md) shows the per-session form).

It still loses what left the process and was never acknowledged. n5 is
suppressed exactly as in S04, and the chart's retry covers it.

## Who drops what, who retries how

| What was lost | Lost by | Told how | Retried by |
|---|---|---|---|
| Two tracker deltas in flight | per-session replay (S05.4) | nobody | the tracker's resync, which something must trigger (S05.6) |
| The same deltas | not lost under whole-system replay (S05.11) | – | – |
| n5 | the crash; replay suppresses it | the campaign's `pending` | the second click or the timeout ([S04.12](S04-crash-campaign-recorded.md#then-either-of-two-things-finishes-the-job)) |

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
