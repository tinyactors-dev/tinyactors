# S08: Cancel and a second click

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

The user clicks again just as the attempt's timeout comes due. Delayed
sends are envelopes in core's pool, so `<cancel>` removes the envelope,
and a timer cancelled while still pending never arrives. The host controls
which operation completes first. Posting a click before a clock update
does not by itself make the click complete first.

**Parameters:** as in [S01](S01-fanout.md). The campaign is in
`attempt`/`sending` with `pending = {n5}`. `t` is due at t0+30s and is
the only envelope: **pool 1/64.** The first click's request `9f` is still
open.

## Order A: the click before the clock

| Step | Ring | Record | What happens |
|---|---|---|---|
| S08.1 | h2c | [`h2c.time`][h2c.time] `{t0+29.9s}`, then [`h2c.event`][h2c.event] `{campaign/42, send.requested, origin: http:req/c4}`, seq `q` | The user clicks again. The explicit time update supplies the live clock for the click. |
| S08.2 | host | – | The wake timer fires at t0+30s. The host retains that time but does not post it until input `q` completes. |
| S08.3 | – | – | Core admits the click. **pool 2/64.** Older runnable work may precede it. When it is taken, its macrostep time is t0+29.9s and stays fixed through any budget pause. |
| S08.4 | c2h | [`c2h.send`][c2h.send] `{x-notify, n5}`, [`c2h.macrostep_ended`][c2h.macrostep_ended], [`c2h.wake`][c2h.wake] `{t0+59.9s}`, [`c2h.input_completed`][c2h.input_completed] `{source_seq: q}` | `send.requested` re-enters `attempt`. Its `<cancel sendid="t">` removes all matching pending timers. Its entry schedules a new `t`, due t0+59.9s. **pool 1/64.** The host now knows the click completed. |
| S08.5 | h2c | [`h2c.time`][h2c.time] `{t0+30s}` | The host posts the retained time. Nothing is due. The old `t` no longer exists. |

## Order B: the clock before the click

| Step | Ring | Record | What happens |
|---|---|---|---|
| S08.6 | h2c | [`h2c.time`][h2c.time] `{t0+30s}` | The host drives the due timeout to completion before posting the click. This finite trace has no older runnable work. |
| S08.7 | c2h | [`c2h.send`][c2h.send] `{x-notify, n5}`, [`c2h.macrostep_ended`][c2h.macrostep_ended], [`c2h.wake`][c2h.wake] `{t0+60s}` | `t` is due and runs first: `retrying` (attempt 2) → `attempt`, with a new `t` → n5 is sent again. |
| S08.8 | h2c, c2h | [`h2c.time`][h2c.time] `{t0+30s+ε}`, [`h2c.event`][h2c.event] `{send.requested, c4}`; then [`c2h.send`][c2h.send], [`c2h.macrostep_ended`][c2h.macrostep_ended], [`c2h.wake`][c2h.wake] `{t0+60s+ε}`, [`c2h.input_completed`][c2h.input_completed] | The click re-enters `attempt`. It cancels attempt 2's pending `t` and schedules another. n5 is sent a third time, and the processor removes the duplicate. |

In both orders the chart sees exactly one timeout or none, never a
cancelled pending timer. h2c fixes command-read order; the host's completion
wait establishes the additional execution order in A. Core owns cancellation.
A replay seed uses these rules in its new execution; different resource
conditions may still produce different configuration and values
([recording and replay](../reference/recording-and-replay.md#session-replay-and-whole-system-replay)).

## Posting order alone is insufficient

Suppose older runnable envelopes precede the click. The host posts the
click and time 30s in one batch. Core admits the click, then may read the
time update before the click is taken. The old timeout becomes runnable
and leaves the deadline index. The click's later `<cancel>` cannot remove
that already-dispatched timeout. This is permitted by SCXML's cancellation
rule; admission is not an execution barrier, whatever the number of macrostep slots.
The host uses the completion wait in order A when cancellation must win.

A bounce or `c2h.input_refused` for `q` is not successful completion. The
host must retry the click or explicitly decide to advance time without it.

## The two open requests

The first click's request `9f` is never answered, because `replyTo` now
names `c4`. The host closes `9f` with its own HTTP timeout. A chart that
wants to be polite can reply `superseded` to the old `replyTo` before
overwriting it. That is a change to the chart, not to the protocol.

## Who drops what, who retries how

| What was lost | Dropped by | Told how | Retried by |
|---|---|---|---|
| The old `t` (order A) | core, by `<cancel>` (S08.4) | – | not needed |
| The reply to `9f` | the chart, by overwriting `replyTo` | the host's HTTP timeout | the user, if they are still looking |
| Duplicates of n5 (order B) | the notify processor | not visible to the chart | – |

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
[c2h.input_completed]: ../../../../tinyactors-core/protocol/generated/MESSAGES.md#c2hinput_completed
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
