# Tinyactors actor/statechart header skeleton

This archive captures the C API shape discussed for a tiny in-process actor system using SCXML-like execution semantics without requiring XML.

## Design points represented here

- `ta_` prefix throughout.
- `#pragma once` in every header.
- One authored public header, `include/tinyactors.h`, in the style of `sqlite3.h`.
- One private header per subsystem, collected by `src/internal.h`.
- Immutable statechart definitions, actor/session instances, and explicit resumable execution.
- Parallel state configurations are represented as sets of active atomic states, with definition-specific compact layouts.
- Event names are structural trie IDs rather than strings; prefix matching implements SCXML event descriptor semantics efficiently.
- `error.*`, `done.state.*`, and `done.invoke.*` are structured event names.
- Runtime/plugin faults become SCXML-style internal events; they are distinct from C API misuse/failure.
- Logical time and delayed sends are runtime state.
- Named actor identity and logical addressing are core semantics; storage and physical routing are host-provided.
- Every resident interpreter can be inspected, including active configuration, history, continuation, queued internal events, invocations, and a host data-model root view.
- Session images capture/reconstruct Tinyactors-owned interpreter state against one exact immutable definition revision.
- Host data-model object graphs are bindings, not serialized session-image contents. Python, Ruby, JavaScript, and native hosts keep ownership of their existing objects.
- Host values are fat pointers with lifetime/clone hooks; core does not require a universal serialization format.
- Advanced durability—portable checkpoints, command sourcing, replay, effect deduplication, storage transactions—is a separate layer over core session images.
- Data models, I/O processors, invokers, clocks, storage, and concurrency remain under embedder control.
- The mandatory SCXML Event I/O Processor is built in; embedders provide its
  external-event scheduling/routing callback rather than reimplementing SCXML
  target and envelope semantics.

## Layout

- `include/tinyactors.h` — the complete, authored public API. Every public
  function is declared here with `TA_API`; nothing else is public.
- `src/*.{c,h}` — canonical implementation modules. Their headers are private:
  they include `tinyactors.h` and add layouts and internal functions.
- `include/tinyactors.c` — generated implementation amalgamation of `src/*.c`.
- `include/tinyactors.api.json` — generated, checked-in description of the
  public API for binding generators (`mise run api`; see
  `docs/internal/api-ir.md`). `api/annotations.rb` records what C types
  cannot express.
- `tests/*.c` — compile-only checks for internal and public header use.

## Intentional omissions

This is a header/API skeleton, not a completed implementation. It intentionally does not ship an XML parser/importer or a durability subsystem. It also leaves representation choices such as hash tables, actor name indexes, transition-set scratch storage, and portable checkpoint encoding to implementation work or optional modules.

## Public header workflow

Declare public types and `TA_API` functions in `include/tinyactors.h`; keep
layouts and internal functions in the private module headers. Add a new module
pair to `src/` and list its header in `src/internal.h` in dependency order.

Run `mise run amalgamate` from `tinyactors-core` to produce the distributable
`tinyactors.c`, in which every non-public function has internal linkage; only
that generated file is not edited by hand. `mise run amalgamate --check` is
suitable for CI and also rejects public prototypes without `TA_API` and private
redeclarations of public functions. See ADR 0041.
