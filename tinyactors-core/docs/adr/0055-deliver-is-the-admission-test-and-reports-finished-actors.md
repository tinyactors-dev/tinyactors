# ADR 0055: `ta_actor_deliver` is the admission test and reports finished actors

**Status:** Proposed; refines ADR 0011, ADR 0014 and ADR 0029
**Date:** 2026-09-23

## Context

ADR 0011 and ADR 0029 give the host the external queue. The host offers an
event, and core accepts it or answers busy. For that to work at scale, the
host must be able to act on the answer to `ta_actor_deliver()` alone.

Today the answers are:

| Actor state | Result |
|---|---|
| Unknown, stale, or unloaded ID | `TA_ERR_NOT_FOUND` |
| Mid-macrostep or already holding an event | `TA_ERR_BUSY` |
| Finished (reached a top-level final) | `TA_ERR_INVALID` |
| Malformed event (bad or root name) | `TA_ERR_INVALID` |

The finished case uses the same code as a caller bug. A host cannot tell "drop
this message, its receiver completed" from "your envelope is wrong". So the
Node-API binding calls `ta_actor_inspect()` before every delivery just to read
`residency == TA_ACTOR_DONE`. That doubles the per-message cost; before ADR 0054 it also made each message
scale with the system.

`ta_actor_resume()` already reports a finished actor without an error: it
returns `TA_OK` with `TA_RUN_DONE`. Delivery should be just as clear.

## Decision

1. **Add `TA_ERR_DONE = -11`.** `ta_actor_deliver()` returns it when the
   target is resident and finished. Nothing is retained and the event is
   untouched. Other functions that reject a finished actor also return
   `TA_ERR_DONE` rather than `TA_ERR_INVALID`.
2. **`TA_ERR_INVALID` from `ta_actor_deliver()` means a caller error only.**
   Examples are a null event, an unknown event name, or the root name.
3. **Deliver's result set is complete for scheduling.** A host decides what
   to do with a queued message from the result alone:
   - `TA_OK`: resume the actor;
   - `TA_ERR_BUSY`: keep the message queued for this actor;
   - `TA_ERR_DONE` or `TA_ERR_NOT_FOUND`: the receiver is gone, so drop the
     message or route it to a dead-letter policy;
   - anything else: a host bug or resource failure.
   Hosts are not expected to inspect an actor before delivery. The manual
   says so.
4. Bindings map `TA_ERR_DONE` to their own vocabulary (`"done"` in
   Node-API).

## Consequences

- One core call per delivered message, with no inspect. Together with
  ADR 0054, the host's per-message cost does not depend on the population.
- This is an ABI addition: a new `ta_result` value. Hosts that treat
  unknown negative results as failures keep working but will report finished
  targets as errors until they handle the new code.
- Tests that assert `TA_ERR_INVALID` for delivery to a finished actor change
  to `TA_ERR_DONE`.
