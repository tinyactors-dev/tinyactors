# Data-model implementation checklist

A plugin author should be able to answer all applicable questions below.

## Definition

- Can expressions/scripts be validated or compiled once per definition?
- Which semantic capabilities are unsupported?
- Is shared prepared state immutable after preparation?

## Instance/binding

- What is the per-actor adapter byte cost?
- What host object does `binding` refer to?
- Who owns that object's lifetime?
- Can a fresh adapter instance rebind to the same/restored host object during session reconstruction?
- What is the uninitialized/default value?

## Expressions

- Are conditional expressions side-effect-free by contract?
- How is `In(state)` implemented through `ta_dm_host`?
- What makes a value/location legal?
- How are writes to protected system data rejected?

## Foreach

- What counts as iterable?
- What is the iteration order?
- How is a shallow iteration snapshot represented while a macrostep is paused?
- Does it retain host values safely for the lifetime of an in-process session image?

## Inspection

- Does `inspect` return a useful host-native root view?
- Can operational tooling inspect a huge object lazily without copying/traversing it in Tinyactors?

## Events and system data

- How is foreign event data converted?
- How is an unavailable/illegal payload represented?
- How are `_event`, session identity, machine name, I/O metadata, and platform values exposed?

## Optional durability

- If process-independent checkpoints are required, how does the higher-level durability component resolve the host binding after restart?
- How are interpreter-held host values in events/timers/continuations encoded or rebound?
- Which external I/O results must be journaled for deterministic replay/reconciliation?

## Errors

- Does every semantic failure return `TA_OP_ERROR` with useful fault detail rather than escaping as a process-level failure?
- Are host exceptions caught and translated at the plugin boundary where appropriate?
