# Architecture decision records

ADRs are the primary record of txml design intent. Unless marked otherwise, the decisions below are **Accepted**.

| ADR | Decision |
|---|---|
| [0001](0001-xml-1-0-without-i-o-or-a-c-library.md) | [XML 1.0 without I/O or a C library](0001-xml-1-0-without-i-o-or-a-c-library.md) |
| [0002](0002-one-linked-chunk-arena-per-document-nothing-relocates.md) | [One linked-chunk arena per document; nothing relocates](0002-one-linked-chunk-arena-per-document-nothing-relocates.md) |
| [0003](0003-the-parser-produces-a-document-the-token-stream-stays-internal.md) | [The parser produces a document; the token stream stays internal](0003-the-parser-produces-a-document-the-token-stream-stays-internal.md) |
| [0004](0004-documents-are-index-addressed-columns-in-document-order.md) | [Documents are index-addressed columns in document order](0004-documents-are-index-addressed-columns-in-document-order.md) |
| [0005](0005-subtrees-are-index-ranges-and-cursors-are-values.md) | [Subtrees are index ranges and cursors are values](0005-subtrees-are-index-ranges-and-cursors-are-values.md) |
| [0006](0006-names-are-interned-per-document.md) | [Names are interned per document](0006-names-are-interned-per-document.md) |
| [0007](0007-document-errors-are-data-c-errors-are-results.md) | [Document errors are data; C errors are results](0007-document-errors-are-data-c-errors-are-results.md) |
| [0008](0008-input-is-copied-into-a-padded-staging-window.md) | [Input is copied into a padded staging window](0008-input-is-copied-into-a-padded-staging-window.md) |
| [0009](0009-pausable-parsing-through-feed-step-and-budget.md) | [Pausable parsing through feed, step, and budget](0009-pausable-parsing-through-feed-step-and-budget.md) |
| [0010](0010-every-unbounded-construct-has-a-limit.md) | [Every unbounded construct has a limit](0010-every-unbounded-construct-has-a-limit.md) |
| [0011](0011-the-writer-takes-events-not-documents.md) | [The writer takes events, not documents](0011-the-writer-takes-events-not-documents.md) |
| [0012](0012-conformance-scope-utf-8-namespaces-optional-internal-subset-no-validation.md) | [Conformance scope — UTF-8, namespaces, optional internal subset, no validation](0012-conformance-scope-utf-8-namespaces-optional-internal-subset-no-validation.md) |
| [0013](0013-streaming-by-mark-and-rewind.md) | [Streaming by mark and rewind](0013-streaming-by-mark-and-rewind.md) |
| [0014](0014-one-authored-public-header.md) | [One authored public header](0014-one-authored-public-header.md) |
| [0015](0015-simd-readiness-without-simd.md) | [SIMD readiness without SIMD](0015-simd-readiness-without-simd.md) |

txml is the XML front end that `tinyactors-core` deliberately lacks: core ADR 0001 adopts SCXML semantics without an XML dependency, core ADR 0025 defers an importer/exporter to a separate component, and core ADR 0023 sets the freestanding constraints txml inherits. Where a decision here mirrors one there, the text says so.

See [`../internal/`](../internal/) for the implementation reference and [`../../tests/spec/`](../../tests/spec/) for the executable half of these decisions.
