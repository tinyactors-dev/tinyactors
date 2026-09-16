# Data-model instance lifecycle

Each resident actor/session has one data-model adapter instance associated with its definition. The adapter instance may be very small: in language bindings it can be only a binding to an already-existing host object graph.

## Creation sequence

Conceptually:

```text
allocate actor storage
init data-model adapter with host binding
declare/install SCXML-visible bindings/system views as required by the adapter
apply definition initialization semantics
enter initial configuration
execute entry actions
```

The exact ordering of declarations/initializers follows the selected data model's semantics.

## Host binding

`ta_datamodel_init_instance_params.binding` is supplied by actor creation/reconstruction. A Python binding may point at the application's `PyObject *`; a C binding may point at a native struct. Tinyactors does not assume that the adapter instance owns the underlying business data.

## Late binding state

Late-binding initialization progress that affects SCXML execution belongs to Tinyactors interpreter/session-image state. Reconstructing the interpreter must not accidentally repeat initialization merely because a fresh adapter wrapper was bound to the same host object.

## Destruction and reconstruction

Destroying an actor calls `destroy_instance()` after statechart termination semantics. Capturing a session image does not serialize or destroy the host object. Reconstruction initializes a new adapter instance around the caller-supplied binding and restores Tinyactors-owned interpreter state around it.
