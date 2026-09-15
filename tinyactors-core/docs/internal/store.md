# `store.h`

A store is an optional host adapter for actor persistence.

The interface must support saving, loading, and removing actor session state without assuming a filesystem. Backends may include:

- SQLite;
- application databases;
- append-only logs;
- memory maps;
- flash sectors;
- browser storage mediated by the host;
- in-memory test stores.

The present `save(actor, writer)` / `load(actor, reader)` skeleton needs refinement: the ownership between core serialization and store transport should be unambiguous, and atomic timer/session persistence must be supported.

The core requirement is semantic, not storage-shaped: unload/reload must reproduce the same actor session.
