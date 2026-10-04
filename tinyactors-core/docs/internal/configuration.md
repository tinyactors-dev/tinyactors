# `configuration.h`

The semantic SCXML configuration is a legal set of active states. Tinyactors generally stores only active **atomic leaves**; ancestors are derivable from the definition.

The compiler fills one `ta_configuration_layout`: one bit per state, stored in whole 64-bit words. Only atomic and final states are ever set.

Generic interpreter code must support multiple simultaneously active atomic leaves. `In(state)` is true when any active leaf is the state or a descendant of it.

Parallel entry adds an active descendant in every child region. Transition selection begins independently from all active atomic leaves, then conflict resolution yields the microstep transition set.
