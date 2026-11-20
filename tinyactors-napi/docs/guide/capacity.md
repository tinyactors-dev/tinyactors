# Capacity and memory

This guide is part of operating a host (the README's [Operating a
host](../../README.md#operating-a-host)).

A host keeps its actors in blocks of memory it sizes from its `config`,
one block (a **region**) per segment, that is per image. You can
leave the sizes to the host, which grows a region when something in it
runs out, or size a host up front for the population you expect. This
guide says what the sizes are, how a region grows, how to read what a
host uses, and what an actor costs.

## The sizes

The `config` option's `system` map gives each new segment its first sizes. Each
key is a whole number; an absent key, or 0, is the default:

| Key | Default | What it counts |
|---|---|---|
| `maxActors` | 64 | actors, hibernated ones included |
| `maxEvents` | 256 | events waiting in the actors' queues |
| `maxTimers` | 64 | pending delayed sends |
| `maxRequests` | 16 | running invocations (child charts and services) |
| `workspaces` | 4 | actors in the middle of a step at once (a sender keeps its workspace until its send is handled); never refused: an actor waits for one, and the host adds more |
| `instanceBytes` | derived from the image and `maxActors` | the actors' variables and states |
| `stringBytes` | 16384 + 16 × `maxActors` | strings the actors hold |
| `ticksPerSecond` | 1,000,000,000 | the host's clock rate |

A size is a segment's first size, not a limit. When a pool fills, the
host grows that segment between runs, into a new region with more of what
ran out and the rest as it was. A pool doubles, or grows by more, up to 16 times
at once, when doubling would add less than a quarter of the region, so a
small pool beside a large region grows in a few copies, and in proportion
to itself. A grown region
never shrinks. A run or an input is refused (`TA_ERROR_NO_MEMORY`) only
when a pool is at its largest size (`maxActors` is at most 8,388,606) or
the system allocator refuses the memory.

## Reading what a host uses

`host.hostStats` ([Inspecting a host](inspection.md)) answers with
`pools`, an array of one entry per live segment, in segment order. Each
entry has its `segment` (the image's number: its place among `new Host`'s charts, or what `addImage` returned) and, for
`actors`, `events`, `timers`, `workspaces`, `requests` and `slabBytes`
(the instance and string memory), the pool's `capacity`, what it holds
now (`used`) and the most it held at once (`highWater`). Each segment
grows on its own, so their pools are reported apart: two images, or a
migration's old and new versions, are two entries.

```js
import { compileCharts, Host } from '@tinyactors/node';

const compiled = compileCharts([{ name: 'idle.scxml', text: `
<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="idle"><state id="waiting"/></scxml>` }]);
const image = compiled.image;
compiled.close();

for (const system of [{}, { maxActors: 1000 }]) {
  const host = new Host(image, { system: 'demo', config: { system } });
  const before = (await host.hostStats()).pools[0].actors;
  for (let i = 0; i < 1000; i++) host.spawn('idle');
  const after = (await host.hostStats()).pools[0].actors;
  console.log(`maxActors ${system.maxActors ?? 'default'}: capacity ${before.capacity}, ` +
    `then ${after.used} used, high water ${after.highWater}, grown: ${after.capacity > before.capacity}`);

  host.addImage(image);
  console.log('segments reported:', (await host.hostStats()).pools.map((pool) => pool.segment).join(', '));
  host.close();
}
```

```output
maxActors default: capacity 64, then 1000 used, high water 1000, grown: true
segments reported: 0, 1
maxActors 1000: capacity 1000, then 1000 used, high water 1000, grown: false
segments reported: 0, 1
```

`highWater` against `capacity` is what a config sized up front needs: run
a realistic load, read the high-water marks, and give the next host those
sizes. `host.hostConfig()` answers the sizes a host was made with, every
default filled in.

## Sizing a host up front

Growing costs a copy of the region each time, and a grown region keeps
its size, so a host that will hold a known population is better sized for
it. A hibernated actor keeps its slot and its record in the host (about
64 bytes), so `maxActors` counts every actor, while the instance and
string memory are needed for the resident ones only. For `R` actors
resident out of `N` on a counter chart of a few variables, the
repository's hibernation gate uses:

```js
import { Host } from '@tinyactors/node';

const N = 2_000_000; // actors
const R = 1000;      // resident at most
const room = 64;
const host = new Host([], { system: 'big', config: { system: {
  maxActors: N + room, maxEvents: 4096,
  instanceBytes: 2048 * (R + room), stringBytes: 16384 + 64 * (R + room),
} } });
console.log((await host.hostConfig()).system.maxActors);
host.close();
```

```output
2000064
```

A chart with more variables or longer strings needs more per resident
actor; read `slabBytes.highWater` under load to see.

## What an actor costs

The [memory program](https://github.com/tinyactors-dev/tinyactors/tree/main/tinyactors-napi/examples/memory) measures what one more actor, message and
delayed send costs in a process: its resident memory, the host
included. Run it for your runtime, datamodel, chart and data:

```sh
node examples/memory/main.mjs --counts 1000000 --datamodel native
```

One run of it (2026-11-20; Intel Xeon @ 2.10GHz, 4 CPUs, 16 GiB, Linux;
node 22.23.5, bun 1.4.1; a counter chart with one `<data>`, messages of
`{"text": "hello"}`, tracing on). The hibernated row keeps at most 1000
actors resident and the rest in a store.

| runtime, datamodel | actor, started | actor, after an event | actor, hibernated | message waiting | delayed send pending |
|---|---|---|---|---|---|
| node, native | 531 B | 671 B | 124 B | 315 B | 357 B |
| bun, native | 618 B | 800 B | 101 B | 386 B | 456 B |
| node, quickjs | 1041 B | 1639 B | 97 B | 312 B | 364 B |
| bun, quickjs | 1049 B | 1653 B | 99 B | 318 B | 364 B |

The fixed costs of a host and of the engine spread over the population,
so an actor costs less in a larger one. Node on the native datamodel:

| | 10,000 | 100,000 | 1,000,000 | 2,000,000 | 4,000,000 |
|---|---|---|---|---|---|
| per actor, started | 1327 B | 774 B | 531 B | 519 B | 501 B |
| per actor, after an event | 1956 B | 920 B | 671 B | 665 B | 644 B |
| per actor, hibernated | 1147 B | 326 B | 124 B | 103 B | 92 B |
| per message waiting | 1146 B | 444 B | 315 B | 296 B | 286 B |
| per delayed send pending | 1296 B | 544 B | 357 B | 341 B | 332 B |

Each row's data, at every population and on every runtime, is in
[the run's CSV](https://github.com/tinyactors-dev/tinyactors/blob/main/tinyactors-napi/examples/memory/results/2026-11-20.csv), for a chart of your own.

- **A started actor** holds its variables in the datamodel and its
  records in the core: 196 B for this chart at a million (81 B, its
  instance and its strings).
- **After an event** an actor keeps that event as `_event` until the
  next one.
- **A hibernated actor** keeps its core records only, 81 B to 89 B, and
  its blob is in your store (145 B on the native datamodel, 147 B on
  quickjs, for this chart).
- **A message** costs 106 B in the core (57 B with tracing off) and its
  data in the datamodel. **A delayed send** holds its message until it
  falls due: 151 B in the core (102 B with tracing off).
- **quickjs** keeps every actor's variables in its own heap, bounded by
  `quickjs.memoryLimit`, 1 GiB unless the host sets another. A million
  resident actors after an event need about 1.2 GB of it, so the run
  above sets 8 GiB ([the quickjs sandbox](quickjs.md#the-limits)).

What a chart's scripts declare adds nothing per actor: its helpers are
the chart's, made once, and a top-level `var` is one more variable
([Writing charts](charts.md#scripts)).

## Work does not grow with the population

`tools/scale.mjs` (`mise run scale` in the repository) times every
operation of the API on a host of N actors and on one of 2N, each in a
process of its own. It fails when an operation costs more than 1.5 times
as much at 2N: per call for an operation on one actor, per actor for one
on the whole population. Most of the actors are hibernated in SQLite and
one in a hundred runs an invoked service; then the same with every actor
resident.

One run of it, alone on the machine (2026-11-20; Intel Xeon @ 2.10GHz,
4 CPUs, 16 GiB, Linux; node 22.23.5, bun 1.4.1). Each cell is the cost at
N, then at 2N, in microseconds, and their ratio. Every operation passed.

Hibernated: 2,000,000 and 4,000,000 actors, at most 1000 counters
resident, 20,000 and 40,000 actors each running an invoked service.

| operation | unit | node, native | node, quickjs | bun, native | bun, quickjs |
|---|---|---|---|---|---|
| populate (spawn and hibernate) | µs per actor | 10.88 → 10.56 (0.97) | 7.43 → 7.56 (1.02) | 7.52 → 7.81 (1.04) | 6.26 → 6.02 (0.96) |
| spawn | µs per call | 5.25 → 5.19 (0.99) | 2.62 → 2.60 (0.99) | 4.11 → 4.16 (1.01) | 2.43 → 2.45 (1.01) |
| terminate | µs per call | 0.867 → 0.786 (0.91) | 0.571 → 0.575 (1.01) | 0.952 → 0.893 (0.94) | 0.751 → 0.720 (0.96) |
| send | µs per call | 3.79 → 3.64 (0.96) | 1.90 → 1.77 (0.93) | 3.19 → 3.00 (0.94) | 2.12 → 2.06 (0.97) |
| sendMany | µs per event | 3.98 → 3.86 (0.97) | 2.18 → 2.04 (0.93) | 2.87 → 2.83 (0.99) | 2.01 → 1.85 (0.92) |
| send (hibernated: woken from the store) | µs per call | 32.27 → 27.93 (0.87) | 28.29 → 27.92 (0.99) | 24.71 → 25.96 (1.05) | 33.43 → 33.10 (0.99) |
| hibernate | µs per call | 5.41 → 4.93 (0.91) | 4.21 → 4.17 (0.99) | 3.56 → 3.44 (0.97) | 4.07 → 3.86 (0.95) |
| wake | µs per call | 6.94 → 6.38 (0.92) | 3.90 → 3.69 (0.95) | 6.91 → 7.27 (1.05) | 4.19 → 4.43 (1.06) |
| actor (by name) | µs per call | 3.96 → 4.01 (1.01) | 3.96 → 3.88 (0.98) | 1.32 → 1.20 (0.91) | 1.13 → 1.12 (0.99) |
| actorState | µs per call | 4.26 → 4.01 (0.94) | 4.15 → 4.11 (0.99) | 3.90 → 3.73 (0.96) | 3.66 → 3.58 (0.98) |
| hostStats | µs per call | 25.65 → 25.05 (0.98) | 24.49 → 23.49 (0.96) | 19.35 → 19.60 (1.01) | 18.96 → 18.23 (0.96) |
| I/O processor round trip | µs per call | 10.11 → 10.16 (1.01) | 6.81 → 6.73 (0.99) | 7.09 → 6.87 (0.97) | 5.77 → 5.54 (0.96) |
| transport round trip | µs per call | 30.55 → 31.31 (1.02) | 25.92 → 26.36 (1.02) | 25.19 → 26.15 (1.04) | 22.05 → 23.06 (1.05) |
| delayed send and its timer | µs per call | 6.28 → 6.19 (0.98) | 3.09 → 3.01 (0.97) | 4.95 → 5.15 (1.04) | 3.35 → 3.37 (1.01) |
| invoked service round trip | µs per call | 13.52 → 13.93 (1.03) | 10.33 → 10.12 (0.98) | 10.13 → 10.66 (1.05) | 9.15 → 9.79 (1.07) |
| sweep (every actor) | µs per actor | 0.006 → 0.005 (0.87) | 0.006 → 0.005 (0.87) | 0.006 → 0.006 (0.95) | 0.006 → 0.006 (0.91) |
| migrate (one session) | µs per call | 24.09 → 25.32 (1.05) | 16.51 → 16.95 (1.03) | 35.62 → 20.64 (0.58) | 16.90 → 16.30 (0.96) |
| snapshot | µs per actor | 0.100 → 0.092 (0.92) | 0.079 → 0.084 (1.07) | 0.081 → 0.087 (1.08) | 0.101 → 0.092 (0.92) |
| restore (and its first turn) | µs per actor | 0.291 → 0.298 (1.02) | 0.217 → 0.215 (0.99) | 0.223 → 0.217 (0.98) | 0.198 → 0.192 (0.97) |
| close | µs per actor | 0.114 → 0.128 (1.13) | 0.120 → 0.157 (1.31) | 0.126 → 0.127 (1.01) | 0.120 → 0.139 (1.16) |

All resident: 1,000,000 and 2,000,000 actors. Two million resident
actors take up to 5.2 GiB on quickjs, so four million do not fit in 16
GiB beside the scenario's other work.

| operation | unit | node, native | node, quickjs | bun, native | bun, quickjs |
|---|---|---|---|---|---|
| populate (spawn, the segment growing) | µs per actor | 5.42 → 5.52 (1.02) | 4.28 → 3.85 (0.90) | 4.23 → 4.14 (0.98) | 3.76 → 3.63 (0.97) |
| spawn | µs per call | 4.84 → 4.86 (1.00) | 2.47 → 2.34 (0.95) | 4.06 → 4.14 (1.02) | 2.47 → 2.36 (0.95) |
| send | µs per call | 5.82 → 5.89 (1.01) | 3.88 → 3.97 (1.02) | 4.88 → 4.90 (1.00) | 4.01 → 4.19 (1.05) |
| hibernate | µs per call | 6.29 → 6.54 (1.04) | 6.89 → 6.31 (0.92) | 4.54 → 4.92 (1.09) | 6.78 → 6.85 (1.01) |
| wake | µs per call | 7.48 → 7.37 (0.99) | 2.85 → 2.59 (0.91) | 5.08 → 5.45 (1.07) | 3.19 → 3.14 (0.98) |
| snapshot | µs per actor | 4.49 → 4.23 (0.94) | 5.51 → 5.65 (1.03) | 3.50 → 3.20 (0.91) | 6.11 → 5.59 (0.92) |
| migrate (a chart, every actor) | µs per actor | 17.99 → 17.80 (0.99) | 11.44 → 10.58 (0.93) | 14.38 → 14.06 (0.98) | 11.59 → 9.99 (0.86) |
| restore (and its first turn) | µs per actor | 13.04 → 12.69 (0.97) | 4.35 → 4.96 (1.14) | 7.32 → 6.98 (0.95) | 4.57 → 4.79 (1.05) |
| close | µs per actor | 0.310 → 0.309 (0.99) | 1.71 → 1.53 (0.90) | 0.296 → 0.323 (1.09) | 1.74 → 1.56 (0.90) |

Peak resident memory, at N and 2N: hibernated node native 1733 / 2579
MiB; node quickjs 1773 / 3382 MiB; bun native 1680 / 3251 MiB; bun quickjs
1345 / 2554 MiB; all resident node native 1706 / 3007 MiB; node quickjs
2555 / 5274 MiB; bun native 2271 / 3828 MiB; bun quickjs 2517 / 5278 MiB.
The run's files, every operation and sample, are in
[tools/results/2026-11-20](https://github.com/tinyactors-dev/tinyactors/tree/main/tinyactors-napi/tools/results/2026-11-20) in the repository; `mise run scale`
writes them to `build/scale-*.json`. The root `AGENTS.md` ("Published
numbers") says when to run it again.
