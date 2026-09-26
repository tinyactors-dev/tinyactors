# Delayed-send lifecycle

1. Executable content evaluates the send event, target, I/O type, data, send ID, and delay using the current data-model state.
2. For zero delay, the send can be dispatched through the selected I/O processor at the appropriate semantic point.
3. For a positive delay, Tinyactors retains all values needed later and creates a delayed-send record with absolute logical `due` time.
4. The record is inserted into the system deadline index and remains owned by the originating actor/session.
5. A matching cancel from the same actor removes all still-pending sends with the requested send ID, subject to the race that a send may already have been dispatched. The host can do the same from outside the chart with `ta_actor_withdraw_send()`, naming the owner and the send ID (ADR 0084): it removes the same sends, never a submitted one (`TA_ERR_DONE`), advances the owner's session epoch, and writes a lifecycle-like `WITHDRAWN` record.
6. Once logical time reaches/exceeds `due`, the send becomes eligible. Dispatch calls the I/O processor with the materialized send. Taking a pending (not yet submitted) send from its owner's list advances the owner's session epoch, as a withdrawal does: an image captured before holds the send and is stale (ADR 0014, ADR 0054's implementation notes).
7. Successful dispatch removes/releases timer state. Dispatch failure becomes `error.communication...` in the sending actor's internal event flow as required by the send semantics.
8. Unload preserves the timer. Destroy/termination discards actor-owned delayed sends according to SCXML session lifetime rules.

Persistence must preserve steps 3-5 across process restart without re-evaluating expressions.
