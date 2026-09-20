# Conformance scope

This file states plainly what txml does and does not implement, so nobody has
to infer it from the code. ADR 0012 records the decision and ADR 0017 removed
the one part of it that was configurable; this is the detail.

## Implemented

- **XML 1.0 well-formedness** for the document entity: elements, attributes,
  character data, CDATA sections, comments, processing instructions, and the
  prolog.
- **UTF-8 only**, validated. Ill-formed sequences, overlong encodings,
  encoded surrogates, and characters outside the XML `Char` production are
  `TXML_DOC_ENCODING`. Transcoding is an adapter the embedder puts in front of
  `txml_parse_feed`.
- **Namespaces in XML 1.0**: prefix resolution, default declarations,
  `xmlns=""` undeclaration, the pre-bound `xml` and `xmlns` prefixes,
  duplicate-attribute detection on **expanded** names, and the rule that an
  unprefixed attribute has no namespace.
- **The five predefined entities** and numeric character references, decimal
  and hexadecimal.
- **Line-end and attribute-value normalization** exactly as XML §2.11 and
  §3.3.3 describe for non-tokenized attributes.
- **The internal DTD subset**, always: internal general entity declarations
  and `ATTLIST` default values. Declared defaults appear as attributes flagged
  `TXML_ATTR_DEFAULTED`. The `DOCTYPE` itself is kept as a
  `TXML_NODE_DOCTYPE` node whose text is the declaration as written.
- **A concrete syntax tree** (ADR 0017): comments, processing instructions,
  every text run including whitespace-only ones, the XML declaration and the
  DOCTYPE are all nodes. Nothing the input contains is discarded.

## Not implemented

| Feature | Status |
|---|---|
| External entities, external DTD subset | **Never fetched.** There is no I/O path. A reference to an undeclared entity is `TXML_DOC_UNDECLARED_ENTITY`; an external subset declaration is skipped. |
| DTD validation | No. Content models and attribute types are not checked. |
| XML Schema, RELAX NG | No, and out of scope. |
| XPath, XSLT, XInclude, XML Base, xml:id | No. `xml:space` and `xml:lang` are ordinary attributes. |
| XML 1.1 | No. `version="1.1"` is `TXML_DOC_ENCODING`. |
| Encodings other than UTF-8 | No. A declaration naming another encoding is `TXML_DOC_ENCODING` rather than a silent reinterpretation. |
| Parameter entities, conditional sections, notations | Parsed over and ignored inside the internal subset; never expanded. |
| Standalone declaration | Stored as an attribute of the declaration node, and otherwise ignored — it only matters to a validating processor. |
| Entity references left unexpanded | No. txml expands; it has no entity-reference node. |

## The honest statement

txml is intended to behave as a conforming non-validating XML processor *for
documents whose entities are all internal*. XML 1.0 §5.1 requires such a
processor to process the internal subset for entity declarations and attribute
defaults, and txml always does; there is no configuration in which it does
not. ADR 0012 originally made this a choice, and ADR 0017 removed the choice
because a parser with two behaviours has to be asked which one it had.

It still cannot accept a document that requires reading an external entity,
because it cannot read anything. This is a deliberate trade (ADR 0001), not an
omission to be fixed.

Practically: SCXML, SVG fragments, configuration files, RSS, and most
machine-generated XML need none of the missing features. Documents built
around external entity sets do, and txml is the wrong tool for them.

## Round-trip fidelity

A round trip (`parse`, then `txml_write_subtree`) preserves the **XML
Infoset**, not the exact bytes. Since ADR 0017 the *document* retains
everything the input contained, so what a round trip loses is now a property
of the writer rather than of the parser.

### Retained by the document

| Detail | How |
|---|---|
| Expanded element and attribute names | names column |
| Prefixes | recorded per name, as first written |
| Attribute order | attributes are contiguous in document order |
| Namespace declarations | kept as flagged attributes, re-emitted in place |
| `<a/>` versus `<a></a>` | `TXML_NODE_EMPTY` plus `TXML_WRITE_EMPTY_TAGS` |
| Character data, including whitespace-only runs | pool runs; no run is ever dropped |
| CDATA content **and section boundaries** | a CDATA section is always its own text node, flagged `TXML_NODE_CDATA`, and never merges with the text around it |
| Comments, processing instructions | always nodes |
| The XML declaration | a `TXML_NODE_DECLARATION`; version, encoding and standalone are readable as its attributes |
| DOCTYPE | a `TXML_NODE_DOCTYPE` whose text is the declaration as written |

### Not preserved

| Detail | Why |
|---|---|
| Entity and character references | expanded during parsing; the writer re-escapes only what it must |
| `\r\n` and lone `\r` | normalized to `\n`, as the spec requires |
| Attribute-value whitespace | normalized to spaces, as the spec requires |
| Quote style, spacing inside tags, attribute line breaks | not in the Infoset |
| A second spelling of one expanded name | shares the name ID; each node and attribute keeps its own prefix (ADR 0020) |

### Declarations and doctypes have no event

The writer takes events (ADR 0011) and its vocabulary has no call for either
construct, so `txml_write_subtree` emits them directly from the document
instead (ADR 0019): a declaration as `<?xml` plus its attributes in stored
order plus `?>`, a doctype as `<!DOCTYPE` plus its text verbatim plus `>`.
Both reproduce their input bytes exactly. `TXML_WRITE_DECLARATION` means
"emit one when the document has no declaration node" and never doubles an
existing one.

A producer that emits events without a document still cannot write a doctype
and gets only the fixed declaration from the flag, which is where ADR 0011
left it. The SCXML exporter needs neither.

Byte equality with the input is still the wrong test for a round trip. The two
properties that do hold are stated in `testing.md`: a parsed document survives
write-then-parse unchanged, and output reaches a byte fixed point after one
round trip.
