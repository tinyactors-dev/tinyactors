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

The current skeleton omits some metadata required for full SCXML semantics, including explicit initial-transition executable content, transition type (`internal`/`external`), entry/exit action ranges, invocation declarations, late/early data binding metadata, and executable-content opcodes. These are implementation gaps, not intentional semantic omissions.

Definitions should be compiled/validated before registration and never mutated while actors reference them.
