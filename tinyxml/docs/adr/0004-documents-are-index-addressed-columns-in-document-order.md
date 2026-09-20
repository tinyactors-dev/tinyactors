# ADR 0004: Documents are index-addressed columns in document order

**Status:** Accepted; amended by [ADR 0017](0017-the-parser-produces-a-concrete-syntax-tree.md) (the positions column is no longer optional) and [ADR 0016](0016-a-document-owns-two-arenas-values-and-tree.md) (the columns are split across two arenas)  
**Date:** 2026-09-20

## Context

A conventional DOM allocates a node per element and links it to parent, children, and siblings with pointers. libxml2's `xmlNode` is roughly 120 bytes of mostly pointers on a 64-bit target. That representation costs an allocation per node, scatters a traversal across the heap, and makes every structural question a pointer walk. It also ties the layout to the public API: hand out an `xmlNode *` and the struct is frozen forever.

txml parses in document order and never needs to insert a node between two existing ones, which removes the reason a linked representation exists.

## Decision

A document is a set of columns, each a paged vector of fixed-size entries allocated from the document's arena (ADR 0002), and each addressed by a `uint32_t` index. Nodes are appended in document order, so a node's index is both its identity and its position: index order *is* document order.

- **nodes**, 24 bytes: `kind` (`uint8_t`), `flags` (`uint8_t`), `depth` (`uint16_t`), `parent` (`uint32_t`), `end` (`uint32_t`), `name` (`uint32_t`), and an eight-byte union — `{ first_attribute, attribute_count }` for an element, `{ offset, length }` into the pool for text, comment, and PI data.
- **attributes**, 12 bytes: `name`, `value_offset`, `value_length`, contiguous per element so an element needs only a first index and a count.
- **names**: the interned expanded names (ADR 0006), each referring to a namespace entry and to pool bytes for the local part and the prefix as written.
- **namespaces**: deduplicated URIs, so comparing namespaces is comparing integers.
- **pool**: the bytes — normalized text, attribute values, name characters, URIs — allocated as runs from the arena.
- **positions**, 16 bytes, present only under `TXML_KEEP_POSITIONS`, parallel to `nodes`.

`end` is the only field ever rewritten: it is `TXML_INDEX_NONE` while the element is open and is patched to one past the last descendant when the end tag arrives. The parser tracks open elements on its own bounded stack, so the patch is O(1) and requires no search.

Columns are struct-of-arrays rather than one array of fat records because the scans that matter read one field. `txml_find_next` reads only `name`; a histogram reads only `kind` and `name`. The page directory of each column is sized once at init from `limits.max_document_bytes`, so index-to-address is a shift, a mask, and two loads, with no chunk walk.

## Consequences

Parsing is append-only and allocation-light: no per-node allocation, and a traversal walks memory forward. Storing a query result costs four bytes, not eight, and the result can be sorted, compared for document order, or written to a file.

The layout stays private. Callers hold indices and cursors (ADR 0005), never pointers into a column, so moving `name` into its own column, adding a field, or splitting hot from cold data is an internal change. ADR 0015 depends on exactly this freedom.

The representation is also naturally relocatable: it contains indices and pool offsets, never internal pointers. A future compaction step could flatten a finished document into one contiguous image that is `memcpy`-able, shareable read-only across threads, or emitted as constant data, in the spirit of core ADR 0019. That is not part of this decision, only enabled by it.

The trade-off is that the structure is fixed once parsed. There is no insert, no remove, and no reparent; editing a document means writing a new one through the writer (ADR 0011). For a library whose purpose is reading documents and re-emitting them, that is the right side of the trade.
