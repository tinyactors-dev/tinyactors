# Named parameters and payload materialization

Send, invoke, and completion data may be constructed from named values.

## Parameter source

Each compiled parameter has a name and exactly one source:

```text
value expression
location expression
```

The core evaluates parameters in the definition's required order and accumulates successful results as:

```c
typedef struct {
    ta_atom name;
    ta_value value;
} ta_named_value;
```

If a parameter expression/location fails, the core generates `error.execution` and omits that name/value pair.

## Materialization

The engine calls:

```c
make_params(params, count, ..., &payload, &fault)
```

because only the selected data model knows the correct aggregate representation.

Examples:

- Python -> `dict` or plugin-defined duplicate-aware mapping;
- Ruby -> `Hash` or plugin-defined structure;
- JavaScript -> object/Map/custom wrapper;
- fixed C slots -> compact record;
- JSON model -> JSON object/array representation.

## Duplicate names

The core preserves duplicate pairs when building the list because SCXML send semantics require provided parameter/name-list entries not to be silently dropped. How a data model represents duplicates is plugin/profile-specific.

## Delayed sends

`make_params()` occurs when the send action executes. The resulting `ta_value` is retained in the delayed-send record. It is not rebuilt when logical time reaches the deadline.
