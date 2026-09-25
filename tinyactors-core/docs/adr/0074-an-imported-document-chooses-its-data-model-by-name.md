# ADR 0074: An imported document chooses its data model by name, and a data model learns how each expression is used

**Status:** Accepted and implemented (2026-09-25). Refines ADR 0053 (section 4, `ta_definition_import`); applies ADR 0058. Serves napi ADR 0019.
**Date:** 2026-09-25

## Context

`ta_definition_import` (ADR 0053) decodes a chart with a codec, compiles
the spec and registers the definition. Its params took one `ta_datamodel`.
The compiler refuses a spec whose `datamodel` differs from the adapter's
`v->name` (`TA_COMPILE_ERROR_DATAMODEL_MISMATCH`), so a host had to know
which data model a document wants before it could import it.

A host that offers several data models (napi ADR 0019 registers them by
the SCXML `datamodel` name: `ecmascript`, `null`, ...) only learns the
document's choice by decoding it. Decoding in the host first and then
calling `ta_definition_compile_spec` would work, but a compile error then
loses its position in the document: only `ta_definition_import` holds the
input it needs to map an error back to a line and column (ADR 0058).

## Decision

**`ta_definition_import` takes the data models the document may choose
from, and picks one by the document's `datamodel` name.**

```c
TA_COUNTED_BY(datamodel_count) const ta_datamodel *datamodels;
uint32_t datamodel_count;
```

replaces `ta_datamodel datamodel` in `ta_definition_import_params`.

- **By name:** a document whose `datamodel` attribute names one gets the
  entry whose `v->name` equals it.
- **No attribute:** the first entry. SCXML leaves the default data model to
  the platform; the host says which by putting it first.
- **A name no entry has:** the import compiles against the first entry, so
  the compiler's existing mismatch error refuses the chart, with its
  subject the name, located at the `datamodel` attribute in the input.
  There is one refusal path, and it is located.
- **At least one entry.** An empty list is misuse: `TA_ERR_INVALID`.

`ta_definition_compile_spec` keeps one data model: a spec built in memory
has no document to choose for, and its caller already knows.

**`prepare_definition` also learns how each expression is used.** A data
model that compiles source text (napi ADR 0019) compiles a guard, a value,
a location and a script differently, and must do it at define time so a
refusal is located. Until now the expression table said only what each
expression is, not where the chart uses it.

```c
typedef enum {
    TA_EXPR_ROLE_VALUE = 0,  /* expr, eventexpr, targetexpr, delayexpr, typeexpr, sendidexpr, foreach array, log expr */
    TA_EXPR_ROLE_CONDITION,  /* a transition's cond, an <if>/<elseif> cond */
    TA_EXPR_ROLE_LOCATION,   /* assign location, idlocation, a namelist or <param> location */
    TA_EXPR_ROLE_SCRIPT      /* a <script> body, the document's or in content */
} ta_expr_role;
```

`ta_datamodel_prepare_definition_params` gains `roles`, one per
expression, by the same index. The compiler records the role where it
records the expression, and the definition keeps the table in its arena:
one `ta_expr_role` per expression per definition, nothing per actor.
Adapters that don't need it ignore it.

## Consequences

- A host with several data models imports any document with one call, and
  a refusal still says where in the document (ADR 0058).
- Every caller changes to a list (AGENTS.md: change APIs in place): the
  test host's import and its invoker (which offers the parent's data
  model only), the codec's tests, core's codec test, and the Ruby
  scenario and round-trip helpers.
- No cost per actor or per event: the choice is a scan of a short list at
  import.

## Testing

`tinyactors-scxml/tests/ta_scxml_test.c` (`test_datamodel_by_name`): a
document naming the second of two offered data models gets it; a document
naming none gets the first; a document naming one not offered is refused
with `TA_COMPILE_ERROR_DATAMODEL_MISMATCH`, subject the name, located at
the attribute; an empty list is `TA_ERR_INVALID`. `test_expression_roles`
imports a document with every kind of expression through a data model
that records what `prepare_definition` gets, and checks each role.
