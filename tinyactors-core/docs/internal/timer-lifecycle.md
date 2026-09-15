# Delayed-send lifecycle

1. Executable content evaluates the send event, target, I/O type, data, send ID, and delay using the current data-model state.
2. For zero delay, the send can be dispatched through the selected I/O processor at the appropriate semantic point.
3. For a positive delay, Tinyactors retains/clones all values needed later and creates a delayed-send record with absolute logical `due` time.
4. The record is inserted into the system deadline index and remains owned by the originating actor/session.
5. A matching cancel from the same actor removes all still-pending sends with the requested send ID, subject to the race that a send may already have been dispatched.
6. Once logical time reaches/exceeds `due`, the send becomes eligible. Dispatch calls the I/O processor with the materialized send.
7. Successful dispatch removes/releases timer state. Dispatch failure becomes `error.communication...` in the sending actor's internal event flow as required by the send semantics.
8. Unload preserves the timer. Destroy/termination discards actor-owned delayed sends according to SCXML session lifetime rules.

Persistence must preserve steps 3-5 across process restart without re-evaluating expressions.
