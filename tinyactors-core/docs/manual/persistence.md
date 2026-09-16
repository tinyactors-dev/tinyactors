# Persistence and residency

Persistence is optional and host-controlled. Tinyactors serializes/restores a
complete actor session, while the host owns the storage backend, transactions,
directory, and eviction policy. A host may write a checkpoint to a SQLite row,
a PostgreSQL column, an object, or any other blob-shaped store, then release
the resident actor only after that write is durable.

`unload` means **remove the resident execution state from RAM while preserving the actor's existence**. `destroy` means terminate/remove the actor. These operations are semantically different.

A complete actor checkpoint must preserve enough state that eviction + restore
under a compatible session schema is observationally invisible to the
statechart:

- active configuration, including all parallel regions;
- shallow/deep history values;
- data-model instance state;
- current macrostep continuation if paused;
- current external event and pending internal events if paused;
- active invocation bookkeeping;
- delayed sends, including absolute logical due times and already-evaluated payload/target/event information;
- named self address and definition family/session schema needed to resolve the actor.

Timers remain semantically owned by the actor. A host that evicts actors owns
the durable deadline index and loads/routes an actor when work becomes due. A
restored timer already due is eligible immediately. Destroying an actor cancels
its delayed sends; eviction does not.

Host-native `ta_value` implementations decide whether and how their values serialize. If an actor contains a non-serializable value, unloading that actor may fail with `TA_ERR_NOT_SERIALIZABLE`.

When a new definition accepts the old checkpoint session schema, the host
restores it directly. When it does not, the application replaces the actor:
fence its named address, create a new actor with host-owned migration data,
bootstrap that actor using normal SCXML behavior, persist it, and atomically
replace the directory entry. Tinyactors does not prescribe an event log or a
migration language.
