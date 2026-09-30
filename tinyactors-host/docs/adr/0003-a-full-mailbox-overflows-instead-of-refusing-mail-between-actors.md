# ADR 0003: A full mailbox overflows instead of refusing mail between actors

**Status:** Accepted (2026-09-30). Amends napi ADR 0005 point 9 (the router no longer answers RETRY for a full queue) and kernel ADR 0002 (`accepted` is written for mail accepted past the limits too). Supersedes in part core ADR 0095, whose submitted sends a full mailbox no longer makes.
**Date:** 2026-09-30

## Context

napi ADR 0033 section 7 promises mail between persisted actors exactly
once. The argument: a message is either delivered and logged, or queued
and logged (`accepted` in the receiver's log, kernel ADR 0002), and the
sender's replay never sends it again, because "sent is what happened"
(core ADR 0089 section 3).

Backpressure breaks the argument. `maxQueuedMessages` and
`maxRetainedBytes` (napi ADRs 0005 point 9 and 0035) limit the messages
the kernel's scheduler holds. They are limits of the whole system, not of
one receiver: `queued` counts every accepted message not yet delivered or
reported, in every mailbox, every mailbox waiting for a loader, and the
dead list. When the limit is reached, `tah_router_submit()` answers
`TA_EXTERNAL_EVENT_RETRY`:
- the receiver's log gets no `accepted`;
- the sender's `fired`, for a delayed send, is taken back
  (`tah_inputs_undo()`);
- core keeps the send in the sender's deadline index, marked submitted,
  and offers it again at every dispatch (core ADR 0054).

Until the router takes it, the send exists only in memory. It is lost:
1. **after a crash and a replay from `spawned`.** The sender's replay
   drops an immediate send and a delayed send whose due time has passed,
   because both count as sent. Neither knows that the router refused it;
2. **after an eviction and a process exit** (the owner's graceful
   restart). An evicted sender leaves its submitted sends behind as
   orphans (core ADR 0054), which die with the process. napi's `evict()`
   refuses while a send is still due, but a binding that evicts through
   the kernel does not have to ask;
3. **after a crash while the sender is evicted,** for the same reason.

Core ADR 0095 fixed only the checkpoint: a checkpoint taken while a send
waits restores it, and the replay drops it again at the `fired` that took
it or at an `evicted`. It left the three cases above open, and said so
("A replay from `spawned`, with no checkpoint, still loses such a send").

### Does a later send overtake a refused one today?

Yes, rarely. A submitted send is offered again only at a dispatch, which
runs at the end of each turn, after the turn's deliveries and dead-letter
reports have made room. A later immediate send from the same sender is
offered at once, in the macrostep that makes it. When room appears
between the dispatch that refused the first send and that macrostep, the
second is accepted and the first is not:
- room appears outside a turn only by an application's action: a
  `send()` cancelled or timed out while its message is queued
  (`tah_sched_cancel()`), or a message waiting for a loader withdrawn;
- out of memory, the router refuses one send and may take the next.

Otherwise the refused send is taken at the dispatch before the sender's
next macrostep, or the next send is refused as well and waits behind it
in deadline order. So today's rule, "messages to one target are delivered
in the order they were accepted" (napi ADR 0005 point 7), holds, but the
order of acceptance can differ from the order the sender sent in.

## Decision

### 1. The router always accepts mail between actors

Mail that reaches the kernel through the router (core's
`ta_external_event_router`) is accepted even when the queue is at
`maxQueuedMessages` or `maxRetainedBytes`. That is:
- a `<send>` from a chart to another actor, by name or by session ID,
  immediate or delayed (when it fires);
- mail between an invoked child chart and its parent (`#_parent`,
  `#_<invokeid>`), a child's `done.invoke`, and events autoforwarded to a
  child chart, which core routes the same way (`ta_invoke_submit()`);
- mail a replay owes (`tah_replay_redeliver()`), which the log accepted
  before the crash.

The sender's send completes: the router answers ACCEPTED. For a logged
receiver the kernel writes `accepted` into its log when it accepts the
message, as it does for any queued message (kernel ADR 0002 section 2).
Nothing is left submitted in the sender for a full queue.

### 2. Past the limits a message waits in its receiver's overflow

A message accepted past the limits is marked as **overflow**. It joins
its receiver's mailbox like any other message, in arrival order, so the
order of acceptance is the order of sending:
- **the scheduler does not deliver an overflowed message until the queue
  has room.** Then it moves the message into the mailbox: it counts
  against the limits from then on;
- **receivers whose next message waits for room queue up** in a FIFO of
  their own. When room appears, the scheduler serves them in that order,
  one message each, and puts them on the ready queue. A receiver whose
  next message is overflow while others wait for room joins the end of
  that queue, so a busy receiver cannot take all the room that its own
  deliveries free;
- **mail that waits for a loader** (napi ADR 0020) overflows the same
  way, in the address's waiting mailbox, and keeps its mark when the load
  hands it to the actor;
- **a message whose receiver is gone or finished** becomes a dead letter,
  as before, whether it overflowed or not.

"The receiver's overflow" is a mark on its messages, not a second list:
the mailbox walks, moves and burials that handle mail today (a
replacement's transfer, a load's hand-over, a checkpoint's owed
messages, an unload's or a destroy's dead letters, napi's `mailbox()`,
`evict()`'s check for mail waiting) handle overflowed mail with it, in
order.

### 3. Defaults, decided for now and open to change

- **No cap on the overflow.** Memory grows as it did before: the
  overflow used to wait in the senders' deadline indices, also without a
  cap. The scheduler counts it (`overflow` in its stats, napi's
  `stats()` and metrics, the test host's `hosted.mailboxes`), so it is
  visible.
- **One rule for logged and unlogged receivers:** first in, first out at
  the receiver for everyone.
- **Mail from the application keeps its behaviour.** `post()`, `send()`,
  an invocation reporter's `event()` and `done()`, the test host's posts,
  and the kernel's own reports to a parent (a quarantined child, a child
  that failed to start) still answer `queue-full` (`TA_ERR_BUSY`) at the
  limits. The application is outside the log and can retry; a chart
  cannot.
- **The SCXML invoker's forward is not router mail.** An event
  autoforwarded to a child that the kernel's SCXML invoker started
  (`tah_scxml_forward()`) is posted directly, and is still refused at the
  limits; core drops a forward its invoker refused. Such children are
  unnamed, so nothing of theirs is logged.
- **Out of memory can still answer RETRY.** A router that cannot write
  `fired`, number a message or take an envelope refuses the send, core
  keeps it submitted, and the loss cases of the Context stay open while
  memory is exhausted.

### 4. The per-address limit of a load is unchanged

A family's `maxWaiting` (napi ADR 0020) still makes mail past it a dead
letter (`waiting-full`), reported to the application. It is a limit of one
address's load, not backpressure: the send completes, and the application
hears what was dropped.

## Consequences

- **`maxQueuedMessages` and `maxRetainedBytes` now bound what the
  scheduler delivers from, not what it holds,** for mail between actors.
  The application's own mail is still refused at them. Queued messages
  (`queued` in the stats) can exceed `maxQueuedMessages` by the
  overflow.
- **Mail between persisted actors is exactly once across a full queue.**
  The three loss cases of the Context are gone, except under out of
  memory.
- **A receiver's log grows at its senders' pace,** not at its own: a
  slow logged receiver writes an `accepted` for every message sent to it,
  where before the senders waited.
- **Order.** Mail to one receiver is delivered in the order it was
  accepted, and a chart's mail is accepted in the order it was sent: the
  overtaking described in the Context no longer happens for a full queue.
  Across receivers, the room queue serves receivers in the order they
  asked for room, as the ready queue serves them in the order they got
  mail.
- **A sender is not slowed down by a full receiver any more.** Before,
  its sends waited; now they are accepted and wait at the receiver.
- **Costs:**
  - per actor: nothing. The mark that a receiver waits for room fits in
    the scheduling table's reserved byte, and an empty overflow is no list
    at all;
  - per message: nothing. The overflow mark fits in the envelope's
    padding;
  - per system: a queue of the receivers waiting for room, allocated when
    the first one waits, and two counters.
- **Core ADR 0095's machinery serves only out of memory.** Whether it
  still earns its place is decided in the implementation notes below.

## Considered and rejected

- **A: log the refusal in the sender's log,** so that a replay knows the
  router refused a send and keeps it. The replay would have to hold every
  replayed immediate send until the later records say whether it was
  refused, match refusals and takes to sends by send ID, and deduplicate
  after an eviction, and it would only work for a persisted sender: mail
  from an unlogged sender to a logged receiver would still be lost.
- **B, chosen: the receiver accepts, and the overflow waits at the
  receiver.** The message is logged once, where it is delivered, as all
  other mail is.
- **Accept past the limits and deliver at once,** with no overflow mark.
  Simpler, but the limits would then bound nothing for mail between
  actors, and a receiver past them would take room the rest of the
  system is waiting for.
- **A separate overflow list per receiver.** It needs two more indices in
  every scheduling-table entry, 8 bytes per actor, for mail that the
  mailbox list can hold in order with one mark.

## Implementation notes

### The router overflows full mailboxes (2026-09-30)

**Where things are.**

- **`tah_message.may_overflow`** (`scheduler.h`) asks for section 2. The
  router sets it for all its mail (`tah_router_submit()`), and so does
  `tah_replay_redeliver()` for owed mail. `tah_sched_post()`,
  `tah_sched_post_waiting()` and `tah_sched_post_dead()` accept such a
  message past the limits instead of answering `TA_ERR_BUSY`, and mark its
  envelope. Everything else posts without it and is refused as before.
- **The envelope's mark** (`tah_envelope.overflow`) is a bit beside `live`
  and `has_jump`, which became bit-fields: the envelope stays 104 bytes.
  A marked envelope counts in `tah_sched.overflow`, not in `queued` and
  `retained`, until it is moved in (`tah_sched_move_in()`). Giving it back
  uncounts it from whichever it is in.
- **The room queue** (`tah_sched.room`, a ring of actor IDs, allocated at
  its first push) and **the parked mark** (`tah_sched_entry.parked`, one
  byte of the entry's reserved two: the entry stays 16 bytes):
  - a delivery whose head is overflow moves it in when there is room and
    no actor waits for room; otherwise the actor is parked on the room
    queue and stays `READY`, off the ready queue;
  - `tah_sched_unpark()` runs before a turn's deliveries and after its
    dead-letter reports. While there is room it pops parked actors, moves
    each one's head in and puts it on the ready queue;
  - an entry whose actor was woken, given new mail at its head by a
    transfer or a load's hand-over, finished or removed has its mark
    cleared, and its entry on the room queue is skipped when popped.
- **Quiet:** a scheduler with parked actors and room is not quiet; one
  with parked actors and no room is, so a full system does not spin.
  `tah_system_cancel()` asks for a turn, since a withdrawn message makes
  room.
- **Counters:** `tah_sched_stats.overflow`, and `queued` now includes it.
  napi's `stats().messages.overflow` and the metric
  `tinyactors.messages.overflow` (a gauge); the test host's
  `hosted.mailboxes` answers `overflow`, and `hosted.create` takes
  `max_queued_messages`. `tah_queued_message.overflow` marks a message in
  a mailbox walk.

**What needed no change, and why.** Overflowed mail is in the receiver's
mailbox list, so everything that walks or moves the list handles it:
- `accepted` is written by the `QUEUED` note, before the envelope is
  linked, as for any message;
- a checkpoint's owed messages (`tah_checkpoint_owed()`), in order;
- a replacement's transfer and a load's hand-over, which keep the marks;
- a destroy's, unload's or finish's dead letters (the test host's
  `hosted.evict` of an actor with mail waiting makes them dead letters
  too), and napi's `evict()`, which refuses while any mail waits.

**Tests.**

- `mailbox_test`, `overflow_between_actors`: past `max_queued` the mail
  between actors is accepted, in order, and the application's is refused;
  nothing overflowed is delivered without room, and the system is quiet;
  room goes to the actor that asked first; overflow to an actor that is
  destroyed is a dead letter and makes no room; the counters return to 0.
- `replay_test`, both families logged, each address's records in its own
  log: n1 sent to sink/b while the queue is full overflows, with its
  `accepted` in sink/b's log and nothing held by ping/p1. It is delivered
  once when room appears; after a crash and a replay of both from
  `spawned`; after ping/p1's eviction and an exit; and from a checkpoint
  of sink/b, which owes it.
- The test host's hosted scenarios: the same through `hosted.create`'s
  limit, a crash and an eviction.
- napi `test/replay.js`, `mailPastAFullQueue` on Node and Bun: a delayed
  send that fires while mail waiting for a load holds the queue full; a
  crash, and an eviction and exit, then a new system that loads both from
  their logs: the ping is delivered once.
- **Replaced:** `replay_test`'s three tests of a send waiting in the sender
  after RETRY (core ADR 0095's kernel tests). A full queue no longer makes
  one; core's own tests of ADR 0095 still ran until the change below.

**Costs, measured.** Per actor nothing: the scheduling entry stays 16
bytes and the envelope 104; `tah_sched` grows by 88 bytes per system (the
room queue's ring and a counter), and the ring allocates nothing until an
actor parks. `scale.js` in `test:full` reports 328.3 native B/actor at a
million actors, on Node and Bun, and still no allocator call in steady
state. `mise run bench-compare 0c61dd1` (Node machine a99e44a37e2e, Bun
machine c989bd79a51f), with the variants the first two runs flagged run
again four times each:

| | Node | Bun |
|---|---|---|
| tracing, untraced, ns/event | 497 → 485 (-2.3%) | 413 → 416 (+0.7%) |
| tracing, summary without logs, ns/event | 952 → 965 (+1.4%) | 889 → 899 (+1.2%) |
| tracing, full + values, RSS B/actor | 1,462 → 1,453 (-0.6%) | 1,372 → 1,519 (+10.7%) |
| input log, logged, ns/event | 3,929 → 3,977 (+1.2%) | 3,230 → 3,157 (-2.3%) |
| native B/actor, every variant | unchanged | unchanged |

Bun's RSS in "full + values" is the one figure outside the 5% rule. Its
core, native and heap bytes per actor and its allocations per event are
unchanged, and Bun's RSS moves as much the other way in variants next to
it (tracing "full", -9.5%; its heap in "logged" moved -45.6% in one run
and +82.4% in the other), so it is the runtime's collector, not this
change: nothing it adds runs unless the queue is full.
