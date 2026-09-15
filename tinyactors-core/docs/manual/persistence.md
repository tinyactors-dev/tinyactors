# Persistence and residency

Persistence is optional and host-controlled.

`unload` means **remove the resident execution state from RAM while preserving the actor's existence**. `destroy` means terminate/remove the actor. These operations are semantically different.

A complete actor snapshot must preserve enough state that unload + reload is observationally invisible to the statechart:

- active configuration, including all parallel regions;
- shallow/deep history values;
- data-model instance state;
- current macrostep continuation if paused;
- current external event and pending internal events if paused;
- active invocation bookkeeping;
- delayed sends, including absolute logical due times and already-evaluated payload/target/event information;
- name and definition identity needed to resolve the actor.

Timers remain semantically owned by the actor even when physically indexed in a system-wide timer structure. Destroying an actor cancels its delayed sends; unloading it does not.

Host-native `ta_value` implementations decide whether and how their values serialize. If an actor contains a non-serializable value, unloading that actor may fail with `TA_ERR_NOT_SERIALIZABLE`.
