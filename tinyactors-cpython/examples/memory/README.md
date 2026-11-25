# Memory

This program measures what an actor, a message and a delayed send cost
on your interpreter. It prints whole bytes per thing.

```sh
cd examples/memory
python main.py
python main.py --counts 1000000
python main.py --datamodel ecmascript
python main.py --chart my-chart.scxml --data '{"text": "a longer message"}'
```

```
python 3.13.18, datamodel python, tracing on, 1000000 of each

                                memory  Python heap  core reserves    blob
per actor, started               408 B         90 B          196 B
per actor, after an event        714 B        386 B          196 B
per actor, hibernated             85 B          0 B           82 B   145 B
per message waiting              325 B        209 B          106 B
per delayed send pending         371 B        209 B          151 B
```

## What the columns mean

- **memory** is what the process holds more for each thing, the host
  included: its own pages, not the code of mapped files. Plan with this
  number.
- **Python heap** is the part Python's objects take: on the `python`
  datamodel, an actor's variables and a message's data. quickjs keeps its
  own heap, which only memory shows.
- **core reserves** is the core's records for each thing. The host
  reserves them when it sizes its pools, and they count in memory once
  used.
- **blob** is what a hibernated actor leaves in your store.

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

Each row runs in a process of its own, twice: once for memory and once
for the Python heap, since `tracemalloc` takes memory of its own. The
host's pools are sized for the count up front, so no pool grows during
the measurement. It takes the memory, makes the things, and takes it
again. The difference, divided by the count, is the row. Memory counts
from before the host is made; the Python heap from after.

On Linux, memory is the process's anonymous pages, in memory or swapped
out (`RssAnon` and `VmSwap` in `/proc/self/status`): what the host, the
interpreter and quickjs allocate. It leaves out the pages of mapped
files, the code of the interpreter and the extension, which the kernel
drops when the machine runs short of memory and reads back when they
run again: they are no cost per thing, and dropped between the two
samples they made a row negative. On macOS, which has no `/proc`, it is
the peak resident set.

The numbers vary by a few bytes between runs.

## The data

[`results/2026-11-20.csv`](results/2026-11-20.csv) holds one run: both
datamodels at 10,000, 100,000, 1,000,000, 2,000,000 and 4,000,000 of
each thing, on one machine (2026-11-20; Intel Xeon @ 2.10GHz, 4 CPUs,
16 GiB, Linux; CPython 3.13.18). It came from:

```sh
for datamodel in python ecmascript; do
  python main.py --counts 10000,100000,1000000,2000000,4000000 \
    --datamodel $datamodel --csv results/2026-11-20.csv
done
```

Each line is one row at one count: `runtime`, `datamodel`, `tracing`,
`count`, `row`, `memory`, `heap`, `core_reserves`, `blob` (bytes),
`failed` (what stopped a row that failed), `machine` and `date`. Plot
`memory` against `count` for each `row`.

A new run goes into a new file under `results/`, named by its date; the
earlier files stay. The root `AGENTS.md` ("Published numbers") says when
to run it again and which documents quote it.

The Python README's "Memory" and "Sizing the config" say how to size a
host. `mise run layout` in `tinyactors-core` lists the core's records.
