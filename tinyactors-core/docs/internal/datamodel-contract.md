# Data-model contract

A `ta_datamodel` is a fat pointer:

```c
typedef struct {
    void *self;
    const ta_datamodel_vtable *v;
} ta_datamodel;
```

It separates statechart semantics from an embedder-selected data language and value representation.

## The core owns

- actor/session lifecycle;
- state configuration and parallel-state semantics;
- transition selection;
- early/late binding timing;
- internal/external event processing;
- `error.*` event generation and control consequences;
- send/invoke timing;
- pausing/resuming executable content;
- definition and action IR;
- when system variables change;
- when a foreach begins, advances and ends.

## The data model owns

- expression syntax/compiled expression implementation;
- legal values;
- locations and assignment semantics;
- named variable/binding representation;
- script semantics;
- interpretation of opaque literals/content;
- collection shallow-copy semantics and iteration order;
- host-native representation of system variables;
- conversion of incoming event data to a legal value;
- aggregation of named parameter pairs;
- decoding an evaluated SCXML Event I/O Processor target into a structured
  internal/self/parent/invoke/actor target;
- persistence of plugin-owned per-actor state.

## Important non-goal

The vtable does not contain callbacks named after XML elements. A callback exists only when the runtime needs a semantic capability.

For example, `<param expr="x">` and a native builder's `TA_VALUE_EXPR` parameter both compile to the same value-evaluation IR. `<param location="x">` compiles to location-read IR.
