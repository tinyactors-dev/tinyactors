# ADR 0012: Conformance scope — UTF-8, namespaces, optional internal subset, no validation

**Status:** Accepted; the optional internal subset is superseded by [ADR 0017](0017-the-parser-produces-a-concrete-syntax-tree.md)  
**Date:** 2026-09-20

## Context

"XML parser" covers everything from a tag scanner to Xerces. Leaving the boundary implicit is how libraries end up quietly non-conforming in ways their users discover in production. XML 1.0 also sets a specific bar for a non-validating processor that is easy to miss: such a processor must still read the internal DTD subset, because entity declarations and attribute defaults there change the document's content.

The core faced the same honesty question about SCXML and answered it directly in its ADR 0001, stating plainly that the library is not a conforming SCXML processor and why.

## Decision

txml implements the syntax of XML 1.0 and Namespaces in XML 1.0, specifically:

- **Well-formedness**: element structure and matching end tags, attributes, character and predefined entity references, CDATA sections, comments, processing instructions, and the XML declaration.
- **Encoding**: UTF-8 input only, validated as it is scanned, plus the XML `Char` restrictions — no C0 controls other than tab, line feed, and carriage return, no surrogates, no U+FFFE or U+FFFF. Violations are `TXML_DOC_ENCODING` (ADR 0001).
- **Namespaces**: prefix binding and scoping, default namespaces for elements, and the rule that an unprefixed attribute has no namespace. Names are compared as expanded names (ADR 0006). An unbound prefix is `TXML_DOC_UNDECLARED_PREFIX`.
- **Normalization**: line endings to `\n` and attribute-value whitespace, performed on the copy into the pool (ADR 0008).
- **Internal DTD subset**, only under `TXML_INTERNAL_SUBSET`: internal general entity declarations and `ATTLIST` attribute defaults. Defaulted attributes appear with `TXML_ATTR_DEFAULTED`. Expansion is bounded by `entity_expansion_budget` (ADR 0010). With the flag off, the internal subset is skipped and a reference to an entity it declares is `TXML_DOC_UNDECLARED_ENTITY`.

Out of scope, permanently or for now: DTD or schema validation of any kind, XPath, XInclude, XSLT, canonicalization, XML 1.1, parameter entities beyond what the internal subset needs, and — by ADR 0001 — anything external.

Stated plainly: with `TXML_INTERNAL_SUBSET` off, txml is **not** a conforming non-validating XML processor, because it ignores declarations that affect document content. With the flag on and for documents that reference nothing external, it is intended to behave as one. Which of the two an embedder wants is a configuration decision, made visible rather than hidden behind a default.

## Consequences

An embedder can tell from one paragraph whether txml fits, instead of discovering the boundary through a failing document. For the SCXML importer that motivates the library, the scope is comfortably sufficient: SCXML documents use namespaces and no DTDs.

The eventual measure of the reading side is the W3C XML Conformance Test Suite, run over the categories in scope: every not-wf case must fail with a document error at the right position, and every valid in-scope case must parse. Cases that depend on external entities, validation, or XML 1.1 are marked out of scope in the suite runner rather than silently skipped, so the gap stays visible.

Dropping validation keeps the library small and predictable, and it keeps a whole class of quadratic and exponential behaviours out of the code entirely. A consumer that needs validation validates the document model after parsing, which for a known vocabulary is usually a short cursor walk (ADR 0005).
