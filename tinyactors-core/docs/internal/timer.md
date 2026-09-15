# `timer.h`

A delayed send contains the **already evaluated** semantics of a future dispatch:

- timer ID;
- owning actor/session;
- send ID;
- absolute logical due time;
- structured event name;
- I/O processor type atom;
- target value;
- payload value;

`ta_delayed_send.io_type` is the resolved SCXML `<send>` type. It is retained with
the other evaluated send arguments so that due-time dispatch selects the same I/O
processor without reconstructing information from interpreter state.

## Global index, actor ownership

The running system may store all delayed sends in a global heap/wheel so the next deadline is efficient. Ownership remains `(actor, send_id)`, enabling session-scoped cancellation and actor destruction.

## Persistence

Actor snapshot/store transactions must preserve delayed sends together with session state. An unloaded actor's timers remain live. A destroyed actor's timers are cancelled/discarded.
