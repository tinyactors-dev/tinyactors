# ADR 0035: A system is created with its parameters

**Status:** Accepted and implemented (2026-09-29); see the implementation notes. Refined by ADR 0036: the import path sizes its refusal by `compileTextBytes`, the staging bound holds outside turns and staging lives with the sink, `traceValueBytes` is `valueBytes`, and the parameters travel as one `tinyactors.params` attribute. Applies core ADR 0092 (system parameters). Amends:
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

- **Where:** `src/params.{c,h}` (napi's parameter table and defaults, as
  enumerators), `src/napi_system.c` (reading `parameters`, the
  `parameterTable` export, the trace value buffer), `src/system.{c,h}` (the
  budgets and mailbox limits from the parameters, the turn's stop at the
  staging bound), `src/pump.{c,h}` (`tna_pump_core_params` and the
  scheduling options are gone), `src/scheduler.{c,h}` (`stop` on
  `tna_turn_params`), `src/otlp.{c,h}` (the staging kept is the parameter;
  a cut value is marked), `src/napi_trace.c`, `src/napi_datamodel.{c,h}`,
  `src/napi_children.c`, `src/events.{c,h}`, `lib/params.js` (validation
  and `parameterInfo()`), `lib/system.js`, `index.js`, `index.d.ts`,
  `README.md`, `docs/tracing.md`. Commits `0170abe`, `b5666f4`, `c68d9c7`,
  `6222b6f`, and the one with these notes; core's `e5de36e` (interpreterBytes
  64 KiB) and `807b744` (compileTextBytes) came with them.
- **Validation happens in three places, and none copies another's bounds.**
  `lib/params.js` checks names and bounds against the addon's table
  (`parameterTable()`, which reads core's rows through `ta_param_info_of()`
  and napi's from `src/params.c`), and throws `invalid` naming the parameter
  and its bounds. The native side checks napi's again, and core checks its
  own and names the one it refuses (`rejected`).
- **`logValueBytes` is not a parameter.** `setTraceSink`'s `logLimit` turned
  out to be a rate (log records per definition and name per second), which
  is the sink's configuration, so it stays there. A log record's value
  goes through the same buffer as a trace value, so `traceValueBytes`
  bounds both.
- **`eventBlockCache` follows `messageBudget` unless the application sets
  it**, not core's default of 4. A turn holds up to `messageBudget` event
  blocks at once. `test/scale.js` and `test/allocations.js` check that a
  warm system allocates nothing per message, and with 4 cached blocks every
  default system allocated one per delivery past the fourth (16,320 core
  allocator calls in scale.js's steady state). The price is the prefill:
  1,024 blocks of 792 bytes, about 811 KB a system at the default budget. An
  application that runs many small systems lowers `messageBudget` or sets
  `eventBlockCache`.
- **The staging bound is on turns.** After each resume and delivery the
  scheduler asks whether the turn has staged `traceStagingBytes` of spans
  and logs (`tna_otlp_traces_size` and `_logs_size`, both O(1)); if so the
  turn ends there, and the rest waits for the next turn. A macrostep's
  records are never split, so a request can pass the bound by one
  macrostep's worth. `spawn()` writes its lifecycle span when it is called,
  outside any turn, so a burst of spawns is not bounded by it (the first
  request after 3,000 spawns was 648 KB at a 64 KiB bound; every request
  after it was about 66 KB). The staging buffers are not allocated at
  creation: they grow to the bound on first use and are kept, so a system
  that never traces reserves none.
- **A cut value is marked.** `scxml.event.data.truncated` and
  `scxml.value.truncated` (booleans) are new. A fault's reason and a log
  record's body are cut without a mark. The limit counts bytes of the JSON's
  UTF-8, as the old `valueLimit` did, and its default rose from 256 to 1,024.
- **`inspectSystem()` does not exist.** The parameters are reported by
  `System.parameters` (frozen) and by `stats().parameters`.
- **Resource attributes are strings,** because `resource` takes strings
  only: `tinyactors.param.traceValueBytes` is `"1024"`. An attribute the
  application names itself wins. They make each request about 900 bytes
  larger, so `test/tracing.js`'s small-request case now allows 4,000 bytes,
  still within Bun's pooled Buffers.
- **The data model table** (`registeredDatamodels` adapters, their names of
  `datamodelNameBytes`, the choices array, and the refusal of core's
  `compileMessageBytes`) is allocated when the data model is set up, and
  freed after core, whose definitions point at the adapters. Names are read
  into stack buffers of the parameter's ceiling (256 bytes). The choices a
  compile is offered are one array the data model owns: `define()`, a
  child compiled at invoke time and `selectDatamodel()` never run inside
  one another.
- **The import refusal keeps 1,023 bytes** (`tna_import_failure.message`),
  whatever `compileTextBytes` allows.
- **Tests:** `test/tracing.js`: a 16 KiB payload arrives whole with
  `traceValueBytes: 16384` and is cut and marked at the default; a 16-byte
  bound cuts and marks; `valueLimit` is refused; a 64 KiB staging bound splits
  3,000 deliveries into many requests of under 128 KiB with every step
  traced; the resource names the parameters, the application's attribute
  wins, `stats().parameters` and a frozen `System.parameters`.
  `test/datamodel.js`: a raised `renderBytes` renders a 200,000-byte event
  name; `registeredDatamodels` 0, 2 and 20, and `datamodelNameBytes` 8.
  `test/types/usage.ts`, `test/version.js` (the export), and every suite
  that used `scheduling` now passes `parameters`.
- **Memory per default system** (200 systems, Node 22 and Bun 1.4.1, M4
  Max): native memory 1,105,080 bytes at `ce8f89e`, 908,472 now (the
  difference is exactly core's `interpreterBytes` from 256 KiB to 64 KiB);
  RSS about 1.05 MB before and after. Most of what is left is the event-block
  prefill above. (The `ce8f89e` build crashed on Bun in this probe, so there
  is no Bun figure for it.)
- **Benchmark** (`bench-compare ce8f89e`, 100,000 actors; Node machine
  a99e44a37e2e, Bun c989bd79a51f): with 2 runs six rows passed 5%. The
  flagged variants rerun with 4 runs: Node, nothing outside 5%
  (untraced 464 to 477 ns/event, +2.9%, in the 2-run pass). Bun: the traced
  variants' RSS per actor +7.7% to +11.9%, and `decoded` +9.4% ns/event.
  Bun's traced RSS varies from 1,437 to 1,622 bytes per actor between
  identical runs of one build, turning the parameter attributes off does not
  move it, and turning the staging split off raises it (about 1,840), so
  the flags are within Bun's spread; `decoded` decodes about 20 more
  resource attributes per request. Native bytes per actor fell 0.6%, and
  allocator calls per event stayed 0.
