# ADR 0103: Macrostep slots run inputs in turn

**Status:** Accepted (2026-10-05); not yet implemented. Refines ADRs 0055, 0061, 0075 and 0092 (the microstep limit can no longer be 0); removes napi's quarantine (napi ADRs 0008 and 0025).
**Date:** 2026-10-05

## Context

The [ring protocol proposal](../../../docs/proposals/ring-proto/README.md)
moves the host's queues and scheduler into core. Core takes an input,
runs its macrostep, and writes the effects to c2h. This ADR records how
core shares its one thread among sessions while it does that. The source
is [macrostep.md](../../../docs/proposals/ring-proto/reference/macrostep.md#macrostep-slots)
and item 10 of [migration.md](../../../docs/proposals/ring-proto/reference/migration.md#adrs-to-write).

Today a session runs its macrostep to completion, and the host decides
who runs next:

- **ADR 0055** makes `ta_actor_deliver` the admission test. A session
  that is inside a macrostep answers `TA_ERR_BUSY`, and the host keeps
  the mail in its own mailbox.
- **ADR 0061** lets a macrostep pause between units of executable
  content, and moves the paused run into a `ta_continuation` the actor
  owns. The host's pump gives the next turn to another actor.
- **ADR 0075** stops a macrostep at a count of committed microsteps. The
  actor stays mid-macrostep and answers `TA_ERR_BUSY`. The limit can be
  turned off (0, `TA_MICROSTEP_LIMIT_NONE`).
- **napi ADR 0008** quarantines a runaway: it holds the actor until an
  operator calls `release()`. **napi ADR 0025** tells an invoked child's
  parent with `error.communication.invoke.<id>.quarantined`.

With the scheduler in core, core needs its own answer to one question.
What happens to every other session while one macrostep runs long?

- **Long but correct macrosteps exist.** A chart walks a list with a
  guarded eventless transition, a parser is driven by `<raise>` chains, or
  a `<foreach>` runs over 10,000 items. With plain run to completion each
  of them holds every other session, and every client that waits on one,
  until it ends.
- **A loop that never ends holds the whole system** until a limit stops
  it. Core cannot tell a loop from long work before the limit.
- **Today's limit misses two loops.** An eventless transition whose guard
  keeps throwing commits no microstep: each pass raises an error that is
  consumed and ignored. An `<invoke>` that fails, and whose error
  re-enters its state, makes a chain of short macrosteps. Neither reaches
  a per-macrostep count of committed microsteps.

Interleaving is for **isolation, not for deadlock**. Deadlock is
prevented elsewhere: a local send that does not fit is refused in the
chart, and a slot waits only for c2h, which the host drains
([macrostep.md](../../../docs/proposals/ring-proto/reference/macrostep.md#a-full-c2h-pauses-the-slot)).
Isolation means that a long or looping macrostep holds one slot, not the
system.

The pieces exist. ADR 0061 already stops a run between units and moves
it into a continuation. ADR 0070 makes that move unable to fail. The
index pool (`src/index_pool.h`) and the chunk chain (`src/chunks.h`) give
fixed-size items that never move, and `src/bytes.h` gives a byte sink
that writes into a chain without allocating.

## Decision

1. **Slots.** Core has up to `maxConcurrentInputs` macrostep slots
   ([BEHAVIORS.md](../../../docs/proposals/ring-proto/BEHAVIORS.md#maxconcurrentinputs)).
   A slot is one session's execution of one input, from take until
   `c2h.input_completed`. That includes the macrostep, the post-macrostep
   routing and invocation startup, and any internal-only stabilization
   macrosteps that startup errors cause. A slot holds the taken envelope,
   the outgoing mail list, the outstanding c2h reserve and the chain
   count. A session that holds a slot is **active**.
   - A session runs one input at a time. Several sessions run inputs at
     once, one per slot.
   - Default 64, minimum 1, maximum 65,536. **With one slot, this is
     plain run to completion.**
   - Slot records are allocated on demand, between macrosteps, as the
     pool is. When core would fill a slot and every record is busy, it
     allocates one more, up to `maxConcurrentInputs`. If the allocation
     fails, the envelope waits for a slot to free. No error results.

2. **Filling a slot.** Core fills a free slot from the run queue in
   arrival order, and skips envelopes whose target is active. A skipped
   envelope moves to the end of its session's **deferred list**. When the
   session's input completes, core returns the first envelope of that
   list to the run queue at the position its arrival number gives it
   ([envelope.md](../../../docs/proposals/ring-proto/reference/envelope.md#mail-for-an-active-session)).
   - The host never sees a busy session. `TA_ERR_BUSY` stops being an
     answer to the host (refines ADR 0055), and the host's mailboxes and
     scheduler go.
   - A session's mail neither jumps ahead of other sessions' earlier mail
     nor falls behind their later mail.
   - With one slot, nothing is ever deferred.

3. **Turns.** Only one slot runs at a time, on the system's one thread.
   Core works through its slots in turn. A slot runs until its input's
   chain settles, or until it has used `slotSlice` steps in this turn
   ([BEHAVIORS.md](../../../docs/proposals/ring-proto/BEHAVIORS.md#slotslice)).
   Then core moves to the next slot that can run, within the same
   `ta_run`.
   - A step is a microstep, a selection pass or a unit of executable
     content, as ADR 0061 counts them. Default 1,024, minimum 1, maximum
     1,048,576.
   - **Slots interleave only between units of executable content,** where
     ADR 0061 already lets a run stop. They never interleave inside a data
     model call: a synchronous call that never returns still holds the
     thread.
   - A slot also leaves its turn when c2h has no room for the next unit's
     record. It waits before that unit, and nothing of the unit has run.
   - When `ta_run`'s `budget` or `action_budget` runs out, the running
     slot stays where it is and nothing is copied (refines ADR 0061: the
     budgets are per `ta_run`, not per actor).
   - Parking happens only between top-level units. A child's exit
     handlers, run in a nested interpreter while the parent cancels an
     invocation, finish inside the unit that started them.

4. **One running workspace and a park area.** The system has one running
   workspace: the interpreter scratch, the internal queue with its
   ordinary quota and its mandatory floor, and the frame stack, sized for
   the largest registered definition. Only the running slot uses it.
   - **Parking.** When a slot's slice ends, or it waits for c2h, core
     copies its parked execution into the **park area**: its
     continuation at its own definition's size, its pending internal
     events in order, and its counters (chain count, slice position,
     trace keys). The taken envelope and the outgoing mail are already in
     the pool, and the c2h reserve is a number, so they do not move.
   - **Chunks.** The park area holds parked executions as chains of
     256-byte chunks, each with a 4-byte link to the next (`src/chunks.h`
     over `src/index_pool.h`). Parking writes through a chunk sink and
     resuming reads back through a chunk source (`src/bytes.h`); resuming
     returns the chunks to the free list. A larger chart takes more
     chunks, not a larger block, so the area does not fragment.
   - **The area grows between macrosteps only.** Parking happens inside a
     macrostep, which never allocates, so it takes only chunks already on
     the free list. After each macrostep end and at admission, core tops
     the free list up to one parked execution of the largest registered
     definition.
   - **The ceiling is derived:**
     `maxConcurrentInputs × (largest continuation + full internal queue)`,
     rounded up to whole chunks, computed at registration. No byte
     parameter must be set. `parkAreaBytes` is an optional lower cap
     ([BEHAVIORS.md](../../../docs/proposals/ring-proto/BEHAVIORS.md#parkareabytes)).
     0 disables parking, which is plain run to completion.
   - **Parking can fail without harm.** A slot that finds too few free
     chunks at the end of its slice keeps the thread for another slice,
     and core tries again at its next slice end. Correctness never
     depends on parking; only isolation does.
   - **Giving memory back is the host's decision.** Chunks return to the
     free list, not to the allocator. `ta_system_trim(system, keep)`, a
     native call between `ta_run` calls, frees whole free pages of the
     pool, the name area, the park area and the deadline index. Core never
     trims on its own: when to give memory back is policy.

5. **The chain limit** (refines ADR 0075). `microstepLimit` counts one
   input's whole chain, from take until `c2h.input_completed`:
   - every microstep;
   - every selection pass, including one that enables nothing and only
     consumes an internal event;
   - every invocation-startup pass.

   Units of executable content do not count. A `<foreach>` is one
   microstep however long it runs; it holds one slot and rotates with the
   others. **The limit cannot be turned off:** 0 and
   `TA_MICROSTEP_LIMIT_NONE` are refused, for the system and for a
   definition. `ta_actor_resume_params.no_microstep_limit` goes with
   `ta_actor_resume`. A chart whose legitimate work needs more steps gets
   a higher limit for its definition, set ahead of time.

6. **A runaway ends the input, not the session.** The check sits between
   steps, where nothing is open. At the limit core applies ADR 0082's
   abandon at that boundary:
   - the configuration and data after the last completed microstep stay;
   - the internal queue and the local mail the macrostep sent are freed
     and never delivered;
   - effects already written to c2h stay done;
   - `c2h.macrostep_ended` and `c2h.input_completed` say `runaway`, and
     core writes ADR 0075's `TA_TRACE_RUNAWAY` record, with
     `runaway_cause` `chain_limit` (or `internal_queue` for a mandatory
     overflow);
   - the slot is free, and the session's deferred list returns its first
     envelope to the run queue. **The session lives on** and takes its
     next input.

   The chart does not see it: no `error.*` event is raised, as ADR 0075
   decided. The host sees it, and decides what to do about a session that
   runs away on every input, for example `h2c.destroy` after a number of
   runaways. That is application policy.

7. **Quarantine is removed** (napi ADRs 0008 and 0025). napi's
   quarantine, `system.release()` and the parent's
   `error.communication.invoke.<id>.quarantined` event go. Slots already
   keep one bad actor from taking the loop's turns. Holding a runaway
   would also:
   - let its mail fill the pool while nobody looks;
   - leave the host's promise for its input pending;
   - stop a replay whose seed loops from ever being published.

   A child whose input ran away is not stuck. A parent that waits for its
   `done.invoke` relies on its own timeout, as for any child that does not
   finish.

8. **Operators read without attaching, and intervene by attaching.**
   `ta_actor_inspect` is a native call between `ta_run` calls. It changes
   nothing and stops no other session. Its result says whether the session
   is **stable** (in no slot) or **active** (in a slot), and gives an
   active session's position (slot, unit, `<foreach>` iteration) as
   ADR 0083 does
   ([debugging.md](../../../docs/proposals/ring-proto/reference/debugging.md#inspection-without-attaching)).
   An attached debugger takes exclusive ownership of the whole system and
   every slot. It can inspect any active slot, finish a microstep without
   starting another, abandon at a legal boundary (ADR 0082) and drive an
   operator transition. Other sessions stay stopped while it is attached.

### What this ADR does not decide

- The rings, the c2h reserve and the full-c2h wait: they are other ADRs
  of the proposal. This ADR uses them.
- The internal queue's ordinary quota and mandatory floor, and the
  runaway on a mandatory overflow: this ADR names the cause only.
- The exact signature of `ta_run` and its statuses, and the debugger's
  attachment API.
- A debugger that holds one session while the others run. It is outside
  the design.
- A lower, compiler-derived `microstepLimit` for a chart whose internal
  transitions form no cycle. It is possible later and changes no
  protocol.
- Default sizes beyond those stated here. Step 4.5 of
  [IMPLEMENTATION.md](../../../docs/proposals/ring-proto/IMPLEMENTATION.md)
  measures them.

## Consequences

- **A long or looping macrostep holds one slot, not the system.** Other
  sessions get a turn about every `slotSlice` steps.
- **The host keeps no mailboxes and no scheduler.** Mail for an active
  session waits in core, on the deferred list, in arrival order.
- **Records of different sessions interleave on c2h.** Order holds within
  one macrostep; the header's `session` tells the host which macrostep a
  record belongs to.
- **A loop that today's limit misses ends.** Throwing guards and failing
  invoke chains count toward the chain limit.
- **A chart cannot opt out of the limit.** A chart whose correct work
  passes the default needs a higher per-definition limit. A runaway's
  input completes with `runaway`, and its later inputs run.
- **No one holds a live session for an operator.** Reading is
  `ta_actor_inspect`; intervening freezes the whole system under the
  debugger.
- **Parking can briefly cost isolation.** Chunks taken during a burst of
  rotations come back only when those executions resume, and new chunks
  arrive only at the next macrostep end. A slot that cannot park runs to
  the end of another slice.
- **After a burst the park area stays at its high-water mark** until the
  host calls `ta_system_trim`.
- **Costs.** These figures are estimates; step 4.5 measures them and
  records them in its implementation notes:

  | | Before | After |
  |---|---|---|
  | per slot record | none (the host's scheduler entries) | about 48 bytes, on demand; at most 3,072 bytes with the default 64 slots |
  | per parked execution | a `ta_continuation` owned by each paused actor (ADR 0061) | about 1 KiB for a chart of 100 states, plus about 48 bytes per pending internal event, in 256-byte chunks, only while parked |
  | per actor | 332 bytes (ADR 0092) | +8 bytes: the head and tail of the deferred list |
  | park area | none | at most `maxConcurrentInputs` × (largest continuation + full internal queue), rounded to whole chunks; a reserve of one parked execution once any macrostep needs it; 0 when no macrostep outlasts one slice |
  | running workspace | per actor while it runs | one per system, sized for the largest definition |
  | allocations in a macrostep | none (ADR 0070) | none; slot records and park chunks are allocated between macrosteps |

  The per-actor figure counts the deferred list only. The proposal's
  +12 bytes per actor also includes the head of the actor's pending
  delayed sends, which another ADR of the proposal records.
- **With one slot, or with `parkAreaBytes` 0,** core is plain run to
  completion: no slot rotates and nothing parks. With one slot, nothing
  is deferred either, and only the one slot record is paid.
