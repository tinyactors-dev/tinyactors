# `types.h`

`types.h` contains low-level scalar and shared helper types.

Key rules:

- public handles use fixed-width integer types;
- array/span indices should remain compact and definition-local where possible;
- `ta_actor_id` is an opaque 64-bit runtime identity; internal slot/generation decomposition may be used to reject stale references;
- runtime atom/event-name/definition IDs are process-local accelerators;
- exact definition identity, logical actor addresses, and any portable durability encoding must use stable logical forms rather than assuming numeric runtime handles survive restart;
- `ta_session_image` is opaque because its structural storage is interpreter-owned and is not a public wire format.
