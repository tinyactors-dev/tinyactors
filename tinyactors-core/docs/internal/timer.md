# `timer.h`

A delayed send contains the **already evaluated** semantics of a future dispatch:

- timer ID;
- owning actor/session;
- send ID;
- absolute logical due time;
- structured event name;
- target value;
- payload value;
- I/O processor/type information in the final representation.

The current `ta_delayed_send` skeleton still needs an explicit I/O processor/type field if the send type cannot be recovered elsewhere.

## Global index, actor ownership

The running system may store all delayed sends in a global heap/wheel so the next deadline is efficient. Ownership remains `(actor, send_id)`, enabling session-scoped cancellation and actor destruction.

## Persistence

Actor snapshot/store transactions must preserve delayed sends together with session state. An unloaded actor's timers remain live. A destroyed actor's timers are cancelled/discarded.
