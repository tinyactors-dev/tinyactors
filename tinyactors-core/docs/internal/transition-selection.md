# Transition selection and parallel conflict handling

For a named event, transition search begins from **every active atomic state**. For each atomic state, candidates are considered from the atomic state outward through ancestors until enabled transitions are found according to SCXML rules.

A candidate is enabled when:

- its event pattern matches the current event, or it is eventless during an eventless-selection pass;
- its condition is absent or evaluates true.

Condition-evaluation failure is treated according to SCXML expression-error semantics: queue `error.execution...` and treat the condition as not enabling that transition.

Because parallel regions can independently select transitions, the initial candidate set may contain multiple transitions. The interpreter removes conflicting transitions according to SCXML preemption/priority semantics, including source ancestry and document/build order. The resulting enabled set is one microstep.

The microstep computes all required exits before applying entries. Exit order, transition-action order, and entry order remain deterministic even though multiple parallel regions are semantically active at once.

Targetless transitions execute content without changing configuration. Internal vs external transition type affects whether a compound source is exited/re-entered when all targets are descendants of the source.
