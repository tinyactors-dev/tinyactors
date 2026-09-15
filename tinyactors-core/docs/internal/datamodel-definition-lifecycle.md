# Data-model definition lifecycle

A data-model implementation has two levels of state.

## Plugin-global state

`ta_datamodel.self` belongs to the embedder. Examples include a Python interpreter integration, a Ruby extension object, or a stateless fixed-slot implementation.

## Prepared definition state

`prepare_definition()` may compile or resolve all definition-level data-model artifacts once:

- condition expressions;
- value expressions;
- location expressions;
- scripts;
- data declaration metadata;
- literal decoders or schemas;
- fast slot/index mappings.

The returned `shared` object is immutable during actor execution and is shared by every actor using the definition.

This is essential for high actor density: expression ASTs, bytecode, host-language code objects and name tables must not be duplicated per actor.

## Validation

Definition preparation is also the correct time to reject unsupported capabilities. Examples:

- a Null data model definition containing value expressions;
- a plugin without foreach support used by a definition that contains foreach IR;
- invalid expression syntax when the plugin chooses eager expression validation.

SCXML permits expression syntax errors to be detected at load time or deferred to runtime. `ta` therefore allows both eager and lazy plugin strategies, but the chosen behavior should be documented by the plugin.
