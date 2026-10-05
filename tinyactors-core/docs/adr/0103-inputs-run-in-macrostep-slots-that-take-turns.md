# ADR 0103: Inputs run in macrostep slots that take turns, and a runaway input is abandoned

**Status:** Accepted (2026-10-04); implementation pending (ring-proto Phase 4.5), except that the abandon of a runaway input (section 4's end of an input at the limit, with status `runaway`) is implemented in 4.2a on the one slot, without the chain limit's widening, without refusing a limit of 0 or `TA_MICROSTEP_LIMIT_NONE`, and without removing napi's quarantine (the kernel reports a runaway to the `fault` hook, section 8, but quarantines nothing). Refines ADR 0055 (mail for an active session waits on its deferred list), ADR 0061 (a paused run stays in the running workspace or is parked, not moved into the actor) and ADR 0075 (the limit counts an input's whole chain, cannot be turned off, and abandons the input). Records the removal of napi's quarantine (napi ADRs 0008 point 1 and 0025).
**Date:** 2026-10-04

## Context

After ADR 0099, core takes inputs from its run queue and runs each to
completion, one at a time for the whole system. That is run to completion
with one continuation, and it has a cost the kernel's scheduler used to
hide: **any long macrostep stops every other session until it ends.**

- Long but correct macrosteps exist: a guarded eventless transition that
  walks a list, a parser driven by `<raise>` chains, a `<foreach>` over
  10,000 items.
- A loop that never ends holds the system until the microstep limit.
  ADR 0075's limit counts only committed microsteps in one macrostep, so it
  misses an eventless transition whose guard keeps throwing (each pass
  raises an error that is consumed, and no microstep commits), and an
  `<invoke>` that fails and re-enters its state (each macrostep of the
  chain is short).
- napi quarantines a runaway (napi ADRs 0008 and 0025): it holds the actor
  where it stopped until an operator calls `release()`. Under ADR 0099 a
  held input would hold the pool's only slot, its mail would fill the pool
  while nobody looks, the host's promise for it would never settle, and a
  replay whose seed loops could never be published.

Core already moves a paused run into a continuation (ADR 0061, ADR 0070).
Running several inputs in turn is that move, done to give other sessions a
turn ([macrostep.md](../../../docs/proposals/ring-proto/reference/macrostep.md#macrostep-slots)).

## Decision

### 1. Up to `maxConcurrentInputs` slots

A **slot** is one session's execution of one input, from take until the
input completes: its taken envelope, its outgoing list, its chain count
and, from Phase 5.1, its c2h reservation. A session that holds a slot is
**active**. Each session still runs one input at a time; up to
`maxConcurrentInputs` sessions run inputs at once.

- **Slot records** are an index pool (block A5) of about 48 bytes each,
  filled on demand at safe points: when every record is busy and an
  envelope could run, core adds one, up to `maxConcurrentInputs`. If that
  fails, the envelope waits for a slot.
- **One slot runs at a time,** on the system's thread. Slots are not
  threads.

### 2. Slots take turns, `slotSlice` steps each

A slot runs until its input's chain settles or until it has used
`slotSlice` steps in this turn, where a step is a microstep, a selection
pass or a unit of executable content, as ADR 0061 counts them. Then core
moves to the next slot that can run, in slot order. **Slots change only
between units of executable content,** where ADR 0061 already lets a run
stop, never inside a data-model call: a synchronous call that never returns
still holds the thread.

`ta_run`'s budgets (ADR 0099) are per call, across all slots. When one runs
out, the running slot stays in the running workspace and nothing is
copied; the next call continues it.

### 3. Mail for an active session waits on its deferred list

When core fills a slot, it walks the run queue from its head. An envelope
whose target is active moves to the end of that session's **deferred
list** (block B1, a head and a tail in the actor record, 8 bytes). The
walk takes the first envelope whose target is not active. When an input
completes, core moves the first envelope of the session's deferred list to
the **head** of the run queue and leaves the rest deferred, so the
session's inputs run in arrival order, one at a time, without being
walked again.

- Every envelope on a deferred list arrived before any envelope for that
  session still in the run queue, so each sender's order to each receiver
  holds. A deferred envelope put at the head may overtake envelopes for
  other sessions; SCXML orders only one sender's events to one receiver.
- A deferred envelope is still runnable: it counts against the pool, and is
  recycled like any other when its session is destroyed (ADR 0099).
- **This is what ADR 0055's busy answer becomes:** the host never sees an
  active session.
- With one slot, nothing is ever deferred.

### 4. The chain limit

`microstepLimit` counts, for one input, from take until its completion:

- every microstep;
- every selection pass, including one that enables nothing and only
  consumes an internal event;
- every invocation-startup pass (ADR 0102).

Units of executable content do not count: a `<foreach>` over 10,000 items
is one microstep, always ends, and rotates with the other slots.

**The limit must be finite.** 0 and `TA_MICROSTEP_LIMIT_NONE` are refused,
for the system parameter (now 1 to 2^31−1, default 100,000) and for a
definition's override. `ta_actor_resume_params.no_microstep_limit` went
with `ta_actor_resume` (ADR 0099).

**At the limit, core ends the input, not the session.** The check sits
between steps, where nothing is open, and applies ADR 0082's abandon there:

- the configuration and data after the last completed microstep stay;
- the internal queue and the mail the input sent that is still on its
  outgoing list are freed and never delivered;
- effects already made (external sends, logs, invocations) stay made;
- the input completes with status **`runaway`** (`TA_INPUT_RUNAWAY`, later
  `c2h.input_completed` and `c2h.macrostep_ended`), and core writes ADR
  0075's `TA_TRACE_RUNAWAY` record with the configuration and the
  transitions of the step that reached the limit. The warning at a tenth of
  the limit stays;
- the slot is free, and the session takes its next input from its
  deferred list.

The chart is not told, as ADR 0075 decided: the macrostep an `error.*`
would belong to has just been stopped. The host decides what to do about a
session that keeps running away, for example destroying it after some
number of runaways. ADR 0102's `capacity` abandon uses the same path and
the same boundary.

### 5. One running workspace and a park area

**The running workspace** is the interpreter scratch (`interpreterBytes`),
the render region, the internal queue with its two quotas (ADR 0102) and
the frame stack, sized for the largest registered definition. There is one
per system, as under run to completion. Only the running slot uses it.

**A parked execution** is what a slot needs to continue: its continuation
at the size its own definition needs (`ta_continuation_capacity()`, about
1 KiB for a chart of 100 states), the internal events actually pending
(about 48 bytes each, in order), and its counters (chain count, slice
position, trace keys). Its taken envelope and outgoing mail are already in
the pool, so they do not move.

**The park area** holds parked executions as chains of 256-byte chunks, a
4-byte link each (blocks A5 and A6), written and read with the binary
serializer through a byte sink over a chunk chain (blocks D1 and D2).
Parking writes the state into chunks from the free list; resuming reads it
back into the running workspace and returns the chunks. A larger chart
takes more chunks, not a larger block, so the area never fragments.

- **Growth at safe points only.** A rotation is in the middle of a
  macrostep, so parking takes only chunks already free. After each
  macrostep end and at admission, core tops the free list up to one parked
  execution of the largest registered definition.
- **The ceiling is derived:** `maxConcurrentInputs × (largest continuation
  + a full internal queue, ordinary and mandatory)`, in whole chunks,
  recomputed at registration. `parkAreaBytes` is an optional lower cap;
  its default is the ceiling, and a larger value changes nothing.
- **Parking can fail, and nothing breaks.** A slot that finds too few free
  chunks at the end of its slice keeps the thread for another slice, and
  tries again at its next slice end. That is run to completion for that
  macrostep. Correctness never depends on parking, only isolation.
- **Parking happens between top-level units only.** A cancelled child's
  exit handlers, run in a nested interpreter (ADR 0060), finish inside the
  unit that started them.

Returning free chunks of the park area and the pool to the allocator, a
`trim` call, is left open.

### 6. Parking is introduced disabled, then enabled

Step 4.5 lands in two commits:

1. **Slots without parking.** Slot records, deferred lists, the chain
   limit, slice counting, and `parkAreaBytes` with the value 0 meaning no
   parking. With parking off, a slot that reaches its slice end keeps the
   thread, so this commit is plain run to completion with the new chain
   limit and deferred lists, and every existing suite must pass unchanged.
2. **Parking on.** The park area, rotation, and the derived default for
   `parkAreaBytes`, in its own commit, so that any change of behaviour
   bisects to it. `bench-compare` against the commit before step 4.2 is
   recorded here, with bytes per actor before and after.

### 7. What an active session refuses

Operations that need a session outside a macrostep (unload, capture,
checkpoint) answer `TA_ERR_BUSY` for an active session, and from Phase 5
`c2h.bounced` with reason `active`. Destroying an active session abandons
its slot first. So an image never carries a continuation: ADR 0061 section
4 no longer applies. Inspection (ADR 0083) reports where any active slot
stands, from the running workspace or from its parked continuation.

### 8. Quarantine is removed

napi's quarantine kept one bad actor from taking the loop's turns. Slots do
that now, and the chain limit ends a runaway's input. The implementation
removes, in napi's call sites only:

- quarantine as napi ADR 0008 point 1 describes it, and the `"quarantined"`
  scheduling state; points 2 to 4 of napi ADR 0008 (the pump never throws,
  only misuse throws, generated result codes) stay;
- `release()`, which lifted the limit for a held macrostep;
- the parent's `error.communication.invoke.<id>.quarantined` event (napi
  ADR 0025): a child whose input ran away is not stuck, and a parent waiting
  for its `done.invoke` relies on its own timeout, as for any child that
  does not finish.

The `fault` hook reports an input that completed with status `runaway`.
An operator who wants to look at a live actor attaches the debugger. The
napi ADRs themselves get their status lines from the napi ADR that makes
napi a binding over core's rings.

### 9. Parameters

| Parameter | Default | Minimum | Maximum | Bounds |
|---|---|---|---|---|
| `maxConcurrentInputs` | 64 | 1 (run to completion) | 65,536 | slot records |
| `slotSlice` | 1,024 | 1 | 1,048,576 | steps a slot runs before core moves on |
| `parkAreaBytes` | derived (the ceiling) | 0 (no parking) | the ceiling | the park area, in 256-byte chunks |
| `microstepLimit` (changed) | 100,000 | 1 | 2^31−1 | steps of one input's chain |

## Consequences

- **One session never stops the others.** A long or looping input holds
  one slot of 64; the rest go on. With `maxConcurrentInputs` 1, or
  `parkAreaBytes` 0, the system is run to completion, as before.
- **A runaway is not held.** Its input ends with `runaway`, its slot frees,
  its mail runs, the host's promise settles, and a looping replay seed can
  be published.
- **Memory:**
  - per actor: +8 bytes for the deferred list's head and tail (with
    ADR 0101's 4 for the owner list, +12 on today's 332);
  - per system: up to 64 slot records of about 48 bytes (3 KiB), filled on
    demand; a park reserve of one parked execution of the largest
    definition (about 1 KiB for 100 states); more only while slots are
    parked, up to the derived ceiling. With 64 slots of a 100-state chart
    all parked, about 64 KiB of continuations, plus their pending events;
  - per parked execution: its continuation plus 48 bytes per pending
    internal event, in 256-byte chunks.
- **Cost:** a rotation copies a few hundred bytes to a few kilobytes, once
  per slice. Short macrosteps finish inside one slice and never rotate.
- **Order:** records of different sessions' macrosteps interleave; within
  one session nothing changes.
- **API:** `TA_MICROSTEP_LIMIT_NONE` and a limit of 0 are refused; napi
  loses `release()`, the `"quarantined"` state and the `.quarantined`
  event from `index.d.ts`, marked with `// status:` where its
  implementation lags.
- **Tests:** interleaving with `slotSlice` 1; a runaway that holds one slot
  while another session's mail runs; the guard that keeps throwing and the
  failing `<invoke>` loop both stop at the limit; a slot that cannot park
  keeps the thread and finishes correctly; deferred mail keeps each
  sender's order; `mise run test:full` before each commit.
