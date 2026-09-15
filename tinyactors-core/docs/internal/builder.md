# `builder.h`

The builder is the source-language-neutral construction API for definitions.

It should eventually express the entire statechart model needed by the interpreter:

- atomic, compound, parallel, final, shallow-history, and deep-history states;
- initial transitions and their actions;
- entry/exit actions;
- transitions with zero or more event descriptors, optional conditions, type, targets, and actions;
- data declarations/binding metadata as required by the selected data model;
- sends, raises, conditionals, iteration, assignments/scripts, invokes, finalize blocks, and extension actions.

Compilation performs structural validation and emits compact immutable tables/layouts.

`ta_definition_add_transition()` creates a transition with no event patterns.
It is eventless until at least one pattern is added, so it is considered only
during eventless stabilization. To add a named-event pattern, first intern
each literal segment with `ta_system_intern_atom()`. Pass the complete ordered
array of literal and wildcard `ta_event_pattern_segment` values in one
`ta_event_pattern_spec` to `ta_definition_add_event_pattern()`. A wildcard
segment has `TA_EVENT_PATTERN_SEGMENT_WILDCARD` set and matches zero or more
name segments, so the segments `error`, `*`, `retry` match `error.retry` and
`error.network.retry`. The builder rejects empty patterns and invalid segment
flags; all literal atoms must belong to the builder's system. It copies the
complete specification immediately. A pattern consisting only of a wildcard is
the explicit universal matcher.

Compilation packs patterns and their atom/wildcard segments into shared
immutable definition tables. A transition matches a named event when any of
its patterns matches. Pattern matching remains prefix-compatible: trailing
event-name segments are allowed after a pattern has matched. An empty
transition pattern list is never a wildcard.

SCXML XML import, generated C, Ruby/Python builders, or other DSLs should all target this same builder/IR rather than adding alternate runtime semantics.
