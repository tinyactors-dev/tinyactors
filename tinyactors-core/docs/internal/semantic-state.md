# Semantic state of an actor

The observable running session is larger than the resident `ta_actor` struct currently shown in the header skeleton. A correct persistence implementation treats the following as one logical unit:

1. actor identity and optional name;
2. definition identity/revision;
3. active state configuration, including all parallel regions;
4. shallow and deep history values;
5. data-model instance state;
6. active invocation records and cancellation state;
7. delayed sends owned by the actor;
8. when a macrostep is in progress:
   - the current external event;
   - the internal event queue in FIFO order;
   - selected/enabled transition information if selection has passed that point;
   - exit/transition-action/entry cursors;
   - executable-content program counter and loop state;
   - states pending invocation or any equivalent continuation data.

The invariant is stronger than byte-for-byte restoration: unloading and reloading must be **observationally invisible to the statechart**, assuming host values and external services honor their persistence contracts.
