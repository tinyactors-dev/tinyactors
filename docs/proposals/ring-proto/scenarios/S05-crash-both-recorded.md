# S05: Crash, both recorded

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

The same crash, with `campaign/*` and `progress/*` both recorded per
session. Recording the tracker does not help: two deltas were in the
pool, and both sides' replays agree they never happened. A replayed
tracker is also worse off than a fresh one, because it doesn't resync on
its own. Then the same crash once more, using **replay of the whole system
from its h2c stream** to seed a new execution together. That execution
generates its own local mail; it does not recover the old pool exactly.

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

**Lesson:** journal inputs useful for seeding primary actors, such as the
campaign. Let derived actors start fresh and resync with the current source.
Neither a macrostep end nor replay establishes what executed before a crash.

## Whole-system replay

An application can instead record **the h2c stream itself**: every record
it posted, in order, then use that stream to seed a new execution together
([recording and replay](../reference/recording-and-replay.md#session-replay-and-whole-system-replay)).
This execution's configuration, values and pending mail may differ, even
with the same definitions and parameters. The stream does not record
budgets, ring draining or the exact point core stopped.

| Step | Ring | Record | What happens |
|---|---|---|---|
| S05.8 | – | – | The persisted stream is S01.1–S01.17: both spawns, `h2c.time` t0, `send.requested`, and the processor's five results. It does not include S01.20, because the crash came first. |
| S05.9 | – | – | The host chooses parameters and compatible definitions for the new execution; this example starts with the old settings. |
| S05.10 | h2c | Every selected record, each with the common flag `replaying` | Core runs both sessions together. Mail they generate is delivered inside core. Effects leaving the process (`c2h.send`, `c2h.invoke`, …) are suppressed while seeding. |
| S05.11 | – | – | The selected stream ends, and the host waits for every supplied input's `c2h.input_completed` before publishing. Generated mail need not drain. The new execution has its own configuration, values, timers and pending mail. Which tracker deltas are still pending depends on this execution's scheduling and resource conditions; the old **pool 3/64** is not a recovery guarantee. |
| S05.12 | h2c | [`h2c.publish`][h2c.publish] `{address: "", at: t0+2s}` | An empty address publishes every replaying session at once. |
| S05.13 | c2h | One aggregate [`c2h.published`][c2h.published] `{session: 0}`, then live effects | Both sessions begin live execution from the derived state. The host may execute effects on receipt. A tracker resync reads the campaign's current state; replay does not promise the old view or exactly two pending deltas. |

What whole-system replay costs:

- The log holds every input of every session, including those of
  sessions nobody cares about.
- Parameters and definitions are useful context for selecting a new seed.
  A larger pool could turn an old capacity error into a success and derive
  different state. That is accepted.
- Seeding from a shorter stream requires the application to choose suitable
  starting state. A per-session checkpoint ([S06](S06-checkpoint.md)) is
  not a snapshot of all sessions and their shared queues.

It still loses what left the process and was never acknowledged. n5 is
suppressed exactly as in S04, and the chart's retry covers it.

## Who drops what, who retries how

| What was lost | Lost by | Told how | Retried by |
|---|---|---|---|
| Two tracker deltas in flight | per-session replay (S05.4) | nobody | the tracker's resync, which something must trigger (S05.6) |
| The old in-flight deltas | whole-system replay generates new mail rather than restoring the old queue (S05.11) | no exact recovery report | resync reads the current source state |
| n5 | the crash; replay suppresses it | the campaign's `pending` | the second click or the timeout ([S04.12](S04-crash-campaign-recorded.md#then-either-of-two-things-finishes-the-job)) |

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
[c2h.finished]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hfinished
[c2h.destroyed]: ../../../../tinyactors-core/src/generated/MESSAGES.md#c2hdestroyed
[tel.trace]: ../../../../tinyactors-core/src/generated/MESSAGES.md#teltrace
[tel.dropped]: ../../../../tinyactors-core/src/generated/MESSAGES.md#teldropped
