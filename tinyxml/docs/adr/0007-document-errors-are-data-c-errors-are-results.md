# ADR 0007: Document errors are data; C errors are results

**Status:** Accepted  
**Date:** 2026-09-20

## Context

Two completely different things can go wrong in an XML library, and C APIs habitually merge them into one error enum. A document can be malformed — an unclosed tag, a bad byte sequence, an undeclared prefix — which is ordinary, expected, and entirely under the control of whoever produced the input. Separately, the *caller* can be wrong: a null handle, a cursor from another document, `txml_parser_finish` before the parse is done. The first needs a position and a message a human can act on; the second is a bug in the calling program and needs no position at all.

The core faces the same split and resolves it in ADR 0008: SCXML `error.*` events are control flow, `ta_result` is for C API failure. txml follows that precedent.

## Decision

A malformed document ends the parse with `TXML_STEP_FAILED`. The details are then read as data through `txml_parser_error`, which fills a `txml_document_error`: a `txml_document_error_code` saying what kind of failure it was, a `txml_limit_kind` that is meaningful when the code is `TXML_DOC_LIMIT`, a `txml_position` with byte offset, line, and column, and a fixed English `message` owned by the library. The message is a diagnostic for humans; the code is the API contract, and a caller that branches on wording is doing it wrong.

`txml_result` covers everything else: `TXML_ERR_INVALID` for API misuse, `TXML_ERR_NOMEM` for a refused allocation, `TXML_ERR_LIMIT` for a limit reached outside parsing, `TXML_ERR_NOT_FOUND` for an absent name, attribute, or optional column, and `TXML_ERR_PENDING` for an answer that depends on unparsed input. `TXML_ERR_DOCUMENT` exists so that a call can say "the document is not well-formed, read `txml_parser_error`", never to carry the details itself.

Cursor calls never report document errors. Navigation returns `txml_move`, which distinguishes "no such node" (`TXML_NONE`) from "not yet known" (`TXML_PENDING`), and reading functions return the plain fact — `TXML_NAME_NONE`, an empty `txml_str`, `TXML_INDEX_NONE` — for nodes where the question does not apply.

Memory exhaustion is deliberately neither of the two. `TXML_STEP_NEED_MEMORY` is a pause, not a failure: the document is intact, the parser is resumable, and the caller decides whether to extend the budget (ADR 0002).

## Consequences

An embedder can report a parse failure usefully — file, line, column, and cause — without the library knowing what a file is. The positions come from the optional position tracking only for nodes; the error position is always available, because it is recorded as the parser scans rather than stored per node.

Tests can assert on failures precisely. The spec suite pins the code, the limit kind, and the position of every malformed case, which is far stronger than asserting that parsing "failed", and it is why `txml_position` counts bytes rather than characters: the expected column is unambiguous.

A caller can also legitimately ignore the distinction at first — check `txml_parse_step` for `TXML_STEP_FAILED`, print the message, move on — and reach for codes only when it wants to react differently to, say, `TXML_DOC_LIMIT` than to `TXML_DOC_SYNTAX`.
