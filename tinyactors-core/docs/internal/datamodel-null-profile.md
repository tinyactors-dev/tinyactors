# Null data-model profile

Core ships this profile as `ta_null_datamodel()` (ADR 0057,
`src/null_datamodel.c`). Hosts use it rather than writing their own.

The Null data model is the minimum semantic profile and should remain exceptionally small.

## Supported

- conditional `In(state)` only.

## Unsupported

- user data bindings;
- value expressions;
- location expressions;
- assignment;
- scripts;
- foreach;
- content/data manipulation requiring a data model;
- application access to system variables.

`prepare_definition` refuses a chart with any expression other than
`In(id)`, naming the expression (ADR 0058), so such a chart fails when it is
defined. Operations that carry no expression but need data (literal
`<content>`, a `<data>` without an initializer, `In()` used as a value)
fault at run time as `error.execution`.

The implementation can therefore have:

```text
instance_size = 0
instance_alignment = 1
```

and no per-actor data-model allocation.

`In(state)` is evaluated directly through `ta_dm_host_in_state()`, which queries the statechart configuration and therefore remains correct for parallel states.
