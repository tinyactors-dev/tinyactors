# Architecture

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](README.md)

How the pieces fit together. Each section links to the reference that has the
details. Messages are cited by stable ID; all of them are in
[MESSAGES.md](protocol/generated/MESSAGES.md).

## Layers, before and after

| Layer | Today | With the ring protocol |
|---|---|---|
| **Core** | Semantics. Calls vtables for the router, I/O, invokers, the data model, the directory and the trace sink. Holds delayed sends. No queues. | Semantics **and the inputs it has not run yet**: the envelope pool, the deadline index and the name area. It writes rings instead of calling most vtables. Only the data model stays a vtable. |
| **Host kernel** (`tinyactors-host`) | Mailboxes, scheduler, router, effects queue, recorder, replay, loads, offloads, OTLP; the WAL was removed by kernel ADR 0006 | **Deleted.** OTLP encoding stays as a library function that reads `tel`. |
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
            │   watermark)   │ deadline index (delayed sends)      │──▶ macrostep slots ────┐  │
            │                │ waiting lists for unloaded addresses│  (K, taken in turn)    │  │
            │                │ deferred lists for active sessions  │                        │  │
            │                └──────────────▲──────────────────────┘                        │  │
            │                    mail, timers│ (loopback; never leaves core)               │  │
            │                               └────────────────────────────────────────────┘  │
            │   name area: bytes of names made at run time        data model ◀── vtable ──▶ binding
            │                                                                                │
            │  ──c2h──▶ host: c2h.input, c2h.send, c2h.invoke, c2h.macrostep_ended,             │
            │           c2h.wake, c2h.bounced, c2h.load_needed, c2h.unloaded, c2h.state, ...     │
            │  ──tel──▶ host: tel.trace, tel.dropped (lossy)                                  │
            └────────────────────────────────────────────────────────────────────────────────┘
```

| Piece | Where | Reference |
|---|---|---|
| Rings `h2c`, `c2h`, `tel` and their control block | Core allocates them at system creation, in memory the host can read | [reference/ring.md](reference/ring.md) |
| Envelope pool, run queue, deadline index, admission, fairness | Core | [reference/envelope.md](reference/envelope.md) |
| Name area; interning only at compile time; matching by lookup only | Core | [reference/names.md](reference/names.md) |
| The macrostep: no allocation, end reports, budgets, capacity errors; macrostep slots, the chain limit and quarantine | Core | [reference/macrostep.md](reference/macrostep.md) |
| Data model and value references | A vtable the binding implements | [reference/data-model.md](reference/data-model.md) |
| Complete definition submission, registration, release and retirement | Host requests; core compiles between macrosteps | [reference/definitions.md](reference/definitions.md) |
| Debugger attachment, inspection, abandonment and transitions | Debugger temporarily owns the whole system; commands stay outside the rings | [reference/debugging.md](reference/debugging.md) |
| Recording, replay, checkpoints | Core writes `c2h.input` and `c2h.state`; the application stores them | [reference/recording-and-replay.md](reference/recording-and-replay.md) |
| Telemetry and OTLP | Core writes `tel`; a library encodes OTLP | [reference/telemetry.md](reference/telemetry.md) |
| Unloading and loading, **to bound RAM only; not persistence** | Core writes images and asks for loads; the host keeps images and wakes for the life of the process | [reference/unloading.md](reference/unloading.md) |
| Parameters | `ta_system` creation | [BEHAVIORS.md](BEHAVIORS.md#parameters) |

## Protocol, native integration and debugging

- **Host requests and results go through rings.** That covers inputs,
  complete declarative definitions, registration holds and retirement,
  effects, macrostep end reports, bounces, loads, unloads, checkpoints, wakes
  and telemetry.
  Definition compilation has structured error results (ADR 0058); bindings
  need no separate native definition-management interface. The test host
  forwards requests after mapping process-local value and buffer handles.
- **Native integration stays ordinary API calls:**
  - creating and destroying a system, accessing its rings;
  - `ta_run(budget)`;
  - installing and calling the synchronous data model vtable;
  - value ownership and byte source/sink integrations.
- **Debugger commands stay outside the production protocol.** The host
  suspends its pump and transfers exclusive ownership of the whole system
  to a debugger at a point where core has yielded. Native primitives let
  the debugger inspect, abandon at a legal boundary and drive an operator
  transition through the normal transition engine. Detaching returns
  ownership to the pump. There is no operator ring or operator command
  admission ([reference/debugging.md](reference/debugging.md)).

The accepted definition boundary and the remaining representation questions
are in [reference/definitions.md](reference/definitions.md). The protocol
does not mirror individual C builder operations.

## The host loop

All a binding does, in pseudo-code:

```
on start:          create the system (parameters), install native integrations
                   post h2c.definition_register for each complete spec
                   drive the loop; await successful definition results before spawn
on an edge input:  if admitting and h2c and the bounded submission table have room:
                     retain the original request; post h2c.event
                   else refuse (503) or keep it within an explicit bound (policy)
on wake timer:     retain the latest time; post h2c.time(now) before new ordinary inputs
loop:
  ta_run(budget)                         # admits from h2c, runs macrosteps to completion
  read each available c2h record:
    c2h.definition_result → resolve the request; reclaim its buffers; report compile errors
    c2h.input         → recorder; journal on the application's schedule
    c2h.send          → I/O processor, immediately on receipt
    c2h.invoke / c2h.invoke_cancel / c2h.invoke_forward → invoker, on receipt
    c2h.log           → logger, on receipt
    c2h.macrostep_ended → report how the macrostep ended
    c2h.input_completed → resolve source_seq; release the retained original
    c2h.input_refused   → reject source_seq; release or retry the retained original
    c2h.quarantined     → fault hook; an operator answers h2c.release or h2c.abandon
  c2h.wake            → (re)arm the one timer
  c2h.load_needed     → fetch the image → h2c.load, or h2c.load_failed, echoing request_seq
  c2h.unloaded        → store image and wake_at in the index of unloaded sessions
  c2h.bounced         → reject source_seq; release or retry the retained original (policy)
  tel                 → OTLP encoder, when the exporter asks
  I/O results (acks, rejections, errors) → h2c.event / h2c.invoke_report
```

The host never routes mail between actors, never orders inputs for a busy
actor, never keeps timers for resident actors, and never holds anything that
belongs to the middle of a macrostep.

The submission table has a host-configured bound. It retains the original
metadata and its own data reference until a terminal result names the
submission's `source_seq`; the reference transferred to h2c is separate.
The host must not accept an input it cannot track within that bound.
`admitting` is a hint, not a reservation: an earlier macrostep can consume
capacity after an event is posted. Core consumes and bounces such an event
without executing it, so control commands behind it can still progress.
The host drains c2h after every yield and prioritizes load answers, load
failures and clock updates over new ordinary inputs.

Core reads h2c in order, and applies control commands between steps, where
no slot is inside a unit of executable content. An admitted event is
queued, not necessarily completed before the next h2c command is read.
A paused or quarantined slot does not hold commands back: a control that
needs a session outside a macrostep (`h2c.unload`, `h2c.checkpoint`) is
bounced while that session holds a slot, and `h2c.destroy` abandons the
slot first. A slot keeps the system time it took its input at. If the host needs an input's effects to precede a control
command, it waits for that input's `c2h.input_completed` before posting the
command. Refusal requires an explicit retry or application decision.

## The life of an input

1. **Edge.** The host posts [`h2c.event`](protocol/generated/MESSAGES.md#h2cevent).
   Its name is bytes and its data is a `vref`. If `h2c` is full, the host
   refuses here. ([S09](scenarios/S09-edge-backpressure.md))
2. **Admission.** Core moves the record into an envelope while the pool is
   below `admissionWatermark` and its complete text fits. Its name bytes go to the name area. It is
   appended to the run queue, whose arrival order keeps each sender's
   messages to a receiver in order. If the target isn't loaded, it waits on
   that address's list, and core writes [`c2h.load_needed`](protocol/generated/MESSAGES.md#c2hload_needed).
   ([S07](scenarios/S07-unloaded-tracker.md))
   An input that cannot be admitted is consumed and answered with
   `c2h.bounced`, after reserving its result bytes; it leaves no envelope
   or transferred value reference behind. Until it is taken, the host can
   withdraw an admitted input with [`h2c.withdraw`](protocol/generated/MESSAGES.md#h2cwithdraw),
   which frees its envelope and answers `c2h.input_refused` (`withdrawn`).
   Records are never edited after commit.
3. **Take.** When a macrostep slot is free, core picks the next runnable
   envelope whose session isn't active, mail before admission by
   `fairnessRatio`. Mail for an active session waits on its deferred list. If the session is recorded, core writes
   [`c2h.input`](protocol/generated/MESSAGES.md#c2hinput).
   The envelope stays owned through invocation startup and its internal follow-up.
4. **Macrostep.** Data model calls go through the vtable. Each effect is
   either a `c2h` record or a new envelope (mail, delayed send). Nothing
   allocates. A resource that is full fails the action as
   `error.execution.capacity`. ([S02](scenarios/S02-full-pool.md))
5. **Macrostep end and startup.** Core writes [`c2h.macrostep_ended`](protocol/generated/MESSAGES.md#c2hmacrostep_ended),
   then starts surviving pending invocations. Resulting internal errors run
   before another external input. Once the chain and notification flushing
   settle, core writes `c2h.input_completed` and frees the envelope. The host may already have executed effects
   during a budget pause; recording and persistence follow application policy.
   An h2c input also receives `c2h.input_completed` with its `source_seq`,
   even for an unrecorded session. An admitted input refused before being
   taken receives `c2h.input_refused` instead.
   ([S01](scenarios/S01-fanout.md))

## The life of a send

| `<send>` to | Becomes | Delivered | Full | Reference |
|---|---|---|---|---|
| An I/O processor, no delay | [`c2h.send`](protocol/generated/MESSAGES.md#c2hsend) | on receipt, by the host, including before the macrostep end | `c2h` has no room: `error.execution.capacity` | [ring](reference/ring.md) |
| A local session (`#_scxml_…`, an actor URI) | an envelope on the run queue, at the macrostep end | when core takes it | pool or name area is full: `error.execution.capacity` | [envelope](reference/envelope.md) |
| Anything, with a delay | an envelope in the deadline index | when `h2c.time` reaches its due time: mail is taken, an I/O send becomes `c2h.send` | as above | [envelope](reference/envelope.md) |
| An unloaded session | an envelope that waits; `c2h.load_needed` | after `h2c.load` | as above; if the load fails, the envelope becomes `error.communication` for its sender | [unloading](reference/unloading.md) |
| A session that doesn't exist | `error.communication` for the sender, in the same envelope | — | — | [envelope](reference/envelope.md) |

`<cancel>` removes an envelope from the deadline index. It is exact: a
cancelled delayed send never arrives. ([S08](scenarios/S08-cancel-and-second-click.md))

## Time

- The host owns the clock. It moves the clock by posting
  [`h2c.time`](protocol/generated/MESSAGES.md#h2ctime).
- A live input uses the system clock when it is taken. That time is fixed
  for its entire macrostep, including budget pauses. Its h2c header `at`
  does not advance the live clock. The host posts a clock update before
  an input when it wants the current time to apply to that input.
- A replay input uses its supplied `at` when taken, advancing only its
  replaying session's clock under the monotonic-time rules. Refused input
  changes no clock.
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
- **Recording observes inputs; the application owns journaling.** Effects
  may execute on receipt, independently of recording or persistence.
- To replay a session, post `h2c.spawn` with the replaying flag, then the
  recorded inputs, await their `c2h.input_completed` results, then post
  `h2c.publish`. A bounce or refusal requires retry or an explicit choice
  to omit that seed input; admission alone does not establish completion.
- **Replay seeds new execution:** it derives configuration and data model
  values before publish. It may produce different state as definitions,
  resource conditions, budgets and draining change.
- A whole-system h2c stream can seed several sessions together, including
  local mail generated by that new execution. It does not restore the old
  system's exact scheduling or in-flight mail.

Details: [reference/recording-and-replay.md](reference/recording-and-replay.md).
Crash cases: [S03](scenarios/S03-crash-unrecorded.md),
[S04](scenarios/S04-crash-campaign-recorded.md),
[S05](scenarios/S05-crash-both-recorded.md),
[S06](scenarios/S06-checkpoint.md).

## Memory

| Fixed at system creation (or filled on demand up to its bound, outside macrosteps) | Per actor | Per definition |
|---|---|---|
| `h2c`, `c2h`, `tel`; the envelope pool; name area; `macrostepSlots` slots, each a continuation, scratch, an ordinary internal quota and compiler-sized mandatory workspace, filled on demand | its record (about 332 bytes today) and list heads, including its deferred list; invocation slots sized by the definition; a pool token per active local invocation | compiled chart, mandatory-event bounds and authored names, interned |

Nothing in the table grows during a macrostep. The data model's own memory
belongs to the data model. Figures are in [BEHAVIORS.md](BEHAVIORS.md#parameters)
and [reference/envelope.md](reference/envelope.md).

## Threads

**A `ta_system` is not thread-safe.** Core and the host take turns on the
system's own thread, and only that thread touches the rings. Macrostep
slots are not threads: they take turns on that same thread. A host that
works on other threads (an I/O thread pool, a Node worker) puts its own
queue or channel in front of the system. Separate systems share nothing,
so they may run on separate threads. The test host forwards records between
its process and its client, and it too touches the rings only from the
system's thread.

Why: no atomics or memory ordering in core, the same code on an MCU or in
wasm, and a data model (JavaScript) that is single-threaded anyway
([reference/ring.md](reference/ring.md#one-thread)).
