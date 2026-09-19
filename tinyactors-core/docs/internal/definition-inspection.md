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

Host expressions (`TA_EXPRESSION_HOST`) are exported as the retained handles,
borrowed; they are not retained again.

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
  `docs/internal/definition.md`, `docs/internal/public-api.md`,
  `docs/manual/inspection.md`, `docs/internal/api-gaps.md` if it lists these.
- ADR 0028's status notes the refinement by ADR 0043.
