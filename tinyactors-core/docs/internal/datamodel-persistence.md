# Data-model persistence

Actor unloading must be observationally invisible to the statechart.

The data-model plugin therefore snapshots all per-actor state that is not already owned by the core.

```c
ta_result (*snapshot)(..., ta_snapshot_writer *writer);
ta_result (*restore)(..., ta_snapshot_reader *reader);
```

## Plugin snapshot includes

Potential examples:

- named variable values;
- host-object references encoded into a durable form;
- plugin-specific scope state;
- script-created variables;
- data-model metadata that changes at runtime.

## Core snapshot separately includes

- active parallel configuration;
- history;
- current macrostep continuation;
- current/internal events;
- late-binding initialized bits;
- active invocations;
- delayed sends/timers;
- live temporary `ta_value`s owned by a suspended continuation, including foreach iteration snapshots.

The plugin/value implementation must make those live values serializable if the actor is to be unloadable while they are present.

## Host objects

A Python plugin can wrap `PyObject *`, Ruby can wrap `VALUE`, and Node can wrap a host reference. Lifetime management is deliberately plugin-specific.

If a particular wrapped value cannot be serialized, snapshot/unload may fail. The common case should be easy, but the core does not pretend every arbitrary host object is persistable.
