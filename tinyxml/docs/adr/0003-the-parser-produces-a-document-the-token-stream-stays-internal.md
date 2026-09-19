# ADR 0003: The parser produces a document; the token stream stays internal

**Status:** Accepted  
**Date:** 2026-09-20

## Context

An earlier draft of this design exposed two reading models: a public pull-token API (`next token` returning start, end, text, comment, PI) for streaming consumers, and a tree on top of it for everyone else. That is the shape Expat and `xmlTextReader` users are used to, and it looks like it costs nothing to offer both.

It costs plenty. Two public models mean two sets of lifetime rules, two sets of conformance tests, two things a binding must wrap, and two documents describing what a well-formedness error means. It also forces artifacts of the token layer into the public contract: text arrives in fragments because a run may span a chunk boundary or be interrupted by an entity reference, and line-end normalization splits a run at every `\r`. Every consumer then reassembles what the parser deliberately took apart.

## Decision

The parser's product is a `txml_doc`. The token layer exists inside the implementation and is not public.

Streaming is served by the document itself rather than by a second API. The columns are append-only and in document order, so a consumer may navigate what has been parsed while parsing continues: `txml_parser_doc` is valid from `txml_parser_init` onward. An element whose end tag has not arrived carries `TXML_NODE_OPEN`, its `txml_subtree_end` is `TXML_INDEX_NONE`, and a movement that cannot yet be answered returns `TXML_PENDING` instead of `TXML_NONE`. The three-valued `txml_move` is what lets one API serve both styles: parse to `TXML_STEP_DONE` and walk, or interleave `txml_parse_step` with cursor moves and process each subtree as it closes.

Because normalization happens while copying bytes into the pool (ADR 0008), the fragmenting disappears from the model. Entity references are expanded, `\r\n` and lone `\r` become `\n`, and attribute-value whitespace is normalized, all before a node is appended. One text run is therefore always one contiguous `txml_str`, whatever the chunk boundaries were.

## Consequences

There is one reading model to learn, to test, to document, and to bind. A Ruby, Python, Node, or WASM binding wraps cursors — plain values over a handle — and never has to marshal a callback or a token union across the boundary.

Chunk boundaries become invisible to consumers. This is directly testable, and ADR 0009 makes it a property the spec suite checks: splitting the same input at every possible boundary must produce an identical document.

The cost is that a consumer who only wants to count elements still materializes nodes for the subtree it is scanning. ADR 0013's mark-and-rewind is the answer for record-shaped documents, keeping memory flat without reintroducing a token API. If a genuine need for tokens appears later, it can be added as a separate reading mode with its own ADR; it is deliberately not part of the first public surface.
