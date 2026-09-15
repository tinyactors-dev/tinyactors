# Data declarations and initialization

Data declarations are represented independently of their initializer.

```c
typedef struct {
    ta_atom name;
    ta_state_id owner_state;       /* root/sentinel for top-level */
    ta_initializer_id initializer; /* none, expr, literal, prepared */
} ta_data_decl;
```

The precise internal structure may differ, but the semantic fields are needed.

## Declaration

Every binding exists from actor initialization, including with late binding.

The core invokes:

```c
dm->declare(..., name, ...);
```

for each declaration.

A data model can represent the uninitialized/default state however it chooses.

## Early binding

All initializers are applied during actor/session initialization.

## Late binding

The declaration exists immediately, but its initializer runs only immediately before entry actions the first time the owning state is entered.

The engine tracks an initialized bit per late-bound declaration or per compiled initialization group.

## Initializer source

An initializer may be:

- absent;
- an expression ID;
- an opaque literal ID;
- a prepared host value;
- an environment override.

The engine evaluates/imports the initializer and uses `set_named()` to install the result.

## Failure

If an initializer cannot produce a legal value, statechart semantics generate `error.execution`, while the named binding remains present with the data model's empty/uninitialized/default representation.
