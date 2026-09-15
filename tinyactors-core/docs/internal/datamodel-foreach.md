# Foreach semantics

A foreach action combines data-model behavior with interpreter control flow.

## Start

The engine calls:

```c
foreach_snapshot(collection_expr, ..., &snapshot, &count, &fault)
```

The plugin must:

1. evaluate the collection expression;
2. verify it is iterable;
3. create the semantic equivalent of a shallow copy;
4. preserve the data model's iteration order;
5. return an opaque snapshot value and finite iteration count.

If this fails, the engine terminates the foreach and its containing executable-content block and queues `error.execution`.

## Each iteration

For index `i`, the core obtains:

```c
foreach_item(snapshot, i, ..., &item, &index_value, &fault)
```

and then:

```c
bind_iteration(item_name, index_name, &item, &index_value, ...)
```

`bind_iteration` creates item/index variables if they do not already exist and otherwise replaces their values.

The index value is data-model-defined. It need not be a C integer even though the engine's traversal position is.

## Pause/resume

The continuation stores:

```text
snapshot ta_value
current numeric position
count
body program counter
item/index names
```

Therefore pausing after any iteration or inside the body does not depend on a host stack frame.

## Mutation during the loop

Mutating the original collection has no effect on traversal because the snapshot fixes membership/order at foreach start. Mutating the objects referenced *by* the shallow copy follows the host data model's normal shallow-copy semantics.
