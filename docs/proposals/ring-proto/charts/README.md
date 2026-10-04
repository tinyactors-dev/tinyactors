# Stress-test charts

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

These charts are the worked example of the proposal. The
[scenarios](../scenarios/README.md) run them through a fanout, a full pool,
crashes, unloading and hostile input, and cite the protocol messages each
step uses.

All three use `datamodel="ecmascript"`. Their values (the notification
objects, the `pending` set, the counts) live in the binding's JavaScript
heap. Core holds only value references to them
([data model](../reference/data-model.md)).

| Chart | Address | What it demonstrates |
|---|---|---|
| [`campaign.scxml`](campaign.scxml) | `campaign/<id>` | The brutal version. One HTTP click fans out to every notification and sends one tracker message per notification. It is reliable through its own structure. |
| [`campaign-windowed.scxml`](campaign-windowed.scxml) | `campaign/<id>` | The same job clocked by acknowledgements. Its use of the pool and of c2h is bounded by its window, not by the batch size, so it cannot hit a capacity error and its slot never waits long for c2h. A full c2h fails neither chart, so the window is good practice for bounding work in flight, not a requirement. |
| [`progress.scxml`](progress.scxml) | `progress/<id>` | A derived actor. It resyncs from the campaign instead of being recorded and replayed. |

## campaign.scxml

| Structure | Why | Scenario |
|---|---|---|
| `attempt` schedules the timeout `t` in its `onentry`, before `sending` fans out | A capacity error abandons the rest of its block ([macrostep](../reference/macrostep.md)). A timer scheduled after the fanout would be lost with it. | [S02](../scenarios/S02-full-pool.md) |
| `error.execution.capacity` in `attempt` → `throttled` | The pool or the name area was full partway through the fanout. Every notification that went out is in `sent`. The rest wait. A full c2h raises nothing: the slot waits for the host to drain, and the fanout goes on ([macrostep](../reference/macrostep.md#a-full-c2h-pauses-the-slot)). | [S02](../scenarios/S02-full-pool.md) |
| `throttled` swallows its own `error.execution.capacity` | Its resume timer may itself find no room. Without this transition the error would re-enter `throttled` and loop until the microstep limit stops it. | [S02](../scenarios/S02-full-pool.md) |
| `outcome.received`, raised on every acknowledgement or rejection | An outcome means the system has drained some work. It is the cheapest signal to try again. | [S02](../scenarios/S02-full-pool.md) |
| `error.communication` with `_event.sendid in idOf` | A notification never reached the processor. It leaves `sent`, so the next attempt sends it again. | [S01](../scenarios/S01-fanout.md) |
| A bare `error.communication` | Tracker mail that could not be delivered. Progress is optional, so the failure is ignored. | [S07](../scenarios/S07-unloaded-tracker.md) |
| `sending.timeout` → `retrying` → `attempt`, up to `maxAttempts` | Anything that was lost (a notification, an acknowledgement, a reply) is covered by sending everything still pending again. | [S04](../scenarios/S04-crash-campaign-recorded.md), [S09](../scenarios/S09-edge-backpressure.md) |
| `send.requested` while `active` → `attempt` | A second click answers the new request and sends what is pending again. The notify processor removes duplicates by its own key. | [S08](../scenarios/S08-cancel-and-second-click.md) |
| `progress.sync`, answered in every state | Lets the tracker rebuild its view at any time. | [S04](../scenarios/S04-crash-campaign-recorded.md), [S05](../scenarios/S05-crash-both-recorded.md) |
| `complete` answers later clicks with `ok` | A click after success is idempotent. | [S03](../scenarios/S03-crash-unrecorded.md) |

## campaign-windowed.scxml

- **The window.** An attempt sends `windowSize` notifications. Each
  acknowledgement or rejection raises `window.slot`, which sends one more.
- **Failures.** A communication error puts the notification back in the
  queue but keeps its slot taken. A processor that is down is not hammered,
  and the next attempt starts with a full window.
- **Progress.** One absolute `progress.snapshot` per macrostep, instead of
  one delta per notification.
- **The bound.** Per macrostep it writes at most `windowSize` `c2h.send`
  records and puts at most two envelopes in the pool, for 10 notifications
  or 10 million.

## progress.scxml

- **Bootstrap.** The tracker starts in `syncing`. It sends `progress.sync`
  to the campaign and waits for a `progress.snapshot`. A 5 second
  `sync.timeout` makes it ask again.
- **Deltas.** It applies `progress.sent`, `acked`, `rejected` and `failed`
  in any state. Mail between two sessions arrives in send order
  ([envelope](../reference/envelope.md)), so a delta sent before the
  snapshot is overwritten by it and never counted twice.
- **Updates.** Every change pushes `progress.update` to the browsers over
  `x-ws`.
- **Resync.** `progress.resync`, from an operator or a browser, starts
  `syncing` again.
- **No recording.** The tracker is the example of an actor that should not
  be recorded ([S05](../scenarios/S05-crash-both-recorded.md)).
