# ADR 0010: Every unbounded construct has a limit

**Status:** Accepted  
**Date:** 2026-09-20

## Context

XML lets a document dictate how much work and memory a parser spends. Nesting can be arbitrarily deep, an element can carry arbitrarily many attributes, names can be arbitrarily long and arbitrarily numerous, and internal entities can expand exponentially — the billion-laughs attack, which is a handful of bytes that expands to gigabytes. Parsers have historically bolted defenses on afterwards, one CVE at a time, often as hard-coded thresholds or heuristics the embedder cannot see or adjust.

txml cannot afford that approach twice over. It targets environments with a fixed memory budget, where "it usually does not happen" is not a safety property, and it forbids recursion, so deep nesting must be a number rather than a stack overflow.

## Decision

Every document-controlled cost has a field in `txml_limits`, which the embedder supplies in `txml_parser_config`:

- `max_depth` bounds element nesting, and with it the parser's open-element stack.
- `max_attributes` bounds the attributes on one element, and with it the start-tag scratch the parser must buffer before it can resolve namespaces.
- `max_name_length` bounds the bytes of one qualified name.
- `max_names` bounds distinct expanded names per document, so name-table flooding hits a wall (ADR 0006).
- `max_document_bytes` bounds the total document arena, which also fixes the page directories of ADR 0004.
- `entity_expansion_budget` bounds the bytes produced by entity expansion across the whole document. This is the billion-laughs defense, expressed as a byte budget rather than a depth or ratio heuristic: whatever the shape of the expansion, the total output is capped.

`TXML_LIMITS_DEFAULT` gives sensible values for a server-sized host and is a macro rather than an extern constant, so a caller — and a header-only compile test — can use it without linking anything.

Reaching a limit is a document error: the parse ends with `TXML_STEP_FAILED`, and `txml_parser_error` reports `TXML_DOC_LIMIT` with a `txml_limit_kind` naming which one and a position saying where. It is not an API failure, because the caller did nothing wrong; it is a fact about the input (ADR 0007).

## Consequences

Memory use is a function of configuration, not of input. An embedder can compute a worst case before parsing anything, which is what makes the fixed-budget and firmware profiles honest rather than hopeful.

Security defenses stop being special cases. There is no "entity expansion protection" mode to enable, no ratio heuristic to tune, and no threshold hidden in the implementation: the limits are the defense, they are visible in the caller's own code, and a caller who wants stricter behaviour lowers a number.

Because a limit failure names its kind, an embedder can react differently to a document that is merely bigger than expected than to one that is hostile — raise `max_names` for the first, reject the second — instead of seeing one opaque failure.

The cost is that a legitimate document exceeding a default is rejected until the caller raises the limit. That is the intended trade: a parser in a fixed budget must fail predictably rather than allocate optimistically, and the error says exactly which number to change.
