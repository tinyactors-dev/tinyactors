# ADR 0036: Parameters hold on every path

**Status:** Accepted and implemented (2026-09-29); see the implementation notes. Refines napi ADR 0035 (a system is created with its parameters) on four points found while implementing it; core ADR 0093 settles the fifth, the event-block cache. Its code moved to the host kernel: see [kernel ADR 0001](../../../tinyactors-host/docs/adr/0001-the-host-kernel-holds-what-every-host-needs.md) section 6 and [ADR 0037](0037-napi-is-a-binding-over-the-host-kernel.md).
**Date:** 2026-09-29

## Context

Implementing napi ADR 0035 left four places where a parameter does not
mean what it says, or costs more than it should (ADR 0035's
implementation notes):
1. **The import path cuts a formatted compile error at 1,023 bytes,**
   whatever `compileTextBytes` (core ADR 0092) allows. It uses a fixed
   buffer in `src/import.c`.
2. **The trace staging bound (`traceStagingBytes`) does not cover
   `spawn()`.** Spawn writes its lifecycle span outside any turn, so a burst
   of 3,000 spawns produced one 648 KB request against a 64 KiB bound.
   Staging also grows to its bound on first use, instead of being allocated
   when it is set up.
3. **`logValueBytes` was not added.** `logLimit` is a rate, not a size. Log
   values share the buffer of `traceValueBytes`, so the name says less than
   the parameter does.
4. **Every OTLP request's resource carries about 20 `tinyactors.param.<name>`
   attributes,** about 900 bytes per request.

Each was decided in review, 2026-09-29, from two alternatives. This ADR
records the choice and why.

## Decision

### 1. The import path sizes its buffer from `compileTextBytes`

The refusal text from `define(text, { format })` is formatted into a
buffer of `compileTextBytes`, allocated with napi's other buffers at
creation. So a compile error reads the same, and is as long, on every path.

| | A: size it from the parameter (chosen) | B: keep the fixed buffer and document the cut |
|---|---|---|
| For | the parameter means what it says everywhere; a local change | nothing to change |
| Against | up to 64 KiB per system if an application asks for it, within the rules | one parameter behaving differently depending on the path, the inconsistency parameters exist to remove |

**Why A.** B is a bug against the parameter, documented rather than
fixed.

### 2. The staging bound is absolute, and staging exists while a sink does

- **Any record that would overflow staging makes napi hand the sink a
  request first.** That covers a turn's spans, and lifecycle records made
  outside a turn (spawn, destroy, load, unload). So no request is larger
  than `traceStagingBytes` plus one record.
- **The sink may therefore be called outside a turn,** synchronously, from
  inside the call that made the record. Its contract in `index.d.ts` says
  so. Nothing is dropped.
- **Staging is allocated whole when a trace sink is installed,** and freed
  when it is removed. A system with no trace sink reserves nothing.

| | A: absolute bound, allocated up front (chosen, refined) | B: queue records made outside a turn into the next turn, grow lazily |
|---|---|---|
| For | the bound holds on every path; memory is known once tracing starts | sinks are still called only at the end of a turn; nothing is reserved without tracing |
| Against | a sink can be called outside a turn; 1 MiB is reserved while tracing | the bound can still be exceeded between turns, so it is not a bound; lazy growth breaks ADR 0092's rule 3 |

**Why A, refined.** A bound that a burst of spawns can exceed is not a
bound. Allocating staging when the sink is installed, rather than when
the system is created, keeps the rule's intent. Memory is fixed from the
moment tracing is on, and a system without tracing pays nothing, so A's
1 MiB cost falls only on systems that trace.

### 3. One value limit, named `valueBytes`

`traceValueBytes` is renamed `valueBytes`. It is the size, in bytes of
rendered JSON, at which values are cut in trace attributes and in log
records alike. It keeps its default (1024) and bounds (at most 1 MiB).
`logLimit` stays a rate, in the sink's configuration.

| | A: add `logValueBytes` with its own buffer | B: one shared limit, named for what it is (chosen) |
|---|---|---|
| For | logs, often high-volume and shipped to paid storage, could be cut shorter than traces | honest and minimal; one number, matching the code |
| Against | a parameter and a buffer for a need nobody has stated | logs and traces cannot differ; splitting later is a rename plus a parameter |

**Why B.** Name what exists. Nothing is published, so splitting the limit
later is cheap if an application asks for it.

### 4. One compact attribute for the parameters

The resource of every OTLP request carries one string attribute,
`tinyactors.params`, instead of an attribute per parameter:

```
v1;valueBytes=16384;interpreterBytes=131072
```

- It lists only the parameters that differ from their defaults, in name
  order, after the version of the defaults (`v1`).
- A system with every default sends `v1`.
- The version changes when a default changes, so a reader knows which
  defaults the omitted parameters had. `parameterInfo()` and
  `System.parameters` still give every value to code in the process.

| | A: send the full set once, per sink, with a hash in later requests | B: one compact attribute per request (chosen) |
|---|---|---|
| For | nearly no cost per request | every request describes itself; tens of bytes, `v1` for a default system; still queryable |
| Against | a backend that samples, splits or expires requests may lose the only one with the set; a hash alone says nothing | one string to parse; a reader must know the defaults of that version |

**Why B.** The attributes exist so that a trace says which caps applied,
and every request must be able to say that on its own. B keeps that
property at a fraction of the cost.

## Consequences

- **`compileTextBytes`, `traceStagingBytes` and `valueBytes` mean what they
  say on every path.**
- **A trace sink may be called outside a turn.** Sinks that assumed
  otherwise must accept it. The OTLP tools in `tools/` and the tests are
  checked.
- **A system reserves trace staging only while it traces.**
- **`traceValueBytes` becomes `valueBytes`:** callers, tests and docs change.
- **An OTLP request shrinks by about 900 bytes,** or by less than that when
  parameters differ from their defaults.
- **Costs:**
  - per system without tracing: the staging buffer is no longer reserved;
  - with tracing: `traceStagingBytes`, reserved when the sink is installed;
  - per request: the attribute's length.

  The implementation measures request sizes and RSS per system, before and
  after.

## Implementation notes

- **Commits:** `ed53212` (§1), `8a31b00` and `e67dff0` (§2), `43331b1` (§3),
  `2174e61` (§4). Core ADR 0093, decided with this one, is `ee1db2c`.
- **§1, where:** `src/import.{c,h}` (`tna_import_failure.message` is a
  pointer and a size; `tna_import_failure_open` and `_reset`),
  `src/napi_children.{c,h}`, `src/napi_system.{c,h}` (one block of three
  buffers allocated at creation: the refusal, scratch to reframe a child's
  refusal, and the invoker's fault text, each `compileTextBytes` plus 256
  bytes of napi's framing). The invoker's fault text had the same 1,023-byte
  array and uses the same block. **Test:** `test/import.js`
  (`longRefusals`): a 1,000-byte unknown target arrives whole at
  `compileTextBytes` 16384 (1,093 bytes of text), for a document and for a
  child nested at define(), and is cut as core cuts it at the default.
- **§2, where:**
  - `src/runtime.c`: `tna_call_end` hands the sink a request when a call
    from JavaScript ends outside a turn with the staging at or past the
    bound;
  - `src/napi_trace.c` (`tna_trace_flush_if_full`,
    `tna_trace_after_method`);
  - the hand-written methods that run core outside a turn and do not use
    `tna_call` (`completeLoad`, `offload`, `loadOffloaded`, replay's
    `prepare` and `run`);
  - `src/otlp.c`: `tna_otlp_init` reserves the span staging whole and
    fails `setTraceSink` with `out-of-memory` when it cannot.

  The check runs at the end of each call, not per record, so a request can
  exceed the bound by one call's records. The largest measured was 64 KiB
  plus under 4 KiB for 3,000 spawns at a 64 KiB bound.
- **§2, a correction found by the benchmark (`e67dff0`).** Shrinking the
  staging back to the bound after every request reallocated it every turn
  under a steady traced load: each turn that stops at the bound overshoots
  it by one macrostep. That cost 0.03 allocator calls per event in `full +
  values`. It now shrinks only after a burst past four times a typical
  turn's use, the rule the staging followed before, but back to the bound
  instead of freed.
- **§2, test:** `test/tracing.js` (`stagingBoundOutsideTurns`):
  - no staging reserved without a sink;
  - a sink reserves the bound whole;
  - 3,000 spawns at 64 KiB call the sink while the loop runs, outside a
    turn;
  - no request past the bound plus 4 KiB;
  - all 3,000 spawned spans arrive;
  - removing the sink frees the staging.

  `tools/otlp.js` and the debugger take whatever batches arrive, so being
  called outside a turn needed no change there. `index.d.ts` and
  `docs/tracing.md` say the sink can be called outside a turn and what
  happens to an exception it throws.
- **§3:** a rename across `src/params.{c,h}`, `src/napi_system.{c,h}`,
  `src/napi_trace.c`, `lib/system.js`, `index.d.ts`, `README.md`,
  `docs/tracing.md` and the tests. `setTraceSink({ valueLimit })` still
  throws, now naming `valueBytes`.
- **§4, where:** `lib/params.js` (`DEFAULTS_VERSION` `v1`,
  `parameterAttribute`), `lib/system.js`. **Test:** `test/tracing.js`:
  - a default system sends `v1` and no per-parameter attributes;
  - changed parameters are listed in name order
    (`v1;interpreterBytes=131072;microstepLimit=777;valueBytes=4096`);
  - the application's own attribute wins;
  - `defaultsVersion()` records v1's defaults, and fails when a default
    changes without a new version.
- **§4, request size:** a default system's first request went from 2,441
  to 1,457 bytes (−984). The small-request test is back to its 1,000-byte
  bound.
- **Benchmark** (`bench-compare 5b46a4a`, 100,000 actors; Node machine
  a99e44a37e2e, Bun c989bd79a51f):
  - **Bun:** ns/event within 5% everywhere (untraced +0.4%). RSS per actor
    is down in most traced variants (−48% in `decoded`), and up 6% in
    `macrosteps`, within Bun's run-to-run spread noted in ADR 0035.
  - **Node:** the first 2-run pass flagged +6% to +16% ns/event in traced
    variants. A 4-run pass of the flagged variants read −0.4% to +1.5%, so
    it was noise. RSS per actor is down up to 63% (`decoded`), from the
    staging no longer growing on first use.
  - **`alloc/event` reads 0.01 where it read 0.00** in every variant. That
    is core ADR 0093's warm-up, counted from the first delivery; steady
    state is zero (ADR 0093's notes).
- **Memory per default system:** without a sink, native 97,464 bytes (core
  ADR 0093's notes). With a sink, plus the staging (1 MiB by default).
