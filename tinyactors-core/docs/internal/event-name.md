# `event_name.h`

The event-name table implements the structural representation required by ADR 0007.

Each node stores:

- parent event-name ID;
- one atom segment;
- depth;
- flags/cached metadata as needed.

The root is empty. Built-in roots include `error`, `error.execution`, `error.communication`, `error.platform`, `done`, `done.state`, and `done.invoke`.

## Matching

A descriptor `P` matches event `E` if `P` equals `E` or is a token-prefix ancestor of `E`. Matching is case-sensitive. `*` is represented as a wildcard pattern in transition metadata. `foo`, `foo.`, and `foo.*` are equivalent SCXML descriptors at import/build time.

## Generated names

Prefer append operations:

```text
DONE_INVOKE + invoke-id-segment
ERROR_EXECUTION + datamodel + condition
```

Do not format `done.invoke.123` or `error.execution.foo` and then parse it again.

## Persistence

`ta_event_name_id` is a runtime handle. Snapshots must encode the segment sequence or a stable dictionary key and re-intern it on restore.
