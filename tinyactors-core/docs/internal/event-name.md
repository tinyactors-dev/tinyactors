# `event_name.h`

The event-name table implements the structural representation required by ADR 0007.

Each node stores:

- parent event-name ID;
- one atom segment;
- depth;
- flags/cached metadata as needed.

The root is empty. Built-in roots include `error`, `error.execution`, `error.communication`, `error.platform`, `done`, `done.state`, and `done.invoke`.

## Matching

A prefix descriptor `P` matches event `E` if `P` equals `E` or is a token-prefix ancestor of `E`. Matching is case-sensitive. Compiled transitions store a span of `ta_event_pattern` records, each of which is a sequence of `ta_event_pattern_segment` records. Literal segments contain atoms; segments marked `TA_EVENT_PATTERN_SEGMENT_WILDCARD` match zero or more event-name segments and can occur at any position. Thus the atom sequence `error`, `*`, `retry` matches `error.retry` and `error.network.retry`; remaining event-name segments are allowed to preserve SCXML prefix matching. A transition matches a named event when any of its patterns matches. An empty pattern list is eventless, not a wildcard. SCXML's trailing forms `foo`, `foo.`, and `foo.*` remain equivalent at import/build time.

## Generated names

Construct an event name from one ordered `ta_atom_list`:

```text
[DONE, INVOKE, invoke-id-segment]
[ERROR, EXECUTION, datamodel, condition]
```

Use `ta_system_intern_atom()` once for each reusable segment, then pass the
atom list to `ta_event_name_intern()`. Do not format `done.invoke.123` or
`error.execution.foo` and parse it again. No string-based event-name parser or
append API is exposed.

## Persistence

`ta_event_name_id` is a runtime handle. Snapshots must encode the segment sequence or a stable dictionary key and re-intern it on restore.
