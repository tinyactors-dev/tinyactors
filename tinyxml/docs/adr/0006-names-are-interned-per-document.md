# ADR 0006: Names are interned per document

**Status:** Accepted  
**Date:** 2026-09-20

## Context

Element and attribute names repeat constantly, and comparing them as strings is the dominant cost in most XML consumers. Storing a name as a `uint32_t` in the node column (ADR 0004) requires deduplicating names anyway, so the question is only what that integer means and who assigns it.

An earlier draft answered this with a *profile*: a caller-supplied vocabulary, interned once and shared across documents, so that an SCXML importer could `switch` on compile-time constants. The profile also carried a second job — it was going to be the only thing interned, so that a hostile document could not flood the name table. That second job does not survive scrutiny, and neither does the first as a general mechanism: a profile is a list of names you already know, and there is no way to build one for a document you have never seen. A generic tool — a pretty-printer, a histogram, a schema sniffer, a format converter — has no vocabulary to declare.

## Decision

The profile is dropped. Names are interned per document, always, with no prior knowledge required.

Every distinct expanded name in a document gets one entry in that document's name table. A name ID is an index into that table and is meaningful only for that document, which the header states and which ADR 0005 repeats as a cursor rule. Namespace URIs are deduplicated in their own column, so two names in the same namespace share a namespace index and namespace comparison is an integer comparison. The prefix as written is retained for round-tripping (`txml_node_prefix`) but is not part of a name's identity, so `a:foo` and `b:foo` bound to the same URI are one name.

Queries resolve strings to integers once. `txml_doc_find_name` takes a `txml_qname` and returns a name ID, after which `txml_find_next`, `txml_find_attribute`, and any hand-written scan compare `uint32_t`s. `txml_doc_name_count` and `txml_doc_name_at` enumerate the table, so a tool can discover a document's vocabulary instead of declaring it.

`txml_doc_find_name` returns `TXML_NAME_NONE` when the document contains no such name. That is not merely a "not found": it proves that no node can match, so the caller can abandon the entire query without scanning a single node.

Name-table growth is bounded by `limits.max_names` (ADR 0010). A document with a million distinct names reaches that limit and produces a `TXML_DOC_LIMIT` document error with `TXML_LIMIT_NAMES`.

## Consequences

Any document can be queried at full speed with no setup: one hash lookup per query to resolve a name, then integer comparisons. The generic and the specialized consumer use the same API, and there is no second interning concept to explain, test, or bind.

The interning cost is one hash lookup per *distinct* name, not per tag, because a repeated name hits the table immediately. That cost was already being paid by deduplication.

What is lost is the profile's genuine advantage: name IDs are document-local, so a consumer that parses many documents of a known vocabulary must resolve its names once per document and cannot `switch` on compile-time constants. In practice that is a handful of `txml_doc_find_name` calls into a small struct at the start of each document — the SCXML importer will do exactly that — which is a fair price for removing a concept that could not serve half of the library's users.
