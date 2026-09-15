# Literals and content

The runtime must support data values that did not originate from an expression, without making XML part of the ABI.

## Opaque typed literal

Suggested representation:

```c
typedef enum {
    TA_LITERAL_TEXT,
    TA_LITERAL_BYTES,
    TA_LITERAL_STRUCTURED,
    TA_LITERAL_FRONTEND
} ta_literal_kind;

typedef struct {
    ta_literal_kind kind;
    ta_str media_type;   /* optional */
    ta_bytes bytes;
    ta_atom frontend;    /* optional namespace/importer identity */
} ta_literal;
```

The definition owns the bytes. The data model converts the literal into a legal `ta_value` with `import_literal()`.

## Why this is not `import_content`

The same semantic mechanism serves:

- data initializers;
- assignment literals;
- send/invoke content;
- final/done data;
- generated definitions;
- future non-XML importers.

The IR supplies context separately when context affects downstream interpretation.

## Expression content

Content produced by an expression simply uses `eval_value()`; no additional data-model callback is required.

## Empty-content fallback

SCXML requires an empty-string fallback when a content expression fails. An SCXML importer/profile can compile or provide a canonical empty-text literal and ask the data model to import it. The generic runtime does not need a special `make_empty_string()` method.

## Resource loading

Fetching an external resource is not inherently a data-model operation. A frontend/definition loader or host resource service resolves it into a literal before execution when the chosen profile requires load-time fetching.
