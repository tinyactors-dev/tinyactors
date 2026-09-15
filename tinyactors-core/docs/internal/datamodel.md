# `datamodel.h`

The data-model interface is deliberately broad and host-defined.

Definition-level lifecycle:

1. `prepare_definition` compiles/prepares expression metadata shared by all actors;
2. `instance_size` / `instance_alignment` define per-actor storage;
3. `init_instance` / `destroy_instance` manage one actor's model state.

Execution operations include conditions, value expressions, locations, assignment, and scripts. Missing SCXML operations such as iteration/content/data initialization may be added as the builder/executable-content representation becomes concrete.

`ta_dm_host` exposes statechart context such as `In(state)`, current event, session/actor identity, and eventually other system variables. The data model should not receive an unrestricted pointer to interpreter internals.

Snapshot/restore is part of the data-model contract because host-native model state may require custom serialization.
