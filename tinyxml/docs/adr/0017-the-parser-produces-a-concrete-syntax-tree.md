# ADR 0017: The parser produces a concrete syntax tree

**Status:** Accepted  
**Date:** 2026-09-20  
**Amended by:** [ADR 0023](0023-nodes-and-attributes-record-where-they-end.md): a node also records where it ends, taking the per-node cost from 40 bytes to 56  
**Supersedes:** the retention flags in [ADR 0003](0003-the-parser-produces-a-document-the-token-stream-stays-internal.md) and the optional internal subset in [ADR 0012](0012-conformance-scope-utf-8-namespaces-optional-internal-subset-no-validation.md)

## Context

The parser had seven flags. Five of them decided what to throw away — `TXML_KEEP_COMMENTS`, `TXML_KEEP_PIS`, `TXML_KEEP_CDATA`, `TXML_KEEP_POSITIONS`, `TXML_SKIP_BLANK_TEXT` — and one decided whether to read the internal subset. Each is a fork in the node table, so the same bytes parse to different trees depending on configuration.

That has a cost everywhere. A consumer cannot write `cursor_at(doc, 3)` without knowing which dialect produced the document. A test fixture has to state its flags before its expected nodes mean anything, and the spec corpus carries `@flags` lines for exactly that reason. Worst, a document parsed without its comments, its PIs or its declaration cannot be written back as what it was, which quietly undercuts the round-trip properties the writer exists to provide: `write(parse(x))` was only ever equal to `x` for the subset of flags that happened to keep everything.

The flags were also not symmetric with the query interface. A consumer that wants elements only already has `txml_next_element` and `txml_find_next`, which skip every other kind in one call. Dropping nodes at parse time to save a consumer a function call trades a permanent loss of information for a marginal walk.

Two constructs were dropped unconditionally, which is the same problem without even a flag to notice it: the XML declaration was validated and discarded, and the DOCTYPE was skipped unless the internal subset flag was set. A document with either could not round trip at all.

## Decision

The parser produces a **concrete syntax tree**: every construct in the input becomes a node, and nothing is dropped.

- **Comments and processing instructions** are always nodes.
- **All text is kept**, including whitespace-only runs between elements and whitespace in the prolog and epilog, which become text children of the document node. There is no longer a "whitespace outside the document element is not a node" special case.
- **A CDATA section is always its own text node**, flagged `TXML_NODE_CDATA`, and never merges with adjacent text.
- **The XML declaration is a node**, `TXML_NODE_DECLARATION`, whose `version`, `encoding` and `standalone` pseudo-attributes are ordinary attributes in namespace 0. It is read with `txml_first_attribute` and `txml_find_attribute` like any element, so it needs no accessors of its own. It reverses nothing about ADR 0003's separate point that it is not a processing instruction: it has its own kind.
- **The DOCTYPE is a node**, `TXML_NODE_DOCTYPE`, whose text is the declaration as written, and **the internal subset is always read**. Internal general entity declarations and `ATTLIST` defaults always take effect; expansion stays bounded by `entity_expansion_budget` (ADR 0010).
- **Positions are always recorded.** `txml_node_position` never returns `TXML_ERR_NOT_FOUND`.
- **An encoding signature is not content.** A leading UTF-8 byte order mark is consumed, counted in the byte offset and given no node, and the writer does not emit one. It is the one thing in the input that is not kept: it says how the bytes are encoded, not what the document says, and XML §4.3.3 puts it outside the document entity's content. U+FEFF anywhere else is ordinary character data.

`txml_parser_flags` therefore had one member left, `TXML_INPUT_PADDED`, which was not a retention decision at all: it was a promise about the caller's buffer (ADR 0008). ADR 0022 withdrew it, and the parser now has no flags.

## Consequences

One document has one tree. An index means the same thing to everyone, a fixture needs no dialect line, and `parse(write(parse(x)))` equals `parse(x)` for every document rather than for flag-matched ones. The byte fixed point in `testing.md` becomes a property of the library instead of a property of a configuration.

ADR 0012's honesty caveat disappears with the flag it was about. That ADR had to say that with `TXML_INTERNAL_SUBSET` off txml is *not* a conforming non-validating XML processor, because it ignored declarations that change document content, and that which of the two behaviours you got was a configuration decision. There is now one behaviour, and for documents that reference nothing external it is the conforming one.

The cost is memory, and it is not small. Every whitespace run between elements is a node; an indented document is mostly whitespace nodes. Positions add 16 bytes to every node, taking the per-node cost from 24 bytes to 40. A pretty-printed configuration file can easily cost several times what it did before. `max_document_bytes` is the control, and an embedder that finds the tree too expensive for its profile should say so — that is the conversation this ADR wants to have, rather than seven flags each silently changing the answer.

The internal subset is no longer optional, so every document pays for the entity machinery and `entity_expansion_budget` becomes load-bearing rather than conditional. It was already the defence against expansion attacks; it is now the only one, and it applies always.

Consumers that wanted the lean tree lose nothing they cannot get back in a walk. `txml_next_element` and `txml_find_next` skip text, comments, PIs, the declaration and the doctype without the caller testing a kind, which is the same code an elements-only consumer was already writing.

The spec corpus pays the transition cost: `@flags` lost its vocabulary (and, with ADR 0022, the directive itself) and most node tables gain the nodes that used to be dropped. That is a one-time edit against fixtures whose whole purpose is to pin this kind of decision, and after it the tables say what the parser does rather than what one configuration of it does.
