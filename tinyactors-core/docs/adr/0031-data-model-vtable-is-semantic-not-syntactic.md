# ADR 0031: Data-model vtable is semantic, not syntactic

- Status: Accepted
- Date: 2026-09-15

## Context

SCXML defines data manipulation using syntax such as `<data>`, `<assign>`, `<foreach>`, `<content>`, `<param>`, `<script>`, value expressions, location expressions and system variables.

Tinyactors does not use XML as its native definition format. Definitions are built programmatically and compiled into an immutable IR. Mirroring every XML element in the C vtable would couple the runtime ABI to one serialization syntax and make non-XML frontends unnecessarily awkward.

At the same time, the current data-model surface is too small. `eval_condition`, `eval_value`, `read_location`, `assign`, and `execute_script` do not by themselves express all required runtime semantics.

## Decision

The data-model ABI exposes semantic operations required by compiled IR instructions, not XML-shaped operations.

The semantic families are:

1. definition preparation and teardown;
2. instance allocation, initialization and teardown;
3. declaration of named data bindings;
4. value initialization and replacement;
5. conditional, value and location evaluation;
6. literal/content import into legal data-model values;
7. script execution;
8. stable shallow-copy iteration;
9. binding iteration variables;
10. construction of a payload from named parameters;
11. event-data import and system-variable access;
12. snapshot and restore.

The core owns the SCXML-specific control rules around these operations, including when they are called and what error event is generated when an operation fails.

## Consequences

A future XML importer translates `<data>`, `<assign>`, `<foreach>`, `<content>`, `<param>`, and `<script>` into the same IR used by a native C builder.

A data-model plugin does not need to know whether an operation originated from XML, a C builder, generated code, or another frontend.

Some plugins can deliberately implement only a subset. The Null data model, for example, has no locations, values, scripting, or foreach support beyond `In(state)` conditions.
