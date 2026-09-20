# ADR 0047: An allocation failure inside a macrostep is retryable

**Status:** Accepted; applies ADR 0009 and ADR 0023
**Date:** 2026-09-20

## Context

ADR 0023 puts every allocation behind the host's `ta_allocator`, so that core
runs on a fixed pool, an RTOS heap, or a WASM linear memory. For such a host,
refusing an allocation is an ordinary operating condition, not a crash: the
pool fills, the host frees or grows something, and work continues.

`docs/internal/errors.md` decides what such a refusal is — a `ta_result`
("API/runtime failures such as … allocation failure"), never an SCXML
`error.*` event, because invariant 8 draws the line at failures the SCXML
semantics define, and running out of memory is not one. So `ta_actor_resume`
answers `TA_ERR_NOMEM` and the host deals with it.

What the contract never said is what happens to the **macrostep that was
half-way through**. Two readings were open: the interpreter is suspended and
the host may resume it once memory is available, or the actor is poisoned and
the host must destroy it.

Writing `tinyactors-test/ruby/scenarios/allocation_failure.rb` turned that
gap into an observation. Today core does neither: `ta_actor_resume` correctly
answers `TA_ERR_NOMEM`, and then the **next** resume answers `TA_OK` with
`TA_RUN_STABLE` and leaves the actor resident, idle, and holding an **empty
configuration** — the half of the macrostep that ran is kept, the rest is
forgotten, and the actor still accepts events that can no longer match
anything. That contradicts ADR 0010, under which a running interpreter's
configuration is a set of active atomic states, never none of them.

## Decision

**`TA_ERR_NOMEM` from `ta_actor_resume` suspends the macrostep; it does not
end it.** The actor is left exactly as a `TA_RUN_PAUSED` return leaves it, at
the last interpreter boundary before the allocation that failed:

- its configuration, history, delayed sends, invocations, data-model binding,
  continuation, current external event, and internal queue are unchanged;
- `ta_actor_inspect` reports it as mid-macrostep, with the external event it
  is processing;
- `ta_actor_deliver` answers `TA_ERR_BUSY`, as for any paused actor;
- a later `ta_actor_resume`, after the host has freed memory, continues from
  that boundary, and the macrostep's outcome is the one it would have had
  without the interruption.

**An allocation that can fail happens before the microstep it serves becomes
visible, or is undone before the error is returned.** Retrying is only
meaningful if a failed resume never leaves a half-applied microstep, so this
is an obligation on the implementation, not advice. It is the discipline the
interpreter already follows to be pausable and resumable at all (ADR 0009)
and to make `<foreach>` resumable (ADR 0033).

**Nothing statechart-visible happens.** No `error.*` event is queued, no log
record is written, and the chart cannot observe that the interruption
happened. Memory pressure is the host's concern (invariant 8).

### Why not "poisoned"

- ADR 0023's whole premise is that a full pool is normal. "Destroy the actor"
  is the wrong answer for a conversation actor that needs the host to free a
  buffer and try again.
- Core already has a suspension mechanism at exactly the right granularity.
  Retry reuses the continuation, so it costs no new machinery; poisoning
  would cost new surface.
- A poisoned actor would need an inspectable state of its own, rules for
  whether it can be captured, unloaded, or replaced (ADRs 0014, 0038), and a
  way for the host to discover it. All of that to reach a strictly worse
  outcome.
- Without the rollback obligation above, "poisoned" also becomes the excuse
  for leaving a half-applied microstep behind — which is the behaviour this
  ADR exists to rule out.

## Consequences

- A host loop treats `TA_ERR_NOMEM` like `TA_RUN_PAUSED`, with one extra
  step: free or grow memory first, then resume. Retrying is safe and may be
  repeated.
- An interrupted-and-retried macrostep is indistinguishable from an
  uninterrupted one, which makes it an `assert_equivalent_runs` interruption:
  the same test shape as pausing, residency changes, and capture/reconstruct.
- An actor suspended this way is an ordinary paused actor for capture and for
  session images: ADR 0038 already captures the continuation, the current
  event, and the internal queue, so an image taken here reconstructs and the
  retry may happen after the reconstruction, in another process.
- Today's behaviour is a bug against this ADR. `allocation_failure.rb` states
  the contract and is marked as an expected failure naming it; when core
  implements it, the marker is removed in the same change.
- The decision says nothing about *which* allocation fails. Core may fail at
  any boundary, and the host cannot predict where; what it can rely on is
  that resuming continues rather than restarts.
