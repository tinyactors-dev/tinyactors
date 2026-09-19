# ADR 0042: Definitions are inspectable and export to their spec

**Status:** Accepted  
**Date:** 2026-09-19

## Context

A registered definition is opaque. The public API answers only per-state
questions (`ta_definition_state()`), and the definition's identity is visible
only through a live actor (`ta_actor_inspection.definition`). Several consumers
need more:

- **Hosts that move values across a serialization boundary** (a JSON-RPC codec,
  an invoker) must know which data-model adapter instance a definition was
  compiled with, so they can encode and decode values for it. Two adapters
  never share value representations, so a host cannot guess.
- **Persistence and replacement** must compare exact revisions (ADR 0028,
  ADR 0038) before an actor exists, and must find the registered definition
  for a stored identity.
- **Operators** need memory facts per definition and per actor (ADR 0018).
- **Verification, debugging, and visualization** (ADR 0020, ADR 0021) need
  the structure: initial states, transitions with their event descriptors,
  conditions and targets, invocations, and done data. A fuzzer needs the event
  descriptors to generate relevant events.
- **Hosts** want to check before spawning that every literal I/O processor and
  invoker type the chart names is registered, rather than discovering a gap
  as `error.execution` in the middle of a session.

Exposing the structure through one query function per element (transition
info, initial info, invoke info, descriptor formatting, ...) would roughly
double the public header, each consumer would still need a different slice,
and executable content would remain unreachable.

Separately, `ta_actor_inspect()` always calls the data model's `inspect`
operation. A host that only needs an actor's definition pays for building the
whole data-model root, and inspection has adapter side effects.

## Decision

1. **`ta_definition_inspect()`** returns fixed-size facts about a registered
   definition: its runtime ID, exact identity, `<scxml name>`, the data-model
   adapter instance it was compiled with (to identify, never to call), its
   binding mode, state count, lifecycle facts (ADR 0043), and memory
   measurements. Counts appear only for lists the API can read.
2. **`ta_definition_export()`** rebuilds the author-level `ta_scxml_spec` of a
   registered definition into a caller-supplied `ta_arena`. The input types
   already describe the whole chart, so export adds no structural types.
   Export is faithful: compiling an exported spec without an explicit identity
   yields the same fingerprint as the original. Where the compiler normalizes
   authored text, the definition retains the authored text.
3. **`ta_system_find_definition()`** resolves an exact identity to the
   registered, non-retired definition.
4. **Actor inspection is cheap and has no adapter side effects.** The
   data-model root moves out of `ta_actor_inspection` into
   `ta_actor_datamodel_root()`. Actor inspection gains `allocated_bytes`.
5. **Memory facts are measurements, not layout.** They may change between
   releases without breaking callers; configuration and history layouts stay
   private.

This is a C-struct export. SCXML XML import/export stays optional and outside
the core (ADR 0025); an XML exporter can be built on top of this one.

## Consequences

- Hosts can route values by the receiving definition's adapter without a
  host-side registry.
- One property test (export, recompile, compare fingerprints) checks the
  exporter against every chart the test suite compiles.
- Definitions keep a little more authored text (for example literal delay and
  event descriptor text). This is definition-level memory, shared by every
  actor (ADR 0018).
- `ta_actor_inspection` changes shape. Callers that read `datamodel_root`
  switch to `ta_actor_datamodel_root()`.
- Exported expressions are borrowed from the definition; host expressions stay
  valid only while the definition is alive.

See [definition inspection](../internal/definition-inspection.md) for the API
design.
