# `snapshot.h`

Snapshots are streaming through host-provided reader/writer callbacks; the core does not require files. A snapshot is one complete actor session blob, not separate actor and timer records.

## Required logical contents

See `semantic-state.md`. At minimum, a snapshot must preserve configuration, history, data model, paused continuation/current event/internal queue, invocations, actor identity/name, definition identity, and actor-owned delayed sends.

## Checkpoint identity and header

`ta_snapshot_header` is a decoded logical header, not a native wire struct.
Its `definition` field identifies a UTF-8 definition family and a session
schema. Writers encode both fields as logical bytes/integers; they never write
the C struct directly.

`self_address`, when present, is the named actor's UTF-8 logical address. It
is not a `ta_atom`. Ephemeral actors use a runtime `ta_actor_id` and do not
need a durable address.

During recovery the system resolves a definition family and checks that the
target definition accepts the checkpoint's session schema before it decodes
definition-local state. A missing family returns `TA_ERR_NOT_FOUND`; an
unsupported schema returns `TA_ERR_SESSION_SCHEMA`. Applications replace
actors to migrate incompatible business logic; Tinyactors performs no implicit
migration.

## Persistence boundary

`ta_snapshot_session()` writes the complete durable state of one actor, including its actor-owned delayed sends. `ta_restore_session()` restores that state and registers its delayed sends with the running system's timer heap. The latter operation must be idempotent by actor and send identity, so loading an actor does not duplicate a timer already indexed in the same running system.

The host commits one blob per actor. A column in a relational database can hold that blob directly; an object store can write a new immutable blob and then atomically publish the pointer to it. The core does not own store batches: the host persists a checkpoint before it evicts resident state.

## Runtime timer index

The timer index belongs to `ta_system` only. It is not a host persistence surface and is not independently durable. It allows the running system to find due sends for actors that are unloaded while idle.

After a process restart, the index is rebuilt by restoring actor snapshots. The host may enumerate its stored actor blobs and call `ta_actor_recover()` with a reader for each one; a timer whose absolute logical due time is already past is immediately eligible for dispatch. A system must never require a host-maintained timer index for correctness.

## Runtime handles

Atoms, event-name IDs, definition IDs, and host service handles cannot automatically be treated as stable persistent identifiers. Snapshot encoding must serialize/resolve their logical forms. Event names are encoded as logical UTF-8 segments and interned when restored.
