# S08: Cancel and a second click

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

The user clicks again just as the attempt's timeout comes due. Delayed
sends are envelopes in core's pool, so `<cancel>` removes the envelope,
and a cancelled timer never arrives. That holds whichever way the click
and the clock are ordered in h2c. For contrast, the same moment under the
rejected alternative, where the host keeps the timers.

**Parameters:** as in [S01](S01-fanout.md). The campaign is in
`attempt`/`sending` with `pending = {n5}`. `t` is due at t0+30s and is
the only envelope: **pool 1/64.** The first click's request `9f` is still
open.

## Order A: the click before the clock

| Step | Ring | Record | What happens |
|---|---|---|---|
| S08.1 | h2c | [`h2c.event`][h2c.event] `0x0102` `{campaign/42, send.requested, origin: http:req/c4}` at t0+29.9s | The user clicks again. |
| S08.2 | h2c | [`h2c.time`][h2c.time] `0x0101` `{t0+30s}` | The host's timer fires right after. |
| S08.3 | – | – | Core reads h2c in order. It admits the click. **pool 2/64.** It runs s1's macrostep **to completion before it reads the next h2c record** ([macrostep](../reference/macrostep.md#run-to-completion)). |
| S08.4 | c2h | [`c2h.send`][c2h.send] `{x-notify, n5}`, [`c2h.macrostep_ended`][c2h.macrostep_ended] `{s1.m…}`, [`c2h.wake`][c2h.wake] `0x0209` `{t0+59.9s}` | `send.requested` in `active` sets `replyTo` to `c4`, clears `sent` and re-enters `attempt`. Its `onexit` `<cancel sendid="t">` **removes the envelope**. Its `onentry` schedules a new `t`, due t0+59.9s. **pool 1/64.** |
| S08.5 | – | – | Core reads `h2c.time t0+30s`. Nothing is due. The old `t` no longer exists. |

## Order B: the clock before the click

| Step | Ring | Record | What happens |
|---|---|---|---|
| S08.6 | h2c | [`h2c.time`][h2c.time] `{t0+30s}`, then [`h2c.event`][h2c.event] `{send.requested, c4}` | |
| S08.7 | c2h | [`c2h.send`][c2h.send] `{x-notify, n5}`, [`c2h.macrostep_ended`][c2h.macrostep_ended], [`c2h.wake`][c2h.wake] `{t0+60s}` | `t` is due and runs first: `retrying` (attempt 2) → `attempt`, with a new `t` → n5 is sent again. |
| S08.8 | c2h | [`c2h.send`][c2h.send] `{x-notify, n5}`, [`c2h.macrostep_ended`][c2h.macrostep_ended], [`c2h.wake`][c2h.wake] `{t0+60s+ε}` | Then the click re-enters `attempt`. It cancels attempt 2's `t` exactly and schedules another. n5 is sent a third time, and the processor removes the duplicate. |

In both orders the chart sees exactly one timeout or none, never a
cancelled one. h2c fixes the input order, and core alone owns cancellation.
A replay seed uses these rules in its new execution; different resource
conditions may still produce different configuration and values
([recording and replay](../reference/recording-and-replay.md#session-replay-and-whole-system-replay)).

## The rejected alternative: the host keeps the timers

| Step | Where | What happens |
|---|---|---|
| S08.9 | host | `t` fires at t0+30s. The host posts `sending.timeout` into h2c. |
| S08.10 | h2c | The click lands behind it. Core takes the click first only if the host posted it first. Here it didn't. |
| S08.11 | core | If the click is taken first, `<cancel>` becomes a notice to the host, but the timeout is already in h2c. The chart then gets **a timeout for an attempt it has already replaced**, and retries early. SCXML allows this: once the delay has passed, a cancel has no effect. It is harmless here only because retrying is idempotent. A chart that reuses a sendid for something that isn't idempotent would have to compare `_event.sendid` against an `idlocation`. |

The proposal keeps timers in the pool. The price is that they use pool
capacity ([envelope](../reference/envelope.md)). The gain is that a
cancel is exact, and a checkpoint is one artifact ([S06](S06-checkpoint.md)).

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
