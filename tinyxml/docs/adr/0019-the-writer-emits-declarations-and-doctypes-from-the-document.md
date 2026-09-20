# ADR 0019: The writer emits declarations and doctypes from the document

**Status:** Accepted  
**Date:** 2026-09-20  
**Amends:** [ADR 0011](0011-the-writer-takes-events-not-documents.md), [ADR 0017](0017-the-parser-produces-a-concrete-syntax-tree.md)

## Context

ADR 0017 made the XML declaration and the DOCTYPE nodes, so that a parsed document is what it was read from. ADR 0011 made the writer take events, and its vocabulary is start, attribute, text, CDATA, comment, PI, and end. Neither construct has an event.

That leaves a hole in the middle of the thing ADR 0017 was for. `txml_write_subtree` walks a document and emits events, and its walk now reaches two node kinds it cannot express. `TXML_WRITE_DECLARATION` is not an answer: it emits a fixed `<?xml version="1.0" encoding="UTF-8"?>` regardless of what the input carried, so a document that declared `standalone="yes"`, or no encoding, comes back as something it was not. A document with a doctype cannot be written back at all. Round-trip fidelity is the property ADR 0017 bought, and without a decision here the implementation would pick an answer by accident.

## Decision

`txml_write_subtree` emits both directly from the document rather than through an event.

A `TXML_NODE_DECLARATION` is written as `<?xml`, then its attributes in stored order as `name="value"`, then `?>`. A `TXML_NODE_DOCTYPE` is written as `<!DOCTYPE`, then its text verbatim, then `>` — which reproduces the original bytes exactly, because the text is defined as the bytes between those delimiters.

`TXML_WRITE_DECLARATION` keeps its meaning for event-driven producers and gains a precise one for documents: emit a declaration when the document has no declaration node, and never double an existing one.

This is consistent with what `txml_write_subtree` already is. It is not a thin loop over the event calls: it resolves namespace bindings against the document's namespace column to decide which declarations a start tag needs (ADR 0011's own round-trip rule). It is the document-aware half of the writer, and emitting two document-only constructs is the same kind of work.

No public function is added. The writer stays at thirteen.

## Consequences

Every construct a parse produces can be written back, so `write(parse(x))` is byte-exact for the whole of what ADR 0017 keeps, not for most of it. The `@roundtrip` fixtures can cover a declaration and a doctype, which is where this would otherwise have been discovered.

A producer that emits events without a document still cannot write a doctype, and can only get the fixed declaration from the flag. That is exactly where ADR 0011 left it, so nothing regressed; what changed is that documents are no longer limited to it. The SCXML exporter that motivates the writer needs neither construct — SCXML documents carry no DTD — so the gap has no consumer today.

If one appears, `txml_write_doctype` and a declaration event are additive, and the event API is where they belong. Deciding that now would mean designing a declaration event — three pseudo-attributes, two of them optional, under a three-parameter limit — for a caller that does not exist.
