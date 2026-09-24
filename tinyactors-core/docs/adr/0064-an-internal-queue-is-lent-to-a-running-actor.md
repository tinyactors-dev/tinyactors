# ADR 0064: An internal queue is lent to a running actor

**Status:** Accepted and implemented; refines ADR 0018, applies ADR 0047 and ADR 0061. See the implementation notes for where the code differs from the text. Refined by ADR 0069 (the queue becomes the event block, lent from delivery; the host sizes its cache).
**Date:** 2026-09-23

## Context

ADR 0018 keeps rarely used facilities out of an actor's fixed storage. They
live in sparse state, which is allocated when an actor first needs it. The
internal event queue is one of them: `ta_actor_sparse_state.internal_events`
is a `ta_vec` of `ta_queued_event`.

Every microstep needs that queue. ADR 0047 requires that a failed
allocation happens before any part of a microstep is visible. A microstep
may raise an internal event, so `ta_prepare_microstep_queue()`
(`src/interpreter.c`) reserves one free slot before each microstep of a
bootstrapped actor. That call creates the sparse state if there is none,
and the first reservation gives the vector a capacity of eight events. At
the end of the macrostep, `ta_compact_sparse()` destroys the vector and,
when nothing else is in use, frees the sparse state.

So even a trivial actor, whose macrostep takes one transition and raises
nothing, makes two allocations and two frees for each external event. On a
64-bit host a `ta_queued_event` is 96 bytes, so the vector alone is about
770 bytes, plus the sparse state, for a queue that stays empty. An actor
that keeps sparse state because it has pending delayed sends (ADR 0054)
still loses its vector at the end of every macrostep and allocates it again
at the next one. At four million actors with a message each, that is millions of
allocator calls that do nothing useful.

The storage cannot simply be kept per actor. Eight queued events for every
actor that has ever run would more than double the memory the scale test
measures for a simple actor (about 560 bytes).

Only one interpreter runs at a time in a system (ADR 0017), apart from the
nested interpreters that exit a cancelled invoked child (ADR 0060). So the
number of queues in use during a run is small. Only an actor that stops
with events still queued needs a queue of its own afterwards.

## Decision

1. **An internal queue is its own object, and the actor holds one pointer
   to it.** A `ta_internal_queue` holds the queued events and its capacity
   in one allocation. `ta_actor` gains `ta_internal_queue *internal_queue`,
   which is null when the actor has no queued internal events and is not
   running. `internal_events` leaves the sparse state. An actor no longer
   needs sparse state because it runs.
2. **The system keeps a small cache of empty queues** (four by default,
   enough for the nested interpreters of ADR 0060).
3. **A run is lent a queue.** When `ta_actor_resume()` starts a
   bootstrapped actor's run, the actor uses:
   - its own queue if it has one (it paused earlier, or events were queued
     for it outside a run);
   - otherwise, a queue from the cache;
   - otherwise, a new allocation.

   Getting the queue happens before the first microstep becomes visible.
   If allocation fails, the run returns `TA_ERR_NOMEM` with nothing
   changed, as the reservation does today (ADR 0047). Before each
   microstep, the run still makes sure one slot is free. That grows the
   queue only when it is full.
4. **When the run returns, the queue goes back or stays.**
   - Empty queue (the actor is stable, or finished): the queue goes back to
     the cache, or is freed when the cache is full. The actor's pointer
     becomes null.
   - Events still queued (a budget pause, a pause inside executable
     content (ADR 0061), or a `TA_ERR_NOMEM` retry): the actor keeps the
     queue. Keeping it moves a pointer, so it cannot fail. A pause never
     needs an allocation for the queue.
5. **Enqueueing outside a run** (`ta_actor_report_invoke()`, loading an
   image) uses the actor's queue if it has one, or takes one as in point 3.
   It fails with `TA_ERR_NOMEM` as it does today.
6. **Sparse state holds only what is uncommon:** invocations, the parent,
   the completion, activation effects, delayed sends and a paused
   continuation. `ta_compact_sparse()` no longer checks or destroys the
   internal queue.
7. **Session images** capture and restore the queue's events as today. A
   restored actor with queued events gets its own queue.
8. **Measurement.** The scale test (`tinyactors-test/tests/scale_test.c`)
   counts allocator calls. After warm-up, delivering an event to a simple
   actor and resuming it to stable makes no allocator call.

## Consequences

- An actor that raises no internal events does no queue work beyond
  taking and returning a cached queue. An actor with delayed sends keeps its
  sparse state but no longer reallocates a queue for each message.
- Each actor gains one pointer (8 bytes). An actor that never uses an
  uncommon facility never allocates sparse state, where today every
  macrostep allocates it for a while.
- A paused actor keeps a whole queue. That is at most the number of paused
  actors, which the host bounds with its budgets.
- The ADR 0047 guarantee is unchanged: the only allocation that can fail
  happens before the microstep is visible, and a pause never allocates for
  the queue.
- Hosts see no API change.

## Implementation notes

2026-09-23.

- **The queue** is `ta_internal_queue` in `src/actor.h`: a count, a
  capacity and the events as a flexible array, in one allocation. It
  starts at 8 events and doubles when full. `ta_actor.internal_queue` is
  the one pointer; inspection's `internal_events` points into it.
- **Lending** happens in `ta_queue_room()` (`src/interpreter.c`), which the
  pre-microstep reservation and every enqueue call. It takes the actor's
  own queue, else the newest cached one, else a new one, and doubles a
  full queue. The run takes its queue at its first microstep, not at the
  top of `ta_actor_resume()`. That is the same point in ADR 0047's terms:
  before anything in the run is visible.
- **Returning** happens in `ta_queue_settle()`, called when
  `ta_actor_resume()` returns, whatever the result, and when an actor is
  freed. A queue that grew past 64 events is freed instead of cached, so
  the cache never holds more than four small queues.
- **Nested interpreters** (ADR 0060) need nothing special. Each actor
  holds its own queue while it runs, and a cancelled child's queue goes
  back to the cache when the child is freed. The four cached queues cover
  that nesting without an allocation once warm.
- **Deviation: sparse state before a microstep.** Decision 1 says an actor
  no longer needs sparse state because it runs. That holds only for charts
  whose microsteps never keep anything there. The old reservation created
  the sparse state together with the queue, and that hid the other users,
  which call `ta_sparse()` halfway through a microstep:
  - a refused or delayed send (`ta_actor_schedule_send`);
  - an invocation of an entered state;
  - a top-level final state's completion;
  - a replacement candidate's held effects.

  The allocation-failure scenario caught this. With a warm queue cache,
  the relay's refused self-send was the first allocation, and it failed
  after exit and transition content had run, leaving the actor in no
  state. The compiler now records `microstep_needs_sparse` (a `<send>`, an
  `<invoke>`, or a top-level final state), and the reservation also
  creates the sparse state for such a chart, and for an unpublished
  replacement candidate, before the microstep is visible. A chart with
  none of these, the common case of decision 8, still runs with no sparse
  state and no allocator call. A chart with a `<send>` makes the same one
  sparse allocation per macrostep as before, but no longer a queue
  allocation.
- **Measurement** (`tinyactors-test/tests/scale_test.c`,
  `check_event_allocations`): after warm-up, 1,000 `go`/`done` round trips
  on a simple actor and 1,000 `rearm` events on an actor holding a pending
  delayed send. Built against the core before this change, the first
  makes 2.00 allocations and 2.00 frees per event and the second 1.00 and
  1.00. Now both make 0.00, and the test requires 0.
- **Per-actor memory** grows by the pointer, from about 555 to 563 bytes.
- **Scenario:** `pausing: queued internal events survive pauses and
  residency changes` pauses a chart after every action. Whenever events
  are queued it captures, unloads and loads the actor, and checks the log
  and configuration against an uninterrupted run.
