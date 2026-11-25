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

One run of it at a million of each (2026-11-25; Intel Xeon @ 2.10GHz,
4 CPUs, 16 GiB, Linux; node 22.23.5), on the native datamodel and on
quickjs with an 8 GiB heap:

```output
node 22.23.5, datamodel native, tracing on, 1000000 of each

                              core reserved  core used  host other  quickjs  accounted  process  blob
per actor, started                    196 B      179 B         0 B               196 B    532 B
per actor, after an event             196 B      179 B         0 B               196 B    673 B
per actor, hibernated                  82 B       81 B         0 B                82 B    118 B 145 B
per message waiting                   106 B      106 B         0 B               106 B    309 B
per delayed send pending              151 B      151 B         0 B               151 B    353 B

core's records: actor 81 B, event 106 B, timer 45 B
```

```output
node 22.23.5, datamodel quickjs, tracing on, 1000000 of each

                              core reserved  core used  host other  quickjs  accounted  process  blob
per actor, started                    196 B      179 B         0 B    600 B      796 B   1036 B
per actor, after an event             196 B      285 B         0 B   1176 B     1372 B   1634 B
per actor, hibernated                  82 B       81 B         0 B      0 B       82 B     92 B 147 B
per message waiting                   106 B      106 B         0 B    178 B      284 B    309 B
per delayed send pending              151 B      151 B         0 B    178 B      329 B    358 B

core's records: actor 81 B, event 106 B, timer 45 B
```

The process column by population, from the same run (bytes per thing):

| runtime, datamodel, row | 10,000 | 100,000 | 1,000,000 | 2,000,000 | 4,000,000 |
|---|---|---|---|---|---|
| node, native, actor, started | 803 B | 720 B | 532 B | 510 B | 495 B |
| node, native, actor, after an event | 1446 B | 870 B | 673 B | 654 B | 639 B |
| node, native, actor, hibernated | 637 B | 269 B | 118 B | 100 B | 91 B |
| node, native, message waiting | 602 B | 392 B | 309 B | 293 B | 285 B |
| node, native, delayed send pending | 737 B | 489 B | 353 B | 338 B | 331 B |
| node, quickjs, actor, started | 1141 B | 958 B | 1036 B | 1034 B | 1033 B |
| node, quickjs, actor, after an event | 1833 B | 1566 B | 1634 B | 1632 B | 1631 B |
| node, quickjs, actor, hibernated | 741 B | 159 B | 92 B | 88 B | 85 B |
| node, quickjs, message waiting | 563 B | 336 B | 309 B | 307 B | 306 B |
| node, quickjs, delayed send pending | 629 B | 382 B | 358 B | 356 B | 355 B |
| bun, native, actor, started | 1081 B | 661 B | 601 B | 589 B | 589 B |
| bun, native, actor, after an event | 1677 B | 887 B | 781 B | 746 B | 748 B |
| bun, native, actor, hibernated | 802 B | 155 B | 93 B | 87 B | 84 B |
| bun, native, message waiting | 760 B | 473 B | 386 B | 372 B | 373 B |
| bun, native, delayed send pending | 1275 B | 557 B | 448 B | 434 B | 433 B |
| bun, quickjs, actor, started | 1153 B | 965 B | 1041 B | 1045 B | 1034 B |
| bun, quickjs, actor, after an event | 1773 B | 1573 B | 1645 B | 1638 B | 1635 B |
| bun, quickjs, actor, hibernated | 571 B | 135 B | 90 B | 86 B | 84 B |
| bun, quickjs, message waiting | 511 B | 339 B | 313 B | 308 B | 306 B |
| bun, quickjs, delayed send pending | 550 B | 371 B | 356 B | 355 B | 355 B |

Every count of the run completed. The other columns are the same on
node and bun, and vary by at most a byte between counts from 100,000 up.

## What the columns mean

Each column but process comes from a count that is exact: the core's
definitions, its own counters, or the bytes an allocator was asked for.

- **core reserved** is the core's region for one more of the thing: what
  a host sized for the count holds beyond one sized for none of it. The
  host holds it from its creation, whether the things exist or not, so
  plan with this number. For an actor it is its record (81 B), the
  instance of the image's largest chart, and its share of the string
  slab (16 B by default).
- **core used** is what the things use of that region, from the core's
  own counters (`host.stats`' pools): the records in use of each pool
  times the record's size, which the last line prints, and the slab of
  instances and strings. A hibernated actor keeps its record and gives
  its instance back; a name it was spawned with stays in the string slab.
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
  1000 actors in memory and the rest in a store, measured once the last
  1000 are hibernated too. It uses 81 B of records (the reservation is
  81 B to 83 B, its share of the region's indexes included), and
  its blob is in the store; its instance went back to the slab, and a
  name it was spawned with would stay in the string slab (the default
  config reserves 16 B of strings per actor).
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

[`results/2026-11-25.csv`](results/2026-11-25.csv) holds the run the
tables above come from: node and bun, each datamodel, at 10,000,
100,000, 1,000,000, 2,000,000 and 4,000,000 of each thing, on one
machine with nothing else running (2026-11-25; Intel Xeon @ 2.10GHz,
4 CPUs, 16 GiB, Linux; node 22.23.5, bun 1.4.1). It came from:

```sh
for runtime in node bun; do
  $runtime examples/memory/main.mjs --counts 10000,100000,1000000,2000000,4000000 \
    --datamodel native --csv examples/memory/results/2026-11-25.csv
  $runtime examples/memory/main.mjs --counts 10000,100000,1000000,2000000,4000000 \
    --datamodel quickjs --quickjs-memory-limit 8589934592 --csv examples/memory/results/2026-11-25.csv
done
```

Each line is one row at one count: `runtime`, `datamodel`, `tracing`,
`count`, `row`, `core_reserved`, `core_used`, `host_other`, `quickjs`,
`accounted`, `process`, `blob` (bytes), `failed` (what stopped a row
that failed), `machine` and `date`. Plot `process` against `count` for
each `row` to see the fixed costs spread.

[`results/2026-11-20.csv`](results/2026-11-20.csv) is an earlier run of
the program before it accounted each part: its columns are `memory` (the
whole resident set, which also counted the runtime's code paged in),
`heap`, `core_reserves` and `blob`.

A new run goes into a new file under `results/`, named by its date; the
earlier files stay. The root `AGENTS.md` ("Published numbers") says when
to run it again and which documents quote it.

[Capacity and memory](../../docs/guide/capacity.md) says how to size a
host. `mise run layout` in `tinyactors-core` lists the core's records.
