# ADR 0101: Delayed sends are envelopes

**Status:** Accepted (2026-10-05); implementation in progress (see the implementation notes). Refines ADR 0026 and ADR 0054.
**Date:** 2026-10-05

## Context

ADR 0012 keeps delayed sends in core, on a logical clock the host moves.
ADR 0026 says a send belongs to its session but may be indexed globally.
ADR 0054 stores each owner's sends in its sparse state, in one array
sorted by `(due, timer id)`, and keeps a global heap of owners, keyed by
each owner's front send. That is `src/timer.c` and `src/timer.h`, 639
lines today, plus a `ta_timer_list` of 32 bytes in the sparse state of
every actor with a pending send, plus that list's two arrays.

The ring protocol proposal puts every pending input in one pool of
envelopes per system
([envelope.md](../../../docs/proposals/ring-proto/reference/envelope.md)).
Mail, admitted events and invocation reports are envelopes. A delayed send
is pending input too: it waits for a time instead of a slot. Keeping a
second store for it means a second capacity, a second quota and a second
path into session images.

ADR 0054 also has costs of its own:
- inserting or cancelling one send moves array elements, O(k) for an
  owner with k sends;
- scheduling an owner's first send can allocate its sparse state and grow
  its arrays, inside a macrostep;
- a send the router refused becomes "submitted" and may outlive its owner
  in a system-owned orphans list.

The ring protocol removes the router: a due send to an I/O processor is a
`c2h.send` record, and no send is ever refused and held back.

This ADR records item 4 of the proposal's migration plan
([migration.md](../../../docs/proposals/ring-proto/reference/migration.md#adrs-to-write)).

## Decision

1. **A delayed send is an envelope with a due time.** A delayed `<send>`
   takes an envelope from the pool in state `timer` and sets its `due`.
   The envelope goes into the pool's deadline index and onto its sender's
   owner list. This is the same for every target: a local session or an
   I/O processor. The envelope keeps the evaluated data and content, the
   I/O processor type, the target and the event metadata. Firing evaluates
   nothing again.
   ([envelope.md](../../../docs/proposals/ring-proto/reference/envelope.md#delayed-sends-envelopes-with-a-due-time))

2. **The deadline index is a binary heap of envelope indexes.** It orders
   envelopes by `(due, send_id)`. Each envelope records its position in
   `heap_index` (`src/heap.h`), so a removal from anywhere costs O(log n).
   The heap's positions live in a paged array (`src/paged_array.h`) that
   grows in the pool's refill and is trimmed with the pool by
   `ta_system_trim`, between `ta_run` calls.
   - **Why a paged array:** at the largest pool (`poolEnvelopes`
     16,777,216) the positions take 64 MB. One reallocated array would
     copy 64 MB at a single refill and need it in one piece. Pages are
     added without a copy; only the directory of pages is copied, when it
     fills.
   - **Why it can be trimmed:** whole free pages above the reserve go back
     to the allocator with the pool's own pages, so the index never holds
     more than the pool it indexes.

3. **The owner list.** Each session has an intrusive list of its pending
   delayed sends in send order, linked through the envelope's
   `owner_next` (`src/list.h`). It replaces ADR 0054's per-owner arrays.
   The list needs no memory of its own.

4. **`<cancel>` is exact.** Core walks the sender's owner list and removes
   and frees every pending envelope whose `send_id` matches, duplicate
   authored IDs included. A timer that core has already turned into input
   cannot be cancelled. Core fires a timer only when it applies
   `h2c.time`, with nothing between, so a cancelled timer never arrives
   and there is no race with the host.

5. **One wake.** Core writes
   [`c2h.wake`](../../src/generated/MESSAGES.md#c2hwake) when the earliest
   due time in the index changes. While c2h is full, core keeps the latest
   head and writes the current deadline before it completes the input or
   time operation. The host keeps one timer for resident sessions.

6. **Order is `(due, send_id)`.** When
   [`h2c.time`](../../src/generated/MESSAGES.md#h2ctime) moves the clock,
   every timer due by then leaves the index in that order. `send_id`
   counts per sender:
   - one sender's timers due at the same time leave in send order;
   - timers of different senders with the same due time have no order
     between them, as mail between sessions has none.

   A due timer for a local target joins the end of the run queue. A due
   timer for an I/O processor is written as a `c2h.send` with `macrostep`
   0 and no `c2h.macrostep_ended`, and its envelope is freed after the
   record commits. If the record does not fit, the envelope keeps all its
   references and core returns so the host can drain c2h.

7. **Timers travel with their owner's image.** An unload writes the
   session's pending delayed sends into its image, with their target text,
   and frees their envelopes. `c2h.unloaded` carries `wake_at`, the
   earliest of them, with the `has_wake` flag. The host keeps an index of
   `wake_at` for unloaded sessions and posts `h2c.load` when one comes
   due. A load restores the timers with their stored due times, and they
   rejoin the deadline index; the next `h2c.time` fires any that are due
   ([unloading.md](../../../docs/proposals/ring-proto/reference/unloading.md)).

8. **A timer that falls due for an unloaded target** waits like other
   mail, on the address's waiting list. If no waiting entry or no
   name-area room is left, even after the refill at that safe point, it
   is recycled in place as `error.communication` for its sender. It does
   not stay due.

9. **Capacity is the pool's.** A timer takes an envelope until it is due,
   whatever its target. When the pool is full, the delayed `<send>` fails
   as `error.execution.capacity.pool`.
   [`maxPendingSends`](../../../docs/proposals/ring-proto/BEHAVIORS.md#maxpendingsends)
   becomes a per-session quota inside the pool: past it, the send fails
   the same way, as if the pool were full for that session. Its default
   stays no quota.

10. **Removed:** ADR 0054's per-owner sorted arrays, the sparse state that
    holds them, the global heap of owners, the orphans list and the
    submitted state. `timer.c` and `timer.h` are replaced by the deadline
    module of plan step 4.3.

## Consequences

- **Costs.** These are estimates. Step 4.3 measures them and its
  implementation notes record the figures.

  | | Before (ADR 0054) | After |
  |---|---|---|
  | One pending delayed send | an element of two arrays in the owner's sparse state, plus one owner heap slot per owner | one envelope (88 bytes, shared with all pending input) plus 4 bytes for its position in the deadline index |
  | Per actor | a 32-byte `ta_timer_list` in sparse state, only for an actor with sends | a 4-byte owner list head in every actor |
  | Code | `timer.c` and `timer.h`, 639 lines | the deadline module over `heap.h`, `paged_array.h` and `list.h` |
  | Allocation in a macrostep | scheduling an owner's first send may allocate | none: the send takes an envelope that already exists |

  The owner list head counts as 4 bytes, as in the proposal. If it keeps
  a tail for appends, as `ta_list` does, it is 8.
- **Cancel is O(k)** for an owner with k pending sends, a walk with no
  array moves. ADR 0054's rule holds: a question about one session costs
  that session's size.
- **Dispatch order across senders changes.** ADR 0054 orders ties by a
  system-wide timer ID. This ADR orders ties only within one sender. A
  test that relies on an order between senders' timers with the same due
  time must stop doing so.
- **A fanout of timers is limited by the pool**, not by slots per actor.
  10,000 per-notification timeouts take 10,000 envelopes.
- **The host keeps two things:** one timer, set by `c2h.wake`, and its
  index of unloaded sessions by `wake_at`. It may merge them into one.
- **A session's state is one artifact.** Its pending timers are in its
  image and in its `c2h.state`. Replay rebuilds them.

**What this ADR does not decide:**
- the envelope's layout, the pool's default size and its refill, which
  belong to the decision that pending inputs are envelopes (migration
  item 3);
- the image format for timers, beyond that they travel in it;
- what replaces operator withdrawal of one pending send (ADR 0084), and
  how a replaying session's timers behave (ADR 0089);
- how inspection presents pending sends, which ADR 0054 sections 4 and 5
  define over the arrays this ADR removes.

## Implementation notes

**Delayed sends are envelopes (2026-10-06).** `src/deadline.c` replaces
`timer.c` and `timer.h`. The host's dispatch call is unchanged so far;
time and dispatch change with the next part of the step.

- **The record.** A timer envelope holds its due time, its send order (in
  `source_seq`), its send ID and its owner's runtime ID (`from`) in the
  envelope's own fields, the data and a host processor's evaluated target
  in `data` and `content`, and the rest in one text field of about 41
  bytes: event name, I/O processor, destination, invocation, literal
  content and target IDs, and a submitted send's origin. A traced send
  adds the trace field. A pending send takes one envelope and one 64-byte
  chunk, two when traced.
- **The owner list** is a 4-byte head on the actor, with new sends at the
  front. Readers that need order (inspection, export, images) sort a
  session's sends by `(due, order)`, a heapsort over its k sends.
  `ta_actor` grows from 240 to 248 bytes (the head and its padding); the
  sparse state loses its 32-byte `ta_timer_list`, and an actor with a
  pending send no longer needs a sparse state for it.
- **A due send to a session gives its envelope back before it is
  delivered**, and the delivery takes one again, at a safe point, so it
  never finds the pool full (decision 6). The submitted state remains only
  for a send its host I/O processor refuses (out of memory), until I/O
  processors become `c2h.send` records; such a send keeps its place in the
  index and, when its owner goes, stays indexed on no list.
- **Inspection and export** read a session's sends through a view buffer
  the system keeps: room for 16 sends from creation, grown at the pool's
  refill to the longest owner list seen, and otherwise when they ask,
  outside any macrostep, so only then can they answer `TA_ERR_NOMEM`. An
  owner list keeps its length beside its head, which fills padding:
  `ta_actor` stays 248 bytes.
- **Order.** Ties are broken by the send order, a system-wide counter, as
  ADR 0054's timer ID did, so dispatch order did not change.
- **Tests.** Core tests that made a send submitted by filling the pool now
  make its I/O processor refuse it; populations of tens of thousands of
  pending sends run with a pool and name area sized for them.
- **Invocation tokens.** Since a kept completion is a timer, and a timer
  takes an envelope, a full pool could drop a completion. A local invoked
  child therefore holds its invocation token (`TA_ENVELOPE_INVOCATION_TOKEN`)
  from its spawn: one envelope and name-area room for one traced timer
  record. Publishing the completion gives the token back first, so the
  completion's mail or its keeping timer takes that room. A child restored
  from an image takes its token again. The unused `TA_ENVELOPE_CHILD_STOP`
  state is removed: core stops a child synchronously.

