# ADR 0102: A macrostep never allocates core memory

**Status:** Accepted (2026-10-04); implementation pending (ring-proto Phase 4.4). Supersedes ADR 0070 for core's memory, and ADR 0047; refines ADR 0097 section 4 (`TA_OP_NOMEM` goes). Section 6 lists what stays of each.
**Date:** 2026-10-04

## Context

ADR 0047 says a macrostep interrupted by `TA_ERR_NOMEM` can be retried.
ADR 0070 makes that hold everywhere: a run that runs out of memory stops
where it stands, at the last boundary before the allocation that failed,
and a later resume finishes it. Every allocation that can fail mid-run
needs its own care: a reservation, or a step that can be done again. The
machinery is a spare continuation and a spare sparse state per system,
the NOMEM branches of the run loop, queue marks and truncation, and
`TA_OP_NOMEM` in the plugin contract. ADR 0070's open points list what it
still gets wrong: expressions with side effects run twice, autoforwarding
resumes by index, a receiver out of memory can reorder a sender's events.

After ADRs 0099, 0100 and 0101, the allocations ADR 0070 lists have other
homes ([macrostep.md](../../../docs/proposals/ring-proto/reference/macrostep.md#what-this-replaces-the-out-of-memory-stop-adrs-0047-0070)):

| Allocation that could fail mid-run (ADR 0070) | Now |
|---|---|
| a second `<raise>` or error in one microstep | an entry of the internal queue's ordinary or mandatory quota, sized before the run (sections 2 and 3) |
| a delayed `<send>` growing its owner's storage | an envelope from the pool (ADR 0101) |
| interning an `eventexpr`, `typeexpr` or target | a lookup (ADR 0100) |
| scratch for `<param>`s | fixed scratch, sized at creation (ADR 0092) |
| `done.state` events, a top-level final state's completion | mandatory storage; a local parent's completion uses the invocation's token (section 4) |
| recording an invocation | a slot in the session's record, sized by the definition's `<invoke>` count when the session is created |
| holding a send or a `<log>` for an unpublished actor | a replaying session writes no effects (Phase 5.5); its timers are envelopes |

What remains is the internal queue, which still grows, invocation startup,
which creates sessions inside the macrostep, and data models that answer
`TA_OP_NOMEM`. This ADR closes those, and deletes the stop machinery.

## Decision

### 1. Where core allocates

Core allocates in these places only, all outside a macrostep:

- **system creation:** the control block and rings (ADR 0098), the running
  workspace;
- **definition registration:** the compiled form, its atoms (ADR 0100), and
  growing the running workspace and any nested interpreter workspace for
  it, mandatory storage included;
- **spawn and load:** the session's record, sized by its definition, and an
  invoked session's sparse state;
- **refills at safe points,** up to their bounds: the pool, the name area,
  the waiting table and the deadline index (ADR 0099, ADR 0101), slot
  records and park-area chunks (ADR 0103);
- **invocation startup,** which runs after the macrostep (section 4).

A failed allocation in any of them changes nothing. `TA_ERR_NOMEM` can come
only from these, never from a macrostep. **A core test installs an
allocator that refuses every request once a macrostep has begun, and
checks that it is never called** while charts raise, send, delay, cancel,
fail guards, finish and invoke.

### 2. Capacity is an error the chart sees

An action that needs a resource that is full fails as
`error.execution.capacity.<resource>`:

| Resource | When |
|---|---|
| `pool` | a local `<send>` or a delayed send finds no envelope it may take (ADR 0099 section 3, ADR 0101) |
| `names` | its text does not fit in the name area |
| `internal` | a `<raise>` (or `#_internal` send) finds the ordinary quota full |
| `c2h`, `record` | from Phase 5: an effect's record does not fit in `c2h`, or could never fit |

It is an execution error, raised by the action that could not be done,
with SCXML's rule for an error in executable content: **the rest of the
block is abandoned.** A fanout that runs out fails once, at the first send
that does not fit. The resource is the last segment of the name, so a chart
that only cares matches `error.execution.capacity`, one that cares which
matches the full name, and a chart written without capacity in mind
catches it in its `error.execution` handler. `_event.data` names the action
and the `<foreach>` iteration, as `ta_trace_position` gives them. The set of
resources is closed; the segments are atoms from system creation
(ADR 0100).

### 3. The internal queue: a fixed queue with two quotas

The internal queue is a fixed-entry FIFO (block B4) of entries of about 48
bytes, in the running workspace, plus one per nested interpreter. Two C3
quotas share its one order:

- **Ordinary,** `internalQueueEvents` entries (default 256, minimum 0,
  maximum 65,536), for chart raises. A raise that exhausts it fails as
  `error.execution.capacity.internal`, and that error goes into mandatory
  storage.
- **Mandatory,** sized by the compiler, for errors and `done.state` events.
  An ordinary raise cannot use it.

Mandatory events do not jump the queue: both quotas preserve enqueue order.
This is a capacity distinction, not a priority.

**The compiler records five per-definition maxima:**

| | The most mandatory events from |
|---|---|
| `B` | the bootstrap: data initialization, the top-level `<script>`, the initial microstep |
| `F` | a top-level final state's completion and done data, and a stop's cleanup |
| `E` | one microstep: one per executable block it can run (a block fails at most once), plus data initialization and `done.state` events |
| `C` | one transition-selection pass: guard errors |
| `V` | one invocation-startup pass: start errors |

**Mandatory storage is `B + F + K × max(E, C, V)` entries,** where `K` is the
system parameter **`mandatoryPasses`** (default 64, minimum 1, maximum
4,096). The running workspace is sized for the largest registered
definition, at registration, with checked arithmetic; registration fails
atomically if the sum overflows or the allocation fails.

**The bound is enforced between passes.** Before every pass (a microstep, a
selection pass or a startup pass), at the boundary where ADR 0103 checks
the chain limit, core requires as many free mandatory entries as that kind
of pass can produce (`E`, `C` or `V`). `B` and `F` are reserved for the
bootstrap and the end and never count toward the check. If the room is
not there, **core abandons the input exactly as a runaway is abandoned**
(ADR 0103): ADR 0082's abandon at that boundary, status **`capacity`** in
place of `runaway`, ADR 0082's ABANDONED trace record with a fault that
names mandatory storage, and the session takes its next input. No mandatory event is ever
dropped: a pass starts only when everything it can add fits.

**Why not size for the whole chain.** The conservative bound from the
proposal is `B + L*E + S*C + I*V + F` for `L` microsteps, `S` selection
passes and `I` startup passes in one input's chain. With the default
`microstepLimit` of 100,000 and `E = C = V = 4`, that is about 1.2 million
entries, some 55 MiB per system, for a chart whose every pass adds at most
four. The maintainer's decision:

- **More than `K` passes' worth of undequeued mandatory events is
  pathological.** Each selection pass takes one internal event off the
  queue. A chart that keeps adding errors faster than it consumes them for
  64 passes is looping on failures, and its input is better abandoned
  with a status that says so than run on.
- **Memory per definition stays proportional to the chart,** not to
  `microstepLimit`. A host that raises the limit for a long-running chart
  pays nothing for it in mandatory storage. With `max(E, C, V) = 4`,
  `B = 8` and `F = 4`, a definition needs 268 entries, about 12.6 KiB.

A chart that legitimately accumulates more is the reason `mandatoryPasses`
is a parameter; there is no per-definition override until one is needed.

### 4. Invocation startup runs after the macrostep

Entering a state marks its invocations pending; leaving it before
stabilization drops them. Only surviving pending invocations are
evaluated and started, after the macrostep end, in state entry order and
then document order (SCXML 3.13 and appendix D). Argument evaluation and
child creation leave the entry block.

- Startup is a safe point: it may allocate. Starting a **local child**
  first takes its **lifetime token**, one pool envelope and the name
  storage for its `done.invoke.<id>` and completion metadata, then creates
  the session. The token is held for the invocation's life, ordinary sends
  cannot take it, and it becomes the child's `done.invoke` on the parent's
  external queue, or the child's stop when the parent exits the invoking
  state, without taking another envelope. A token does not count against
  the macrostep reserve.
- Starting a **host service** calls the invoker (`c2h.invoke` from Phase
  5.2).
- A start either completes or leaves the invocation unstarted and queues
  its error in mandatory storage (`V`). SCXML processes invocation errors
  before the next external event, so an error starts an internal-only
  stabilization macrostep of the same session, with its own macrostep
  number, before the input completes. The input completes only when
  stabilization and startup settle.
- Unloading is refused for a session that owns an active local invocation
  or is an active locally invoked child (`TA_ERR_BUSY`, later
  `c2h.bounced` with `active_local_invoke`).

### 5. A data model that runs out of its own memory answers `TA_OP_ERROR`

`TA_OP_NOMEM` leaves `ta_op_status`. A data model, invoker or byte sink
that cannot get its own memory answers `TA_OP_ERROR`, and the chart sees
`error.execution`, as for any failed operation. ADR 0097's `copy_out`
answers `TA_OP_ERROR` only. The memory is the data model's, so core can
neither free it nor know when it is free; an operation that ran out partway
may already have changed something, so a retry would not be safe; and
charts are built to handle errors. The test host's jq and QuickJS data
models and SCXML invoker, and napi's data model, change where they answer
`TA_OP_NOMEM` today, in the same commit (napi's files are listed in it).

### 6. What goes, and what stays

**Deleted:** the NOMEM branches of the run loop; the parts of
`ta_reserve_stop` and `ta_stop*` that only running out of memory uses; the
system's spare continuation and spare sparse state; the queue's mark and
truncate; `TA_OP_NOMEM`; `microstep_needs_sparse` and every reservation
made only so that a step could stop for memory.

**ADR 0070:** its rule that running out of memory stops a run where it
stands, its stop reservation (section 2), its per-step retry table
(section 3), section 4 and section 5 go, and its open points 1, 3, 5, 6,
7 and 8 close with them. What stays is section 1's resumable phases
(`ta_run_phase`) and the continuation, which a budget pause and parking
(ADR 0103) still use, and its rule that saving a pause needs no allocation.

**ADR 0047:** goes. Nothing interrupts a macrostep for memory.

**ADR 0097:** section 4's `TA_OP_NOMEM` row goes; `TA_OP_ERROR` is
unchanged.

## Consequences

- **A macrostep cannot fail for core's memory.** The out-of-memory sweeps
  shrink to creation, registration, spawn, load, refills and invocation
  startup. The test host's sweep over macrosteps
  (`out_of_memory.rb`) becomes a sweep over those points.
- **A shortage in the host becomes chart behaviour.** A JavaScript heap
  that is briefly full is `error.execution` where it used to be a pause.
  That is what charts are for.
- **Memory:** per system, the spare continuation (248 bytes to about 1.3 KB)
  and spare sparse state (176 bytes) go; the running workspace gains the
  ordinary quota (256 × 48 bytes, about 12 KiB) and the largest
  definition's mandatory storage. Per actor: nothing in this step. Per
  local invocation: one envelope, its token, while it lives. The
  per-actor figures before and after, with `bench-compare`, are recorded
  in the implementation notes of this step.
- **Startup is later than today's.** An invocation starts after its
  macrostep end, not inside it, as SCXML describes. Charts that observed a
  child before the macrostep ended could only do so through effects; the
  IRP tests check the order.
- **API:** `ta_op_status` loses `TA_OP_NOMEM`; `ta_input_status` gains
  `TA_INPUT_CAPACITY`, and the catalog's `input_completion_status` and
  `macrostep_status` gain `capacity`. Two parameters: `internalQueueEvents`
  and `mandatoryPasses`.
