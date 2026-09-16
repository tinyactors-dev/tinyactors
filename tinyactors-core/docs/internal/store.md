# Durability/store boundary

Tinyactors core defines no storage backend, transaction batch, object store, or universal persistence lifecycle.

`store.h` is currently an empty compatibility/module placeholder. The reusable durability design belongs above core session images and may define its own store interfaces for SQLite, PostgreSQL, object storage, browser storage, embedded flash, or application-specific systems.

Core responsibilities stop at named actor/session semantics, inspection, and reconstructible in-memory session images. A durability component is responsible for turning those images plus stable host-reference identities into portable checkpoints and for coordinating any command journal/effect protocol it chooses.
