# `snapshot.h`

Snapshots are streaming through host-provided reader/writer callbacks; the core does not require files.

## Required logical contents

See `semantic-state.md`. At minimum, a snapshot must preserve configuration, history, data model, paused continuation/current event/internal queue, invocations, actor identity/name, definition identity, and actor-owned delayed sends.

## Atomicity

The current split between `ta_snapshot_actor()` and `ta_snapshot_actor_timers()` is useful internally but dangerous as the only persistence contract. A store that commits actor bytes and timers separately can lose or duplicate delayed work on crash.

The intended persistence contract is one logical transaction. A backend may physically normalize timers into a separate table/index, but commit/recovery must preserve the actor snapshot and its timer set consistently.

## Runtime handles

Atoms, event-name IDs, definition IDs, and host service handles cannot automatically be treated as stable persistent identifiers. Snapshot encoding must serialize/resolve their logical forms.
