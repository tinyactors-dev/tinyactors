# `definition.h`

A definition is immutable runtime program data.

It owns or references:

- state hierarchy and kinds;
- document/build order;
- transition source/target sets;
- event patterns;
- conditions;
- executable action programs;
- initial/default entry metadata;
- completion event IDs;
- history layout;
- configuration layout;
- data-model implementation and prepared shared state.

The compiled IR includes initial-transition executable content, transition type
(`internal`/`external`), entry/exit handler ranges (one action block per
`<onentry>`/`<onexit>` handler), invocation declarations,
late/early data binding metadata, executable-content opcodes, and final-state
done-data declarations.

Definitions should be compiled/validated before registration and never mutated while actors or session images reference them.

Where each element was written is not program data: a definition compiled
from a spec with source spans keeps them in a side table beside the program
(ADR 0086), one span per state, transition, action and expression, and
`ta_definition_span()` reads it. Nothing at run time looks at it, and it is
not part of the revision.

A registered definition is inspectable (ADR 0042): `ta_definition_inspect()`
reports its ID, identity, `<scxml name>`, data-model adapter, binding mode,
state count, and lifecycle and memory facts, and `ta_system_find_definition()`
resolves an exact identity to a registered definition. The only mutable fields
are `actor_count`, a resident-actor counter that `ta_actor_spawn()` increments
and actor teardown decrements, and `retired`, which
`ta_system_retire_definition()` sets (ADR 0043); they are bookkeeping, not
program data. A retired definition is freed when `actor_count` reaches zero:
`ta_actor_free()` uncounts the actor as its last step, after the data-model
instance, session storage, invocations, and the actor record are released, and
frees the definition there when it is retired. See
[definition inspection](definition-inspection.md).

The system keeps registered definitions in a slot table
(`ta_definition_slot`: definition, generation, free-list link). An ID is
`(generation << 32) | slot`; freeing a definition bumps its slot's generation
and pushes the slot on the free list, so `ta_definition_lookup()` rejects stale
IDs. Because slots are reused, each definition also records a registration
serial, and `ta_system_find_definition()` picks the newest match by serial.

Each compiled definition owns a `ta_definition_identity`: its UTF-8 logical
family and an opaque exact revision fingerprint. `ta_definition_builder_set_identity()`
supplies it before compilation and must use the same family name as the
builder. The compiled definition copies/owns the revision bytes.

Any change capable of altering interpretation of definition-local session
state must produce a new revision. Multiple revisions of one family may remain
registered simultaneously. Existing session images reconstruct against their
exact revision; applications upgrade named actors by creating a new interpreter
under the target revision and replacing the logical directory entry.
