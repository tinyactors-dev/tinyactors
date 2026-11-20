# Memory

This program measures what an actor, a message and a delayed send cost
on your runtime. It prints whole bytes per thing.

```sh
node examples/memory/main.mjs                       # or: bun examples/memory/main.mjs
node examples/memory/main.mjs --counts 1000000
node examples/memory/main.mjs --datamodel quickjs --quickjs-memory-limit 8589934592
node examples/memory/main.mjs --chart my-chart.scxml --data '{"text": "a longer message"}'
```

```output
node 22.23.5, datamodel native, tracing on, 1000000 of each

                                memory      heap  core reserves    blob
per actor, started               531 B     138 B          196 B
per actor, after an event        671 B     274 B          196 B
per actor, hibernated            124 B       0 B           82 B   145 B
per message waiting              315 B      56 B          106 B
per delayed send pending         357 B      55 B          151 B
```

## What the columns mean

- **memory** is what the process holds more for each thing, the host
  included. Plan with this number.
- **heap** is the datamodel's part of it: the JavaScript engine's heap on
  the native datamodel, quickjs's own heap on quickjs. An actor's
  variables and a message's data are here.
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

Each row runs in a process of its own. It sizes the host's pools for the
count up front, so no pool grows during the measurement. It takes the
memory after a collection, makes the things, and takes it again. Memory
counts from before the host is made; the heap from after.

The host and the engine have fixed costs, which a small count shows
more. The numbers vary by a few bytes between runs.

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

A new run goes into a new file under `results/`, named by its date; the
earlier files stay. The root `AGENTS.md` ("Published numbers") says when
to run it again and which documents quote it.

[Capacity and memory](../../docs/guide/capacity.md) says how to size a
host. `mise run layout` in `tinyactors-core` lists the core's records.
