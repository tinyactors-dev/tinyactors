# Conformance scope

This file states plainly what txml does and does not implement, so nobody has
to infer it from the code. ADR 0012 records the decision; this is the detail.

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
- **The internal DTD subset**, optionally (`TXML_INTERNAL_SUBSET`): internal
  general entity declarations and `ATTLIST` default values. Declared defaults
  appear as attributes flagged `TXML_ATTR_DEFAULTED`.

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
| Standalone declaration | Parsed, ignored — it only matters to a validating processor. |
| Entity references left unexpanded | No. txml expands; it has no entity-reference node. |

## The honest statement

With `TXML_INTERNAL_SUBSET` **off**, txml is **not** a conforming
non-validating XML processor. XML 1.0 §5.1 requires a non-validating
processor to process the internal subset for entity declarations and attribute
defaults, and with the flag off txml skips it.

With the flag **on**, txml is a conforming non-validating processor *for
documents whose entities are all internal*. It still cannot accept a document
that requires reading an external entity, because it cannot read anything.
This is a deliberate trade (ADR 0001), not an omission to be fixed.

Practically: SCXML, SVG fragments, configuration files, RSS, and most
machine-generated XML need none of the missing features. Documents built
around external entity sets do, and txml is the wrong tool for them.

## Round-trip fidelity

A round trip (`parse`, then `txml_write_subtree`) preserves the **XML
Infoset**, not the exact bytes.

### Preserved

| Detail | How |
|---|---|
| Expanded element and attribute names | names column |
| Prefixes | recorded per name, as first written |
| Attribute order | attributes are contiguous in document order |
| Namespace declarations | kept as flagged attributes, re-emitted in place |
| `<a/>` versus `<a></a>` | `TXML_NODE_EMPTY` plus `TXML_WRITE_EMPTY_TAGS` |
| Character data, including whitespace | pool runs, unless `TXML_SKIP_BLANK_TEXT` |
| CDATA content | `TXML_NODE_CDATA` with `TXML_KEEP_CDATA` |
| Comments, processing instructions | with `TXML_KEEP_COMMENTS` and `TXML_KEEP_PIS` |

### Not preserved

| Detail | Why |
|---|---|
| Entity and character references | expanded during parsing; the writer re-escapes only what it must |
| `\r\n` and lone `\r` | normalized to `\n`, as the spec requires |
| Attribute-value whitespace | normalized to spaces, as the spec requires |
| Quote style, spacing inside tags, attribute line breaks | not in the Infoset |
| CDATA section boundaries | adjacent character data merges into one text node |
| A second spelling of one expanded name | the name's prefix is the first one seen |
| DOCTYPE and the internal subset | not stored; declared entities and defaults are already applied |
| The XML declaration | not stored. The writer emits one on `TXML_WRITE_DECLARATION`, independently of what the input had. |

The XML declaration is the one deliberate gap: the parser validates it and
drops it, because the public API has no accessor for version, encoding or
standalone. A later ADR may add one; until then, "was there a declaration?" is
not a question a document can answer.

Because of this, byte equality with the input is the wrong test for a round
trip. The two properties that do hold are stated in `testing.md`: a parsed
document survives write-then-parse unchanged, and output reaches a byte fixed
point after one round trip.
