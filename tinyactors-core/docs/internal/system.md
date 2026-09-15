# `system.h`

`ta_system` is the in-process container for shared runtime state.

Responsibilities:

- allocator and scratch arena;
- logical clock;
- atom and event-name interning;
- registered immutable definitions;
- actor directory/name resolution;
- resident actor cache;
- delayed-send deadline index;
- registered I/O processors and invokers;
- optional persistence adapter.

Non-responsibilities:

- creating threads;
- deciding cross-actor scheduling order;
- owning a socket/event loop;
- reading wall-clock time;
- providing a mandatory filesystem;
- hiding host concurrency behind locks.

The current skeleton stores arrays directly. Production high-scale implementations may need hash/index structures, segmented storage, slab pools, or store-backed directories without changing public semantics.
