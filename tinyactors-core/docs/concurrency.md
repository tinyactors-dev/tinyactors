# Concurrency and parallelism

How tinyactors runs many actors, and what you can rely on. Read this before
you put actors behind a web server or spread them over threads. It builds
from one actor up to systems talking to each other.

Two words first, because they are easy to mix up:

- **Concurrency** — many actors in progress at the same time, interleaved.
  tinyactors gives you that everywhere, with one thread.
- **Parallelism** — several actors executing at the same instant on
  different cores. You get it by running several *systems* (threads,
  workers, processes). One system never
  runs two actors at the same instant.

## 1. One actor: run to completion

An actor (an SCXML session) handles one event at a time. Processing an event
is a *macrostep*: the transition, all eventless transitions it enables, and
all internal events (`<raise>`) — until the actor is *stable*. Only then does
it look at its next external event.

```
 mailbox:  e3  e2  e1 ──▶ ┌────────────────────────────────────┐
                          │ macrostep(e1): transitions,        │
                          │ eventless transitions, <raise>d    │ ──▶ stable ──▶ take e2
                          │ internal events, until stable      │
                          └────────────────────────────────────┘
```

What that buys you, without a single lock in your charts:

- Inside a macrostep the actor's data cannot change under it. Nobody else
  can see or modify its `<datamodel>`.
- Events from one sender arrive in the order they were sent.
- `_event` is the event being processed, for the whole macrostep.

What it does **not** mean: that a macrostep runs uninterrupted on the CPU.
See the next section.

## 2. One system: many actors, one thread, cooperative

A `ta_system` holds any number of actors. It runs them on the thread that
calls `ta_system_run(system, budget)`, one at a time, from a *run queue* of
actors that have work:

```
 run queue:  [A] [B] [C]          ta_system_run(sys, 100000 fuel)
              │
              ▼
           run A for one quantum ──▶ A stable?  ── yes ─▶ A leaves the queue
                                       │
                                       no (still in its macrostep)
                                       ▼
                                 A goes to the back: [B] [C] [A]
```

- **Fuel, not time.** Every step of work costs fuel (a transition, a
  datamodel call, an executable-content instruction). Each actor gets a
  *quantum* of fuel per turn; the call gets a *budget*. Both are counts, so a
  run is deterministic: the same events give the same interleaving, on any
  machine.
- **Yielding is invisible.** When an actor's quantum runs out in the middle
  of a macrostep, it is parked where it was (a few cursors, no stack) and
  resumes later. Its macrostep is still one macrostep: no event is taken
  from its mailbox meanwhile, `_event` is unchanged. Another actor ran in
  between — but it could not see or touch this actor's state, so nothing can
  tell.
- **Nobody starves.** An actor in an endless eventless loop (`a → b → a …`)
  gets its quantum and goes to the back of the queue; the others still run,
  and `ta_system_run` returns to you when the budget is spent
  (`TA_RUN_YIELDED`). You decide when to call it again.
- **Sending is enqueueing.** `<send>` to another actor of the same system
  puts the event in its mailbox and makes it runnable; the sender carries
  on. Nothing runs recursively; there is no call stack between actors.

The intuition: *a system is an event loop for actors.* Like a JavaScript
event loop, it has no data races because only one thing runs at a time, and
it stays responsive because each thing runs only for a while.

### Your thread and the system

`ta_system_run` returns after the budget — with the effects of the run
(`TA_EFFECT_SEND` to the outside, logs, done, requests). In between calls the
system does nothing at all: no threads, no timers firing on their own.
Time moves when you call `ta_system_advance_time`; `ta_run.next_deadline`
tells you when the next delayed send is due, so you can sleep until then.
That is why the same library fits libuv, asyncio, a game loop, a database
backend or a microcontroller.

**Thread safety.** A system is not thread-safe, and does not need to be: it
is a plain block of memory. Use it from one thread at a time (any thread —
it has no thread affinity). While it is running, a datamodel callback may
send events, spawn actors and terminate them: these take the same path as
an internal `<send>`, `<invoke>` or cancel. An event reaches its actor at
its next stable point (never inside the macrostep it was sent from), a
terminated actor exits at its next microstep boundary, and nothing grows,
so a full pool is `TA_ERROR_NO_MEMORY`. Every other call while it
is running is refused (`TA_ERROR_STATE`), so you cannot re-enter it by
accident; handle effects after `ta_system_run` returned. The library has no global mutable state: different systems on
different threads never interact.

## 3. Many systems: parallelism

To use more cores, run more systems — one per thread, worker or process —
and let them address each other:

```
 thread 1                         thread 2
 ┌─────────────────────┐          ┌─────────────────────┐
 │ system "orders"     │  send    │ system "billing"    │
 │ A  B  C ...         │ ───────▶ │ X  Y  Z ...         │
 └─────────────────────┘ (host)   └─────────────────────┘
```

- An actor's address is `tinyactors://<system>/#_scxml_<id>` (or
  `.../@<name>`). A send to another system is a `TA_EFFECT_SEND` for the
  host's transport (`TA_HOST_TRANSPORT`), yours (a queue
  between threads, a socket, a `postMessage`), which delivers it on the
  other side with `ta_host_resolve` and `ta_host_send`, the sender's
  address as origin. Replies use `_event.origin`. See
  [protocol.md](protocol.md#other-systems).
- Systems share nothing, so they need no locks between them. The only
  coordination is the hand-over of events, which you own.
- A datamodel instance belongs to one thread at a time: give each system (or
  each thread) its own `ta_datamodel_ecmascript`. The ECMAScript engine is not
  thread-safe, and does not have to be.
- Order is per sender and receiver: two events from A to X arrive in order;
  events from A and from B to X interleave in arrival order — as with any
  network of actors.

`ta_host` groups several systems that share one address and one datamodel
host (chart versions, segments) — on one thread. It is a convenience for
hot reload and growth, not a parallelism mechanism.

## 4. Between systems: what the transport guarantees

Inside one system, SCXML's rules hold exactly. Between systems there is no
shared clock, no shared lock and no shared transaction, so the guarantees
are those of the transport the application runs; core makes none of its
own:

| between | order | duplicates | loss | errors back |
|---|---|---|---|---|
| segments of one `ta_host` | per sender and receiver | never | never | `error.communication` at once |
| systems through the host's transport | the transport's | the transport's | the transport's | the transport fails the send: `error.communication` with the `sendid` |

What this means for you:

- **A send the transport cannot deliver is failed.** Answer the effect with
  `TA_EVENT_IN_ERROR_COMMUNICATION` and its sendid, and the sender gets
  `error.communication`. Design charts to handle it.
- **Order, retries and duplicates are the transport's.** A transport that
  retries needs receivers that tolerate a repeat; session ids restart in a
  fresh host, so an application that keeps addresses across restarts says
  how it tells incarnations apart.
- **No distributed transactions.** A send to another system is never part of
  a transaction spanning both. It leaves after the step that sent it,
  reaches the other system some time later, and its reply is a new event in
  a new step. "My write and the event, together" is the local system's job.

## 5. What tinyactors never does

- It never starts a thread, never sleeps, never blocks on I/O, never reads a
  clock. Everything happens inside a call you make, and returns.
- It never runs two macrosteps of one actor at the same time, and never
  interleaves two events of one actor.
- It never lets one actor see another's data. All interaction is events.
- It never changes SCXML-observable behaviour because of scheduling: the
  interleaving decides *when* an actor runs, never *what* it computes from
  the events it received.

## Where to read more

- [architecture.md](architecture.md) — the continuation machine (§7), the
  scheduler, mailboxes and timers (§12), hibernation and hot reload (§16).
- [scenarios.md](scenarios.md) — step-by-step walkthroughs, including an
  infinitely looping actor sharing the thread.
- `tools/torture` — scenarios that measure all of this live: `chaos`
  (fairness), `federation` (several systems).
