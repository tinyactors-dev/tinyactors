# ADR 0080: Definition events are lifecycle records too

**Status:** Accepted; not yet implemented. Refines ADR 0067 (three lifecycle record kinds, and a field on the record) and ADR 0079 (the same events, also in the trace stream).
**Date:** 2026-09-26

## Context

ADR 0079 gave core a definition sink: the host hears when a definition
is registered, retired or freed, whatever the trace mask or sampling. It
exists for cleanup. A host that keeps a function, a cache entry or a
watch per definition must hear of every free.

A host that **follows** core, such as a standby replaying a record stream
or a UI showing a live system, reads one ordered stream: the trace
records (ADR 0067), counted by a cursor. The definitions of a system are
a set it can page through (ADR 0078), but nothing in that stream says a
definition came or went. A follower then has two streams, the trace
records and the definition sink, with no order between them. It can't
tell whether "definition 7 registered" came before the first `SPAWNED`
that uses definition 7, or whether "freed" came after its last
`DESTROYED`. It has to merge two streams, and it gets that wrong exactly
where a definition is registered in the middle of a run: a child chart
compiled when an `<invoke>` runs (ADR 0077).

Actor lifecycle records already solve the same problem for actors: they
are records outside macrosteps, never sampled, selected by the mask like
every other kind.

## Decision

### 1. Three lifecycle record kinds

`ta_trace_kind` gains `TA_TRACE_DEFINITION_REGISTERED`,
`TA_TRACE_DEFINITION_RETIRED` and `TA_TRACE_DEFINITION_FREED`, after the
existing kinds, so no kind's number changes.

- **They are lifecycle records:** outside macrosteps, never sampled, and
  selected by the sink's mask like every kind (ADR 0067 section 2, and
  ADR 0073 section 4 for lifecycle records). They have no trace context,
  so their bit is looked up in `kinds`. A sink that doesn't ask for them
  builds nothing, and the null sink costs one mask test.
- **They are written at exactly the moments the definition sink fires**
  (ADR 0079 section 2), from the same call:
  - REGISTERED for a new registration, including the nested children an
    import registers itself. A registration answered by an existing
    definition with the same identity writes nothing;
  - RETIRED the first time a definition is retired;
  - FREED before the definition's storage goes and before its ID goes
    stale: in a cascade leaves first, and at `ta_system_destroy()` for
    every definition still alive.
- **The record carries** `definition`, the ID, and `identity`, its family
  and revision, borrowed for the call like everything in a record. It
  also carries `now`. `ta_trace_record` gains the `identity` field; the
  other kinds leave it empty.
- **A nested registration names no parent.** A child definition is
  registered before `ta_definition_nest()` makes it a child, and it can
  have several parents. A follower learns nesting from the definitions
  themselves (`ta_definition_invoke().nested`, and `holders` in
  `ta_definition_inspect()`, ADR 0078).

### 2. Keys and span IDs

A definition record has no session, so its key uses a session no actor
has: `TA_TRACE_DEFINITIONS_SESSION`, which is `UINT64_MAX`. A session ID
is the runtime ID an actor started with (ADR 0066), and no runtime ID
has slot `UINT32_MAX` (ADR 0059), so no actor's session is ever
`UINT64_MAX`.

- **`macrostep` is the definition ID, and `microstep` is 1, 2 or 3** for
  registered, retired and freed.
- **Keys never repeat within a system:** definition IDs are generational
  and never issued twice (ADR 0043), and each event happens at most once
  to one definition.
- **They never equal an actor's key,** whose session is an actor's.
- **`ta_step_key_span_id()` derives the span ID unchanged,** as for every
  key, so hosts need no new rule. The key's session is not 0, so the
  span ID is the usual mix and not the pass-through reserved for a host's
  parent span ID (ADR 0067 section 6).

### 3. Why both the sink and the records

They serve different readers, and neither can do the other's job.

- **The sink is for cleanup.** It fires whatever the mask, it can't be
  sampled away, and it runs after the record, so a trace sink reading
  the definition finds it as intact as the definition sink does.
- **The records are for following.** They sit in the one ordered stream
  a follower already reads, in order with every other record.

A host that only wants cleanup installs the sink and leaves the kinds out
of its mask. A follower asks for the kinds.

### 4. The ordering guarantee

All in the order of the trace stream, meaning the order in which the
system's one trace sink receives records:

1. **Before the definition sink:** each definition record is written
   immediately before the definition sink is told of the same event, from
   the same call.
2. **REGISTERED before any record that names the definition.** A
   definition is registered before an actor can be spawned from it, so its
   REGISTERED comes before the first `SPAWNED` of that definition, and
   before any record of a macrostep of such an actor.
3. **A definition registered mid-run.** A child chart compiled when an
   `<invoke>` runs is registered inside the invoking actor's macrostep. Its
   REGISTERED comes between that macrostep's records: after the records of
   steps that committed before the invoke started, and before the child's
   `SPAWNED`, and so before any record of the child. It carries no key of
   the invoking macrostep; its key is the definition's own.
4. **FREED after the records that use the definition.** A definition is
   freed only once no actor of it is resident, and core frees it from the
   call that removes its last actor, after that actor's `DESTROYED` (or
   `UNLOADED`) record. So FREED follows the last lifecycle record of the
   last actor that used it, and every record of that actor.
5. **Cascades:** the FREED records of a cascade come in the order the sink
   hears them, leaves first (ADR 0079 section 2).
6. **System destroy:** every actor is destroyed first, with its
   `DESTROYED` record, and then FREED is written for every definition still
   alive, in slot order. So all of destroy's FREED records come after all
   of its `DESTROYED` records.
7. **RETIRED** comes from the call that retires the definition, so it is
   ordered with the records of whatever the host did before and after that
   call. It may come before FREED immediately, when nothing else used the
   definition.

### 5. Relation to ADR 0070 (retries)

A definition event is a real event, not a step's record, so ADR 0067's
rule that a record is written once, when its step commits, doesn't
apply to it. If an invoke registers a child chart and then stops for lack
of memory before spawning the child, and the definition, held by nothing,
is freed, the stream shows REGISTERED and then FREED. The retry registers
it again and writes a second REGISTERED, under a new ID. What the
records describe did happen.

## Consequences

- A follower reads one stream and can build the set of live definitions
  from it, in order with the actors that use them. Starting from a
  snapshot, it pages through `ta_system_definitions()` (ADR 0078), then
  follows the records.
- The record grows by one `ta_definition_identity`. Records are built on
  the stack, per record, only when a sink asks for their kind.
- Nothing changes for a sink that doesn't ask for the kinds, including
  napi's sink by default.
- Hosts that keep a table of kind names, such as napi and the test host,
  add three names.

## Testing

- **Core:**
  - a stream that interleaves both sinks shows each record just before
    the sink's event;
  - REGISTERED before `SPAWNED`, and FREED after `DESTROYED`;
  - identity reuse writes nothing;
  - a cascade leaves first;
  - destroy frees after every `DESTROYED`;
  - a mask without the kinds writes none;
  - the keys are unique, and none equals an actor's.
- **Test host:** `trace.recording` journals the records, and the
  definition-event scenarios assert their order against actor records on
  the shared `seq` counter.
- **napi:** the kinds are in `TraceKind`, and asking for them doesn't
  double-report in OTLP output (napi ADR 0017's definition logs stay the
  one OTLP path).
