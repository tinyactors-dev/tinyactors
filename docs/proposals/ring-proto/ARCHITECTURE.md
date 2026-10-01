# Architecture

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](README.md)

How the pieces fit together. Each section links to the reference that has the
details. Messages are cited by stable ID; all of them are in
[MESSAGES.md](protocol/generated/MESSAGES.md).

## Layers, before and after

| Layer | Today | With the ring protocol |
|---|---|---|
| **Core** | Semantics. Calls vtables for the router, I/O, invokers, the data model, the directory and the trace sink. Holds delayed sends. No queues. | Semantics **and the inputs it has not run yet**: the envelope pool, the deadline index and the name area. It writes rings instead of calling most vtables. Only the data model stays a vtable. |
| **Host kernel** (`tinyactors-host`) | Mailboxes, scheduler, router, effects queue, WAL, recorder, replay, loads, offloads, OTLP | **Deleted.** OTLP encoding stays as a library function that reads `tel`. |
| **Binding** (napi, the test host) | Values, the data model, the pump, the public API, plus glue to the kernel | Values, the data model, and a loop that drives the rings ([below](#the-host-loop)) |
| **Application** | Storage and policy | Storage and policy, unchanged: what to record, how to answer at the edge, I/O processors |

The README's layer rule still holds: core decides what inputs mean. Mail
between sessions, its order, and whether a send can be accepted are meaning,
so they belong in core. ([reference/migration.md](reference/migration.md)
lists the ADRs this reverses.)

## The picture

```
            ┌──────────────────────────── core (one ta_system) ─────────────────────────────┐
            │                                                                                │
 host ──h2c──▶ admission ──▶ ┌─────────── envelope pool ───────────┐                           │
            │  (below the    │ one run queue, in arrival order     │                           │
            │   watermark)   │ deadline index (delayed sends)      │──▶ run to completion ──┐  │
            │                │ waiting lists for unloaded addresses│    (one macrostep)     │  │
            │                └──────────────▲──────────────────────┘                        │  │
            │                    mail, timers│ (loopback; never leaves core)               │  │
            │                               └────────────────────────────────────────────┘  │
            │   name area: bytes of names made at run time        data model ◀── vtable ──▶ binding
            │                                                                                │
            │  ──c2h──▶ host: c2h.input, c2h.send, c2h.invoke, c2h.fence, c2h.wake,            │
            │           c2h.bounced, c2h.load_needed, c2h.unloaded, c2h.state, ...            │
            │  ──tel──▶ host: tel.trace, tel.dropped (lossy)                                  │
            └────────────────────────────────────────────────────────────────────────────────┘
```

| Piece | Where | Reference |
|---|---|---|
| Rings `h2c`, `c2h`, `tel` and their control block | Core allocates them at system creation, in memory the host can read | [reference/ring.md](reference/ring.md) |
| Envelope pool, run queue, deadline index, admission, fairness | Core | [reference/envelope.md](reference/envelope.md) |
| Name area; interning only at compile time; matching by lookup only | Core | [reference/names.md](reference/names.md) |
| The macrostep: no allocation, the fence, the budget, capacity errors | Core | [reference/macrostep.md](reference/macrostep.md) |
| Data model and value references | A vtable the binding implements | [reference/data-model.md](reference/data-model.md) |
| Recording, replay, checkpoints | Core writes `c2h.input` and `c2h.state`; the application stores them | [reference/recording-and-replay.md](reference/recording-and-replay.md) |
| Telemetry and OTLP | Core writes `tel`; a library encodes OTLP | [reference/telemetry.md](reference/telemetry.md) |
| Unloading and loading | Core writes images and asks for loads; the host stores images and wakes | [reference/unloading.md](reference/unloading.md) |
| Parameters | `ta_system` creation | [reference/parameters.md](reference/parameters.md) |

## Two kinds of call

- **Traffic goes through rings.** That covers inputs, effects, fences,
  bounces, loads, unloads, checkpoints, wakes and telemetry. Rings are what
  get batched and recorded, and what an out-of-process host (the test host)
  would forward unchanged.
- **Control stays ordinary API calls:**
  - creating a system and registering a definition, whose compile errors need
    a rich answer (ADR 0058);
  - `ta_run(budget)`;
  - operator calls: inspect, abandon, operator transitions;
  - the data model vtable, which runs inside a macrostep.

## The host loop

All a binding does, in pseudo-code:

```
on start:          create the system (parameters), register definitions
on an edge input:  if h2c has room: post h2c.event / h2c.spawn; else refuse (503) or keep it (policy)
on wake timer:     post h2c.time(now)
loop:
  ta_run(budget)                         # admits from h2c, runs macrosteps to completion
  read c2h up to each c2h.fence:
    recorded session?  persist its c2h.input first          (policy)
    then act: c2h.send → I/O processor, c2h.invoke → invoker, c2h.log → logger
  c2h.wake            → (re)arm the one timer
  c2h.load_needed     → fetch the image → h2c.load, or h2c.load_failed
  c2h.unloaded        → store image and wake_at in the index of unloaded sessions
  c2h.bounced         → post again later, or drop                          (policy)
  tel                 → OTLP encoder, when the exporter asks
  I/O results (acks, rejections, errors) → h2c.event / h2c.invoke_report
```

The host never routes mail between actors, never orders inputs for a busy
actor, never keeps timers for resident actors, and never holds anything that
belongs to the middle of a macrostep.

## The life of an input

1. **Edge.** The host posts [`h2c.event`](protocol/generated/MESSAGES.md#h2cevent).
   Its name is bytes and its data is a `vref`. If `h2c` is full, the host
   refuses here. ([S09](scenarios/S09-edge-backpressure.md))
2. **Admission.** Core moves the record into an envelope while the pool is
   below `admissionWatermark`. Its name bytes go to the name area. It is
   appended to the run queue, whose arrival order keeps each sender's
   messages to a receiver in order. If the target isn't loaded, it waits on
   that address's list, and core writes [`c2h.load_needed`](protocol/generated/MESSAGES.md#c2hload_needed).
   ([S07](scenarios/S07-unloaded-tracker.md))
3. **Take.** Core picks the next runnable envelope, mail before admission by
   `fairnessRatio`. If the session is recorded, core writes
   [`c2h.input`](protocol/generated/MESSAGES.md#c2hinput).
   The envelope stays where it is until the fence.
4. **Macrostep.** Data model calls go through the vtable. Each effect is
   either a `c2h` record or a new envelope (mail, delayed send). Nothing
   allocates. A resource that is full fails the action as
   `error.execution.capacity`. ([S02](scenarios/S02-full-pool.md))
5. **Fence.** Core writes [`c2h.fence`](protocol/generated/MESSAGES.md#c2hfence)
   and frees the envelope. The host persists, then acts.
   ([S01](scenarios/S01-fanout.md))

## The life of a send

| `<send>` to | Becomes | Delivered | Full | Reference |
|---|---|---|---|---|
| An I/O processor, no delay | [`c2h.send`](protocol/generated/MESSAGES.md#c2hsend) | after the fence, by the host | `c2h` has no room: `error.execution.capacity` | [ring](reference/ring.md) |
| A local session (`#_scxml_…`, an actor URI) | an envelope on the run queue, at the fence | when core takes it | pool or name area is full: `error.execution.capacity` | [envelope](reference/envelope.md) |
| Anything, with a delay | an envelope in the deadline index | when `h2c.time` reaches its due time: mail is taken, an I/O send becomes `c2h.send` | as above | [envelope](reference/envelope.md) |
| An unloaded session | an envelope that waits; `c2h.load_needed` | after `h2c.load` | as above; if the load fails, the envelope becomes `error.communication` for its sender | [unloading](reference/unloading.md) |
| A session that doesn't exist | `error.communication` for the sender, in the same envelope | — | — | [envelope](reference/envelope.md) |

`<cancel>` removes an envelope from the deadline index. It is exact: a
cancelled delayed send never arrives. ([S08](scenarios/S08-cancel-and-second-click.md))

## Time

- The host owns the clock. It moves the clock by posting
  [`h2c.time`](protocol/generated/MESSAGES.md#h2ctime).
- Core owns the deadline index for resident sessions. It writes
  [`c2h.wake`](protocol/generated/MESSAGES.md#c2hwake) whenever the earliest
  due time changes.
- The host keeps one timer for that, plus an index of unloaded sessions keyed
  by `wake_at` ([reference/unloading.md](reference/unloading.md)).

## Recording and replay

- Recording is a bit on each session, set by an address selector when the
  session is spawned or loaded. A recorded session's inputs appear as
  [`c2h.input`](protocol/generated/MESSAGES.md#c2hinput) before the effects
  they cause.
- **The application persists them before it acts on the fence.** That is the
  whole write-ahead rule.
- To replay a session, post `h2c.spawn` with the replaying flag, then the
  recorded inputs, then `h2c.publish`.
- To replay a whole system, record `h2c` together with the parameters and the
  definitions. Core's scheduling is deterministic, so in-flight mail is
  rebuilt too.

Details: [reference/recording-and-replay.md](reference/recording-and-replay.md).
Crash cases: [S03](scenarios/S03-crash-unrecorded.md),
[S04](scenarios/S04-crash-campaign-recorded.md),
[S05](scenarios/S05-crash-both-recorded.md),
[S06](scenarios/S06-checkpoint.md).

## Memory

| Fixed at system creation (or filled on demand up to its bound, outside macrosteps) | Per actor | Per definition |
|---|---|---|
| `h2c`, `c2h`, `tel`; the envelope pool; the name area; scratch; one continuation; one internal queue | its record (about 332 bytes today) and its list heads; its invocation slots, sized by the definition | compiled chart; authored names, interned |

Nothing in the table grows during a macrostep. The data model's own memory
belongs to the data model. Figures are in [reference/parameters.md](reference/parameters.md)
and [reference/envelope.md](reference/envelope.md).

## Threads

One `ta_system` runs on one thread, as today. The rings are single-producer,
single-consumer:
- an in-process host on the same thread reads and writes them directly;
- a host on another thread, or one that only forwards to another process
  (the test host), uses the acquire/release counters in the control block
  ([reference/ring.md](reference/ring.md)).
