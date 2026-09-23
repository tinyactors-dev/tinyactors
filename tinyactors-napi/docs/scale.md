# Scale measurements

`test/scale.js` (ROADMAP step 10) checks that per-actor costs stay flat
from one population to the next. It asserts ratios and zero allocator calls
in steady state, never absolute durations. This file records the numbers it
printed, so later steps can compare against them.

Run it with a larger cap:

```sh
TAT_SCALE_MAX_POPULATION=4000000 node test/scale.js
TAT_SCALE_MAX_POPULATION=4000000 bun test/scale.js
```

## 2026-09-23: baseline without data (after step 9)

Chart: one `idle` state that takes `ping` (an external self-transition) and
`work` (to a top-level `<final>`). Default scheduling options. macOS, Apple
silicon, 48 GB. Node 22 and Bun 1. Times are CPU time per operation, except
the actor-ID page and the pump turns, which are wall time.

| | Node 4M | Bun 4M | Node 50k | Bun 50k |
|---|---|---|---|---|
| spawn (call), µs/actor | 0.49 | 0.35 | 0.64 | 0.51 |
| bootstrap, µs/actor | 0.10 | 0.11 | 0.11 | 0.13 |
| longest single spawn, µs | 70 | 540 | 78 | 230 |
| largest block allocated, bytes | 65,536 | 65,536 | 65,536 | 65,536 |
| post + delivery, µs/message | 0.32 | 0.24 | 0.22 | 0.22 |
| inspect, µs | 3.85 | 2.05 | 3.77 | 2.06 |
| actor-ID page of 1,024, µs | 8.6 | 7.9 | 8.7 | 7.5 |
| end to end (post `work` to all, flush), µs/actor | 0.61 | 0.45 | 0.67 | 0.50 |
| native bytes/actor (core + addon) | 480 | 480 | 483 | 483 |
| JS heap bytes/actor | 0.0 | 0.0 | 3.0 | 4.1 |
| RSS bytes/actor (from startup) | 512 | 529 | 693 | 939 |
| peak RSS, whole run | 2.7 GB | 2.7 GB | | |
| pump turns during flush, p50 / p99 / max, ms | 1.1 / 1.2 / 13–15 | 0.1 / 0.3 / 15–16 | 1.0 / 1.1 / 1.1 | 0.1 / 0.8 / 0.8 |
| total GC pause over the population's life, ms | 21 | n/a | 6 | n/a |
| addon allocator calls in steady state | 0 | 0 | 0 | 0 |
| core allocator calls per delivered message | 2 | 2 | 2 | 2 |

Plain-JavaScript baseline at 4M (one object per actor with a switch-based
handler, called directly): 40 bytes per object on the JS heap, and 0.027 µs
(Node) or 0.018 µs (Bun) per event.

Every ratio stayed within its bound on both runtimes. Notes:

- **Core allocates once per message for a chart with a top-level `<final>`.**
  The same chart without the final makes no allocator call per message. So
  core allocates and frees something each macrostep when the definition has
  a top-level final state. That is a core finding, left for a core change
  like ADRs 0063 and 0064. The addon's own count is zero.
- **The largest pump turn grows with the population, but p99 does not.** p50
  and p99 stay flat (on Node at the monitor's 1 ms resolution). The maximum
  is one outlier among about 4,000 turns at 4M, most likely a GC pause.
- **Longest single spawn** is noisy (allocator and page faults), as core ADR
  0063's notes found, and is not asserted. The largest block is asserted
  instead: one 64 KiB page at every population.
- **Inspect is the most expensive call per actor** (2–4 µs), because it
  builds a full JS inspection object. It is not a hot path.

## 2026-09-23: with data, and value-box models A and B (step 11)

Chart `data` (`scale.js`, `SCALE_VARIANTS=data`): the JavaScript data model.
Every actor has a data object `{ count, last }`; every `ping` carries a new
object `{ sequence, text }` as event data, and two functions assign from it
(`count + 1`, and `last = event.data`). That is the load of one actor per
websocket connection. Same machine and options as above.

| | Node 4M, model A | Node 4M, model B | Bun 4M, model A | Bun 4M, model B |
|---|---|---|---|---|
| spawn (call), µs/actor | 0.58 | 0.61 | 0.59 | 0.49 |
| bootstrap (data object, initializers), µs/actor | 0.73 | 0.82 | 0.76 | 0.61 |
| post + delivery with data, µs/message | 1.27 | 1.49 | 0.96 | 1.21 |
| inspect, µs | 4.06 | 4.13 | 2.18 | 2.17 |
| end to end, µs/actor | 0.63 | 0.66 | 0.51 | 0.49 |
| native bytes/actor (core + addon) | 528 | 528 | 528 | 528 |
| JS heap bytes/actor | 56 | 65 | 64 | 74 |
| RSS bytes/actor (from startup) | 788 | 713 | 932 | 752 |
| GC pause over the population's life, ms (minor / major) | 154 (106 / 44) | 126 (104 / 22) | n/a | n/a |
| pump turns during flush, p50 / p99 / max, ms | 1.1 / 1.3 / 32 | 1.2 / 1.3 / 27 | 0.2 / 1.2 / 23 | 0.2 / 0.3 / 24 |
| addon allocator calls in steady state | 0 | 0 | 0 | 0 |

- **Model A** is one `napi_ref` per value box (ADR 0011, the default).
  **Model B** is a handle table: an index into one JavaScript array per
  system (`TNA_VALUES_MODEL=table mise run build`).
- **The cost per message with data is about 1 µs**, against about 0.25 µs
  without data and xstate's ~2.7 µs (measured in the design discussion). It
  is two JavaScript calls (the assign functions), one value box for the
  payload, and the event object the context builds.
- **Model B halves major GC time** (the global handles are gone from the
  mark phase) and uses about 10% less RSS on Node, but posts are 17–26%
  slower (two Node-API calls per access instead of one) and minor GC is
  unchanged. Over the population's whole life the GC saving is 28 ms at 4M
  actors. By ADR 0011 point 3, model A stays: B is not clearly better.
- Every ratio from 50k to 4M stayed within its bound with data, on both
  runtimes and both models.

## 2026-09-23: a websocket-shaped load (step 13)

Chart `ws` (`scale.js`, `SCALE_VARIANTS=ws`): one actor per simulated
connection, with data (`{ count, last }`). Every inbound `ping` carries a new
object as event data; the transition assigns from it, sends it out through a
registered I/O processor (`ws.write` via `"ws"`, which counts the writes),
and re-enters the state, whose exit cancels the 30-second idle timeout and
whose entry arms it again. So every actor always holds one delayed send, and
every message cancels and arms one. Same machine and options as above.

| | Node 1M | Node 4M | Bun 4M |
|---|---|---|---|
| spawn (call), µs/actor | 0.57 | 0.57 | 0.59 |
| bootstrap (data, first timeout), µs/actor | 0.84 | 0.87 | 0.91 |
| inbound post + delivery + outbound write + re-arm, µs/message | 2.36 | 2.61 | 1.74 |
| inspect, µs | 5.21 | 5.36 | 2.95 |
| end to end, µs/actor | 0.96 | 1.02 | 0.90 |
| native bytes/actor (core + addon) | 1225 | 1224 | 1224 |
| JS heap bytes/actor | 56 | 56 | 64 |
| RSS bytes/actor (from startup) | 1564 | 1503 | 1679 |
| GC pause over the population's life, ms (minor / major) | 43 (25 / 16) | 153 (104 / 46) | n/a |
| pump turns during flush, p50 / p99 / max, ms | 0.9 / 1.8 / 15 | 0.9 / 1.9 / 56 | 0.4 / 1.0 / 66 |
| longest single spawn, µs | 6,132 | 6,057 | 40,599 |
| largest block, bytes (core's timer heap) | 8,388,608 | 33,554,432 | 33,554,432 |
| allocator calls in steady state, addon / core | 0 / 0 | 0 / 0 | 0 / 0 |

- **About 2.6 µs per message on Node and 1.7 µs on Bun** at 4M, for the
  whole round trip of one connection's message: the inbound post, two
  assign functions, one outbound I/O effect handed to JavaScript after the
  turn, and a cancel plus a new delayed send. Against the `data` chart
  (about 1 µs), the outbound write and the timer re-arm cost about 1.3 µs on
  Node. Every ratio from 50k to 4M stayed within its bound.
- **Holding a timeout per actor costs about 700 bytes** (1224 against 528
  for `data`): the actor's sparse state and its delayed-send list in core.
- **Core finding: the delayed-send owner heap doubles.** Core keeps one
  entry per actor with a pending send (core ADR 0054) in one array grown by
  doubling and copying (`ta_timer_reserve_heap` in
  `tinyactors-core/src/timer.c`). Its largest block is 512 KiB at 50k
  actors, 2 MiB at 200k, 8 MiB at 1M and 32 MiB at 4M, and the spawn that
  crosses a power of two copies all of it: the longest spawn is about 6 ms
  from 1M on (40 ms on Bun at 4M), where the `data` chart stays well under a
  millisecond. This is the spike core ADR 0063 removed from the actor table;
  the owner heap needs the same paging. `scale.js` prints it as a known core
  finding and holds the addon's own largest block flat.
- **The longest pump turn grows at 4M** (56–66 ms, one outlier among
  thousands of turns; p99 stays about 2 ms). It is most likely a GC pause or
  the heap copy above, not a turn's own work.
