# Definition lifecycle

1. Create a builder or importer IR.
2. Add hierarchy, initial states/transitions, transitions, actions, data/invocation metadata, and event patterns.
3. Compile/validate into an immutable `ta_definition`.
4. Prepare the selected data model once for the compiled definition.
5. Register the definition in a `ta_system` and obtain a compact runtime handle.
6. Spawn/load actors referencing the definition.
7. Keep the definition immutable while any actor or snapshot resolution can reference its revision.
8. Unregister/free only when no actor/session/store record requires it, or through an explicit migration/versioning mechanism.

The exact stable revision identity is not yet fixed. A future serialized definition format should make revision/hash compatibility explicit rather than relying on runtime `ta_definition_id`.
