# Scenarios

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

Walkthroughs of the [stress-test charts](../charts/README.md) through the
protocol. Each step has a stable number (`S04.3`) so that
[BEHAVIORS.md](../BEHAVIORS.md) and the reference documents can cite it.
Each step cites the records it uses by their stable ID and code, which
link to the [generated message catalog](../protocol/generated/MESSAGES.md).
Each scenario ends with a table of who drops what and who retries how.

| Scenario | What it shows |
|---|---|
| [S01: Fanout](S01-fanout.md) | The happy path, plus one rejection and one communication error: effects executable on receipt, completion fences, tracker updates, the timeout retry and the HTTP reply. |
| [S02: Full pool](S02-full-pool.md) | 10 notifications in a pool of 8: a capacity error abandons the fanout's block, admission pauses, the edge refuses HTTP. Then the windowed chart does the same job with no errors. |
| [S03: Crash, nothing recorded](S03-crash-unrecorded.md) | Everything in the process is lost. A second click and the notify processor's own key finish the job. |
| [S04: Crash, campaign recorded](S04-crash-campaign-recorded.md) | The campaign is replayed and published. Its timeout survives in the pool. The tracker shows zero until it resyncs. |
| [S05: Crash, both recorded](S05-crash-both-recorded.md) | Per-session replay does not restore in-flight mail. Resync reads current state. Whole-system replay seeds the sessions together and generates new mail, without exact recovery. |
| [S06: Checkpoint](S06-checkpoint.md) | `h2c.checkpoint` and `c2h.state` truncate the log. A restart loads the state and replays the tail. |
| [S07: Unloaded tracker](S07-unloaded-tracker.md) | Mail waits in the pool for an unloaded address, and `c2h.load_needed` brings it back. A failed load recycles the waiting mail into `error.communication`. |
| [S08: Cancel and a second click](S08-cancel-and-second-click.md) | Timers are envelopes, so `<cancel>` is exact in either order of a click and a timeout. |
| [S09: Edge backpressure](S09-edge-backpressure.md) | h2c is full: the host refuses HTTP, and an acknowledgement the host drops is covered by the chart's timeout. |
| [S10: Run-time names](S10-run-time-names.md) | A client posts random event names. They live in the name area, nothing is interned, and memory does not grow. |

## Conventions

- **Replay.** Replay seeds new execution. Illustrated configuration, values
  and pool occupancy are examples, not exact restoration guarantees. Live
  effects may execute on receipt, independently of fences and journaling.
- **Sessions.** `s1` is `campaign/42`, `s2` is `progress/42`. Macrosteps
  are numbered per session: `s1.m3`.
- **Values.** `v1`, `v2`, … are value references into the binding's value
  table ([data model](../reference/data-model.md)). Data model calls go
  through the vtable and are not ring traffic. Steps mention them only
  where they matter.
- **Rings.** `h2c` is host → core, `c2h` is core → host (lossless), `tel` is
  telemetry (lossy) ([ring](../reference/ring.md)).
- **Occupancy.** `pool 6/8` counts envelopes: admitted inputs until their
  macrostep's fence, mail, and delayed sends ([envelope](../reference/envelope.md)).
- **Sendids.** `#_send.<n>` is a generated send ID, the sending session's
  counter (core ADR 0048).
- **Times.** `t0` is the click. Durations come from the charts.
