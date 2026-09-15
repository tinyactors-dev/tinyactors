# Persistence and residency

Persistence is optional and host-controlled.

A configured store receives one complete snapshot blob per actor. To unload,
Tinyactors opens a batch, obtains a staged writer from `begin_save`, streams
the complete session into it, and commits the batch only after serialization
succeeds. To recover after restart, the host enumerates its own stored blobs
and passes each reader to `ta_actor_recover`. This keeps persistence compatible
with a SQLite row, a PostgreSQL column, an object, or any other blob-shaped
host store.

`ta_actor_unload` is a batch of one. When several actor snapshots must become
visible together, use `ta_snapshot_batch_begin`,
`ta_snapshot_batch_unload`, and `ta_snapshot_batch_commit`; use
`ta_snapshot_batch_abort` to discard staged work. A batch contains distinct
per-actor blobs, rather than one system-wide snapshot blob.

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

Timers remain semantically owned by the actor even when physically indexed in a system-wide timer structure. The index is owned by the running Tinyactors system, not the store, and is rebuilt from restored snapshots. A restored timer already due is eligible immediately. Destroying an actor cancels its delayed sends; unloading it does not.

Host-native `ta_value` implementations decide whether and how their values serialize. If an actor contains a non-serializable value, unloading that actor may fail with `TA_ERR_NOT_SERIALIZABLE`.
