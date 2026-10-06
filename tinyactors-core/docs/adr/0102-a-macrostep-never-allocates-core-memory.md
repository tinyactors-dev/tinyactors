# ADR 0102: A macrostep never allocates core memory

**Status:** Accepted (2026-10-05); not yet implemented. Supersedes ADR 0070 for core's memory, and ADR 0047; refines ADR 0064.
**Date:** 2026-10-05

## Context

ADR 0023 puts every allocation behind the host's `ta_allocator`, and ADR
0047 makes a refused allocation inside a macrostep retryable: the
macrostep is suspended, and a later resume finishes it. ADR 0070 makes
that hold everywhere. A run that runs out of memory stops where it
stands, saves its position into a spare continuation, and answers
`TA_ERR_NOMEM`. Every step either reserves what it needs before it has an
effect, or is done again. Plugins gain `TA_OP_NOMEM` to say "I ran out and
changed nothing".

That machinery is large, and each allocation that can fail mid-run needs
its own care. ADR 0070 lists them: a second `<raise>` or error in one
microstep, a delayed `<send>` growing its owner's storage, interning a
run-time name, scratch for `<param>`s, `done.state` events and a
completion, recording an invocation, holding a send or a `<log>` for an
unpublished actor. Its open points show what is still not covered: an
ECMAScript data model cannot say it changed nothing, libjq aborts on its
own failure, an expression with side effects can run twice, and exiting a
cancelled child does not stop.

The [ring protocol proposal](../../../docs/proposals/ring-proto/README.md)
removes the reasons for most of these allocations. Mail and delayed sends
become envelopes in a pool ([envelope.md](../../../docs/proposals/ring-proto/reference/envelope.md)).
Run-time names are looked up, not interned
([names.md](../../../docs/proposals/ring-proto/reference/names.md)).
Effects become c2h records. What is left is the internal queue, the
invoke table, invocation startup and nested interpreters. This ADR
decides that none of them allocate inside a macrostep, and what happens
instead when something is full. The source is
[macrostep.md](../../../docs/proposals/ring-proto/reference/macrostep.md)
and item 2 of the proposal's
[ADRs to write](../../../docs/proposals/ring-proto/reference/migration.md#adrs-to-write).

Two facts shape the decision:

- **A pause helps only when someone other than the session can make
  room.** The host frees c2h by draining it. Only sessions free the pool,
  the name area and their own internal queue.
- **A data model's memory is not core's.** Core cannot free it, cannot
  know when it will be free, and cannot check that a foreign operation
  that ran out partway changed nothing. ADR 0047's retry is safe only for
  operations that changed nothing.

## Decision

1. **Core allocates only at safe points.** A macrostep never calls core's
   allocator. Core allocates when:
   - a system is created (rings, the running workspace);
   - a definition is registered (its compiled form, its atoms, and the
     running workspace grown to the largest registered definition,
     mandatory internal-event storage included);
   - a session is created or loaded (its fixed-size record, sized by its
     definition, and any nested interpreter workspace the definition
     needs, with that workspace's mandatory storage);
   - it refills the pool, the name area, the slot records and the park
     area up to their bounds, at admission and after a macrostep end;
   - it starts invocations, in the startup phase after the macrostep
     (decision 6).

   If an allocation at a safe point fails, that operation is refused as a
   whole and changes nothing. `TA_ERR_NOMEM` arises only there. The data
   model allocates as it likes: that memory is the data model's.
2. **Every overflow becomes a refusal to someone who can retry.** Nobody
   keeps a hidden queue that grows.

   | Who can retry | How they are told |
   |---|---|
   | the external client | the host refuses at its edge when h2c is full or core is not admitting |
   | the sending chart | its action fails as `error.execution.capacity.<resource>` in its own macrostep |
   | nobody (an asynchronous reply) | it is dropped; the chart's own timeout covers it |

   [BEHAVIORS.md](../../../docs/proposals/ring-proto/BEHAVIORS.md) lists
   the cases one by one.
3. **A capacity failure is `error.execution.capacity.<resource>`.** The
   action that could not be done raises it, and it works like any error
   in executable content: the rest of the block is abandoned, so a
   fanout fails once. The last segment says what ran out: `pool`,
   `names`, `internal` or `record`, the catalog's closed
   `capacity_resource` enum. `_event.data` says where: the action and the
   `<foreach>` iteration, as `ta_trace_position` gives them. A chart that
   does not care which resource matches `error.execution.capacity`; a
   chart written without capacity errors in mind catches them in its
   `error.execution` handler. The name, not only the data, carries the
   resource, because SCXML charts choose by matching.
4. **The internal queue has fixed storage in two quotas.** Internal events
   (`<raise>`, `done.state.*`, `error.*`) share one ordered queue, a
   `ta_fifo` (`src/fifo.h`) with two `ta_quota`s beside it
   (`src/quota.h`). It is not in the envelope pool.
   - **The ordinary quota** is `internalQueueEvents` entries (default
     256), for the chart's `<raise>`. A `<raise>` that finds it full fails
     as `error.execution.capacity.internal`.
   - **The mandatory floor** is sized by the compiler, for the events the
     interpreter must deliver: errors and `done.state` events. It is one
     round of every phase: `B + E + C + V + F` entries, for the
     bootstrap, one microstep's independently executed blocks and data
     initialization, one selection pass, one invocation-startup pass, and
     final-state completion data with stop or cancellation cleanup. The
     compiler counts independent failure sites, since SCXML 4.9 stops only
     the failed block; a loop in one block needs one entry. The arithmetic
     is checked. Generated error names and diagnostic positions have
     pre-sized storage too.
   - **Both are allocated outside macrosteps:** at registration in the
     running workspace, once per system, and at spawn or load in each
     nested interpreter workspace. Neither grows during execution.
   - **Beyond the floor, mandatory events take free ordinary entries.**
     All entries keep one FIFO order. The floor is reserved capacity, not
     priority.
   - **A mandatory event that fits in neither is a runaway.** Core
     abandons the input with the chain limit's rules (ADR 0075, as the
     proposal refines it, and ADR 0082's abandon). The input completes as
     `runaway`, and the `TA_TRACE_RUNAWAY` record's `runaway_cause` is
     `internal_queue` instead of `chain_limit`. The queue is cleared.
   - **There is one live queue** in the running workspace, plus one per
     nested interpreter. A parked slot's pending entries wait in the park
     area. This replaces ADR 0064's queue lent to each actor and its cache
     of empty queues.
5. **Why a runaway, and not a pause or a larger floor.**
   - SCXML runs eventless transitions before it takes the next internal
     event. Mandatory events pile up only while a chart keeps stepping
     without reading its queue. A chart that reads its queue as it goes
     never exceeds the floor. A backlog that grows only that way is a
     runaway in all but name.
   - A pause cannot make room, because only the session drains its own
     queue. The rule for every resource is: **pause where the host can free
     space, fail where only sessions can.** A full c2h pauses the slot
     (the host drains it); a full pool, name area or internal queue fails
     in the chart.
   - A floor for the whole chain would be `B + L·E + S·C + I·V + F`
     entries, linear in the chain limit.
   - A coalesced marker in place of the overflowing events would lose a
     `done.state` event and change control flow.
6. **Invocation startup happens after the macrostep.** Entering a state
   marks its invokes pending; exiting it before stabilization removes
   them. The survivors are evaluated and started after the macrostep end,
   in entry order and document order (SCXML 3.13 and appendix D). Starting
   a local child acquires its lifetime envelope token there. An operation
   either starts the service completely, or leaves it unstarted and queues
   its error in mandatory storage, sized for the whole startup pass before
   any invoke runs. The errors start an internal-only stabilization
   macrostep of the same session before its next external input.
7. **An actor's invoke table is fixed.** It is sized from the definition's
   `<invoke>` count when the session is created or loaded. Activation
   effects and pending invokes no longer grow a `ta_vec` inside a
   macrostep.
8. **`TA_OP_NOMEM` leaves `ta_op_status`.** A data model or invoker that
   runs out of its own memory answers `TA_OP_ERROR`, and the chart sees
   `error.execution`, as for any failed operation
   ([data-model.md](../../../docs/proposals/ring-proto/reference/data-model.md)).
   Why an error and not a pause:
   - the memory is the data model's; a pause that waits for it is a guess;
   - an operation that ran out partway, such as an `<assign>` that built
     half an object, may have changed something, so a retry is not safe;
   - `error.execution` is SCXML's answer to an operation that failed, and
     the chart's structure decides what follows;
   - it removes the last path that stops a run where it stands.
9. **What this deletes** (step 4.4 of the
   [plan](../../../docs/proposals/ring-proto/IMPLEMENTATION.md)):
   - ADR 0070's out-of-memory stop: the `TA_ERR_NOMEM` branch of a run,
     `ta_reserve_stop` and `ta_stop*` where only running out of memory
     uses them, the spare continuation and the spare sparse state, and
     marking and truncating the internal queue;
   - ADR 0047's retry guarantee, which has nothing left to protect;
   - ADR 0064's lent queues and their cache;
   - `TA_OP_NOMEM`, and `ta_op_result`'s and `ta_op_failed`'s mapping of
     it.

   A pause between units (ADR 0061) still saves into a continuation; only
   what exists solely for running out of memory goes.

### What this ADR does not decide

- The rings, the c2h reserve and why a full c2h pauses a slot: the ADR on
  the three rings.
- The envelope pool, its admission watermark and when it refills: the ADR
  on envelopes, which also supersedes the rest of ADR 0064.
- Run-time names as bytes, and the name area: the ADR on interning.
- Macrostep slots, parking, the park area and `ta_system_trim`.
- The chain limit itself, which refines ADR 0075. This ADR only adds a
  second cause of runaway.
- Exact floors for charts whose internal transitions form no cycle, and a
  lower `microstepLimit` derived from them. Both are possible later and
  change no protocol.

## Consequences

- **A macrostep has no allocation to fail.** A core test gives a system an
  allocator that refuses every request once a macrostep starts, and
  asserts that it never sees a call.
- **A shortage is chart behaviour.** A passing memory shortage in a data
  model or invoker becomes `error.execution` where it was a retry. That is
  the price of deleting the stop.
- **A chart whose error storm outruns the free ordinary entries is
  abandoned** as a runaway, where it would have finished with an unbounded
  queue.
- **Order inside a block matters to authors.** A timeout sent after a
  fanout is lost when the fanout fails; it goes before
  ([S02](../../../docs/proposals/ring-proto/scenarios/S02-full-pool.md)).
- **ADR 0097's failure rule loses its `TA_OP_NOMEM` case.** A `copy_out`
  that runs out answers `TA_OP_ERROR`, with the `error.execution` outcomes
  ADR 0097 already gives it.
- **ADR 0092's nested regions** are allocated when a session that needs
  them is created or loaded, not on first use while a cancellation runs
  inside a macrostep.
- **Callers change:** the test host's jq and quickjs data models, napi's
  data model and core's null data model answer `TA_OP_ERROR` where they
  answer `TA_OP_NOMEM`.
- **Costs:**
  - **Per system:** the internal queue is the mandatory floor plus 256
    ordinary entries of about 48 bytes, about 12 KiB, once in the running
    workspace and once per nested interpreter workspace. A floor for the
    whole chain would grow with the chain limit; for a chart with three
    failure sites per microstep at the default limit it would be about
    14 MB. That figure is an illustration, not a measurement. The running
    workspace with the defaults is `interpreterBytes` 65,536 plus 12,288,
    77,824 bytes plus the floor.
  - **Per system, removed:** the spare continuation (248 bytes to about
    1.3 KB) and the spare sparse state (176 bytes) of ADR 0070, and ADR
    0064's cache of four queues.
  - **Per actor:** 332 bytes before (`scale_test`, ADR 0089 and ADR 0092).
    After: the internal-queue pointer of ADR 0064 has nothing left to
    point at, since no actor owns a queue, and an actor with
    invocations holds a fixed invoke table sized by its definition. Step
    4.4 measures the figure and records it in this ADR's implementation
    notes.
  - **Per macrostep:** no allocator call, where ADR 0070 allowed a
    reservation per unit.
  - **Tests:** the out-of-memory sweeps (fail every allocation in turn)
    shrink to system creation, definition registration, spawn, load and
    refills at safe points. The sweep inside a macrostep
    (`out_of_memory.rb`, `pause_test`'s refused allocations) has nothing
    left to fail.

## Implementation notes (2026-10-06)

Decision 1 holds for the running session's own macrostep: its pause
storage, internal queue, invoke table, pending invocations, nested
regions and name buffer are allocated when it takes its input, and a
macrostep of a session that invokes host services, raises, sends,
schedules and cancels makes no allocator request
(`tests/macrostep_alloc_test.c`). Decision 4's queue is still one per
session, sized at the take (`internalQueueEvents` plus a floor of
`1 + data + 3 × states + 2 × transitions + invokes`) and returned to a
list of up to 64 spares, rather than one live queue per workspace. A
queue full of mandatory events still stops the session as running out of
memory did, not as a runaway. Data models, `copy_out` and invokers that
run out of memory are `error.execution`; `TA_OP_NOMEM` remains for an I/O
processor's send and for image bytes. Invocation startup inside the
macrostep (decision 6), and with it a local child's spawn and an
unpublished session's held effects, still allocate there.
