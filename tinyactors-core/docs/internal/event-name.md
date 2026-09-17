# `event_name.h`

The event-name table implements the structural representation required by ADR 0007.

Each node stores its parent event-name ID, one atom segment, depth, and flags/cached metadata as needed. The root is empty. Built-in roots include `error`, `error.execution`, `error.communication`, `error.platform`, `done`, `done.state`, and `done.invoke`.

## Matching

A prefix descriptor `P` matches event `E` if `P` equals `E` or is a token-prefix ancestor of `E`. Matching is case-sensitive. Compiled transitions store structural `ta_event_pattern` records rather than reparsing dotted names.

A wildcard segment marked `TA_EVENT_PATTERN_SEGMENT_WILDCARD` matches zero or more event-name segments and may occur at any position. Remaining event-name segments are allowed after the pattern has matched to preserve SCXML prefix matching. An empty transition pattern list is eventless, not a wildcard.

## Generated names

Construct generated names from ordered atom segments such as `[DONE, INVOKE, invoke-id-segment]`, `[ERROR, EXECUTION, datamodel, condition]`, or `[ERROR, COMMUNICATION, INVOKE, invoke-id-segment, UNAVAILABLE]`. Intern reusable segments once and pass complete segment lists to `ta_event_name_intern()`; do not format and reparse dotted strings. Invocation-unavailable errors carry the logical invocation identity in the name because their SCXML invoke-ID envelope field must remain empty. Their name describes the service condition without exposing the interpreter mechanism that detected it.

## Reconstruction/durability

`ta_event_name_id` is a runtime handle. A session image may retain the structural interpreter representation in-process. Any portable durability encoding must record logical segments or another stable dictionary key and re-intern them after restart rather than treating the numeric ID as process-independent.
