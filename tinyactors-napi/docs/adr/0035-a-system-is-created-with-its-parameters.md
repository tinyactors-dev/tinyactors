# ADR 0035: A system is created with its parameters

**Status:** Proposed (2026-09-29). Applies core ADR 0092 (system parameters). Would amend:
- ADR 0005 and ADR 0007: the scheduling budgets and mailbox limits become parameters;
- ADR 0016 and ADR 0017: the trace value and log limits become parameters, and the staging buffer has a fixed size.
**Date:** 2026-09-29

## Context

napi's limits live in several places:
- **`SystemOptions.scheduling`:** `resumeBudget`, `actionBudget`,
  `resumesPerTurn`, `dueBudget`, `microstepLimit`, and the message budget;
- **the mailbox limits** `maxQueuedMessages` and `maxRetainedBytes`;
- **`setTraceSink()`'s `valueLimit`,** default 256 and at most 1023, fixed by
  `char trace_value[1024]` (`src/napi_system.h`), and `logLimit`;
- **compile-time constants:** `TNA_DM_REGISTERED_MAX 8` data models, names
  of up to `TNA_DM_NAME_MAX 32` bytes, `TNA_OTLP_KEEP_BYTES` (1 MiB of
  trace staging kept between turns), and napi's own copy of the event-name
  depth.

granary needs trace values of up to 16 KiB, which no option allows. Core
ADR 0092 introduces system parameters under four rules: set once, bounds
included, every buffer bounded (allocated at creation when every run needs
it), defaults as enumerators. napi
follows the same rules for its own parameters.

## Decision

### 1. `createSystem({ parameters })`

```ts
createSystem({
  parameters: {
    // core's (core ADR 0092)
    microstepLimit?: number; eventBlockCache?: number; eventNameDepth?: number;
    compileMessageBytes?: number; compileTextBytes?: number; compileNearestBytes?: number;
    interpreterBytes?: number; renderBytes?: number; interpreterNesting?: number;
    maxActors?: number; maxPendingSends?: number; maxDefinitions?: number;
    // napi's
    traceValueBytes?: number;       // default 1024, at most 1 MiB
    logValueBytes?: number;         // default 1024, at most 1 MiB
    traceStagingBytes?: number;     // default 1 MiB, at least 64 KiB
    registeredDatamodels?: number;  // default 8, at most 1024
    datamodelNameBytes?: number;    // default 32, at most 256
    resumeBudget?: number; actionBudget?: number; resumesPerTurn?: number;
    dueBudget?: number; messageBudget?: number;
    maxQueuedMessages?: number; maxRetainedBytes?: number;
  },
});
system.parameters;   // every parameter's value, frozen
parameterInfo();     // name → { default, min, max, unit, owner: "core" | "napi" }
```

- **Core's parameters go into `ta_system_config.params`,** and napi's
  are validated by napi, before anything is created.
  - An unknown name, a non-integer, or a value outside its bounds throws
    `invalid`, naming the parameter and its bounds.
  - Core's bounds come from `ta_param_info_of()`, never from a copy in napi.
- **Parameters cannot change after creation,** so `system.parameters` is
  frozen.
- **Removed:**
  - `SystemOptions.scheduling`, and the mailbox limit options, which move
    into `parameters`;
  - `setTraceSink()`'s `valueLimit` and `logLimit`, replaced by
    `traceValueBytes` and `logValueBytes`.
- **Unchanged:** `configureTraces()` still changes detail, kinds and
  sampling at run time. Those are the sink's configuration, not parameters
  (core ADR 0092 rule 1).

### 2. napi's buffers are allocated at creation

Beside core's arena, napi allocates its own, from the allocator it gives
core, sized by its parameters:
- **The trace value buffer** of `traceValueBytes`, and the log value buffer
  of `logValueBytes`. They replace `char trace_value[1024]`.
- **The OTLP staging buffer** of `traceStagingBytes`. A turn whose spans
  would overflow it hands the sink a request early, so one turn may produce
  several requests. OTLP accepts any split, and nothing is dropped.
- **The registered data model table,** with `registeredDatamodels` entries
  of `datamodelNameBytes`.
- **Event names are split** into core's event-name segment buffer, sized by
  `eventNameDepth`. napi's `TNA_EVENT_NAME_MAX_DEPTH` is removed.

Buffers napi does not size per system stay dynamic, and are bounded by
their quotas: mailboxes by `maxQueuedMessages` and `maxRetainedBytes`, and
the scheduler's pages by `maxActors`.

### 3. The parameters are visible where they matter

- **The OTLP resource carries them** as `tinyactors.param.<name>`
  attributes, in every request, so a trace says which caps applied. A cut
  value is still marked as cut, as today.
- **`inspectSystem()`** reports them.

### 4. Defaults

Defaults are enumerators in napi's C (`TNA_PARAM_DEFAULT_*`), exposed
through `parameterInfo()`. `index.d.ts` states each default and bound in
the comment above its property.

## Consequences

- **granary sets `parameters: { traceValueBytes: 16384 }`,** and its payloads
  arrive whole in traces.
- **Behaviour changes:**
  - options move into `parameters`;
  - the trace value limit's default rises from 256 characters to 1024 bytes;
  - the limit is counted in bytes of the rendered JSON, not in characters.
- **Costs:**
  - per system with the defaults: napi's own buffers take about 1 MiB of
    staging plus 2 KiB of value buffers, on top of core's arena (core ADR
    0092);
  - napi's test suites create many systems, so they pass a smaller
    `interpreterBytes` and `traceStagingBytes`;
  - the implementation measures RSS per system with the defaults, before
    and after.

## Implementation notes

None yet.
