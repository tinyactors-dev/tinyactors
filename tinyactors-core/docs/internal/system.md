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
- the built-in SCXML Event I/O Processor and its host event router;
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

Directory entries distinguish resident runtime IDs from unloaded logical
actors. Replace/remove callbacks receive the expected current entry and must
behave as atomic compare-and-swap operations. This is used by replacement,
unload, load, and destruction so a stale runtime actor cannot overwrite or
remove a newer route.

`ta_system_set_external_event_router()` installs the host scheduler/router for
normalized SCXML-session events. Core's standard processor and invoked-child
completion paths submit through it. `ta_system_scxml_io_type()` exposes the
reserved type atom automatically interned at creation; generic I/O registration
cannot replace it.
