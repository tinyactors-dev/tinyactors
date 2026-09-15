# Null data-model profile

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

The definition compiler/preparer rejects definitions that require unsupported data-model operations.

The implementation can therefore have:

```text
instance_size = 0
instance_alignment = 1
```

and no per-actor data-model allocation.

`In(state)` is evaluated directly through `ta_dm_host_in_state()`, which queries the statechart configuration and therefore remains correct for parallel states.
