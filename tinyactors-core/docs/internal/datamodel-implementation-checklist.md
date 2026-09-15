# Data-model implementation checklist

A plugin author should be able to answer all applicable questions below.

## Definition

- Can expressions/scripts be validated or compiled once per definition?
- Which semantic capabilities are unsupported?
- Is shared prepared state immutable after preparation?

## Instance

- What is the per-actor byte cost?
- How are bindings represented?
- What is the uninitialized/default value?
- Can host-provided initial values be imported?

## Expressions

- Are conditional expressions side-effect-free by contract?
- How is `In(state)` implemented through `ta_dm_host`?
- What makes a value legal?
- What makes a location legal?
- How are writes to protected system data rejected?

## Literals

- Which media types/frontends are understood?
- What is the fallback for text/bytes?
- Can literal import create host-native objects safely?

## Foreach

- What counts as iterable?
- What is the iteration order?
- How is a shallow snapshot represented?
- Is the snapshot value serializable if actor unloading is required?
- What value is exposed as the iteration index?

## Parameters

- How are ordered named pairs aggregated?
- How are duplicates represented?

## Events and system data

- How is foreign event data converted?
- How is an unavailable/illegal payload represented?
- How are `_event`, session identity, machine name, I/O metadata and platform values exposed?

## Persistence

- Can every ordinary plugin value be snapshotted?
- Which values make an actor temporarily non-unloadable?
- Is restore deterministic with respect to the serialized form?

## Errors

- Does every semantic failure return `TA_OP_ERROR` with useful fault detail rather than escaping as a process-level failure?
- Are host exceptions caught and translated at the plugin boundary where appropriate?
