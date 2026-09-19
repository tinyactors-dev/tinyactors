# ADR 0005: Subtrees are index ranges and cursors are values

**Status:** Accepted  
**Date:** 2026-09-20

## Context

Given the preorder columns of ADR 0004, the library still has to decide what a caller holds and what structural questions cost. A DOM hands out node pointers, which makes "is A an ancestor of B" an O(depth) walk, "how many descendants" a full traversal, and "which of these two comes first in the document" a walk to a common ancestor. Those are precisely the questions a consumer of a statechart, a configuration file, or a feed asks constantly.

## Decision

Every node stores `end`, one past the index of its last descendant, so a subtree is the half-open range `[txml_index(c), txml_subtree_end(c))`. From that one invariant:

- the first child is `index + 1` when the range is non-empty;
- the next sibling is `end`, when `end` is still below the parent's `end`;
- skipping a subtree is a jump to `end`, not a traversal;
- `txml_contains(ancestor, node)` is two integer comparisons;
- the descendant count is `end - index - 1`;
- document order between two nodes is the comparison of their indices.

Every node also stores `parent`, so a cursor needs no stack to move upward and a cursor obtained from `txml_cursor_at` is immediately as capable as one reached by walking.

A cursor is therefore a value: `txml_cursor` is a document pointer plus a node index, and `txml_attr_cursor` is the same shape for attributes. Cursors are copied, stored in arrays, passed by value, and discarded; they own nothing, allocate nothing, and need no destructor. Navigation takes the cursor by pointer and moves it only on `TXML_MOVED`, so a failed move never loses the caller's position.

Range iteration is public through `txml_index`, `txml_cursor_at`, `txml_subtree_end`, and `txml_find_next`, which scans a node range for a name ID. A consumer that wants every descendant writes a flat `for` loop over indices — no recursion, no explicit stack, and no allocation, which is also what the no-recursion constraint in `AGENTS.md` demands of the library itself.

## Consequences

The structural queries that a tree API usually makes expensive become integer arithmetic, and they stay constant-time regardless of document depth. A consumer can store thousands of results as `uint32_t` indices instead of pointers, and those indices remain meaningful across a `txml_parse_step`, because columns only ever grow.

Because a cursor is two words with no ownership, bindings for Ruby, Python, Node, or WASM wrap it trivially and can pass it by value. There is no cursor lifetime to manage beyond the document's own.

Two rules follow and are stated in the header. Cursors and name IDs belong to one document; mixing them across documents is API misuse, not a comparison that happens to be wrong. And the range functions need a closed node: while an element is open, `txml_subtree_end` is `TXML_INDEX_NONE` and `txml_contains` is false, which is the streaming case of ADR 0003 and the only sharp edge in this design. `txml_doc_rewind` (ADR 0013) is the one operation that can invalidate an index, and it says so.
