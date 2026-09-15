# `configuration.h`

The semantic SCXML configuration is a legal set of active states. Tinyactors generally stores only active **atomic leaves**; ancestors are derivable from the definition.

The compiler chooses a `ta_configuration_layout`:

- `TA_CONFIG_SINGLE_LEAF` only when the definition proves that one atomic leaf is sufficient;
- 32- or 64-bit bitmaps when all potentially active atomic states fit;
- fixed word storage for larger machines.

Generic interpreter code must support multiple simultaneously active atomic leaves. `In(state)` is true when any active leaf is the state or a descendant of it.

Parallel entry adds an active descendant in every child region. Transition selection begins independently from all active atomic leaves, then conflict resolution yields the microstep transition set.
