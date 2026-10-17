# ADR 0025: The pool directory grows with the document

**Status:** Accepted  
**Date:** 2026-10-17  
**Amends:** [ADR 0004](0004-documents-are-index-addressed-columns-in-document-order.md) (for the pool only)

## Context

The string pool is paged like a column: a byte at offset `o` lives at
`pool_directory[o >> pool_shift] + (o & mask)`, with pages the size of
`chunk_size` (clamped to 1024..65536). The directory was allocated once per
document, sized for the largest document the limits allow
(`max_document_bytes / page + 2` pointers), and every slot set to NULL.

An embedder that parses many small documents fits the chunks to them. The Node
binding parses the XML content of a chart's data per event; at 4096 B chunks a
host keeps 787 KB less than at the default 64 KiB, but each parse built a
directory of 16,386 pointers (128 KiB, every one written) against 1,026, and a
parse cost 14 to 27 % more (`tinyactors-napi/docs/allocation.md`, finding 8).

## Decision

The pool directory starts at 16 slots and doubles when a page past its end is
needed, up to the old size. Growing allocates a new array in the values arena,
copies the page pointers and abandons the old array there (counted in `used`,
so the arrays of one document sum to about twice the last). The pages do not
move: no offset, `txml_str` or pending run changes address, so invariant 7
("nothing relocates") holds for everything a caller or a stored offset refers
to. Columns keep their directories sized once: theirs are small (683 pointers
for the default limits), and an index into them is a shift, a mask and two
loads.

## Consequences

- A small document's parse costs the same at any chunk size; its directory is
  16 pointers.
- A large document pays a copy of the directory at each doubling, about the
  size of its final directory in all, and keeps the abandoned arrays in the
  values arena until it is destroyed.
- Reaching the limit is unchanged: a page past `max_document_bytes / page + 2`
  is `TXML_ERR_LIMIT`.
