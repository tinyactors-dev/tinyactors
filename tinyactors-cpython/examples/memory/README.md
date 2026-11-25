# Memory

This program measures what an actor, a message and a delayed send cost
on your interpreter, part by part: what the core reserves and uses, what
the host allocates beside it, quickjs's heap, and what the process
holds. It prints whole bytes per thing.

```sh
cd examples/memory
python main.py
python main.py --counts 1000000
python main.py --datamodel ecmascript
python main.py --chart my-chart.scxml --data '{"text": "a longer message"}'
```

One run of it at a million of each (2026-11-25; Intel Xeon @ 2.10GHz,
4 CPUs, 16 GiB, Linux; CPython 3.13.18 with the GIL), on both datamodels:

```
python 3.13.18, datamodel python, tracing on, 1000000 of each

                              core reserved  core used  host other  quickjs  accounted  process  blob
per actor, started                    196 B      179 B         0 B               196 B    408 B
per actor, after an event             196 B      179 B         0 B               196 B    714 B
per actor, hibernated                  82 B       81 B         0 B                82 B     85 B 145 B
per message waiting                   106 B      106 B         0 B               106 B    325 B
per delayed send pending              151 B      151 B         0 B               151 B    370 B

core's records: actor 81 B, event 106 B, timer 45 B
```

```
python 3.13.18, datamodel ecmascript, tracing on, 1000000 of each

                              core reserved  core used  host other  quickjs  accounted  process  blob
per actor, started                    196 B      179 B         0 B    600 B      796 B   1023 B
per actor, after an event     failed: InvalidError: TA_ERROR_INVALID
per actor, hibernated                  82 B       81 B         0 B      0 B       82 B     85 B 147 B
per message waiting                   106 B      106 B         0 B    178 B      284 B    305 B
per delayed send pending              151 B      151 B         0 B    178 B      329 B    354 B

core's records: actor 81 B, event 106 B, timer 45 B
```

The process column by population, from the same run (bytes per thing):

| datamodel, row | 10,000 | 100,000 | 1,000,000 | 2,000,000 | 4,000,000 |
|---|---|---|---|---|---|
| python, actor, started | 315 B | 349 B | 408 B | 408 B | 408 B |
| python, actor, after an event | 621 B | 655 B | 714 B | 714 B | 714 B |
| python, actor, hibernated | 100 B | 82 B | 85 B | 83 B | 83 B |
| python, message waiting | 338 B | 358 B | 325 B | 325 B | 325 B |
| python, delayed send pending | 402 B | 405 B | 370 B | 370 B | 370 B |
| ecmascript, actor, started | 867 B | 919 B | 1023 B | failed | failed |
| ecmascript, actor, after an event | 1474 B | 1521 B | failed | failed | failed |
| ecmascript, actor, hibernated | 176 B | 90 B | 85 B | 84 B | 83 B |
| ecmascript, message waiting | 256 B | 303 B | 305 B | 305 B | 305 B |
| ecmascript, delayed send pending | 303 B | 350 B | 354 B | 354 B | 354 B |

The rows marked failed are quickjs's heap, limited to 1 GiB: a million
started actors use 600 MB of it, and the events after them did not fit
(`InvalidError: TA_ERROR_INVALID`, as the program printed it). At two
and four million the spawns did not fit: the program printed `failed:
exit 1`, the job's `MemoryError` at `host.spawn`, which has no message
for it to quote. The other columns are the same at every count from
100,000 up, within a byte.

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
  own counters (`host_stats`' pools): the records in use of each pool
  times the record's size, which the last line prints, and the slab of
  instances and strings. A hibernated actor keeps its record and gives
  its instance back; a name it was spawned with stays in the string slab.
- **host other** is what the host allocates beside its regions
  (`allocatedBytes` less `regionBytes` in `host_stats`): nothing per
  thing in a host sized up front.
- **quickjs** is, on the `ecmascript` datamodel, quickjs's heap and the
  C library's header of 16 B beside each of its blocks
  (`host.quickjs_heap_bytes` and `host.quickjs_heap_blocks`). An actor's
  variables and a message's data are here.
- **accounted** is core reserved, host other and quickjs together.
- **process** is what the process holds more for each thing, the host
  included: its own pages, not the code of mapped files. It is the
  machine's and the interpreter's reading, never exact: the gap between
  it and accounted is mostly Python's objects holding the chart's data
  (on the `python` datamodel an actor's variables and a message's data
  are Python objects, which no column above sees), then malloc's
  overhead per block and the fixed costs of a host and an interpreter
  spread over the count.
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
  81 B to 83 B, its share of the region's indexes included), and its
  blob is in the store; its instance went back to the slab, and a name
  it was spawned with would stay in the string slab (the default config
  reserves 16 B of strings per actor).
- **per message waiting:** an event in an actor's mailbox, before the
  actor handles it.
- **per delayed send pending:** a `<send delay>` that has not fallen due.
  It holds its message until then.

A row that fails prints what stopped it. quickjs's heap is limited to
1 GiB, which the Python binding does not let you change: it holds about
a million started actors of the program's chart.

## Options

| Option | Default | What it changes |
|---|---|---|
| `--counts N,N,...` | 100000 | how many of each it makes, one table per count |
| `--datamodel python\|ecmascript` | `python` | the datamodel of the measured charts |
| `--no-tracing` | tracing on | the host's `tracing` |
| `--chart file.scxml` | a counter | the chart of the actors it measures; its `datamodel` decides |
| `--data JSON` | `{"text": "hello"}` | the data of each message and delayed send |
| `--csv file.csv` | none | appends every row to the file, for a chart |

## How it measures

Each row runs in a process of its own. It sizes every pool the row fills
for the count up front, so no pool grows during the measurement. Core
reserved is the difference between the regions of two hosts, one sized
for the count and one for none of it. The rest it reads before and after
making the things: `host_stats`, quickjs's heap and, after a collection,
the process's memory, which counts from before the host is made. The
record sizes come from two more hosts per pool, at 2^16 and 2^17 of it,
as the core checks its own (`tools/layout/layout.c` in
`tinyactors-core`); the example's test checks each core column against
that program, `ta-layout`, which `mise run build` builds beside the
extension.

On Linux, the process's memory is the process's anonymous pages, in memory or swapped
out (`RssAnon` and `VmSwap` in `/proc/self/status`): what the host, the
interpreter and quickjs allocate. It leaves out the pages of mapped
files, the code of the interpreter and the extension, which the kernel
drops when the machine runs short of memory and reads back when they
run again: they are no cost per thing, and dropped between the two
samples they made a row negative. On macOS, which has no `/proc`, it is
the peak resident set.

The process column varies between runs; the others do not.

## The data

[`results/2026-11-25.csv`](results/2026-11-25.csv) holds the run the
tables above come from: both datamodels at 10,000, 100,000, 1,000,000,
2,000,000 and 4,000,000 of each thing, on one machine with nothing else
running (2026-11-25; Intel Xeon @ 2.10GHz, 4 CPUs, 16 GiB, Linux;
CPython 3.13.18). It came from:

```sh
for datamodel in python ecmascript; do
  python main.py --counts 10000,100000,1000000,2000000,4000000 \
    --datamodel $datamodel --csv results/2026-11-25.csv
done
```

Each line is one row at one count: `runtime`, `datamodel`, `tracing`,
`count`, `row`, `core_reserved`, `core_used`, `host_other`, `quickjs`,
`accounted`, `process`, `blob` (bytes), `failed` (what stopped a row
that failed), `machine` and `date`. Plot `process` against `count` for
each `row`.

[`results/2026-11-20.csv`](results/2026-11-20.csv) is an earlier run of
the program before it accounted each part: its columns are `memory`,
`heap` (`tracemalloc`), `core_reserves` and `blob`.

A new run goes into a new file under `results/`, named by its date; the
earlier files stay. The root `AGENTS.md` ("Published numbers") says when
to run it again and which documents quote it.

The Python README's "Memory" and "Sizing the config" say how to size a
host. `mise run layout` in `tinyactors-core` lists the core's records.
