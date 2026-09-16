# Semantic state of an actor

A running actor combines **Tinyactors-owned interpreter state** with a **host-owned data-model binding**. These must not be conflated.

## Interpreter state owned by Tinyactors

The reconstructible session image includes:

1. actor/session identity and optional logical named address;
2. exact definition identity (family + revision fingerprint);
3. active state configuration, including all parallel regions;
4. shallow and deep history values;
5. active invocation records and cancellation state;
6. delayed sends owned by the actor;
7. when a macrostep is in progress:
   - the current external event;
   - the internal event queue in FIFO order;
   - selected/enabled transition information if selection has passed that point;
   - exit/transition-action/entry cursors;
   - executable-content program counter and loop state;
   - states pending invocation or equivalent continuation data.

Interpreter-owned envelopes may retain `ta_value` references to host objects. An in-process image retains those references; it does not serialize their referents.

## Host-owned data model

The data-model object graph is not session-image contents. A Python/Ruby/JavaScript/native binding already owns its objects, which may be arbitrarily large. Reconstruction supplies the binding again to the data-model adapter.

The statechart can still inspect and mutate that object through the adapter. Operational inspection asks the adapter for a root `ta_value` view; it does not require Tinyactors to copy or enumerate the whole graph.

## Reconstruction invariant

Given the exact definition revision, the captured interpreter image, and the corresponding host data-model binding, reconstruction must restore the same interpreter-visible semantic point. Process-independent durability additionally requires a higher-level mechanism to persist/resolve all referenced host values and external-service state.
