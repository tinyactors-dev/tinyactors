# ADR 0020: Prefixes are recorded per node and per attribute

**Status:** Accepted  
**Date:** 2026-09-20  
**Supersedes:** the prefix rule in [ADR 0006](0006-names-are-interned-per-document.md), that the name entry keeps the prefix it was *first* written with

## Context

ADR 0006 made a name's identity `(namespace, local)` and kept one prefix on the name entry, the first one seen. That is the right identity — a scan for `{urn:x}b` must find `<p:b>` and `<q:b>` alike — but the prefix rule was wrong for a concrete syntax tree (ADR 0017), whose point is that a document writes back as what it was.

Two spellings of one name lose information:

```xml
<a><p:b xmlns:p="urn:x"/><q:b xmlns:q="urn:x"/></a>
```

Both elements carry name ID 3. With one recorded prefix, the second reports `p`. Written back, it is `<p:b/>` inside a scope where `p` is not bound: the output is not well-formed namespace-wise, and even where `p` happens to be in scope the text is not what was parsed. Attributes have the same defect (`p:k` and `q:k` on different elements), and an unprefixed spelling of a name in a default namespace collides with a prefixed one the same way. The spec corpus already required the right behaviour (`namespaces.spec`, "two prefixes may bind the same namespace"); the documents disagreed with it.

## Decision

Identity is unchanged. What changes is where the spelling lives.

- The **name entry** still records a prefix: the one the name was first written with. It is now only the *default* prefix.
- Every element and every attribute reports the prefix **it** was written with. When that equals its name entry's default, nothing is stored. When it differs, a `{index, prefix offset, prefix length}` entry is appended to a sparse, index-sorted column — one for nodes, one for attributes — in the tree arena. A lookup is a binary search of that column and, on a miss, the name entry.
- `txml_node_prefix` returns the per-node answer. Attribute prefixes are read through `txml_attribute_view.prefix`, which `txml_attribute_read` fills from the same records the writer uses, so there is one source of truth. (This ADR first kept them behind a private accessor and left the view unchanged; the field was added once the writer and the spec corpus both needed to see them.)

The dense alternatives were rejected. A 4-byte prefix per node grows every node for a case that is rare in real documents, and the 24-byte node has no spare bytes. Making the prefix part of a name's identity would split `{urn:x}b` into one name per spelling and break name scans.

## Consequences

The common document — one consistent prefix per name — pays nothing. A document that spells one name several ways pays 12 bytes per deviating node or attribute, plus the prefix bytes in the pool, and a `txml_node_prefix` call on such a node costs a binary search instead of a load.

`write(parse(x))` can now reproduce prefixes exactly, including a prefix bound only on the element that uses it. The writer, when it exists, takes each element's prefix from the node and each attribute's from the attribute column, and emits a declaration only where the input had one.
