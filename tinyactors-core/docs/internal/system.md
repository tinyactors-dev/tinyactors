# `system.h`

`ta_system` is the in-process container for shared runtime state.

Responsibilities:

- allocator and scratch arena;
- logical clock;
- atom and event-name interning;
- registered immutable definitions;
- resident actor cache and runtime IDs;
- actor-directory protocol delegation;
- delayed-send state for resident sessions;
- registered I/O processors and invokers;
- no persistence adapter or concrete named-address index.

Non-responsibilities:

- creating threads;
- deciding cross-actor scheduling order;
- owning a socket/event loop;
- reading wall-clock time;
- providing a mandatory filesystem;
- hiding host concurrency behind locks.

The host installs `ta_actor_directory` operations for named address claim,
resolution, replacement, and removal. It may use a hash/index structure,
database, shard router, or other backing implementation. The core keeps only
resident runtime state; hosts persist checkpoints and index deadlines for
evicted actors.
