# Definition lifecycle

1. Create a builder or importer IR.
2. Add hierarchy, initial states/transitions, transitions, actions, data/invocation metadata, and event patterns.
3. Assign a logical family and exact opaque revision fingerprint.
4. Compile/validate into an immutable `ta_definition`.
5. Prepare the selected data model once for the compiled definition.
6. Register the definition in a `ta_system` and obtain a compact runtime handle.
7. Spawn actors referencing that exact revision.
8. Keep the definition immutable while any resident actor or session image references it.
9. Unregister/free only when no actor/image can require the revision.

Multiple revisions of one family may coexist. Existing session images reconstruct against their exact revision; new actors can use a newer revision; logical named actors upgrade by replacement rather than by interpreting old definition-local IDs under a new chart.
