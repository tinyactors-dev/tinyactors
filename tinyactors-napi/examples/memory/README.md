# Memory

This program measures what an actor, a message and a delayed send cost
on your runtime, part by part: what the core reserves and uses, what the
host allocates beside it, quickjs's heap, and what the process holds. It
prints whole bytes per thing.

```sh
node examples/memory/main.mjs                       # or: bun examples/memory/main.mjs
node examples/memory/main.mjs --counts 1000000
node examples/memory/main.mjs --datamodel quickjs --quickjs-memory-limit 8589934592
node examples/memory/main.mjs --chart my-chart.scxml --data '{"text": "a longer message"}'
```

The table below predates the accounted columns: it comes from the
earlier version of this program, whose columns were memory, heap, core
reserves and blob. It will be replaced at the next sweep of the
published numbers (the root `AGENTS.md`, "Published numbers"). Its
memory column is today's process, and its core reserves today's core
reserved.

```output
node 22.23.5, datamodel native, tracing on, 1000000 of each

                                memory      heap  core reserves    blob
per actor, started               531 B     138 B          196 B
per actor, after an event        671 B     274 B          196 B
per actor, hibernated            124 B       0 B           82 B   145 B
per message waiting              315 B      56 B          106 B
per delayed send pending         357 B      55 B          151 B
```

Today it prints, for 5000 of each on quickjs (one run on a machine that
was not quiet, so the process column is only an example):

```output
bun 1.4.4, datamodel quickjs, tracing on, 5000 of each

                              core reserved  core used  host other  quickjs  accounted  process  blob
per actor, started                    198 B      179 B         0 B    617 B      815 B   1350 B
per actor, after an event             198 B      285 B         0 B   1194 B     1392 B   1937 B
per actor, hibernated                  81 B      102 B         0 B    146 B      227 B    836 B 147 B
per message waiting                   106 B      106 B         0 B    178 B      284 B    683 B
per delayed send pending              151 B      151 B         0 B    180 B      331 B    662 B

core's records: actor 81 B, event 106 B, timer 45 B
```

## What the columns mean

Each column but process comes from a count that is exact: the core's
definitions, its own counters, or the bytes an allocator was asked for.

- **core reserved** is the core's region for one more of the thing: what
  a host sized for the count holds beyond one sized for none of it. The
  host holds it from its creation, whether the things exist or not, so
  plan with this number. For an actor it is its record (81 B), the
  instance of the image's largest chart, and its strings.
- **core used** is what the things use of that region, from the core's
  own counters (`host.stats`' pools): the records in use of each pool
  times the record's size, which the last line prints, and the slab of
  instances and strings. A hibernated actor keeps its record and gives
  its instance back.
- **host other** is what the host allocates beside its regions
  (`allocatedBytes` less `regionBytes` in `host.stats`): nothing per
  thing in a host sized up front.
- **quickjs** is, on quickjs, the engine's heap and the C library's
  header of 16 B beside each of its blocks (`host.quickjsHeapBytes` and
  `host.quickjsHeapBlocks`). An actor's variables and a message's data
  are here.
- **accounted** is core reserved, host other and quickjs together.
- **process** is what the process holds more for each thing, the host
  included: its own pages, not the code of mapped files. It is the
  machine's and the engine's reading, never exact: the gap between it and
  accounted is mostly the JavaScript engine holding the chart's data (on
  the native datamodel, an actor's variables live in your engine's heap,
  which no column above sees), then malloc's overhead per block and the
  fixed costs of a host and an engine spread over the count.
- **blob** is what a hibernated actor leaves in your store.

The binding's own tables and arenas (its session rows, for one) are not a
column: the package has no API for them. Its tests count them.

## What the rows mean

- **per actor, started:** an actor that ran its first step and waits.
- **per actor, after an event:** the same actor after it handled one
  event. It keeps that event as `_event` until the next one.
- **per actor, hibernated:** an actor in a population that keeps at most
  1000 actors in memory and the rest in a store. It keeps its core
  records, 81 B to 89 B, and its blob is in the store.
- **per message waiting:** an event in an actor's mailbox, before the
  actor handles it.
- **per delayed send pending:** a `<send delay>` that has not fallen due.
  It holds its message until then.

A row that fails prints what stopped it. At its default 1 GiB, quickjs's
heap does not hold a million resident actors after an event:
`--quickjs-memory-limit` raises it.

## Options

| Option | Default | What it changes |
|---|---|---|
| `--counts N,N,...` | 100000 | how many of each it makes, one table per count |
| `--datamodel native\|quickjs` | `native` | the datamodel |
| `--no-tracing` | tracing on | the host's `tracing` |
| `--chart file.scxml` | a counter | the chart of the actors it measures |
| `--data JSON` | `{"text": "hello"}` | the data of each message and delayed send |
| `--quickjs-memory-limit BYTES` | 1 GiB | quickjs's heap limit (`quickjs.memoryLimit`) |
| `--csv file.csv` | none | appends every row to the file, for a chart |

## How it measures

Each row runs in a process of its own. It sizes every pool the row fills
for the count up front, so no pool grows during the measurement. Core
reserved is the difference between the regions of two hosts, one sized
for the count and one for none of it. The rest it reads before and after
making the things: `host.stats`, quickjs's heap and, after a collection,
the process's memory, which counts from before the host is made. The
record sizes come from two more hosts per pool, at 2^16 and 2^17 of it,
as the core checks its own (`tools/layout/layout.c` in
`tinyactors-core`); the example's test checks each core column against
that program, `ta-layout`, which `mise run build` builds beside the
addon.

On Linux, the process's memory is the process's anonymous pages, in memory or swapped
out (`RssAnon` and `VmSwap` in `/proc/self/status`): what the host, the
engine and quickjs allocate. It leaves out the pages of mapped files,
the code of the runtime and the addon, which the kernel drops when the
machine runs short of memory and reads back when they run again: they
are no cost per thing, and dropped between the two samples they made a
row negative. Elsewhere it is the resident set (`process.memoryUsage().rss`).

The host and the engine have fixed costs, which a small count shows
more in the process column. The other columns do not vary between runs.

## The data

[`results/2026-11-20.csv`](results/2026-11-20.csv) holds one run: node and bun, each datamodel, at
10,000, 100,000, 1,000,000, 2,000,000 and 4,000,000 of each thing, on one
machine (2026-11-20; Intel Xeon @ 2.10GHz, 4 CPUs, 16 GiB, Linux). It
came from:

```sh
for runtime in node bun; do
  $runtime examples/memory/main.mjs --counts 10000,100000,1000000,2000000,4000000 \
    --datamodel native --csv examples/memory/results/2026-11-20.csv
  $runtime examples/memory/main.mjs --counts 10000,100000,1000000,2000000,4000000 \
    --datamodel quickjs --quickjs-memory-limit 8589934592 --csv examples/memory/results/2026-11-20.csv
done
```

Each line is one row at one count: `runtime`, `datamodel`, `tracing`,
`count`, `row`, `memory`, `heap`, `core_reserves`, `blob` (bytes),
`failed` (what stopped a row that failed), `machine` and `date`. Plot
`memory` against `count` for each `row` to see the fixed costs spread.
That file predates the accounted columns; a run of this program writes
`core_reserved`, `core_used`, `host_other`, `quickjs`, `accounted`,
`process` and `blob` instead, and the next sweep will add such a file.

That run took memory as the whole resident set, which also counts the
code of the runtime paged in during a measurement, about 5 MB on node:
some 550 B a thing at 10,000 and 55 B at 100,000, and under 12 B from
1,000,000 up, where the table above comes from.

A new run goes into a new file under `results/`, named by its date; the
earlier files stay. The root `AGENTS.md` ("Published numbers") says when
to run it again and which documents quote it.

[Capacity and memory](../../docs/guide/capacity.md) says how to size a
host. `mise run layout` in `tinyactors-core` lists the core's records.
