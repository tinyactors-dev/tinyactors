# Cursors and the range algebra

A `txml_cursor` is `{const txml_doc *doc; uint32_t node;}` — a value. It owns
nothing, allocates nothing, and survives any amount of further parsing. Every
navigation function is a handful of integer operations on the node column.

The whole design rests on one property of the node column:

> Nodes are appended in document order, so a node's descendants are exactly the
> nodes at indices `[index + 1, end)`. **A subtree is a contiguous range.**

## The algebra

Let `n` be the node at `index`, and write `end(i)` for the `end` field.

| Question | Answer | Cost |
|---|---|---|
| First child | `index + 1`, if `index + 1 < end(index)` | one comparison |
| Next sibling | `end(index)`, if `end(index) < end(parent)` | one comparison |
| Parent | the stored `parent` field | one load |
| Is `b` inside `a`? | `index(a) <= index(b) && index(b) < end(a)` | two comparisons |
| Document order of `a`, `b` | `index(a) < index(b)` | one comparison |
| Descendant count of `a` | `end(a) - index(a) - 1` | one subtraction |
| Skip a subtree | jump to `end(index)` | no traversal |
| Whole subtree as a loop | `for (i = index; i < end(index); i++)` | sequential memory |

`txml_first_child`, `txml_next_sibling`, `txml_parent` and `txml_next_element`
implement exactly these. `txml_next_element` is `txml_next_sibling` repeated
until the node's kind is `TXML_NODE_ELEMENT`; it never descends.

Two consequences worth stating explicitly:

- **A cursor never needs a stack.** Upward movement uses the stored `parent`,
  so there is no walk state to keep and no recursion anywhere in the cursor
  API. This is what keeps `txml_cursor` copyable and storable.
- **Document order is index order.** A consumer that collects cursors and
  sorts them by `txml_index` has them in document order, which is the order
  SCXML requires for entering states.

## Node 0 and the edges

- Node 0 is `TXML_NODE_DOCUMENT` with `parent == TXML_INDEX_NONE`.
  `txml_parent` on it returns `TXML_NONE`.
- `txml_doc_root(doc)` returns a cursor on node 0, not on the document
  element. The document element is its first element child.
- `txml_subtree_end` of node 0 equals `txml_doc_node_count(doc)` once the
  parse is done.
- `txml_cursor_at(doc, index)` with `index >= txml_doc_node_count(doc)`
  yields a cursor that every accessor treats as absent: `txml_kind` reports
  `TXML_NODE_DOCUMENT`, moves report `TXML_NONE`, and reads return empty.
  Cursor calls never return `txml_result`, so they cannot report misuse; they
  are total functions by construction.

## While the document is still growing

An element whose end tag has not been parsed carries `TXML_NODE_OPEN` and has
`end == TXML_INDEX_NONE`. Anything that would have to read `end` of an open
node cannot be answered yet:

| Call | On an open node |
|---|---|
| `txml_subtree_end` | `TXML_INDEX_NONE` |
| `txml_contains(open, x)` | `false` |
| `txml_next_sibling` | `TXML_PENDING` |
| `txml_next_element` | `TXML_PENDING` |
| `txml_find_next` | `TXML_PENDING` when it reaches the open frontier before `bound` |
| `txml_first_child` | works as usual: `index + 1` exists as soon as a child is appended |

`TXML_PENDING` is the difference between "there is no such node" and "ask me
again after the next step". A consumer that walks a finished document never
sees it; a streaming consumer treats it as "step, then retry".

After `TXML_STEP_DONE`, no node is open, and `TXML_PENDING` never occurs
again.

## Name scans

`txml_find_next` is the fastest query the document supports:

```c
txml_find query = { .name = name_id, .bound = txml_subtree_end(root) };
txml_cursor c = root;
while (txml_find_next(&c, &query) == TXML_MOVED) { /* ... */ }
```

It advances the index by one and compares the `name` column until it finds a
match below `bound`, and it is defined only in terms of that column: no
strings, no kind test beyond elements, no pointer chasing. `bound ==
TXML_INDEX_NONE` means the whole document.

Two properties make it cheap in practice:

1. **A name that the document does not contain costs nothing.**
   `txml_doc_find_name` returns `TXML_NAME_NONE`, and the caller skips the
   scan entirely. No traversal can beat not traversing.
2. **Bounding by a subtree is free.** `bound = txml_subtree_end(c)` restricts
   the scan to a subtree without any tree walking.

The scan reads one `uint32_t` per candidate node. The node record is 24 bytes,
so a future layout change that splits the `name` column out of the record
raises the candidates per cache line from about 2.7 to 16, and the same loop
is the natural place for a SIMD compare. Because callers only ever hold
indices, that change needs no API change.

## Worked query

Using the seven-node document from `document-layout.md`: for every
`<transition>`, report the state it leaves and check whether its target is
inside that same state.

```c
/* 1. An id index. A subtree is a range, so this is one flat loop:
      no recursion, no stack, sequential memory. */
typedef struct { txml_str id; uint32_t node; } id_entry;

uint32_t id_name = txml_doc_find_name(doc, &(txml_qname){ TXML_STR(""), TXML_STR("id") });
uint32_t end = txml_subtree_end(txml_doc_root(doc));
uint32_t count = 0;

for (uint32_t i = 0; i < end && count < capacity; i++) {
    txml_str id;
    txml_cursor c = txml_cursor_at(doc, i);
    if (txml_find_attribute(c, id_name, &id)) {
        entries[count++] = (id_entry){ id, i };
    }
}

/* 2. Every transition, found by scanning the name column. */
txml_qname transition_qname = { TXML_STR("http://www.w3.org/2005/07/scxml"),
                                TXML_STR("transition") };
txml_find query = { .name = txml_doc_find_name(doc, &transition_qname), .bound = end };
if (query.name == TXML_NAME_NONE) return;          /* nothing to do */

txml_cursor t = txml_doc_root(doc);
while (txml_find_next(&t, &query) == TXML_MOVED) {
    txml_cursor source = t;
    txml_parent(&source);                          /* one load: the stored parent */

    txml_str target_id;
    if (!txml_find_attribute(t, target_name, &target_id)) continue;

    txml_cursor target = txml_cursor_at(doc, lookup(entries, count, target_id));
    bool internal = txml_contains(source, target); /* two comparisons */
    /* ... report ... */
}
```

On the example document the scan visits nodes 1 through 6 and matches 3, 5 and
6. For node 3, `source` is node 2 (`[2, 4)`) and the target `opening` is node
4, so `txml_contains` is `2 <= 4 && 4 < 4` — false, the transition leaves its
state. For node 6, `source` is node 4 (`[4, 7)`) and the target `closed` is
node 2: `4 <= 2` is false, so again it leaves. Both answers cost two integer
comparisons and no tree walking.

A pointer-based DOM answers the same containment question by walking parent
pointers, which is `O(depth)` per test and touches a cache line per hop.
