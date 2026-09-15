# Expressions and locations

SCXML distinguishes three expression classes. `ta` preserves that semantic distinction even if a particular language uses one syntax for all three.

## Conditional expression

```c
eval_condition(expr_id, ..., bool *out, ta_fault *fault)
```

The plugin returns a boolean result. `In(state)` must be supported by every non-null conditional language and must query the engine's complete active configuration, including parallel regions.

If evaluation fails, the engine:

1. treats the condition as false;
2. enqueues a structured `error.execution...` internal platform event;
3. continues according to statechart semantics.

A plugin fault is therefore not normally returned from `ta_actor_resume()` as a C API failure.

## Value expression

```c
eval_value(expr_id, ..., ta_value *out, ta_fault *fault)
```

The result must be a legal value for the data model. It can be used for assignment, parameter construction, content, send arguments, invoke arguments, initializers, etc.

## Location expression

Locations are kept opaque to the core. The core needs two operations:

```c
read_location(location_expr_id, ..., ta_value *out, ...);
assign(location_expr_id, value, ...);
```

This avoids introducing a universal `ta_location` representation unless profiling or a future ABI need justifies one.

A location may represent a variable, object property, array element, host-language lvalue, fixed slot, database-like cell, or another plugin-defined concept.

## Protected locations

A data model must reject attempts to write protected system variables. The core converts that semantic fault into `error.execution`.
