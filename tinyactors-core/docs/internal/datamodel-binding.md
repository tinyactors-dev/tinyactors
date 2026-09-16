# Data-model binding

The data-model adapter connects one interpreter instance to host-owned application state.

`ta_datamodel_init_instance_params.binding` is the host value supplied when an actor is created or reconstructed. In CPython this can wrap the application's existing object; in CRuby a `VALUE`; in Node a JavaScript reference; in a compact C embedding it may point at a native struct or map.

The adapter's per-instance storage should therefore be understood as **binding state**, not necessarily the business data itself. It may contain only a pointer/reference to a much larger host graph.

## Inspection

The optional `inspect` operation returns a host-native root `ta_value` for operational tooling. Tinyactors does not require a universal map/list/string object model and does not recursively traverse the result. Language bindings can expose their existing debugger/serializer/UI facilities directly.

This keeps inspection cheap for huge data models. An LLM conversation can contain millions of tokens while the Tinyactors binding remains one host reference.

## Reconstruction

The data model has no core `snapshot`/`restore` methods. Reconstruction initializes a fresh binding adapter instance around the host value supplied by the caller. In-process capture can reuse the same object immediately. After process restart, the application/durability layer restores or resolves its objects first and then supplies the binding.

## Durable application state

Application persistence is intentionally visible to the statechart when the chart drives it through an I/O processor. Mutating a host object is not the same thing as committing it to a database. States such as `Dirty`, `Persisting`, and `DatabaseCommitted` can model that distinction explicitly.
