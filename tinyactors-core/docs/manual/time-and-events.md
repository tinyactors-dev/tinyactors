# Time and events

## Logical time

TA never reads the wall clock. The embedder supplies monotonic logical time with `ta_system_set_time()`. Delayed sends persist an absolute logical due time, not a remaining duration.

Advancing time makes due sends eligible. Loading a snapshot does not itself perform external I/O.

## Event names

Use `ta_event_name_parse()` at a string boundary and retain the returned `ta_event_name_id` for hot paths. The core interns names as structured token paths.

Prefix matching follows SCXML semantics:

- `error` matches `error`, `error.execution`, `error.execution.datamodel.condition`, ...
- `error.execution` does not match `error.communication`;
- a universal wildcard is represented by transition metadata, not by reparsing every event string.

Generated names such as `done.state.X`, `done.invoke.X`, and detailed `error.*` names should be built structurally, without formatting and reparsing strings.
