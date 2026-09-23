# ADR 0024: The writer can write a content fragment

**Status:** Accepted  
**Date:** 2026-09-23  
**Amends:** [ADR 0011](0011-the-writer-takes-events-not-documents.md)

## Context

The writer produces a document: one element, with comments and processing
instructions around it. Text outside that element, and a second element after
it, are `TXML_ERR_INVALID`.

The SCXML codec (core ADR 0053) needs to write something that is not a
document. An SCXML `<content>` element carries its children to a host adapter
as bytes, and SCXML G.6 requires those bytes to be the children re-serialized
with every namespace they use declared. The children are XML 1.0 production
[43] `content`: text, CDATA sections, and any number of elements, in any
order. For example `<content> <a/> text <b/> </content>` has five children.

`txml_write_subtree` already does the namespace part. Writing a nested element
into a fresh writer declares every binding its names need from outside the
subtree, so `<scxml>` inside `<content>` gets its `xmlns` back. The document
rule is the only thing in the way. Working around it in the consumer means one
writer per child element and escaping text by hand, which duplicates the
writer's escaping rules in a second place.

## Decision

`TXML_WRITE_FRAGMENT` makes the writer produce a content fragment instead of a
document:

- text and CDATA may be written outside every element;
- after the outermost element closes, another may start;
- `txml_write_subtree` of a text node, a CDATA node or a second element at the
  top level is accepted for the same reason.

A fragment has no prolog, so `TXML_WRITE_FRAGMENT` together with
`TXML_WRITE_DECLARATION` is `TXML_ERR_INVALID` at `txml_writer_init`, and a
walk that meets a declaration or DOCTYPE node is refused. Namespace handling
does not change: a binding's scope is still the element that declares it, so
two top-level elements that use the same namespace each declare it.

## Consequences

- The codec writes a `<content>` element's children with one writer and one
  subtree call per child.
- Escaping stays in one place.
- A document writer behaves exactly as before. Every check that refused
  top-level content still does unless the flag is set.
