# Tinyactors actor/statechart header skeleton

This archive captures the C API shape discussed for a tiny in-process actor system using SCXML-like execution semantics without requiring XML.

## Design points represented here

- `ta_` prefix throughout.
- `#pragma once` in every header.
- One internal header per subsystem, collected by `src/internal.h`.
- Immutable statechart definitions, actor/session instances, and explicit resumable execution.
- Parallel state configurations are represented as sets of active atomic states, with definition-specific compact layouts.
- Event names are structural trie IDs rather than strings; prefix matching implements SCXML event descriptor semantics efficiently.
- `error.*`, `done.state.*`, and `done.invoke.*` are structured event names.
- Runtime/plugin faults become SCXML-style internal events; they are distinct from C API misuse/failure.
- Logical time and delayed sends are runtime state.
- Actor snapshots include execution continuation; timer snapshot helpers preserve delayed sends across unload/reload.
- Host values are fat pointers with lifetime and serialization hooks.
- Data models, I/O processors, invokers, storage, clocks, and concurrency remain under embedder control.

## Layout

- `include/tinyactors.h` — embedding/public ABI.
- `src/*.h` — internal module interfaces.
- `src/internal.h` — umbrella include for implementation translation units.
- `tests/smoke.c` — compile-only sanity check.

## Intentional omissions

This is a header/API skeleton, not a completed implementation. It intentionally does not ship an XML parser/importer. It also leaves representation choices such as hash tables, actor name indexes, transition-set scratch storage, and snapshot wire encoding to implementation work.
