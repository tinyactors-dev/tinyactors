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
