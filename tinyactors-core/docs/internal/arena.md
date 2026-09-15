# `arena.h`

The arena is for transient interpreter/build work, not durable actor state.

Typical uses:

- transition selection scratch sets;
- computed entry/exit sets;
- temporary ordering arrays;
- builder compilation work;
- formatting event names at API boundaries.

Resetting the arena must never invalidate data retained by an actor, definition, timer, queued event, plugin, or snapshot. Embedded builds may replace the backing allocator with fixed pools.
