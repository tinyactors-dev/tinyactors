# Memory

This program measures what an actor, a message and a delayed send cost
on your runtime. It prints whole bytes per thing.

```sh
node examples/memory/main.mjs                       # or: bun examples/memory/main.mjs
node examples/memory/main.mjs --datamodel quickjs
node examples/memory/main.mjs --chart my-chart.scxml --data '{"text": "a longer message"}'
```

```output
node 22.23.5, datamodel native, tracing on, 100000 of each

                                memory         heap  core reserves
per actor, started               747 B        138 B          195 B
per actor, after an event        896 B        274 B          195 B
per message waiting              417 B         56 B          106 B
per delayed send pending         521 B         57 B          151 B
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
| `--datamodel native\|quickjs` | `native` | the datamodel |
| `--no-tracing` | tracing on | the host's `tracing` |
| `--chart file.scxml` | a counter | the chart of the actors it measures |
| `--data JSON` | `{"text": "hello"}` | the data of each message and delayed send |

## How it measures

Each row runs in a process of its own. It sizes the host's pools for the
count up front, so no pool grows during the measurement. It takes the
memory after a collection, makes the things, and takes it again. Memory
counts from before the host is made; the heap from after. The
difference, divided by the count, is the row.

The numbers vary by a few bytes between runs. A small count shows the
fixed costs of a host more. Use 100,000 or more.

[Capacity and memory](../../docs/guide/capacity.md) says how to size a
host. `mise run layout` in `tinyactors-core` lists the core's records.
