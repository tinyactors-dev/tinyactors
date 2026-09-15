# `store.h`

A store is an optional host adapter for complete actor-session blobs.

The interface must support saving, loading, and removing actor session state without assuming a filesystem. Backends may include:

- SQLite;
- application databases;
- append-only logs;
- memory maps;
- flash sectors;
- browser storage mediated by the host;
- in-memory test stores.

`begin_batch(out)` opens the atomic visibility boundary. `begin_save(batch,
actor, out)` returns a streaming writer for one staged blob in that boundary.
The core writes exactly one complete session snapshot to that writer. A staged
blob must not replace the currently visible blob until `commit_batch()`
succeeds; on a serialization or commit failure the core calls `abort_batch()`.
`stage_remove()` makes actor deletion part of the same boundary. This works for
a buffered database column, a database transaction, or immutable objects plus
one published manifest pointer.

`open_load(actor, out)` returns a streaming reader for that actor's committed
blob, and `close_load()` releases any backend resource. Actor destruction uses
`stage_remove()` in a batch rather than an independent delete operation.

The store does not save, load, or index timers separately. Delayed sends are
inside the session blob. Tinyactors rebuilds its running timer heap as it
restores blobs.

Store enumeration deliberately remains host-driven. After restart, the host
enumerates its own actor blobs and calls `ta_actor_recover()` with a reader for
each blob it wants to materialize. This keeps SQL tables, object prefixes, and
single-column stores equally natural.

Tinyactors exposes `ta_snapshot_batch_*` for callers that need multiple actor
snapshots to become visible together. `ta_actor_unload()` uses an implicit
batch of one, so stores must support batches even when an embedding application
never creates an explicit one.

The core requirement is semantic, not storage-shaped: unload/reload must reproduce the same actor session.
