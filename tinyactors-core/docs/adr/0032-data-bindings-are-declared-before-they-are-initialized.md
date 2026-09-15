# ADR 0032: Data bindings are declared before they are initialized

- Status: Accepted
- Date: 2026-09-15

## Context

SCXML early and late data binding differ in **when initial values are assigned**, not in whether the named data items exist.

With late binding, all data items are created when the session is initialized, but the initializer associated with a state is applied only the first time that state is entered, before its entry actions. Top-level values supplied by the embedding environment override corresponding top-level defaults.

A single `init_instance()` callback cannot express these semantics cleanly without requiring the plugin to re-implement statechart initialization rules.

## Decision

The compiled definition contains explicit data declarations and initializers.

At actor creation the core:

1. initializes the data-model instance;
2. declares every data binding;
3. installs protected system bindings/views;
4. applies environment-provided top-level overrides;
5. applies all early-bound initializers.

For late binding the core records initialization state per declaration or declaration group. On first entry of the owning state it applies that state's initializers before entry executable content.

The plugin receives semantic operations such as `declare`, `set_named`, and literal/expression evaluation. It does not decide *when* early or late initialization occurs.

## Consequences

The execution engine remains the single owner of early/late binding semantics.

A host language can map a data declaration to a Python global, Ruby object slot, JavaScript property, fixed C slot, or any other representation.

Late-binding progress is part of persistent actor/session state and must survive unloading and reload.
