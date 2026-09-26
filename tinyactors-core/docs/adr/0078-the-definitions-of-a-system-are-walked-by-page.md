# ADR 0078: The definitions of a system are walked by page

**Status:** Accepted and implemented (2026-09-26); see the implementation notes. Applies ADR 0043 and ADR 0077; follows the paging model of ADR 0062.
**Date:** 2026-09-26

## Context

Core can look a definition up by ID (`ta_definition_inspect`) and by
identity (`ta_system_find_definition`), but it cannot list them. A host
that wants to report on every definition has to remember the ones it
registered itself.

napi did exactly that: `lib/system.js` kept a map of the definitions
JavaScript's `define()` returned, and `system.metrics()` and
`watchTraces({ families })` read it. Since ADR 0077, core also registers
definitions the host never names:

- an inline child chart, nested in its parent when the parent is defined
  (`ta_definition_nest`);
- a child chart compiled from text when an `<invoke>` runs.

Neither reaches `define()`'s map. Their per-definition counters, occupancy
and runaway stops (ADR 0072, ADR 0075) were missing from `metrics()`, and
watching the family `order/invoke` found nothing. The system totals
counted them; the breakdown by definition did not. That is the wrong way
round for the "3am" operator, who needs to find which chart is failing.

The user's words: "there should be a way to get a stream of all
definitions."

## Decision

### 1. A paged walk over definition IDs

```c
/*
 * One page of the definitions the system holds, in slot order. A
 * definition ID's low 32 bits are its slot (ADR 0043), so a walk resumes
 * with `from` = the last ID's slot + 1.
 */
typedef struct {
    /* The slot to start at: 0 for the first page. */
    uint32_t from;
    /* The most IDs this page takes; 0 means no limit. */
    uint32_t limit;
    TA_BUFFER(capacity, count) TA_OPTIONAL ta_definition_id *definitions;
    uint32_t capacity;
    /* The IDs this page holds; more than `capacity` yields TA_ERR_NOMEM. */
    TA_OUT TA_ALWAYS uint32_t *count;
} ta_system_definitions_params;

TA_API ta_result ta_system_definitions(const ta_system *system,
                                       TA_IN const ta_system_definitions_params *params);
```

- **What is listed:** every definition the system holds, which is every
  registered definition that is not yet freed, retired ones included.
  A retired definition is still in use by its actors, and its counters
  and occupancy are still live, so a report must see it.
  `ta_definition_inspect` says which are retired; the walk has no flag
  for it. Compiled but unregistered definitions are the caller's and are
  not listed.
- **The shape follows core's buffer convention** (`TA_BUFFER`, as
  `ta_definition_occupancy` and `ta_actor_history` use): a call with no
  buffer answers how many IDs the page holds, and a second call fills
  them. Generated bindings already handle that shape.
- **Why IDs and not a visitor:** ADR 0062 walks actors with a visitor
  because a population can be millions and each visit builds an
  inspection. Definitions are few, and the caller wants IDs to call the
  per-definition reads (`ta_definition_inspect`, `ta_definition_counters`,
  `ta_definition_occupancy`) anyway.

### 2. Changes during a walk

As in ADR 0062, a walk is a sequence of pages, and each page is a
snapshot of its moment. Positions are slots, visited in increasing order:

- a definition held throughout the walk is listed exactly once: its slot
  does not change while it lives;
- a definition registered during the walk is listed only if its slot is
  at or after the next page's `from`;
- a definition freed during the walk is not listed after it is freed;
- a slot freed and reused during the walk lists the new definition, with
  its new generation, when the walk reaches that slot. It is a different
  definition, so this is not a repeat.

### 3. Nesting is reported from parent to child

A nested definition can have more than one parent: ADR 0077 makes
registration idempotent by identity, so the same inline child in two
parent charts is one definition nested twice. A single `parent` field on
the inspection would be wrong. Nesting is therefore reported the way core
already stores it:

- the parent names its children through `ta_definition_invoke()`'s
  `nested`, per `<invoke>`;
- the child's inspection counts its parents (`holders`, ADR 0077).

A host that walks every definition sees each parent, and so every edge.

## Consequences

- A host reports on every definition without remembering which ones it
  registered, and nothing core registers on its own is invisible.
- napi removes its map of defined definitions. `system.definitions()`
  streams the walk; `metrics()` and `watchTraces({ families })` read it,
  so invoked and nested children appear under their own families.
- The API grows by one function and one params struct. Nothing is added
  per definition or per actor.

## Testing

- **Core:** a walk in small pages lists every held definition once; a
  registration and a free between pages behave as section 2 says; a
  retired definition with a resident actor is listed until its last actor
  is gone; a nested child is listed, and its parent names it.
- **napi, on Node and Bun:** `definitions()` streams builder, imported,
  nested inline and invoke-time definitions; `metrics()` reports a nested
  child's counters and occupancy; `watchTraces({ families: ['<parent>/invoke'] })`
  traces an invoke-time child; freed definitions drop out.

## Implementation notes

- **Core:** `ta_system_definitions()` in `src/system.c`, a scan of the
  definition slots from `from`, as the Decision describes. It touches
  nothing that frees definitions. `tests/retirement_test.c` (`test_walk`)
  covers pages of one to three, a size query, a free and a registration
  between pages, a retired definition in use, and a nested child.
- **napi `system.definitions({ pageSize })`** is a lazy iterator over the
  walk (default 1,024 IDs per page, at most 65,536), yielding
  `DefinitionInspection`. Inspections gain `holds`, `nestedBy` (the
  parents' count) and `nested` (the children this chart's `<invoke>`s
  nest), so nesting is read from parent to child, as section 3 says.
- **napi `metrics()`** walks core's definitions instead of its own map, so
  nested and invoke-time children report their counters and occupancy
  under their own families.
- **Where the code differs from the text: `watchTraces({ families })`
  does not read the walk.** A walk at watch time would still miss a
  definition registered afterwards, and invoke-time children are
  registered by core during a run, where no JavaScript runs. So the addon
  keeps the watched family names and resolves each definition the first
  time a delivery meets it (`src/watch.c`), remembering the answer in two
  hash sets (matched, and checked); the check stays constant-time, and
  definitions registered after the watch are caught. Generational IDs make
  remembering a freed definition harmless.
- **napi's map of defined definitions is gone,** with the re-apply it did
  when JavaScript defined a chart of a watched family.
- **Tests:** `test/definitions.js` on Node and Bun (streaming every kind of
  definition in pages of one, retired and freed definitions, a nested
  child's metrics, a watched invoke-time child), and the native
  `watch_test` (families resolved against a real system).

