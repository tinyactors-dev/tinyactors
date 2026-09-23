# Literals and content

The runtime supports data values that did not come from an expression,
without making XML part of the ABI (ADR 0001, ADR 0025). This note records
what shipped, and why it is smaller than the design it started from.

## What a literal is

A literal `<content>` in a spec is `TA_SCXML_VALUE_CONTENT`:

```c
TA_WHEN(TA_SCXML_VALUE_CONTENT) struct {
    ta_str media_type;  /* what the bytes are, as the front end knows it; empty when nothing said */
    ta_bytes bytes;
} content;
```

The compiler copies both into the definition as a `ta_content_def` and hashes
both into the revision, so two charts that differ only in a media type do not
share a fingerprint. `ta_definition_export()` gives both back, so a chart that
a codec decoded recompiles to the same revision (ADR 0053).

A codec writes its own `info.name` as the media type of every literal content
it decodes: an SCXML XML codec says `application/scxml+xml`. What the bytes
are is the codec's rule. An XML codec re-serializes the element's children
with their namespaces declared (SCXML G.6), because a recipient must see a
re-serialization, not the author's bytes.

A literal `src` (of `<invoke>`) and a literal `target` (of `<send>`) are also
stored as content, with an empty media type (`definition.h`). They are text,
not payloads: the interpreter reads them with `text_value()` or the target
grammar, never with `import_content()`.

## Who interprets a literal

Whether bytes become a data-model value depends on who receives them, and
that is chosen by the element's `type` attribute, not by the element. It is
therefore decided at delivery (ADR 0053 section 2):

| Consumer | Receives |
|---|---|
| `<data>`, `<assign>`, `<raise>`, `<donedata>` | the data model's `import_content()` result |
| `<send>` to the SCXML Event I/O Processor | `import_content()`, so the receiving session's `_event.data` is a data-model value (ADR 0040) |
| `<send>` to a host I/O processor | `ta_send.content` and `ta_send.content_type`: the bytes and their media type, not imported |
| `<invoke>` | `ta_invoke_request.content` and `content_type`, not imported: core owns no invoker |

For a host processor or an invoker, interpretation is platform-specific
(SCXML 6.4.2): an `x-clock` service's payload is bytes, a Basic HTTP
processor's is a message body whose `Content-Type` it has been told, and an
SCXML invoker's is a chart it decodes itself. None of them depends on the
data model, so they work under the null profile, which imports nothing.

A delayed send to a host processor stores the `ta_content_id`, not the bytes,
and resolves it against its sender's definition when it is dispatched. A
session image therefore reconstructs it against its exact revision
(ADR 0038).

## What was not adopted

An earlier version of this note proposed:

```c
typedef struct {
    ta_literal_kind kind;  /* TEXT, BYTES, STRUCTURED, FRONTEND */
    ta_str media_type;
    ta_bytes bytes;
    ta_atom frontend;
} ta_literal;
```

consumed by a data-model `import_literal()`, under the heading "Why this is
not `import_content`". What shipped is `import_content()` over bytes, with
`media_type` added by ADR 0053. The other two fields are not needed:

- **`kind`.** Text versus bytes is already carried by which spec member holds
  the literal. A literal `src` or `target` is text and goes to
  `text_value()`; a `<content>` is bytes and goes to `import_content()` or to
  a host adapter.
- **`frontend`.** The media type says what produced the bytes, and the
  consuming `type` atom says who reads them. Together they are what a
  front-end identity was for.

## Expression content

Content produced by an expression (`<content expr>`) uses `eval_value()`. No
additional data-model callback is needed. A host adapter receives it as
`data`, a value, never as `content`: SCXML 5.6.2 makes `expr` and a body
exclusive, and the compiled IR distinguishes them as
`TA_VALUE_SOURCE_EXPR` and `TA_VALUE_SOURCE_CONTENT`.

## Empty-content fallback

SCXML requires an empty-string fallback when a content expression fails. An
SCXML importer or profile can compile a canonical empty-text literal and ask
the data model to import it. The generic runtime does not need a special
`make_empty_string()` method.

## Resource loading

Fetching an external resource is not a data-model operation. A codec asks the
embedder's `ta_chart_src_resolver` for `<data src>` and `<script src>` during
a decode (ADR 0053), and the result enters the spec as a literal. An
`<invoke src>` is not fetched: it is a URI the invoker interprets at run time
(SCXML 6.4), so it stays in the spec as written. Core performs no I/O.
