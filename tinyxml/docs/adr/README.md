# Architecture decision records

ADRs are the primary record of txml design intent. Unless marked otherwise, the decisions below are **Accepted**.

ADRs 0016–0019 and 0023 revise earlier ones. 0016 replaces the single-arena and borrowed-arena parts of 0002, 0017 replaces the retention flags in 0003 and the optional internal subset in 0012, 0018 withdraws 0013, and 0019 closes the writer gap 0017 opened. Each amended ADR says so in its status line; the rest of its text still stands.

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
| [0013](0013-streaming-by-mark-and-rewind.md) | [Streaming by mark and rewind](0013-streaming-by-mark-and-rewind.md) — **Superseded by 0018** |
| [0014](0014-one-authored-public-header.md) | [One authored public header](0014-one-authored-public-header.md) |
| [0015](0015-simd-readiness-without-simd.md) | [SIMD readiness without SIMD](0015-simd-readiness-without-simd.md) |
| [0016](0016-a-document-owns-two-arenas-values-and-tree.md) | [A document owns two arenas: values and tree](0016-a-document-owns-two-arenas-values-and-tree.md) |
| [0017](0017-the-parser-produces-a-concrete-syntax-tree.md) | [The parser produces a concrete syntax tree](0017-the-parser-produces-a-concrete-syntax-tree.md) |
| [0018](0018-streaming-by-mark-and-rewind-is-withdrawn.md) | [Streaming by mark and rewind is withdrawn](0018-streaming-by-mark-and-rewind-is-withdrawn.md) |
| [0019](0019-the-writer-emits-declarations-and-doctypes-from-the-document.md) | [The writer emits declarations and doctypes from the document](0019-the-writer-emits-declarations-and-doctypes-from-the-document.md) |
| [0020](0020-prefixes-are-recorded-per-node-and-per-attribute.md) | [Prefixes are recorded per node and per attribute](0020-prefixes-are-recorded-per-node-and-per-attribute.md) |
| [0021](0021-the-scanner-is-a-table-of-resumable-states.md) | [The scanner is a table of resumable states](0021-the-scanner-is-a-table-of-resumable-states.md) |
| [0022](0022-the-input-padded-escape-hatch-is-withdrawn.md) | [The input-padded escape hatch is withdrawn](0022-the-input-padded-escape-hatch-is-withdrawn.md) |
| [0023](0023-nodes-and-attributes-record-where-they-end.md) | [Nodes and attributes record where they end](0023-nodes-and-attributes-record-where-they-end.md) |

txml is the XML front end that `tinyactors-core` deliberately lacks: core ADR 0001 adopts SCXML semantics without an XML dependency, core ADR 0025 defers an importer/exporter to a separate component, and core ADR 0023 sets the freestanding constraints txml inherits. Where a decision here mirrors one there, the text says so.

See [`../internal/`](../internal/) for the implementation reference and [`../../tests/spec/`](../../tests/spec/) for the executable half of these decisions.
