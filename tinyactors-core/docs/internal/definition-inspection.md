# Definition inspection, export, and retirement

Design for ADR 0042 (definitions are inspectable and export to their spec) and
ADR 0043 (definitions retire; definition IDs are generational). This document
fixes the public API and its semantics and splits the work into packages that
can land as separate commits.

## Who reads a definition, and for what

| Consumer | Question | API |
|---|---|---|
| Value codec, invoker | Which adapter instance encodes and decodes this chart's values? Do parent and child use the same data model? | `ta_definition_inspect().datamodel` |
| Persistence, replacement | Is this the exact revision? Which registered definition has this identity? | `.identity`, `ta_system_find_definition()` |
| Debugger, harness | What is the chart called? When does its data exist? | `.document_name`, `.binding` |
| Debugger, visualizer, fuzzer, model checker | States, initial states, transitions (event descriptors, conditions, targets, type), handlers, invocations, done data | `ta_definition_export()` |
| Host before spawning | Are the literal I/O processor and invoker types this chart names registered? | `ta_definition_export()` (literal `type` values; `typeexpr` stays a runtime check) |
| Operator, host GC | Is it retired? How many actors use it? What does it cost? | `.retired`, `.actor_count`, `.actor_fixed_bytes`, `.definition_bytes` |
| Debugger | Where was a state, transition, action or expression written? Where did an encode write it? | `ta_definition_span()`, `ta_definition_encode_params.spans` (ADR 0085) |
| Operator | Which actors are large right now? | `ta_actor_inspection.allocated_bytes` |
| Hot paths (`In()`, harness state names) | One state's ID, parent, kind, without an arena | `ta_definition_state()` (unchanged) |

## Public API

All functions follow the three-parameter rule; larger inputs use a params
struct.

### Definition inspection

```c
/*
 * A read-only view of one registered definition. Text and the adapter are
 * borrowed from the system and stay valid while the definition exists
 * (a retired definition exists until its last actor is gone).
 */
typedef struct {
    ta_definition_id id;
    /* The exact revision (ADR 0028, ADR 0038). */
    ta_definition_identity identity;
    /* The <scxml name> attribute, which _name reads; empty when absent. */
    ta_str document_name;
    /*
     * The adapter instance the definition was compiled with. `self`
     * identifies the instance and `v->name` the SCXML data-model name.
     * Use it to identify the adapter; never call its operations.
     */
    ta_datamodel datamodel;
    ta_scxml_binding_kind binding;
    /* States readable with ta_definition_state(). */
    uint32_t state_count;
    /* True after ta_system_retire_definition(). */
    bool retired;
    /* Resident actors using this definition, unpublished replacements included. */
    uint32_t actor_count;
    /*
     * Bytes the core allocates for each actor of this definition before it
     * processes events: the actor record, session storage (configuration and
     * history), and the data-model instance. Excludes queues, the sparse
     * state of invoked actors, and memory the data-model adapter allocates
     * itself. A measurement: it may change between releases.
     */
    size_t actor_fixed_bytes;
    /* Bytes of the definition's own storage (its arena). A measurement. */
    size_t definition_bytes;
} ta_definition_inspection;

TA_API ta_result ta_definition_inspect(const ta_system *system,
                                       ta_definition_id definition,
                                       ta_definition_inspection *out);
```

`TA_ERR_NOT_FOUND`: the ID names no existing definition (never registered,
freed after retirement, or a stale generation).

### Export

```c
/*
 * Rebuilds the author-level spec of a registered definition. Arrays are
 * allocated in `arena`; text and expressions are borrowed from the
 * definition and the system. The result stays valid until the arena is
 * reset or the definition is freed, whichever comes first.
 */
typedef struct {
    ta_definition_id definition;
    ta_arena *arena;
    const ta_scxml_spec **out;
} ta_definition_export_params;
TA_API ta_result ta_definition_export(const ta_system *system,
                                      const ta_definition_export_params *params);
```

**Faithfulness invariant.** For a definition compiled from a spec without an
explicit `identity`, compiling the exported spec (again without an identity)
produces the same revision fingerprint. The compiler hashes its input in
`builder.c` (`ta_compiler_hash*`). The rule that makes the invariant hold:

> Every value the compiler hashes must be recoverable exactly from the
> compiled definition.

Where the compiler normalizes or discards authored input, the definition keeps
the authored form. Candidates to check (walk every `ta_compiler_hash*` call to
find all of them):

- literal `delay` text (compiled to milliseconds);
- event descriptor text of transitions (compiled to patterns);
- state IDs the author left empty (if the compiler generates names, export
  must emit the empty ID);
- the shape of initial states (`initial` attribute vs `<initial>` element), if
  the compiler merges them;
- literal content and targets (already bytes and atoms; check);
- the identity's family (`spec->name`) and the explicit `identity` pointer:
  export emits `identity = NULL` when the definition was fingerprinted, and
  the explicit identity otherwise.

Sentinel-terminated arrays, handler lists, and action lists are rebuilt in the
exact document order the compiler recorded.

What the definition keeps for export (`src/definition.h`, filled by
`builder.c`, read by `src/export.c`), from a walk of every `ta_compiler_hash*`
input:

| Hashed input | Compiled form | Kept |
|---|---|---|
| literal `delay` text | milliseconds | `ta_delay_source.text` into `ta_definition.authored_texts` |
| transition `event` text | event patterns (whitespace, `.*`, trailing `.*` normalized) | `ta_transition_def.event_text` into `authored_texts` |
| `<scxml binding>` | early/late (`DEFAULT` and `EARLY` merge) | `ta_definition.authored_binding` |
| initial targets written by the author vs. the SCXML default (first child) | same targets | `ta_initial_transition_def.authored_targets` |
| explicit `identity` vs. fingerprint | identity | `ta_definition.fingerprinted` |
| `<scxml datamodel>` (not hashed; checked against the adapter) | nothing | `ta_definition.authored_datamodel` |

Everything else is recoverable as compiled: state IDs (the compiler rejects
empty IDs, so none are generated), targets, literal IDs, types, names and
labels (atoms), raise and send event names (dotted names without empty
segments, joined again in the export arena), literal content and literal
`target`/`src` (content bytes), expressions (the definition's table), and
list shapes. The compiler rejects `<invoke>` as executable content, and
core has no extension action at all (ADR 0046), so export never meets
either.

The unit test `tests/export_test.c` and the harness check
`tinyactors-test/ruby/export_roundtrip_check.rb` exercise the invariant.

Host expressions (`TA_EXPRESSION_HOST`) are exported as the retained handles,
borrowed; they are not retained again.

### Where elements were written (ADR 0085)

A spec may say where each element was written: `ta_scxml_spec.spans` is a
table of `ta_source_span`s (offset, length, source, start and end line and
column), `sources` names what they point into, and each state, transition,
initial transition, action and expression has a `span` that indexes the
table from 1 (the root `<script>` element's is `script_span`). The compiler
checks each index and source, hashes none of it, and, when at least one span
is known, lays out a side table in one allocation the definition owns and
frees (`ta_definition.spans`, `span_bytes`, counted in `definition_bytes`):
one span per element, by kind and then by ID, as
`ta_definition_span_base()` numbers them, with the source names beside it
(`ta_definition.sources`). A definition compiled without known spans keeps
nothing. A registration answered by identity (ADR 0077) that brings other
spans swaps its table with the live definition's
(`ta_definition_take_spans()`), so the live definition says where the chart
is written now, and the old table is freed with the new compile.

```c
TA_API ta_result ta_definition_span(const ta_system *system,
                                    const ta_definition_span_params *params);
```

answers where element `id` of `kind` (`TA_ELEMENT_STATE`, `_TRANSITION`,
`_ACTION`, `_EXPRESSION`) was written, with its source's name;
`TA_ERR_NOT_FOUND` for an unknown or stale definition, an ID beyond the
counts in `ta_definition_inspection`, a definition without spans, or an
element without one.

Export borrows the side table as the exported spec's `spans`, sets `span` on
every element whose span is known to its place in the table plus one, and
borrows the source names. Compiling the export keeps the spans, and
exporting that again gives the same table. `ta_definition_encode()` with
`spans` asks the export to number every element that way (`numbered` in
`ta_export_request`, `src/export.h`), even with no side table, gives the
codec a `ta_chart_span_sink` that files each report at its number, and
returns the result as a `ta_source_map` by kind and ID.

### Finding a definition

```c
/* The registered, non-retired definition with exactly this identity. */
TA_API ta_result ta_system_find_definition(const ta_system *system,
                                           const ta_definition_identity *identity,
                                           ta_definition_id *out);
```

Family and revision are compared bytewise. `TA_ERR_NOT_FOUND` when none
matches. If several registered definitions share one identity, the most
recently registered wins.

### Actor inspection

- `ta_actor_inspection` loses `has_datamodel_root` and `datamodel_root`, and
  gains `size_t allocated_bytes`: everything the core currently allocates for
  the actor (record, session storage, data-model instance, sparse state, queue
  capacities). A measurement.
- New:

```c
/* The data model's inspection root (its `inspect` operation). Release it
 * with ta_value_release(). TA_ERR_UNSUPPORTED when the adapter has none. */
TA_API ta_result ta_actor_datamodel_root(ta_system *system, ta_actor_id actor, ta_value *out);
```

`ta_actor_inspect()` and `ta_system_visit_actors()` no longer call into the
adapter.

### Retirement

```c
/* Idempotent. See ADR 0043. */
TA_API ta_result ta_system_retire_definition(ta_system *system, ta_definition_id definition);
```

Semantics:

- `ta_actor_spawn()` and `ta_actor_prepare_replacement()` naming a retired
  definition fail with `TA_ERR_NOT_FOUND`. `ta_system_find_definition()` skips
  it.
- Resident actors keep running. `ta_definition_inspect()`,
  `ta_definition_state()`, and `ta_definition_export()` keep working while the
  definition exists.
- The definition is freed (data model `destroy_definition`, arena, slot) when
  it is retired and `actor_count` is zero: immediately in
  `ta_system_retire_definition()`, or when the last actor is destroyed. The
  actor must be fully torn down before the definition it points to is freed.
- `ta_system_destroy()` frees every definition, retired or not.
- Retiring is idempotent also after the definition is freed: an ID whose slot
  has since advanced its generation was issued and freed, so retiring it
  returns `TA_OK`. An ID that was never issued returns `TA_ERR_NOT_FOUND`.
- `ta_actor_spawn()` returns `TA_ERR_NOT_FOUND` (not `TA_ERR_INVALID`) for an
  unknown, stale, or retired ID, and `ta_definition_state()` returns
  `TA_ERR_NOT_FOUND` for an unknown or stale ID, like every other entry point.
- Implementation: `ta_actor_free()` releases the data-model instance, session
  storage, actor state (queues, invocations), and the actor record first, then
  uncounts the actor and, when the definition is retired and unused, calls
  `ta_system_free_definition()`. Because slots are reused, each definition
  carries a registration serial so `ta_system_find_definition()` still returns
  the most recently registered match.

### Generational definition IDs

- `typedef uint64_t ta_definition_id;` The low 32 bits are the slot, the high
  32 bits its generation, as for `ta_actor_id` (`actor.c`).
- The system's definition table stores `{definition, generation}` per slot and
  a free list. Freeing a definition bumps the slot's generation and returns
  the slot to the free list.
- `ta_definition_lookup()` checks the generation; every public entry point that
  takes a `ta_definition_id` goes through it.
- Session images keep storing the identity, never the ID.

## Bindings and the test host

- `mise run api` and `mise run bindings` regenerate the API description and the
  JSON-RPC methods. The new functions become RPC methods where their types
  have codecs. `ta_definition_export()` returns a pointer graph: the
  generated encoder needs encoders for the `ta_scxml_*` spec types (today only
  decoders exist for `compile_spec`), or the method is overridden by hand in
  the test host. Prefer generating the encoders.
- `ta_definition_id` travels as a 64-bit value (decimal string above 2^53-1).
- Callers of `datamodel_root` switch to `ta_actor_datamodel_root()`: the Ruby
  harness (`scenario.rb` `data`), `runner_selftest.rb`, `docs/protocol.md`,
  `docs/manual/inspection.md`, and `api/annotations.rb`.
- The value codec picks the receiving actor's adapter through
  `ta_actor_inspect()` → `definition_id` → `ta_definition_inspect()` →
  `datamodel` (this replaces the host-side definition map in the ECMAScript
  data-model design).
- The SCXML invoker keeps its cache of compiled child definitions; a cached ID
  that returns `TA_ERR_NOT_FOUND` is recompiled.

## Work packages

Each package is one or more commits. After each: `mise run amalgamate`,
`mise run api`, `mise run bindings`, `mise run test`, and
`mise run scenarios` all pass.

1. **Inspection.** `ta_definition_inspect()` (without `retired` and
   `actor_count`, which package 3 adds; report `false` and a correct count if
   the counter is cheap to add now), `ta_system_find_definition()`, the actor
   inspection change with `ta_actor_datamodel_root()` and `allocated_bytes`,
   `definition_bytes` (needs an arena size helper), and all caller updates.
   Core unit tests in `tinyactors-core/tests`.
2. **Export.** `ta_definition_export()`, authored-text retention, spec
   encoders in the bindings, and the round-trip property test: for every chart
   the test suite compiles (unit-test charts and every `scxml_irp` chart),
   export, recompile, and compare fingerprints. Depends on nothing in
   package 1 except the header section it edits.
3. **Retirement and generational IDs.** `ta_definition_id` to 64 bits,
   the slot table with generations and a free list, `actor_count`,
   `ta_system_retire_definition()`, freeing on the last actor, and tests for
   stale IDs, retire-with-actors, retire-without-actors, spawn after retire,
   find after retire, and system destroy with retired definitions.
4. **Consumers.** The ECMAScript data model's value codec uses package 1.

## Documentation to update

- `docs/internal/definition-lifecycle.md` (steps 8 and 9: retirement),
  the file comment of `src/definition.h`, `docs/internal/public-api.md`,
  `docs/manual/inspection.md`.
- ADR 0028's status notes the refinement by ADR 0043.
