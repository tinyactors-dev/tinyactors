# Time and events

## Logical time

Tinyactors never reads the wall clock. The embedder supplies monotonic logical time in milliseconds with `ta_system_set_time()`. Delayed sends persist an absolute logical due time, not a remaining duration.

Advancing time makes due sends eligible. Loading a snapshot does not itself perform external I/O.

## Event names

Intern each event-name segment with `ta_system_intern_atom()`, then construct
the name from one ordered `ta_atom_list` with `ta_event_name_intern()`. The
core accepts no dotted-string event-name parser, so names are always structured
token paths. The list must be non-empty and its atoms must come from that same
system.

Prefix matching follows SCXML semantics:

- `error` matches `error`, `error.execution`, `error.execution.datamodel.condition`, ...
- `error.execution` does not match `error.communication`;
- transitions can hold several compiled atom-segment patterns; a wildcard
  segment can occur anywhere, so `error.*.retry` can match retry events below
  `error` without reparsing event strings;
- an eventless transition has no patterns, so it is checked only during
  stabilization rather than against named events.

Generated names such as `done.state.X`, `done.invoke.X`, and detailed `error.*` names should be built structurally, without formatting and reparsing strings.
