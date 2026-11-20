# Memory

This program measures what an actor, a message and a delayed send cost
on your interpreter. It prints whole bytes per thing.

```sh
cd examples/memory
python main.py
python main.py --datamodel ecmascript
python main.py --chart my-chart.scxml --data '{"text": "a longer message"}'
```

```
Python 3.13.18, datamodel python, tracing on, 100000 of each

                                memory  Python heap  core reserves
per actor, started               349 B         98 B          195 B
per actor, after an event        655 B        394 B          195 B
per message waiting              361 B        215 B          106 B
per delayed send pending         408 B        215 B          151 B
```

## What the columns mean

- **memory** is what the process holds more for each thing, the host
  included. Plan with this number.
- **Python heap** is the part Python's objects take: on the `python`
  datamodel, an actor's variables and a message's data. quickjs keeps its
  own heap, which only memory shows.
- **core reserves** is the core's records for each thing. The host
  reserves them when it sizes its pools, and they count in memory once
  used.

## What the rows mean

- **per actor, started:** an actor that ran its first step and waits.
- **per actor, after an event:** the same actor after it handled one
  event. It keeps that event as `_event` until the next one.
- **per message waiting:** an event in an actor's mailbox, before the
  actor handles it.
- **per delayed send pending:** a `<send delay>` that has not fallen due.
  It holds its message until then.

A hibernated actor keeps only its core records, 81 B. Its variables are
in your store.

## Options

| Option | Default | What it changes |
|---|---|---|
| `--count N` | 100000 | how many of each it makes |
| `--datamodel python\|ecmascript` | `python` | the datamodel of the measured charts |
| `--no-tracing` | tracing on | the host's `tracing` |
| `--chart file.scxml` | a counter | the chart of the actors it measures; its `datamodel` decides |
| `--data JSON` | `{"text": "hello"}` | the data of each message and delayed send |

## How it measures

Each row runs in a process of its own, twice: once for memory and once
for the Python heap, since `tracemalloc` takes memory of its own. The
host's pools are sized for the count up front, so no pool grows during
the measurement. It takes the memory, makes the things, and takes it
again. The difference, divided by the count, is the row.

The numbers vary by a few bytes between runs. A small count shows the
fixed costs of a host more. Use 100,000 or more.

The Python README's "Sizing the config" says how to size a host.
`mise run layout` in `tinyactors-core` lists the core's records.
