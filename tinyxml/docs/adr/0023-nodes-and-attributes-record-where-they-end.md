# ADR 0023: Nodes and attributes record where they end

**Status:** Accepted  
**Date:** 2026-09-21  
**Amends:** [ADR 0017](0017-the-parser-produces-a-concrete-syntax-tree.md)

## Context

ADR 0017 made the document a concrete syntax tree and recorded, for every node, the position of its first byte. That answers "where does this begin" and nothing else. A tool that shows the tree against its source (a parse-tree printer, an editor highlighting a subtree, a diagnostic that underlines a whole element, a refactoring that cuts a node out of the text) needs the other end too, and the library could not say. The consumer that first needed it, the `txml` command-line tool, recovered ends by re-reading the source it had already parsed: finding the `>` that closes a tag while skipping quoted attribute values, matching `-->`, `?>` and `]]>`, and reading a DOCTYPE with its internal subset. That is the parser's grammar written a second time, in the consumer, and it can disagree with the parser on the corner cases that matter most. It also forces the consumer to keep the whole input, which a streaming parser exists to avoid.

The parser knows every end at the moment it finds it: it is the position of the delimiter it has just read.

## Decision

A node records the position one past its last byte, beside the position of its first. `txml_node_span` returns both as a `txml_span`. What "one past the last byte" means for each kind is fixed:

- an element ends after its end tag, or after the `/>` of an empty one;
- a text node ends where the next markup begins (or at the end of the input);
- a comment, processing instruction, declaration, CDATA section or DOCTYPE ends after its closing delimiter;
- the document node ends at the end of the input.

Offsets are raw input bytes, so a byte order mark and unexpanded references lie inside a span at their written length. Columns count bytes, as they already do. An element that is still open has no end yet: `txml_node_span` returns `TXML_ERR_PENDING`, the same answer a move gives for a fact the parse has not reached (ADR 0003).

An attribute records where it was written: the first byte of its name, the first byte inside its quotes, and the position after its closing quote. `txml_attribute_span_read` returns a `txml_attribute_span` with five positions. Two are derived rather than stored: a name is never split across lines, so its end is its start plus the written qualified name (prefix, colon and local part); and the closing quote is one byte, never a line end, so the value's end is one byte before the attribute's end. An attribute with no source text has no span: `TXML_ERR_NOT_FOUND` for one supplied by an `ATTLIST` default, and for the pseudo-attributes of the XML declaration, which the parser reads from a buffer rather than from the window.

Both live in the tree arena as columns parallel to the ones they describe. The positions column widens from a `txml_position` to a `txml_span` per node, and a new attribute-sources column holds three positions per attribute. Neither is folded into the 24-byte node or 12-byte attribute record, for the reason ADR 0017 gave for positions: the scan loops in `cursor.md` read those records and should not pay for what they do not use.

The element and attribute names' own extents are not stored. An element's name starts one byte after its `<`, and a PI's target two bytes after its `<?`; each is as long as the written name, which the document already holds. A consumer that wants them adds arithmetic, not memory.

## Consequences

The cost is memory again, and again it is not small. A node goes from 40 bytes (24 + a 16-byte position) to 56, since the end is a second 16-byte position. An attribute goes from 12 bytes to 60: three positions at 16 bytes each are the price of being able to say where a value starts when the input may write `a = "x"` with whitespace around the `=`, or split it across lines. A document that is mostly attributes pays the most. `max_document_bytes` remains the control; an embedder that finds spans too expensive for its profile should say so, and the answer may then be a build-time choice, which this ADR does not make (ADR 0017 refused the same temptation for the same reason: a parser whose tree depends on configuration has to be asked which tree it built).

The parser gains no state and no pass. Each construct already knows the delimiter that ends it; the end position is captured where the node is appended, or, for an element, where the end tag is consumed. Because it is captured at the append, it is undone with the node by the rollback that a refused allocation triggers, and it cannot depend on how the input was cut: the split-at-every-byte and refused-allocation tests compare spans along with everything else.

The `txml` tool no longer reads the source to find an end, and no longer needs the source at all beyond the parse. What it does with a span (rows, columns, indentation) is its own business.
