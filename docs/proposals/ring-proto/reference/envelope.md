# The envelope and the pool

**Status:** Sketch (2026-10-01), part of [the ring protocol proposal](../README.md)

An **envelope** is one input that a session has not finished with yet. That
covers an external event admitted from h2c, mail from another local session,
a delayed send that is not yet due, an internal event (`<raise>`,
`done.state.*`, `error.*`), and the event a running macrostep took.

All envelopes come from one **pool** per system, with a fixed bound. The pool
is the queue core gains in this design. Sending to a local session, delaying a
send, raising an event and admitting external input all take an envelope from
it. So one bound covers every queued input, and running out of envelopes is a
refusal the sender can see.

This replaces several structures in core today:

| Today | In the pool |
|---|---|
| The host's mailboxes and router (kernel `scheduler.h`, `router.h`) | envelopes in the run queue |
| `TA_ERR_BUSY` from `ta_actor_deliver` (core ADR 0055) | gone: a session is never busy when its envelope reaches the head of the queue ([macrostep.md](macrostep.md#run-to-completion)) |
| An actor's event block and its cache (core ADRs 0069, 0093) | the taken envelope, which stays where it is until the fence |
| The internal queue lent to a running actor (core ADR 0064) | not the pool: one fixed internal queue per running interpreter, with a reserved slot ([below](#the-internal-queue-is-not-in-the-pool)) |
| Delayed sends living with their owner (core ADR 0054) and the global deadline index | envelopes with a due time, in the pool's deadline index |
| Waiting mail for unloaded actors (kernel `loads.h`) | envelopes on a waiting list per address |

## The envelope

It has a fixed size, so the pool is an array. The variable parts (an event
name, an origin URI, an unresolved address) are bytes in the name area
([names.md](names.md)), and the envelope points at them.

| Field | Bytes | Meaning |
|---|---|---|
| `next` | 4 | the next envelope on whatever list holds this one: the run queue, a macrostep's outgoing list, a waiting list, or the free list |
| `owner_next` | 4 | the next pending delayed send of the same sender (for `<cancel>` and unloading) |
| `heap_index` | 4 | where it sits in the deadline index, or none |
| `state` | 1 | free, outgoing, runnable, waiting, taken, timer |
| `type`, `source`, `flags` | 3 | `_event.type`; where it came from (`input_source` in the catalog); `recorded`, `replaying` |
| `target` | 8 | the target session, or 0 while the target is an address in the name area |
| `from` | 8 | the sending session, or 0 for external input |
| `due` | 8 | for a timer: when it fires |
| `data` | 8 | `_event.data`, a vref the envelope holds a reference to |
| `send_id` | 8 | `_event.sendid` as core's `ta_send_id` (a generated counter or an authored literal) |
| `source_seq` | 8 | the h2c seq it was admitted from, or 0 |
| `text` | 8 | its text in the name area: the first chunk and the total length ([names.md](names.md#the-name-area)). The text is the name, then the origin, origintype, invokeid and target address, each prefixed by a `u16` length. |
| | **72** | **estimate**, on a 64-bit host |

The name is resolved to atoms when the envelope is **taken**, not stored as
an ID. So the envelope carries no process-local IDs except session IDs and
`send_id`, both of which already survive images (core ADRs 0066, 0048).

## The pool

- **It is created with the system**, with room for at most
  `poolEnvelopes` envelopes and `nameAreaBytes` of text.
- **It can fill on demand, but only outside macrosteps.** This is the shape
  ADR 0093 uses for the event-block cache: allocate on first use, up to the
  bound, and keep. Growth happens only at points where nothing is running:
  when core admits h2c records, and after a fence. Core keeps a reserve of
  free envelopes there (the most one macrostep may take before failing, see
  below). **A macrostep only takes envelopes that already exist.** A host
  that wants no allocation after start-up sets the pool to fill at creation.
- **Every list is intrusive.** It is a `u32` index in the envelopes
  themselves, so no list needs memory of its own.

### Memory, as estimates

These figures are **estimates** to check against an implementation, not
measurements:

| | Estimate |
|---|---|
| One envelope | 72 bytes, plus 4 for its slot in the deadline index |
| Default pool, 4,096 envelopes | about 304 KiB |
| Default name area | 64 KiB (`nameAreaBytes`, 65,536) |
| Default internal queue, 256 entries of about 48 bytes | about 12 KiB (`internalQueueEvents`) |
| Per actor | the head of its own pending delayed sends: +4 bytes on today's 332 (ADR 0092: "an actor stays at 332 bytes") |
| Today, for comparison | napi's event blocks prefilled at about 811 KB per system before ADR 0093, each queued event (`ta_queued_event`) 96 bytes, and the kernel's mailboxes on top |

A quiet system that fills on demand pays only for the envelopes its peak
needed, as with ADR 0093 today.

## The lists an envelope can be on

| List | Holds | Order | One per |
|---|---|---|---|
| **Run queue** | runnable envelopes: admitted external events, mail, invocation reports, timers that became due | arrival | system |
| **Outgoing list** | mail the running macrostep sent, until its fence | send order | running macrostep |
| **Waiting list** | envelopes for an address that is not resident | arrival | address being loaded |
| **Owner list** | a session's pending delayed sends | send order | session |
| **Free list** | free envelopes | – | system |
| Deadline index | pending delayed sends, as a binary heap of envelope indexes ordered by (due, send order) | – | system |

**There is one run queue in arrival order, not one queue per target.** Under
run to completion, a session is never in the middle of a macrostep when its
envelope reaches the head. Arrival order therefore keeps every
sender-to-receiver pair in order, which is what SCXML asks of one sender's
events. A queue per target would be needed only to skip a busy target, and
nothing is busy.

The one case where envelopes must wait for a particular target is when that
target isn't resident. They go on that address's waiting list (below).

## Admission: from h2c into the pool

Core moves h2c records into the pool only while the pool is below
`admissionWatermark` (a fraction of `poolEnvelopes` and of `nameAreaBytes`).
The rest of the pool is kept free for what sessions send each other.

- **Below the watermark,** core takes the next h2c record, takes an
  envelope, copies the record's text into the name area, takes over its
  vref, and appends the envelope to the run queue (or to a waiting list, see
  below). The h2c bytes are then released.
- **At or above it,** core leaves h2c alone and sets `admitting` to false in
  the control block. h2c fills, and the host refuses at its edge. Nothing is
  lost, and nobody is told anything they can't act on: the external client
  retries.
- **Records that are not events** ([`h2c.time`](../protocol/generated/MESSAGES.md#h2ctime),
  [`h2c.load`](../protocol/generated/MESSAGES.md#h2cload),
  [`h2c.load_failed`](../protocol/generated/MESSAGES.md#h2cload_failed), ...)
  are applied when read and take no envelope. They are still read in order,
  so a `h2c.time` stuck behind events that can't be admitted waits for them.
  That keeps the input order the same during replay ([recording-and-replay.md](recording-and-replay.md)).

### Fairness

When both the run queue and h2c have work, core alternates: it runs up to
`fairnessRatio` envelopes from the run queue, then admits one h2c record if
it is below the watermark. Internal work goes first, so work already started
finishes before new work comes in. The ratio keeps two sessions that mail
each other forever from shutting out external input.

The ratio is part of the system's parameters. Under system replay, the same
parameters give the same order ([recording-and-replay.md](recording-and-replay.md)).

## Mail between local sessions

A `<send>` to a local session (`#_scxml_<session>`, a family address of this
system, `#_parent`, `#_<invokeid>`) doesn't go to c2h:

1. Core takes a free envelope, unless taking it would eat into the reserve
   ([Running out](#running-out)).
2. It fills the envelope from the evaluated send, takes a reference to its
   data vref, and puts the text in the name area.
3. It keeps the envelope on the running macrostep's **outgoing list**, in
   send order. The envelope is on no queue yet, so nothing can take it.
4. At the macrostep's [`c2h.fence`](../protocol/generated/MESSAGES.md#c2hfence),
   core moves the outgoing list, in order, to the end of the run queue. The
   fence's `mail` field counts them. An envelope whose target address isn't
   resident goes to that address's waiting list instead, and core writes
   [`c2h.load_needed`](../protocol/generated/MESSAGES.md#c2hload_needed)
   after the fence ([below](#envelopes-for-an-address-that-isnt-resident)).

So a receiver never sees mail from a macrostep that didn't commit. A macrostep
an operator abandons (ADR 0082) frees its outgoing list. No mail
crosses the host, so no JavaScript runs for it and no vref leaves core.

Mail to an address in another system or process isn't local. It is a
[`c2h.send`](../protocol/generated/MESSAGES.md#c2hsend) for the host's SCXML
I/O processor.

## Delayed sends: envelopes with a due time

This is option A from the design discussion: **core keeps the timers, as
envelopes in the pool.**

- **A delayed `<send>`** takes an envelope in state `timer`, with its `due`.
  The envelope goes into the deadline index and onto its sender's owner list.
  This happens whatever the target: a local session, or an I/O processor.
- **`<cancel>` is exact.** Core finds the envelope on the sender's owner list
  by `send_id`, takes it out of the index and frees it. A timer core has
  already turned into input can't be cancelled. SCXML says the same, and
  core is the only place this can happen, so there is no race with the host.
- **[`h2c.time`](../protocol/generated/MESSAGES.md#h2ctime) moves the
  clock.** Every timer due by then leaves the index in (due, send order):
  - a local target's timer becomes runnable at the end of the run queue;
  - an I/O processor's timer is written as a
    [`c2h.send`](../protocol/generated/MESSAGES.md#c2hsend) and freed.
- **[`c2h.wake`](../protocol/generated/MESSAGES.md#c2hwake)** is written
  whenever the head of the index changes. The host needs one timer.
- **Timers to an I/O processor take an envelope too**, until they are due.
  So a fanout of 10,000 per-notification timeouts takes 10,000 envelopes, and
  the 10,001st `<send delay>` fails as `error.execution.capacity`.

What it replaces: ADR 0054's per-owner storage, the sparse state it grows,
and the global index ordering owners. The per-owner list stays, as `owner_next`.

## Envelopes for an address that isn't resident

An envelope whose target is an unloaded address, or a family address with no
resident session, can't run yet:

1. Core puts it on that address's waiting list: an admitted h2c record at
   admission, mail at its sender's fence (never before, so a macrostep that
   doesn't commit asks for no load). The address bytes are its key, in the
   name area.
2. The first time an address gets a waiting list, core writes
   [`c2h.load_needed`](../protocol/generated/MESSAGES.md#c2hload_needed).
   It writes it once until the host answers.
3. **[`h2c.load`](../protocol/generated/MESSAGES.md#h2cload)** loads the
   session. Its waiting envelopes join the run queue in their order. Its
   delayed sends, kept in its image, rejoin the deadline index.
4. **[`h2c.load_failed`](../protocol/generated/MESSAGES.md#h2cload_failed)**
   recycles them (below).

Waiting envelopes count against the pool. A host that wants to keep mail for
an actor that is slow to load can refuse to post more at its edge. The
`pool.waiting` counter in the control block tells it when.

[unloading.md](unloading.md) covers how sessions leave and return.

## Recycling: an envelope that can't be delivered becomes its own error

When mail can't be delivered, SCXML says the sender gets
`error.communication`. Raising that error must not need room the pool may
not have. So **the envelope is reused**:

| The envelope was | It becomes |
|---|---|
| mail or a timer from session S (target gone, load failed, destroyed with mail waiting) | `error.communication` for S. `_event.sendid` is the send's ID, `_event.data` is released and replaced by a short reason. The envelope joins the run queue with target S. |
| an external event admitted from h2c | Freed. A `tel.trace` of the new trace kind `DEAD_LETTER` reports it. The host already gave the input away; core can't hand it back. |
| a sender that is itself gone | Freed. |

The text part shrinks to the error's name, which is shorter than any text the
envelope already held. So recycling never needs name-area room either.

## The taken envelope stays put until the fence

When a macrostep starts, its envelope moves to state **taken**. It stays in
its slot while `_event` names it, and is freed at the fence. This is ADR
0069's "taking an event does not move it", applied to the pool:

- inspection (ADR 0083) reports the event from the envelope;
- [`c2h.input`](../protocol/generated/MESSAGES.md#c2hinput) for a recorded
  session is written from the envelope;
- if an operator abandons the macrostep (ADR 0082), the envelope is freed as
  ADR 0082 says, and its outgoing list with it.

### The internal queue is not in the pool

Internal events (`<raise>`, `done.state.*`, `error.*`, and `done.invoke.*`
for a child that ends inside the running macrostep) never leave the running
session, so they don't take envelopes. They go on a **fixed internal queue**
of `internalQueueEvents` entries ([parameters.md](parameters.md)), allocated at
creation:

- an entry is a fixed slot: the name (an authored ID, or a name-area chunk
  for a generated segment such as an invocation ID), the data vref, and
  `_event`'s other fields;
- a taken entry stays at the head while it is `_event`, and is freed when the
  next one is taken, as ADR 0069 does today;
- under run to completion only one top-level interpreter runs at a time, so
  there is **one internal queue per system**, plus one per nested
  interpreter (ADR 0060), not one lent to each actor (ADR 0064), and no cache
  of queues;
- **one slot is always kept free** for `error.execution.capacity`
  ([Running out](#running-out)).

A pool that is full therefore never stops a chart from raising its own
events, and a full internal queue never stops mail.

## Running out

There are two kinds of exhaustion, and the chart sees only one of them
([macrostep.md](macrostep.md#two-kinds-of-exhaustion)).

**A `<send>` or delayed send that finds no free envelope, or no name-area
room for its text, fails as `error.execution.capacity.pool` or `.names`. A
`<raise>` that finds the internal queue full fails as
`error.execution.capacity.internal`.** It is an execution error and not a
communication error, deliberately:

- SCXML says that after an error in executable content, the rest of the
  block is not run. So a fanout that runs out fails **once**, at the first
  send that doesn't fit. There is no flood of 9,000 errors to raise.
- The chart knows exactly what happened: everything before the failing
  action ran, and nothing after it did. The event's last segment names the
  resource (`error.execution.capacity.pool`, `.names`, `.c2h`, `.internal`,
  `.record`), and `_event.data` names the action
  ([macrostep.md](macrostep.md#two-kinds-of-exhaustion)).

**Enough of the internal queue is kept free so that the error itself can be
raised:**

- one slot for the `error.execution.capacity` event;
- before each microstep, slots for the platform events the microstep may
  generate (`done.state.*`, at most the definition's static bound, which the
  compiler records).

A chart's own `<raise>` is refused while taking a slot would eat into that
reserve. So the chart's actions run out first, and SCXML's own events never
do. The pool needs no reserve: the error goes on the internal queue, and
recycling reuses the envelope it recycles ([Recycling](#recycling-an-envelope-that-cant-be-delivered-becomes-its-own-error)).
So the pool can fill to its last envelope.

**Open point.** A microstep whose reserve can't be restored, because the
macrostep's own internal events filled the internal queue, can't start. The
proposed rule is to end the macrostep there, as a runaway does (ADR 0075),
with [`c2h.fence`](../protocol/generated/MESSAGES.md#c2hfence) status
`capacity` and a `tel.trace` that says why. It needs a test
showing it can only happen to a chart that already raised
`error.execution.capacity` in the same macrostep.

### The abandoned block, in a chart

Because the rest of the block is abandoned, **where** an action sits in its
block decides what a capacity failure costs:

```xml
<onentry>
  <send id="t" event="sending.timeout" delay="30s"/>   <!-- first: always scheduled -->
  <foreach array="batch" item="n">                    <!-- may fail partway -->
    <send type="x-notify" target="notify:" event="notification" namelist="n"/>
    <send targetexpr="tracker" event="progress.sent"><param name="id" expr="n.id"/></send>
  </foreach>
</onentry>
```

If the timer comes after the `<foreach>`, a fanout that runs out never
schedules its own timeout, and the chart waits forever.
[scenarios/S02-full-pool.md](../scenarios/S02-full-pool.md) walks through this
case, and [BEHAVIORS.md](../BEHAVIORS.md) lists who retries and how.
