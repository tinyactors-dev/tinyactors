# ADR 0036: Parameters hold on every path

**Status:** Proposed (2026-09-29). Refines napi ADR 0035 (a system is created with its parameters) on four points found while implementing it; core ADR 0093 settles the fifth, the event-block cache.
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

None yet.
