# Invariants

Rules an implementation must never violate. Each one is checkable — by a
test, by a compiler flag, or by reading a diff — and each names the mechanism
it protects. A change that breaks one of these is wrong even when the suite is
green.

## Environment

1. **No C library.** Only `stdint.h`, `stddef.h`, `stdbool.h` appear in
   `src/`. Checkable: grep the sources for `#include <`; the build links with
   `-ffreestanding` and `-nostdlib` for the library objects.
2. **No I/O and no clock.** No call can open, read, write, map, or time
   anything. There is no callback through which an embedder could supply one.
   Checkable: the only external symbols an object file references are the
   embedder's two allocator function pointers.
3. **No global mutable state.** Every `static` in `src/` is `const`. Two
   parsers in two threads share nothing and need no locking. Checkable: a
   test asserts the library's `.data`/`.bss` contribution is zero.
4. **No recursion.** No function in `src/` reaches itself, directly or
   through a cycle, and no C-stack depth depends on document nesting.
   Checkable: a call-graph pass over the sources; a test parses a 100 000-deep
   document within a limit raised for the purpose.
5. **No VLAs, no `alloca`.** Every buffer is either a fixed-size struct member
   or an arena allocation. Checkable: `-Wvla -Werror`.
6. **Reads stay in bounds.** A state reads only `[start, limit)` of the
   staging window, which is whole valid characters. A scanner may read beyond
   the last input byte only within the window's 64 bytes of padding. Checkable:
   ASan and valgrind runs of the suite.

## Memory

7. **Nothing relocates.** No memory that was handed to a caller, or that any
   stored offset or index refers to, ever changes address. The allocator
   vtable has no `realloc` and none is simulated. The structures rebuilt on
   growth are the parser's name hash index, which holds name IDs only, and
   the pool's page directory, which holds pointers to pages that stay where
   they are; neither is referenced from outside the document.
8. **A string never spans a pool page.** Every `txml_str` a document returns
   is one contiguous run. An oversized run reserves consecutive directory
   slots that point into a single arena block. A run that streams in moves to
   a bigger block as it grows, but only while pending, before anything can
   refer to it.
9. **Every `txml_str` from a document points into that document's values
   arena.** Never into the tree arena, never into the staging window, never
   into a caller chunk, never into static storage — except
   `txml_document_error.message`, which is documented as static.
10. **Page directories are allocated once.** Their capacity comes from
    `limits.max_document_bytes` at initialization, so they never grow and
    therefore never move.
11. **Parser and document memory are separate.** Transient parser state lives
    in the parser's own arena; the two arenas `txml_doc_arena(doc, which)`
    reports count document bytes only, and `max_document_bytes` bounds the
    sum of their `used`.
12. **`TXML_STEP_NEED_MEMORY` leaves the parser unchanged.** A step that
    cannot allocate has appended no partial node, no partial attribute and no
    partial pool run; calling it again after the allocator recovers produces
    the same result as if the failure had not happened.
13. **A document owns both of its arenas.** Values and tree are created over
    `config.allocator` at `txml_parser_init` and destroyed by
    `txml_doc_destroy`, unconditionally; no arena is ever borrowed from the
    caller and none outlives the document. Checkable: a counting allocator
    sees every byte returned after the document is destroyed.
    Correspondingly, a pool offset, a name entry and a namespace entry live in
    the **values** arena, and a node, attribute and position entry live in the
    **tree** arena — so a release of one cannot invalidate a reference held by
    the other.

## Document structure

14. **Node indices are dense and monotonic.** Nodes occupy `[0,
    txml_doc_node_count(doc))` with no holes, and are appended in document
    order. Node 0 is always `TXML_NODE_DOCUMENT`.
15. **`parent < index` for every node except node 0**, whose `parent` is
    `TXML_INDEX_NONE`. A parent is always appended before its children.
16. **`end > index` for every closed node**, and `end == TXML_INDEX_NONE`
    exactly while `TXML_NODE_OPEN` is set.
17. **`end` is written exactly once per node** — at append time for leaves and
    empty-element tags, at the end tag otherwise — and no other node field is
    ever rewritten after the node is appended.
18. **A subtree is a range.** For every node, its descendants are exactly the
    nodes at `[index + 1, end)`, and `end(child) <= end(parent)` for every
    child.
19. **`depth` agrees with `parent`.** `depth(node) == depth(parent) + 1` for
    every node except node 0, whose depth is 0.
20. **An element's attributes are contiguous** at `[first_attribute,
    first_attribute + attribute_count)` and in document order. Non-element
    nodes have `attribute_count == 0`.
21. **Name IDs are dense from 0** in interning order, and a name ID is
    meaningful only for the document that issued it. `txml_doc_find_name` and
    `txml_node_name_id` agree: resolving a node's expanded name returns that
    node's name ID.
22. **Namespace 0 is the empty URI** and exists in every document, used or
    not. An unprefixed attribute's name always has namespace 0.
23. **The positions column always exists**, with exactly one entry per node.
    `txml_node_position` succeeds for every node of every document; it reports
    `TXML_ERR_INVALID` only for a cursor that is not on a node. Each entry also
    holds where the node ends (ADR 0023): `txml_node_span` succeeds for every
    closed node and is `TXML_ERR_PENDING` for an open one, and the end of a
    node never precedes its start.
24. *Withdrawn with ADR 0018, which removed `txml_doc_rewind`. The number is
    left in place because ADRs 0016 and 0018 cite it.*

## Behaviour

25. **No cursor call allocates.** `txml_first_child`, `txml_next_sibling`,
    `txml_parent`, `txml_next_element`, `txml_find_next`, every reader and
    every attribute call perform zero allocations and cannot fail for lack of
    memory. Checkable: run the suite with an allocator that aborts, after the
    parse completes.
26. **Cursor calls are total.** They never report misuse; an out-of-range or
    foreign cursor reads as absent (`TXML_NONE`, empty strings) rather than
    crashing.
27. **Document errors are data.** A malformed document produces
    `TXML_STEP_FAILED` plus a `txml_document_error` with a position; it never
    produces `TXML_ERR_INVALID`. Conversely, `TXML_ERR_INVALID` is never
    caused by document content.
28. **Terminal states are sticky.** After `TXML_STEP_DONE` or
    `TXML_STEP_FAILED`, further steps repeat the same outcome and append
    nothing.
29. **A failed parse leaves a readable document.** Nodes appended before the
    failure remain valid and navigable.
30. **Every document-controlled cost is bounded by `txml_limits`.** Nesting,
    attribute count, name length, distinct names, total bytes, and entity
    expansion each have a limit, and reaching one produces `TXML_DOC_LIMIT`
    with the matching `txml_limit_kind` — never an allocation loop.
31. **The writer emits only what it can read back.** Every byte it produces
    re-parses to the events it was given, modulo the fidelity table in
    `conformance.md`. Invalid caller input is refused with `TXML_ERR_INVALID`
    before any byte of it is written.
32. **`TXML_NEED_SPACE` is resumable without copying.** The writer holds a
    pointer and offset into the caller's string; only `txml_writer_output`,
    `txml_writer_drain` and `txml_writer_destroy` are legal until the
    operation completes.
33. **Chunk-split independence.** The document produced by a parse does not
    depend on how the input was divided into `txml_parse_feed` calls. This is
    the property the fuzzer targets.

## The concrete syntax tree

34. **Nothing the input contains is dropped.** Every comment, processing
    instruction, text run including a whitespace-only one, the XML declaration
    and the DOCTYPE become nodes, and no configuration changes that, because
    there is no configuration: two parses of the same bytes produce the same
    node table. Checkable: the spec corpus carries no retention flag, because
    there is none left to carry.
35. **A CDATA section is its own text node.** It is flagged `TXML_NODE_CDATA`
    and never merges with the character data on either side, so section
    boundaries survive into the document.
36. **The declaration and the doctype are leaves.** A `TXML_NODE_DECLARATION`
    carries its version, encoding and standalone as ordinary attributes in
    namespace 0 and no text; a `TXML_NODE_DOCTYPE` carries the declaration as
    written as its text and no attributes. Neither is ever `TXML_NODE_OPEN`.
