# Data-model instance lifecycle

Each actor/session has exactly one data-model instance for its definition.

## Creation sequence

Conceptually:

```text
allocate actor storage
init data-model instance
declare all user data bindings
install/provide protected system-variable view
apply top-level host overrides
apply early initializers
enter initial configuration
apply late initializers for states entered for the first time
execute entry actions
```

The exact ordering of independent declarations/initializers may follow the selected data model's semantics. The engine must not accidentally introduce dependencies that the data model forbids.

## Environment overrides

Actor creation may provide named initial values. Overrides apply to matching top-level declarations before their normal default initializer would take effect.

The override is a `ta_value`; conversion/import is the plugin's responsibility if the value originates in another value domain.

## Late binding state

Late-binding initialization progress is statechart session state, not just plugin state. If a data declaration associated with state S has already been initialized, unloading and restoring the actor must not cause it to be initialized again.

## Destruction

Destroying an actor calls `destroy_instance()` after the statechart has performed termination semantics. Unloading an actor does not semantically destroy it; it serializes the plugin instance and later restores it.
