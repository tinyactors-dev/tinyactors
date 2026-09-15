# `history.h`

History is definition-specialized persistent state.

- **shallow history** records active immediate child configuration of the history state's parent;
- **deep history** records the full active descendant configuration down to atomic leaves.

Parallel parents can therefore require history values containing multiple branches. The layout compiler assigns each history pseudo-state a compact slot sized for exactly what it can store.

History values are updated on exit according to SCXML semantics and are included in actor snapshots.
