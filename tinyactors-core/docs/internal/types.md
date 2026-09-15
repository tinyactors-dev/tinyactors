# `types.h`

`types.h` contains low-level internal helpers and includes the public scalar types from `tinyactors.h`.

Key rules:

- public handles use fixed-width integer types;
- array/span indices should remain compact and definition-local where possible;
- `ta_actor_id` is an opaque 64-bit runtime identity; internal slot/generation decomposition may be used to reject stale references;
- do not serialize internal pointer values or assume numeric atom/event-name/definition table IDs survive restart unless explicitly resolved by the persistence layer.
